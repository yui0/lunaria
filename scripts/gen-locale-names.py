#!/usr/bin/env python3
"""Regenerates the tables in src/dvm/locale_names.h from ICU (libicuuc) and tzdata.
Usage: gen-locale-names.py OUT.inc   (then paste the arrays into the header, sorted by code)."""
lib = ctypes.CDLL('libicuuc.so.77')
def fn(n): return getattr(lib, n+'_77')
getc = fn('uloc_getDisplayCountry'); getl = fn('uloc_getDisplayLanguage')
UErr = ctypes.c_int
def disp(f, code, loc):
    buf = (ctypes.c_uint16 * 128)()
    err = UErr(0)
    n = f(code.encode(), loc.encode(), buf, 128, ctypes.byref(err))
    if err.value > 0 or n <= 0: return ''
    return b''.join(int(x).to_bytes(2,'little') for x in buf[:n]).decode('utf-16-le')
# ISO country list from tzdata
countries = []
for l in open('/usr/share/zoneinfo/iso3166.tab', encoding='utf-8'):
    if l.startswith('#') or not l.strip(): continue
    countries.append(l.split('\t')[0])
# extra codes ICU knows that tzdata omits
countries = sorted(set(countries) | {'XK'})
langs = ['aa','ab','af','ak','am','an','ar','as','av','ay','az','ba','be','bg','bh','bi','bm','bn','bo','br','bs','ca','ce','ch','co','cr','cs','cu','cv','cy','da','de','dv','dz','ee','el','en','eo','es','et','eu','fa','ff','fi','fj','fo','fr','fy','ga','gd','gl','gn','gu','gv','ha','he','hi','ho','hr','ht','hu','hy','hz','ia','id','ie','ig','ii','ik','io','is','it','iu','ja','jv','ka','kg','ki','kj','kk','kl','km','kn','ko','kr','ks','ku','kv','kw','ky','la','lb','lg','li','ln','lo','lt','lu','lv','mg','mh','mi','mk','ml','mn','mr','ms','mt','my','na','nb','nd','ne','ng','nl','nn','no','nr','nv','ny','oc','oj','om','or','os','pa','pi','pl','ps','pt','qu','rm','rn','ro','ru','rw','sa','sc','sd','se','sg','si','sk','sl','sm','sn','so','sq','sr','ss','st','su','sv','sw','ta','te','tg','th','ti','tk','tl','tn','to','tr','ts','tt','tw','ty','ug','uk','ur','uz','ve','vi','vo','wa','wo','xh','yi','yo','za','zh','zu','fil']
def cesc(s): return s.replace('\\','\\\\').replace('"','\\"')
out = []
out.append('/* Generated from ICU (CLDR) display names: English and Japanese.  Regenerate with\n * scratch locnames.py; codes are ISO 3166-1 alpha-2 and ISO 639-1/-2. */')
for kind, codes, f in (('country', countries, getc), ('language', langs, getl)):
    out.append('static const struct loc_name loc_%s_names[] = {' % kind)
    for c in codes:
        en = disp(f, c if kind=='language' else '_'+c, 'en'); ja = disp(f, c if kind=='language' else '_'+c, 'ja')
        # ICU returns the code itself when it has no name
        if not en or en.lower()==c.lower() or en.startswith('Unknown'): continue
        out.append('   { "%s", "%s", "%s" },' % (c, cesc(en), cesc(ja or en)))
    out.append('};')
open(sys.argv[1],'w',encoding='utf-8').write('\n'.join(out)+'\n')
print(len(countries), len(langs))
