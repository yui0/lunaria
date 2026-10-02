/* SPDX-License-Identifier: MIT
 * From musl 1.2.5 src/regex/fnmatch.c; see COPYRIGHT in this directory.
 * Lunaria changes: public symbol/flags namespaced, libc-private include
 * removed (unused here), UTF-8 decoded independently of host wchar_t,
 * character classes/case mapping provided by the compiled Unicode tables, and the public header included. */
/*
 * An implementation of what I call the "Sea of Stars" algorithm for
 * POSIX fnmatch(). The basic idea is that we factor the pattern into
 * a head component (which we match first and can reject without ever
 * measuring the length of the string), an optional tail component
 * (which only exists if the pattern contains at least one star), and
 * an optional "sea of stars", a set of star-separated components
 * between the head and tail. After the head and tail matches have
 * been removed from the input string, the components in the "sea of
 * stars" are matched sequentially by searching for their first
 * occurrence past the end of the previous match.
 *
 * - Rich Felker, April 2012
 */

#include <string.h>
#include "../../lunaria_os.h"
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include "luna_unicode.h"

/* Bionic strings are UTF-8; Windows wchar_t cannot hold all Unicode scalars.
 * Reject overlong encodings, surrogates and incomplete sequences. */
static int utf8_next(int32_t *value, const char *text, size_t available)
{
    if (!available) return -1;
    const unsigned char *bytes = (const unsigned char *)text;
    if (bytes[0] < 0x80) { *value = bytes[0]; return bytes[0] ? 1 : 0; }
    unsigned count;
    uint32_t scalar, minimum;
    if (bytes[0] >= 0xc2 && bytes[0] <= 0xdf) { count=2; scalar=bytes[0]&31; minimum=0x80; }
    else if (bytes[0] >= 0xe0 && bytes[0] <= 0xef) { count=3; scalar=bytes[0]&15; minimum=0x800; }
    else if (bytes[0] >= 0xf0 && bytes[0] <= 0xf4) { count=4; scalar=bytes[0]&7; minimum=0x10000; }
    else return -1;
    for (unsigned i=1; i<count; ++i) {
        if (i >= available || bytes[i] < 0x80 || bytes[i] > 0xbf) return -1;
        scalar = (scalar<<6) | (bytes[i]&63);
    }
    if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return -1;
    *value = (int32_t)scalar;
    return (int)count;
}
static int unicode_class(int scalar, const char *name)
{
    if (scalar < 0x80) {
        if (!strcmp(name,"alnum")) return isalnum(scalar);
        if (!strcmp(name,"alpha")) return isalpha(scalar);
        if (!strcmp(name,"blank")) return scalar == ' ' || scalar == '\t';
        if (!strcmp(name,"cntrl")) return iscntrl(scalar);
        if (!strcmp(name,"digit")) return isdigit(scalar);
        if (!strcmp(name,"graph")) return isgraph(scalar);
        if (!strcmp(name,"lower")) return islower(scalar);
        if (!strcmp(name,"print")) return isprint(scalar);
        if (!strcmp(name,"punct")) return ispunct(scalar);
        if (!strcmp(name,"space")) return isspace(scalar);
        if (!strcmp(name,"upper")) return isupper(scalar);
        if (!strcmp(name,"xdigit")) return isxdigit(scalar);
        return 0;
    }
    if (!strcmp(name,"alnum")) return luna_uc_property(scalar,LUNA_UC_ALNUM);
    if (!strcmp(name,"alpha")) return luna_uc_property(scalar,LUNA_UC_ALPHABETIC);
    if (!strcmp(name,"blank")) return luna_uc_blank(scalar);
    if (!strcmp(name,"cntrl")) return luna_uc_control(scalar);
    if (!strcmp(name,"digit")) return luna_uc_digit(scalar);
    if (!strcmp(name,"graph")) return luna_uc_graph(scalar);
    if (!strcmp(name,"lower")) return luna_uc_property(scalar,LUNA_UC_LOWERCASE);
    if (!strcmp(name,"print")) return luna_uc_print(scalar);
    if (!strcmp(name,"punct")) return luna_uc_punct(scalar);
    if (!strcmp(name,"space")) return luna_uc_space(scalar);
    if (!strcmp(name,"upper")) return luna_uc_property(scalar,LUNA_UC_UPPERCASE);
    if (!strcmp(name,"xdigit")) return luna_uc_xdigit(scalar);
    return 0;
}

#define END 0
#define UNMATCHABLE -2
#define BRACKET -3
#define QUESTION -4
#define STAR -5

static int str_next(const char *str, size_t n, size_t *step)
{
	if (!n) {
		*step = 0;
		return 0;
	}
	if ((unsigned char)str[0] >= 128U) {
		int32_t wc;
		int k = utf8_next(&wc, str, n);
		if (k<0) {
			*step = 1;
			return -1;
		}
		*step = k;
		return wc;
	}
	*step = 1;
	return str[0];
}

static int pat_next(const char *pat, size_t m, size_t *step, int flags)
{
	int esc = 0;
	if (!m || !*pat) {
		*step = 0;
		return END;
	}
	*step = 1;
	if (pat[0]=='\\' && pat[1] && !(flags & LUNA_FNM_NOESCAPE)) {
		*step = 2;
		pat++;
		esc = 1;
		goto escaped;
	}
	if (pat[0]=='[') {
		size_t k = 1;
		if (k<m) if (pat[k] == '^' || pat[k] == '!') k++;
		if (k<m) if (pat[k] == ']') k++;
		for (; k<m && pat[k] && pat[k]!=']'; k++) {
			if (k+1<m && pat[k+1] && pat[k]=='[' && (pat[k+1]==':' || pat[k+1]=='.' || pat[k+1]=='=')) {
				int z = pat[k+1];
				k+=2;
				if (k<m && pat[k]) k++;
				while (k<m && pat[k] && (pat[k-1]!=z || pat[k]!=']')) k++;
				if (k==m || !pat[k]) break;
			}
		}
		if (k==m || !pat[k]) {
			*step = 1;
			return '[';
		}
		*step = k+1;
		return BRACKET;
	}
	if (pat[0] == '*')
		return STAR;
	if (pat[0] == '?')
		return QUESTION;
escaped:
	if ((unsigned char)pat[0] >= 128U) {
		int32_t wc;
		int k = utf8_next(&wc, pat, m);
		if (k<0) {
			*step = 0;
			return UNMATCHABLE;
		}
		*step = k + esc;
		return wc;
	}
	return pat[0];
}

static int casefold(int k)
{
	int c = luna_uc_upper(k);
	return c == k ? luna_uc_lower(k) : c;
}

static int match_bracket(const char *p, int k, int kfold)
{
	int32_t wc;
	int inv = 0;
	p++;
	if (*p=='^' || *p=='!') {
		inv = 1;
		p++;
	}
	if (*p==']') {
		if (k==']') return !inv;
		p++;
	} else if (*p=='-') {
		if (k=='-') return !inv;
		p++;
	}
	wc = p[-1];
	for (; *p != ']'; p++) {
		if (p[0]=='-' && p[1]!=']') {
			int32_t wc2;
			int l = utf8_next(&wc2, p+1, 4);
			if (l < 0) return 0;
			if (wc <= wc2)
				if ((unsigned)k-(unsigned)wc <= (unsigned)wc2-(unsigned)wc ||
				    (unsigned)kfold-(unsigned)wc <= (unsigned)wc2-(unsigned)wc)
					return !inv;
			p += l-1;
			continue;
		}
		if (p[0]=='[' && (p[1]==':' || p[1]=='.' || p[1]=='=')) {
			const char *p0 = p+2;
			int z = p[1];
			p+=3;
			while (p[-1]!=z || p[0]!=']') p++;
			if (z == ':' && p-1-p0 < 16) {
				char buf[16];
				memcpy(buf, p0, p-1-p0);
				buf[p-1-p0] = 0;
				if (unicode_class(k, buf) ||
				    unicode_class(kfold, buf))
					return !inv;
			}
			continue;
		}
		if ((unsigned char)*p < 128U) {
			wc = (unsigned char)*p;
		} else {
			int l = utf8_next(&wc, p, 4);
			if (l < 0) return 0;
			p += l-1;
		}
		if (wc==k || wc==kfold) return !inv;
	}
	return inv;
}

static int fnmatch_internal(const char *pat, size_t m, const char *str, size_t n, int flags)
{
	const char *p, *ptail, *endpat;
	const char *s, *stail, *endstr;
	size_t pinc, sinc, tailcnt=0;
	int c, k, kfold;

	if (flags & LUNA_FNM_PERIOD) {
		if (*str == '.' && *pat != '.')
			return LUNA_FNM_NOMATCH;
	}
	for (;;) {
		switch ((c = pat_next(pat, m, &pinc, flags))) {
		case UNMATCHABLE:
			return LUNA_FNM_NOMATCH;
		case STAR:
			pat++;
			m--;
			break;
		default:
			k = str_next(str, n, &sinc);
			if (k <= 0)
				return (c==END) ? 0 : LUNA_FNM_NOMATCH;
			str += sinc;
			n -= sinc;
			kfold = flags & LUNA_FNM_CASEFOLD ? casefold(k) : k;
			if (c == BRACKET) {
				if (!match_bracket(pat, k, kfold))
					return LUNA_FNM_NOMATCH;
			} else if (c != QUESTION && k != c && kfold != c) {
				return LUNA_FNM_NOMATCH;
			}
			pat+=pinc;
			m-=pinc;
			continue;
		}
		break;
	}

	/* Compute real pat length if it was initially unknown/-1 */
	m = strnlen(pat, m);
	endpat = pat + m;

	/* Find the last * in pat and count chars needed after it */
	for (p=ptail=pat; p<endpat; p+=pinc) {
		switch (pat_next(p, endpat-p, &pinc, flags)) {
		case UNMATCHABLE:
			return LUNA_FNM_NOMATCH;
		case STAR:
			tailcnt=0;
			ptail = p+1;
			break;
		default:
			tailcnt++;
			break;
		}
	}

	/* Past this point we need not check for UNMATCHABLE in pat,
	 * because all of pat has already been parsed once. */

	/* Compute real str length if it was initially unknown/-1 */
	n = strnlen(str, n);
	endstr = str + n;
	if (n < tailcnt) return LUNA_FNM_NOMATCH;

	/* Find the final tailcnt chars of str, accounting for UTF-8.
	 * On illegal sequences we may get it wrong, but in that case
	 * we necessarily have a matching failure anyway. */
	for (s=endstr; s>str && tailcnt; tailcnt--) {
		if ((unsigned char)s[-1] < 128U) s--;
		else while ((unsigned char)*--s-0x80U<0x40 && s>str);
	}
	if (tailcnt) return LUNA_FNM_NOMATCH;
	stail = s;

	/* Check that the pat and str tails match */
	p = ptail;
	for (;;) {
		c = pat_next(p, endpat-p, &pinc, flags);
		p += pinc;
		if ((k = str_next(s, endstr-s, &sinc)) <= 0) {
			if (c != END) return LUNA_FNM_NOMATCH;
			break;
		}
		s += sinc;
		kfold = flags & LUNA_FNM_CASEFOLD ? casefold(k) : k;
		if (c == BRACKET) {
			if (!match_bracket(p-pinc, k, kfold))
				return LUNA_FNM_NOMATCH;
		} else if (c != QUESTION && k != c && kfold != c) {
			return LUNA_FNM_NOMATCH;
		}
	}

	/* We're all done with the tails now, so throw them out */
	endstr = stail;
	endpat = ptail;

	/* Match pattern components until there are none left */
	while (pat<endpat) {
		p = pat;
		s = str;
		for (;;) {
			c = pat_next(p, endpat-p, &pinc, flags);
			p += pinc;
			/* Encountering * completes/commits a component */
			if (c == STAR) {
				pat = p;
				str = s;
				break;
			}
			k = str_next(s, endstr-s, &sinc);
			if (!k)
				return LUNA_FNM_NOMATCH;
			kfold = flags & LUNA_FNM_CASEFOLD ? casefold(k) : k;
			if (c == BRACKET) {
				if (!match_bracket(p-pinc, k, kfold))
					break;
			} else if (c != QUESTION && k != c && kfold != c) {
				break;
			}
			s += sinc;
		}
		if (c == STAR) continue;
		/* If we failed, advance str, by 1 char if it's a valid
		 * char, or past all invalid bytes otherwise. */
		k = str_next(str, endstr-str, &sinc);
		if (k > 0) str += sinc;
		else for (str++; str_next(str, endstr-str, &sinc)<0; str++);
	}

	return 0;
}

int luna_fnmatch(const char *pat, const char *str, int flags)
{
	const char *s, *p;
	size_t inc;
	int c;
	if (flags & LUNA_FNM_PATHNAME) for (;;) {
		for (s=str; *s && *s!='/'; s++);
		for (p=pat; (c=pat_next(p, -1, &inc, flags))!=END && c!='/'; p+=inc);
		if (c!=*s && (!*s || !(flags & LUNA_FNM_LEADING_DIR)))
			return LUNA_FNM_NOMATCH;
		if (fnmatch_internal(pat, p-pat, str, s-str, flags))
			return LUNA_FNM_NOMATCH;
		if (!c) return 0;
		str = s+1;
		pat = p+inc;
	} else if (flags & LUNA_FNM_LEADING_DIR) {
		for (s=str; *s; s++) {
			if (*s != '/') continue;
			if (!fnmatch_internal(pat, -1, str, s-str, flags))
				return 0;
		}
	}
	return fnmatch_internal(pat, -1, str, -1, flags);
}
