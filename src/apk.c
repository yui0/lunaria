/* MPL-2.0. In-process APK installer/launcher. No shell or external extractor.
 * The archive remains byte-for-byte intact; inst/ is the Android installed view. */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "lunaria_os.h"
#include "dvm/charset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <zlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#define APK_PATH 4096
#define APK_META 65536
#define APK_MAX_MANIFEST (16u*1024u*1024u)
static int apk_error(const char *format, ...)
{
   va_list ap;
   va_start(ap, format);
   fprintf(stderr, "[apk] ");
   vfprintf(stderr, format, ap);
   fputc('\n', stderr);
   va_end(ap);
   return -1;
}
static int path_join(char *out, size_t cap, const char *first, const char *second)
{
   int n = snprintf(out, cap, "%s%s%s", first, *first && first[strlen(first) -1] != '/' ? "/" : "", second);
   return n < 0 || (size_t)n >= cap ? apk_error("path too long") : 0;
}
static bool exists(const char *path)
{
   return luna_file_accessat(LUNA_AT_FDCWD, path, 0, 0) == 0;
}
static bool directory(const char *path)
{
   luna_file_info st;
   return !luna_file_infoat(LUNA_AT_FDCWD, path, &st, 0) && (st.mode & 0170000) == 0040000;
}
static int mkdirs(const char *path)
{
   char p[APK_PATH];
   if (strlen(path) >= sizeof p)
      return -1;
   strcpy(p, path);
   char *begin = p + 1;
#ifdef _WIN32
   if (p[0] == '/' && p[1] == '/') {
      char *server = strchr(p + 2, '/');
      char *share = server ? strchr(server + 1, '/') : NULL;
      if (!share)
         return directory(p) ? 0 : apk_error("cannot access share: %s", p);
      begin = share + 1;
   }
#endif
   for (char *s = begin;; s++) {
      if (*s != '/' && *s)
         continue;
      char c = *s;
      *s = 0;
      if (*p && !(strlen(p) == 2 && p[1] == ':') && !directory(p) && luna_file_mkdir(p, 0700)) {
         *s = c;
         return apk_error("cannot create %s: %s", p, strerror(errno));
      }
      *s = c;
      if (!c)
         break;
   }
   return 0;
}
static FILE *open_file(const char *path, const char *mode)
{
   int fd = luna_file_open(path, *mode == 'r' ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return NULL;
   FILE *f = fdopen(fd, mode);
   if (!f)
      luna_fd_close(fd);
   return f;
}
static int seek_file(FILE *f, uint64_t at)
{
   if (at > INT64_MAX)
      return -1;
#ifdef _WIN32
   return _fseeki64(f, (int64_t)at, SEEK_SET);
#else
   return fseeko(f, (off_t)at, SEEK_SET);
#endif
}
static uint16_t le16(const unsigned char *p)
{
   return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t le32(const unsigned char *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const unsigned char *p)
{
   return le32(p) | ((uint64_t)le32(p + 4) << 32);
}
struct zip_entry {
   char *name;
   uint64_t offset, compressed, size;
   uint32_t crc, attrs;
   uint16_t method, flags;
};
struct zip {
   FILE *f;
   struct zip_entry *entries;
   size_t count;
   uint64_t size;
};
static void zip_close(struct zip *z)
{
   if (z->f)
      fclose(z->f);
   for (size_t i = 0; i < z->count; i++)
      free(z->entries[i].name);
   free(z->entries);
   memset(z, 0, sizeof *z);
}
static int zip_open(struct zip *z, const char *path)
{
   memset(z, 0, sizeof *z);
   luna_file_info st;
   if (luna_file_infoat(LUNA_AT_FDCWD, path, &st, 0) || st.size < 22)
      return apk_error("not a ZIP: %s", path);
   z->size = (uint64_t)st.size;
   z->f = open_file(path, "rb");
   if (!z->f)
      return apk_error("cannot open %s", path);
   size_t tail = (size_t)(z->size < 65557 ? z->size : 65557);
   unsigned char *buf = malloc(tail);
   if (!buf)
      goto fail;
   if (seek_file(z->f, z->size - tail) || fread(buf, 1, tail, z->f) != tail) {
      free(buf);
      goto fail;
   }
   size_t end = tail - 22;
   for (;;) {
      if (le32(buf + end) == 0x06054b50 && end + 22 + le16(buf + end + 20) == tail)
         break;
      if (!end) {
         free(buf);
         goto fail;
      }
      end--;
   }
   if (le16(buf + end + 4) || le16(buf + end + 6)) {
      free(buf);
      goto fail;
   }
   uint64_t count = le16(buf + end + 10), cd = le32(buf + end + 16), cd_size = le32(buf + end + 12), eocd = z->size - tail + end;
   free(buf);
   if (count == 65535 || cd == UINT32_MAX || cd_size == UINT32_MAX) {
      unsigned char loc[20], rec[56];
      if (eocd < 20 || seek_file(z->f, eocd - 20) || fread(loc, 1, 20, z->f) != 20 || le32(loc) != 0x07064b50 || le32(loc + 4) || le32(loc + 16) != 1)
         goto fail;
      if (seek_file(z->f, le64(loc + 8)) || fread(rec, 1, 56, z->f) != 56 || le32(rec) != 0x06064b50 || le32(rec + 16) || le32(rec + 20))
         goto fail;
      count = le64(rec + 32);
      cd_size = le64(rec + 40);
      cd = le64(rec + 48);
   }
   if (count > 1000000 || cd > z->size || cd_size > z->size - cd || count > SIZE_MAX / sizeof(*z->entries))
      goto fail;
   z->entries = calloc((size_t)count, sizeof(*z->entries));
   if (!z->entries)
      goto fail;
   uint64_t pos = cd;
   for (uint64_t i = 0; i < count; i++) {
      unsigned char h[46];
      if (pos > cd + cd_size || cd + cd_size - pos < 46 || seek_file(z->f, pos) || fread(h, 1, 46, z->f) != 46 || le32(h) != 0x02014b50)
         goto fail;
      size_t nl = le16(h + 28), xl = le16(h + 30), cl = le16(h + 32);
      if (!nl || nl >= APK_PATH || 46 + nl + xl + cl > cd + cd_size - pos)
         goto fail;
      struct zip_entry *e = z->entries + z->count++;
      e->name = malloc(nl + 1);
      unsigned char *extra = malloc(xl ? xl : 1);
      if (!e->name || !extra) {
         free(extra);
         goto fail;
      }
      if (fread(e->name, 1, nl, z->f) != nl || fread(extra, 1, xl, z->f) != xl) {
         free(extra);
         goto fail;
      }
      e->name[nl] = 0;
      if (memchr(e->name, 0, nl)) {
         free(extra);
         goto fail;
      }
      e->flags = le16(h + 8);
      e->method = le16(h + 10);
      e->crc = le32(h + 16);
      e->compressed = le32(h + 20);
      e->size = le32(h + 24);
      e->attrs = le32(h + 38);
      e->offset = le32(h + 42);
      if (le16(h + 34)) {
         free(extra);
         goto fail;
      }
      bool have64 = false;
      for (size_t k = 0; k + 4 <= xl;) {
         size_t len = le16(extra + k + 2);
         if (len > xl - k - 4) {
            free(extra);
            goto fail;
         }
         if (le16(extra + k) == 1) {
            size_t at = k + 4, limit = at + len;
            have64 = true;
#define ZIP64_FIELD(field) if(e->field==UINT32_MAX){if(limit-at<8){free(extra);goto fail;}e->field=le64(extra+at);at+=8;}
            ZIP64_FIELD(size) ZIP64_FIELD(compressed) ZIP64_FIELD(offset)
#undef ZIP64_FIELD
         }
         k += 4 + len;
      }
      free(extra);
      if (!have64 && (e->size == UINT32_MAX || e->compressed == UINT32_MAX || e->offset == UINT32_MAX))
         goto fail;
      if (e->offset > z->size || e->compressed > z->size - e->offset)
         goto fail;
      pos += 46 + nl + xl + cl;
   }
   return 0;
fail:
   zip_close(z);
   return apk_error("invalid or truncated ZIP: %s", path);
}
static struct zip_entry *zip_find(struct zip *z, const char *name)
{
   for (size_t i = 0; i < z->count; i++)
      if (!strcmp(z->entries[i].name, name))
         return z->entries + i;
   return NULL;
}
/* ZIP paths must stay under the installed tree on all three platforms. */
static bool entry_safe(const char *name)
{
   if (!*name || *name == '/' || strchr(name, '\\') || strchr(name, ':'))
      return false;
   const char *p = name;
   while (*p) {
      const char *e = strchr(p, '/');
      size_t n = e ? (size_t)(e - p) : strlen(p);
      if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
         return false;
      for (size_t i = 0; i < n; i++)
         if ((unsigned char)p[i] < 32)
            return false;
#ifdef _WIN32
      if (p[n - 1] == '.' || p[n - 1] == ' ')
         return false;
      char part[16];
      size_t b = 0;
      while (b < n && p[b] != '.' && b < sizeof(part) -1) {
         part[b] = (char)toupper((unsigned char)p[b]);
         b++;
      }
      part[b] = 0;
      if (!strcmp(part, "CON") || !strcmp(part, "PRN") || !strcmp(part, "AUX") || !strcmp(part, "NUL") ||
          (b == 4 && (!strncmp(part, "COM", 3) || !strncmp(part, "LPT", 3)) && part[3] >= '1' && part[3] <= '9'))
         return false;
#endif
      if (!e)
         break;
      p = e + 1;
   }
   return true;
}
/* Output is streamed: APK/OBB files can be larger than addressable memory. */
struct extraction_progress {
   uint64_t bytes, total, last_report;
   size_t files, count;
   const char *name;
};
/* Report within a large entry as well as between entries. Newlines also keep
 * redirected logs readable; throttle output to twice a second. */
static void extraction_report(struct extraction_progress *p, bool force)
{
   uint64_t now = luna_os_monotonic_ns();
   if (!force && now - p->last_report < UINT64_C(500000000))
      return;
   p->last_report = now;
   unsigned percent = p->total ? (unsigned)(100.0L * p->bytes / p->total) : 100;
   fprintf(stderr, "[apk] %3u%%  %.1f / %.1f MiB  (%zu/%zu files)  %s\n",
           percent, (double)p->bytes / 1048576, (double)p->total / 1048576,
           p->files, p->count, p->name ? p->name : "");
   fflush(stderr);
}
static int zip_read(struct zip *z, const struct zip_entry *e, FILE *out, unsigned char *memory, size_t capacity, struct extraction_progress *progress)
{
   if (e->flags & 1 || !(e->method == 0 || e->method == 8))
      return apk_error("unsupported ZIP entry: %s", e->name);
   unsigned char h[30];
   if (seek_file(z->f, e->offset) || fread(h, 1, 30, z->f) != 30 || le32(h) != 0x04034b50)
      return -1;
   uint64_t at = e->offset + 30 + le16(h + 26) + le16(h + 28);
   if (at > z->size || e->compressed > z->size - at || seek_file(z->f, at))
      return -1;
   if (memory && e->size > capacity)
      return -1;
   unsigned char in[65536], decoded[65536];
   uint64_t left = e->compressed, total = 0;
   uLong crc = crc32(0, NULL, 0);
   z_stream stream = {0};
   bool ended = e->method == 0;
   int status = 0;
   if (e->method == 8 && inflateInit2(&stream, -MAX_WBITS) != Z_OK)
      return -1;
   while (left) {
      size_t n = (size_t)(left > sizeof in ? sizeof in : left);
      if (fread(in, 1, n, z->f) != n) {
         status = -1;
         break;
      }
      left -= n;
      stream.next_in = in;
      stream.avail_in = (uInt)n;
      do {
         size_t got = n;
         const unsigned char *data = in;
         if (e->method == 8) {
            stream.next_out = decoded;
            stream.avail_out = sizeof decoded;
            int r = inflate(&stream, Z_NO_FLUSH);
            got = sizeof decoded - stream.avail_out;
            data = decoded;
            if (r == Z_STREAM_END) {
               ended = true;
               if (left || stream.avail_in) {
                  status = -1;
                  break;
               }
            } else if (r == Z_BUF_ERROR && !stream.avail_in && !got)
               break;
            else if (r != Z_OK) {
               status = -1;
               break;
            }
         }
         if (total > e->size || got > e->size - total) {
            status = -1;
            break;
         }
         crc = crc32(crc, data, (uInt)got);
         if (out && fwrite(data, 1, got, out) != got) {
            status = -1;
            break;
         }
         if (memory)
            memcpy(memory + (size_t)total, data, got);
         total += got;
         if (progress) {
            progress->bytes += got;
            extraction_report(progress, false);
         }
         if (e->method == 0 || ended)
            break;
      } while (stream.avail_in || stream.avail_out == 0);
      if (status || ended && e->method == 8)
         break;
   }
   if (e->method == 8) {
      /* Empty deflate streams still have compressed input. */inflateEnd(&stream);
   }
   if (!ended || total != e->size || crc != e->crc)
      status = -1;
   return status ? apk_error("corrupt ZIP entry: %s", e->name) : 0;
}
static unsigned char *zip_memory(struct zip *z, const char *name, size_t *size)
{
   struct zip_entry *e = zip_find(z, name);
   if (!e || e->size > APK_MAX_MANIFEST)
      return NULL;
   unsigned char *data = malloc((size_t)e->size + 1);
   if (!data)
      return NULL;
   if (zip_read(z, e, NULL, data, (size_t)e->size, NULL)) {
      free(data);
      return NULL;
   }
   data[e->size] = 0;
   *size = (size_t)e->size;
   return data;
}
static int zip_extract(struct zip *z, const char *root, bool overwrite)
{
   if (mkdirs(root))
      return -1;
   struct extraction_progress progress = {.count = z->count};
   for (size_t i = 0; i < z->count; i++) {
      if (UINT64_MAX - progress.total < z->entries[i].size)
         return apk_error("archive expanded size is too large");
      progress.total += z->entries[i].size;
   }
   fprintf(stderr, "[apk] %s: %s (%.1f MiB, %zu files)\n",
           overwrite ? "Extracting" : "Checking cached files / restoring missing files",
           root, (double)progress.total / 1048576, progress.count);
   extraction_report(&progress, true);
   for (size_t i = 0; i < z->count; i++) {
      struct zip_entry *e = z->entries + i;
      progress.name = e->name;
      char path[APK_PATH];
      if (!entry_safe(e->name) || ((e->attrs >> 16) & 0170000) == 0120000)
         return apk_error("invalid archive path: %s", e->name);
      if (path_join(path, sizeof path, root, e->name))
         return -1;
      if (e->name[strlen(e->name) -1] == '/') {
         if (mkdirs(path))
            return -1;
         progress.files++;
         extraction_report(&progress, false);
         continue;
      }
      if (!overwrite && exists(path)) {
         progress.bytes += e->size;
         progress.files++;
         extraction_report(&progress, false);
         continue;
      }
      char *slash = strrchr(path, '/');
      if (slash) {
         *slash = 0;
         if (mkdirs(path))
            return -1;
         *slash = '/';
      }
      FILE *f = open_file(path, "wb");
      if (!f)
         return apk_error("cannot write %s", path);
      int r = zip_read(z, e, f, NULL, 0, &progress);
      if (fclose(f))
         r = -1;
      if (r) {
         luna_file_unlinkat(LUNA_AT_FDCWD, path, 0);
         return -1;
      }
      progress.files++;
      extraction_report(&progress, false);
   }
   progress.name = "done";
   extraction_report(&progress, true);
   return 0;
}
struct manifest {
   char package[256], split[256], activity[512], application[512];
   char orientation[32], theme[32], min_sdk[32], target_sdk[32];
   char providers[APK_META], permissions[APK_META], query_actions[APK_META], query_packages[APK_META];
};
struct axml {
   char **strings;
   size_t count;
};
static void axml_free(struct axml *x)
{
   for (size_t i = 0; i < x->count; i++)
      free(x->strings[i]);
   free(x->strings);
   memset(x, 0, sizeof *x);
}
static const char *axml_string(const struct axml *x, uint32_t i)
{
   return i < x->count ? x->strings[i] : "";
}
static int pool_len(const unsigned char *p, size_t n, size_t *at, bool utf8, uint32_t *value)
{
   if (utf8) {
      if (*at >= n)
         return -1;
      *value = p[(*at)++];
      if (*value & 0x80) {
         if (*at >= n)
            return -1;
         *value = ((*value & 0x7f) << 8) | p[(*at)++];
      }
   } else {
      if (n - *at < 2)
         return -1;
      *value = le16(p + *at);
      *at += 2;
      if (*value & 0x8000) {
         if (n - *at < 2)
            return -1;
         *value = ((*value & 0x7fff) << 16) | le16(p + *at);
         *at += 2;
      }
   }
   return 0;
}
static int axml_pool(struct axml *x, const unsigned char *p, size_t n)
{
   if (n < 28)
      return -1;
   size_t header = le16(p + 2), count = le32(p + 8), base = le32(p + 20);
   bool utf8 = !!(le32(p + 16) & 256);
   if (count > 1000000 || header > n || count > (n - header) / 4 || base > n)
      return -1;
   axml_free(x);
   x->strings = calloc(count ? count : 1, sizeof(*x->strings));
   if (!x->strings)
      return -1;
   x->count = count;
   for (size_t i = 0; i < count; i++) {
      size_t off = le32(p + header + i * 4);
      if (off > n - base)
         return -1;
      size_t at = base + off;
      uint32_t len, unused;
      if (utf8 && pool_len(p, n, &at, true, &unused))
         return -1;
      if (pool_len(p, n, &at, utf8, &len))
         return -1;
      if (utf8) {
         if (len > n - at || n - at - len < 1 || p[at + len])
            return -1;
         x->strings[i] = malloc((size_t)len + 1);
         if (!x->strings[i])
            return -1;
         memcpy(x->strings[i], p + at, len);
         x->strings[i][len] = 0;
      } else {
         if (len > (n - at) / 2 || n - at - (size_t)len * 2 < 2 || le16(p + at + len * 2))
            return -1;
         uint16_t *u = malloc(((size_t)len + 1) * 2);
         char *s = malloc((size_t)len * 3 + 1);
         if (!u || !s) {
            free(u);
            free(s);
            return -1;
         }
         for (uint32_t k = 0; k < len; k++)
            u[k] = le16(p + at + k * 2);
         size_t bytes = jcs_utf16_to_wtf8(u, len, s);
         s[bytes] = 0;
         free(u);
         x->strings[i] = s;
      }
   }
   return 0;
}
static int append(char *out, size_t capacity, const char *value, const char *separator)
{
   size_t a = strlen(out), b = strlen(value), s = a ? strlen(separator) : 0;
   if (a + s + b >= capacity)
      return apk_error("manifest metadata too large");
   if (s)
      memcpy(out + a, separator, s);
   memcpy(out + a + s, value, b + 1);
   return 0;
}
static int copy_text(char *out, size_t cap, const char *value)
{
   if (strlen(value) >= cap)
      return apk_error("manifest value too long");
   strcpy(out, value);
   return 0;
}
static int class_name(char *out, size_t cap, const char *pkg, const char *name)
{
   if (!*name) {
      *out = 0;
      return 0;
   }
   int n = snprintf(out, cap, "%s%s%s", name[0] == '.' || !strchr(name, '.') ? pkg : "", name[0] != '.' && !strchr(name, '.') ? "." : "", name);
   return n < 0 || (size_t)n >= cap ? -1 : 0;
}
struct attr {
   const char *key, *text;
   uint8_t type;
   uint32_t value;
};
static const struct attr *attribute(const struct attr *a, size_t n, const char *key)
{
   for (size_t i = 0; i < n; i++)
      if (!strcmp(a[i].key, key))
         return a + i;
   return NULL;
}
static const char *attr_text(const struct attr *a, size_t n, const char *key)
{
   const struct attr *v = attribute(a, n, key);
   return v ? v->text : "";
}
static int attr_number(char *out, size_t cap, const struct attr *a, size_t n, const char *key)
{
   const struct attr *v = attribute(a, n, key);
   if (!v) {
      *out = 0;
      return 0;
   }
   if (v->type == 3)
      return copy_text(out, cap, v->text);
   return snprintf(out, cap, "%" PRIu32, v->value) < 0 ? -1 : 0;
}
static bool package_valid(const char *s)
{
   if (!*s || strlen(s) > 255 || s[0] == '.' || s[strlen(s) -1] == '.')
      return false;
   for (const char *p = s; *p; p++)
      if (!(isalnum((unsigned char) * p) || *p == '_' || *p == '.') || (*p == '.' && p[1] == '.'))
         return false;
   return true;
}
static int parse_manifest_events(const unsigned char *data, size_t size, struct manifest *m, struct axml x)
{
   memset(m, 0, sizeof *m);
   int result = -1, depth = 0, activity_depth = -1, provider_depth = -1, queries_depth = -1, intent_depth = -1;
   char activity[512] = "", target[512] = "", orient[32] = "", theme[32] = "", app_theme[32] = "", provider[APK_META] = "";
   bool main = false, launcher = false, activity_launch = false;
   for (size_t at = 0; at < size;) {
      if (size - at < 8)
         goto done;
      const unsigned char *p = data + at;
      uint16_t kind = le16(p), header = le16(p + 2);
      uint32_t len = le32(p + 4);
      if (len < 8 || header < 8||header>len || len > size - at)
         goto done;
      if (kind == 1) {
         if (axml_pool(&x, p, len))
            goto done;
      } else if (kind == 0x102) {
         if (len < 36 || header < 16)
            goto done;
         depth++;
         uint32_t name = le32(p + 20);
         if (name >= x.count)
            goto done;
         const char *tag = axml_string(&x, name);
         size_t start = 16 + le16(p + 24), stride = le16(p + 26), count = le16(p + 28);
         if (stride < 20 || count > 1024 || start > len || count > (len - start) / stride)
            goto done;
         struct attr a[1024];
         char numbers[1024][32];
         for (size_t i = 0; i < count; i++) {
            const unsigned char *v = p + start + i * stride;
            uint32_t key = le32(v + 4), raw = le32(v + 8), value = le32(v + 16);
            uint8_t type = v[15];
            if (key >= x.count || le16(v + 12) < 8)
               goto done;
            a[i] = (struct attr) {
               axml_string(&x, key), "", type, value
            };
            if (type == 3) {
               if (value >= x.count)
                  goto done;
               a[i].text = axml_string(&x, value);
            } else if (raw < x.count)
               a[i].text = axml_string(&x, raw);
            else {
               snprintf(numbers[i], sizeof numbers[i], "%" PRIu32, value);
               a[i].text = numbers[i];
            }
         }
         if (!strcmp(tag, "manifest")) {
            if (copy_text(m->package, sizeof m->package, attr_text(a, count, "package")) || copy_text(m->split, sizeof m->split, attr_text(a, count, "split")))
               goto done;
         } else if (!strcmp(tag, "application")) {
            if (class_name(m->application, sizeof m->application, m->package, attr_text(a, count, "name")) || attr_number(app_theme, sizeof app_theme, a, count, "theme"))
               goto done;
         } else if (!strcmp(tag, "activity") || !strcmp(tag, "activity-alias")) {
            if (copy_text(activity, sizeof activity, attr_text(a, count, "name")) || copy_text(target, sizeof target, attr_text(a, count, "targetActivity")) || attr_number(orient, sizeof orient, a, count, "screenOrientation") || attr_number(theme, sizeof theme, a, count, "theme"))
               goto done;
            activity_depth = depth;
            activity_launch = false;
         } else if (!strcmp(tag, "intent-filter") && activity_depth >= 0) {
            intent_depth = depth;
            main = launcher = false;
         } else if (!strcmp(tag, "action")) {
            const char *v = attr_text(a, count, "name");
            if (queries_depth >= 0 && append(m->query_actions, sizeof m->query_actions, v, ","))
               goto done;
            if (intent_depth >= 0 && !strcmp(v, "android.intent.action.MAIN"))
               main = true;
         } else if (!strcmp(tag, "category") && intent_depth >= 0 && !strcmp(attr_text(a, count, "name"), "android.intent.category.LAUNCHER"))
            launcher = true;
         else if (!strcmp(tag, "queries"))
            queries_depth = depth;
         else if (!strcmp(tag, "package") && queries_depth >= 0) {
            if (append(m->query_packages, sizeof m->query_packages, attr_text(a, count, "name"), ","))
               goto done;
         } else if (!strcmp(tag, "provider")) {
            char qualified[512];
            if (class_name(qualified, sizeof qualified, m->package, attr_text(a, count, "name")))
               goto done;
            int n = snprintf(provider, sizeof provider, "%s|%s|", qualified, attr_text(a, count, "authorities"));
            if (n < 0 || (size_t)n >= sizeof provider)
               goto done;
            provider_depth = depth;
         } else if (!strcmp(tag, "meta-data") && provider_depth >= 0) {
            const struct attr *v = attribute(a, count, "resource");
            if (!v)
               v = attribute(a, count, "value");
            if (v) {
               char pair[APK_META];
               int n = snprintf(pair, sizeof pair, "%s~%s", attr_text(a, count, "name"), v->text);
               if (n < 0 || (size_t)n >= sizeof pair)
                  goto done;
               size_t l = strlen(provider);
               if (l && provider[l - 1] != '|' && append(provider, sizeof provider, ",", ""))
                  goto done;
               if (append(provider, sizeof provider, pair, ""))
                  goto done;
            }
         } else if (!strcmp(tag, "uses-permission")) {
            if (append(m->permissions, sizeof m->permissions, attr_text(a, count, "name"), ","))
               goto done;
         } else if (!strcmp(tag, "uses-sdk")) {
            if (attr_number(m->min_sdk, sizeof m->min_sdk, a, count, "minSdkVersion") || attr_number(m->target_sdk, sizeof m->target_sdk, a, count, "targetSdkVersion"))
               goto done;
         }
      } else if (kind == 0x103) {
         if (len < 24 || depth <= 0)
            goto done;
         if (depth == intent_depth) {
            activity_launch |= main && launcher;
            intent_depth = -1;
         }
         if (depth == activity_depth) {
            if (activity_launch && !*m->activity) {
               if (class_name(m->activity, sizeof m->activity, m->package, *target ? target : activity) || copy_text(m->orientation, sizeof m->orientation, orient) || copy_text(m->theme, sizeof m->theme, *theme ? theme : app_theme))
                  goto done;
            }
            activity_depth = -1;
         }
         if (depth == provider_depth) {
            if (append(m->providers, sizeof m->providers, provider, ";"))
               goto done;
            provider_depth = -1;
         }
         if (depth == queries_depth)
            queries_depth = -1;
         depth--;
      }
      at += len;
   }
   if (!depth && package_valid(m->package))
      result = 0;
done:
   axml_free(&x);
   return result ? apk_error("invalid Android manifest") : 0;
}
/* App Bundle manifests use the XmlNode protobuf rather than APK AXML.
 * Translate element events into the same bounded parser used for APKs. */
struct pb { const unsigned char *p; size_t n; };
struct pb_field { unsigned number, wire; uint64_t value; struct pb bytes; };
static int pb_varint(struct pb *p, uint64_t *v)
{
   *v = 0;
   for (unsigned shift = 0; shift < 70; shift += 7) {
      if (!p->n) return -1;
      unsigned c = *p->p++; p->n--;
      if (shift == 63 && c > 1) return -1;
      *v |= (uint64_t)(c & 127) << shift;
      if (!(c & 128)) return 0;
   }
   return -1;
}
static int pb_next(struct pb *p, struct pb_field *f)
{
   uint64_t key, len;
   memset(f, 0, sizeof *f);
   if (pb_varint(p, &key) || !(key >> 3) || key >> 3 > 0x1fffffff) return -1;
   f->number = key >> 3; f->wire = key & 7;
   if (f->wire == 0) return pb_varint(p, &f->value);
   if (f->wire == 2) {
      if (pb_varint(p, &len) || len > p->n) return -1;
   } else if (f->wire == 1) len = 8;
   else if (f->wire == 5) len = 4;
   else return -1;
   if (len > p->n) return -1;
   f->bytes = (struct pb){p->p, (size_t)len};
   p->p += len; p->n -= len;
   return 0;
}
struct bundle_xml { struct axml strings; unsigned char *events; size_t size, cap; };
static int bundle_string(struct bundle_xml *b, struct pb p, uint32_t *index)
{
   if (p.n > APK_MAX_MANIFEST || memchr(p.p, 0, p.n) || b->strings.count >= 65535) return -1;
   char *s = malloc(p.n + 1);
   if (!s) return -1;
   memcpy(s, p.p, p.n); s[p.n] = 0;
   char **strings = realloc(b->strings.strings, (b->strings.count + 1) * sizeof(*strings));
   if (!strings) { free(s); return -1; }
   b->strings.strings = strings;
   *index = b->strings.count;
   strings[b->strings.count++] = s;
   return 0;
}
static void put32(unsigned char *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; i++) p[i] = v >> (8 * i);
}
static int bundle_event(struct bundle_xml *b, const unsigned char *p, size_t n)
{
   if (n > APK_MAX_MANIFEST - b->size) return -1;
   if (b->size + n > b->cap) {
      size_t cap = (b->size + n) * 2;
      unsigned char *events = realloc(b->events, cap);
      if (!events) return -1;
      b->events = events; b->cap = cap;
   }
   memcpy(b->events + b->size, p, n); b->size += n;
   return 0;
}
static int bundle_node(struct bundle_xml *b, struct pb node, unsigned depth)
{
   if (depth > 128) return -1;
   struct pb_field f;
   struct pb element = {0};
   while (node.n) {
      if (pb_next(&node, &f)) return -1;
      if (f.number == 1 && f.wire == 2) element = f.bytes;
   }
   if (!element.p) return 0; /* Text and source positions. */
   struct pb scan = element;
   uint32_t name = UINT32_MAX;
   unsigned char start[36 + 1024 * 20] = {0};
   size_t count = 0;
   while (scan.n) {
      if (pb_next(&scan, &f)) return -1;
      if (f.number == 3 && f.wire == 2) {
         if (bundle_string(b, f.bytes, &name)) return -1;
      } else if (f.number == 4 && f.wire == 2) {
         if (count >= 1024) return -1;
         struct pb attr = f.bytes, key = {0}, text = {(const unsigned char *)"", 0}, item = {0};
         while (attr.n) {
            struct pb_field a;
            if (pb_next(&attr, &a)) return -1;
            if (a.wire != 2) continue;
            if (a.number == 2) key = a.bytes;
            if (a.number == 3) text = a.bytes;
            if (a.number == 6) item = a.bytes;
         }
         if (!key.p) return -1;
         uint32_t ki, vi;
         if (bundle_string(b, key, &ki) || bundle_string(b, text, &vi)) return -1;
         unsigned char *a = start + 36 + count++ * 20;
         put32(a, UINT32_MAX); put32(a + 4, ki); put32(a + 8, vi);
         a[12] = 8; a[15] = 3; put32(a + 16, vi);
         /* Compiled references and enum/integer primitives override raw text. */
         while (item.n) {
            struct pb_field v;
            if (pb_next(&item, &v)) return -1;
            if (v.wire != 2 || (v.number != 1 && v.number != 7)) continue;
            struct pb value = v.bytes;
            while (value.n) {
               struct pb_field number;
               if (pb_next(&value, &number)) return -1;
               if (number.wire == 0 && ((v.number == 1 && number.number == 2) ||
                   (v.number == 7 && (number.number == 6 || number.number == 7 || number.number == 8)))) {
                  a[15] = v.number == 1 ? 1 : 0x10;
                  put32(a + 16, (uint32_t)number.value);
               }
            }
         }
      }
   }
   if (name == UINT32_MAX) return -1;
   start[0] = 2; start[1] = 1; start[2] = 16;
   put32(start + 4, 36 + count * 20); put32(start + 20, name);
   start[24] = start[26] = 20; start[28] = count; start[29] = count >> 8;
   if (bundle_event(b, start, 36 + count * 20)) return -1;
   scan = element;
   while (scan.n) {
      if (pb_next(&scan, &f)) return -1;
      if (f.number == 5 && f.wire == 2 && bundle_node(b, f.bytes, depth + 1)) return -1;
   }
   unsigned char end[24] = {3, 1, 16, 0, 24};
   put32(end + 20, name);
   return bundle_event(b, end, sizeof end);
}
static int parse_manifest(const unsigned char *data, size_t size, struct manifest *m)
{
   if (size < 8 || le32(data) != 0x00080003 || le32(data + 4) < 8 || le32(data + 4) > size)
      return apk_error("APK needs a binary AndroidManifest.xml");
   return parse_manifest_events(data + 8, le32(data + 4) - 8, m, (struct axml){0});
}
static int parse_bundle_manifest(const unsigned char *data, size_t size, struct manifest *m)
{
   struct bundle_xml b = {0};
   int r = bundle_node(&b, (struct pb){data, size}, 0);
   if (!r) r = parse_manifest_events(b.events, b.size, m, b.strings);
   else { axml_free(&b.strings); apk_error("invalid Bundle manifest"); }
   free(b.events);
   return r;
}
static int manifest_zip(struct zip *z, struct manifest *m)
{
   size_t n = 0;
   unsigned char *data = zip_memory(z, "AndroidManifest.xml", &n);
   bool bundle = !data && zip_find(z, "BundleConfig.pb");
   if (bundle) data = zip_memory(z, "base/manifest/AndroidManifest.xml", &n);
   if (!data)
      return apk_error("missing APK manifest");
   int r = bundle ? parse_bundle_manifest(data, n, m) : parse_manifest(data, n, m);
   free(data);
   return r;
}
static int env_set(const char *key, const char *value, bool overwrite)
{
   return luna_os_setenv(key, value, overwrite);
}
static int env_default(const char *key, const char *value)
{
   const char *old = getenv(key);
   return old && *old ? 0 : env_set(key, value, true);
}
static const char *env_value(const char *key, const char *fallback)
{
   const char *v = getenv(key);
   return v && *v ? v : fallback;
}
static bool env_true(const char *key)
{
   const char *s = getenv(key);
   return s && *s && strcmp(s, "0");
}
static char *apk_getcwd(char *out, size_t cap)
{
#ifdef _WIN32
   DWORD n = GetCurrentDirectoryW(0, NULL);
   wchar_t *wide = n ? malloc((size_t)n * sizeof *wide) : NULL;
   if (!wide)
      return NULL;
   DWORD got = GetCurrentDirectoryW(n, wide);
   int bytes = got && got < n ? WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL) : 0;
   free(wide);
   if (!bytes)
      return NULL;
   for (char *p = out; *p; p++)
      if (*p == '\\')
         *p = '/';
   return out;
#else
   return getcwd(out, cap);
#endif
}
static int absolute_path(const char *path, char *out, size_t cap)
{
   if (luna_file_realpath(path, out, cap))
      return 0;
#ifdef _WIN32
   int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
   wchar_t *input = n ? malloc((size_t)n * sizeof *input) : NULL;
   if (!input)
      return -1;
   if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, input, n)) {
      free(input);
      return -1;
   }
   DWORD size = GetFullPathNameW(input, 0, NULL, NULL);
   wchar_t *full = size ? malloc((size_t)size * sizeof *full) : NULL;
   DWORD count = full ? GetFullPathNameW(input, size, full, NULL) : 0;
   int bytes = count && count < size ? WideCharToMultiByte(CP_UTF8, 0, full, -1, out, (int)cap, NULL, NULL) : 0;
   free(input);
   free(full);
   if (!bytes)
      return -1;
   for (char *p = out; *p; p++)
      if (*p == '\\')
         *p = '/';
   return 0;
#else
   if (path[0] == '/')
      return copy_text(out, cap, path);
   char cwd[APK_PATH];
   if (!apk_getcwd(cwd, sizeof cwd))
      return -1;
   return path_join(out, cap, cwd, path);
#endif
}
static char *trim(char *s)
{
   while (isspace((unsigned char) * s))
      s++;
   size_t n = strlen(s);
   while (n && isspace((unsigned char)s[n - 1]))
      s[--n] = 0;
   return s;
}
static int config_load(const char *path, const char *package)
{
   FILE *f = open_file(path, "rb");
   if (!f) {
      if (errno == ENOENT)
         return 0;
      return apk_error("cannot read config %s", path);
   }
   char line[APK_META];
   bool match = !package, section = false;
   int result = 0;
   while (fgets(line, sizeof line, f)) {
      if (!strchr(line, '\n') && !feof(f)) {
         result = apk_error("config line too long");
         break;
      }
      char *p = trim(line);
      size_t n = strlen(p);
      if (!*p || *p == '#')
         continue;
      if (*p == '[' && n >= 2 && p[n - 1] == ']') {
         p[n - 1] = 0;
         section = true;
         match = package && !strcmp(trim(p + 1), package);
         continue;
      }
      if (!match || (!package && section))
         continue;
      char *eq = strchr(p, '=');
      if (!eq)
         continue;
      *eq = 0;
      char *key = trim(p), *value = trim(eq + 1);
      if (strncmp(key, "LUNARIA_", 8))
         continue;
      bool valid = true;
      for (const char *k = key; *k; k++)
         if (!(isalnum((unsigned char) * k) || *k == '_'))
            valid = false;
      if (!valid)
         continue;
      n = strlen(value);
      if (n >= 2 && ((value[0] == '"' && value[n - 1] == '"') || (value[0] == '\'' && value[n - 1] == '\''))) {
         value[n - 1] = 0;
         value++;
      }
      if (env_default(key, value)) {
         result = -1;
         break;
      }
   }
   if (ferror(f))
      result = -1;
   fclose(f);
   return result;
}
static int copy_file(const char *source, const char *target)
{
   if (!strcmp(source, target))
      return 0;
   FILE *in = open_file(source, "rb");
   if (!in)
      return -1;
   FILE *out = open_file(target, "wb");
   if (!out) {
      fclose(in);
      return -1;
   }
   char buf[65536];
   size_t n;
   int r = 0;
   luna_file_info info;
   struct extraction_progress progress = {.count = 1, .name = target};
   bool report = !luna_file_infoat(LUNA_AT_FDCWD, source, &info, 0) && info.size >= 32 * 1048576;
   if (report) {
      progress.total = info.size;
      fprintf(stderr, "[apk] Copying: %s -> %s\n", source, target);
      extraction_report(&progress, true);
   }
   while ((n = fread(buf, 1, sizeof buf, in))) {
      if (fwrite(buf, 1, n, out) != n) {
         r = -1;
         break;
      }
      if (report) {
         progress.bytes += n;
         extraction_report(&progress, false);
      }
   }
   if (ferror(in))
      r = -1;
   fclose(in);
   if (fclose(out))
      r = -1;
   if (r)
      luna_file_unlinkat(LUNA_AT_FDCWD, target, 0);
   else if (report) {
      progress.files = 1;
      extraction_report(&progress, true);
   }
   return r;
}
static int materialize(const char *source, const char *target)
{
   if (exists(target))
      return 0;
   if (!luna_file_link(source, target))
      return 0;
   return copy_file(source, target);
}
static int link_asset(const char *source, const char *target)
{
   if (exists(target)) {
      char actual[APK_PATH], expected[APK_PATH];
      if (luna_file_realpath(target, actual, sizeof actual) && luna_file_realpath(source, expected, sizeof expected) && !strcmp(actual, expected))
         return 0;
      return apk_error("profile resource already exists: %s", target);
   }
   return luna_file_symlink(source, target) ? apk_error("cannot share resource: %s", target) : 0;
}
static bool profile_valid(const char *s)
{
   size_t n = strlen(s);
   if (!n || n > 64 || !(isalnum((unsigned char)s[0]) || s[0] == '_'))
      return false;
   for (size_t i = 0; i < n; i++)
      if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.' || s[i] == '-'))
         return false;
   return true;
}
static int profiles_list(const char *root, const char *package)
{
   puts("default");
   char dir[APK_PATH], relative[512];
   snprintf(relative, sizeof relative, "profiles/%s", package);
   if (path_join(dir, sizeof dir, root, relative))
      return -1;
   luna_directory *d = luna_directory_open(dir);
   if (!d)
      return errno == ENOENT ? 0 : -1;
   luna_directory_entry *e;
   while ((e = luna_directory_read(d))) {
      char path[APK_PATH];
      if (profile_valid(e->d_name) && !path_join(path, sizeof path, dir, e->d_name) && directory(path))
         puts(e->d_name);
   }
   return luna_directory_close(d);
}
static int profile_storage(const char *root, const char *package, const char *profile)
{
   if (!profile_valid(profile) || !package_valid(package))
      return apk_error("invalid profile or package name");
   char relative[512], common[APK_PATH], private_dir[APK_PATH], path[APK_PATH];
   snprintf(relative, sizeof relative, "data/%s", package);
   if (path_join(common, sizeof common, root, relative))
      return -1;
   if (!strcmp(profile, "default")) {
      if (copy_text(private_dir, sizeof private_dir, common))
         return -1;
   } else {
      snprintf(relative, sizeof relative, "profiles/%s/%s", package, profile);
      if (path_join(private_dir, sizeof private_dir, root, relative))
         return -1;
   }
   static const struct {
      const char *key, *subdir;
   } dirs[] = {
      {"ANDROID_EXTERNAL_FILES_DIR", "external/files"}, {"ANDROID_FILES_DIR", "files"},
      {"ANDROID_CACHE_DIR", "cache"}, {"ANDROID_CODE_CACHE_DIR", "code_cache"},
      {"ANDROID_DATABASES_DIR", "databases"}, {"ANDROID_NO_BACKUP_DIR", "no_backup"},
      {"ANDROID_PREFS_DIR", "shared_prefs"},
   };
   for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
      if (path_join(path, sizeof path, private_dir, dirs[i].subdir) || mkdirs(path) || env_set(dirs[i].key, path, true))
         return -1;
   if (path_join(path, sizeof path, common, "obb") || mkdirs(path) || env_set("ANDROID_EXTERNAL_OBB_DIR", path, true))
      return -1;
   if (path_join(path, sizeof path, private_dir, "device-root") || env_default("LUNARIA_GUEST_ROOT", path) || mkdirs(getenv("LUNARIA_GUEST_ROOT")))
      return -1;
   if (strcmp(profile, "default")) {
      const char *keys[] = {"LUNARIA_SHARED_ASSETS", "LUNARIA_PROFILE_SEED_FILES"};
      for (unsigned kind = 0; kind < 2; kind++) {
         char *list = strdup(env_value(keys[kind], ""));
         if (!list)
            return -1;
         char *save = NULL;
         for (char *item = strtok_r(list, ":", &save); item; item = strtok_r(NULL, ":", &save)) {
            if (!profile_valid(item)) {
               free(list);
               return apk_error("invalid profile resource name");
            }
            char resource[APK_PATH], source[APK_PATH], target[APK_PATH];
            if (path_join(resource, sizeof resource, common, "external/files") || path_join(source, sizeof source, resource, item) ||
                path_join(target, sizeof target, getenv("ANDROID_EXTERNAL_FILES_DIR"), item)) {
               free(list);
               return -1;
            }
            if (!exists(source))
               continue;
            int r = kind ? (!exists(target) ? copy_file(source, target) : 0) : link_asset(source, target);
            if (r) {
               free(list);
               return -1;
            }
         }
         free(list);
      }
   }
   return env_set("LUNARIA_PROFILE", profile, true);
}
static bool suffix(const char *s, const char *ext)
{
   size_t n = strlen(s), k = strlen(ext);
   return n >= k && !strcasecmp(s + n - k, ext);
}
static int font_defaults(const char *exedir)
{
   static const struct {
      const char *key, *file;
   } faces[] = {
      {"LUNA_FONT_REGULAR", "Inter-Regular.ttf"}, {"LUNA_FONT_BOLD", "Inter-Bold.ttf"},
      {"LUNA_FONT_ICONS", "LunaSymbols-Solid.otf"}, {"LUNA_FONT_BRANDS", "LunaSymbols-Brands.otf"},
   };
   char fonts[APK_PATH], path[APK_PATH];
   if (path_join(fonts, sizeof fonts, exedir, "fonts"))
      return -1;
   for (size_t i = 0; i < sizeof faces / sizeof faces[0]; i++)
      if (!path_join(path, sizeof path, fonts, faces[i].file) && exists(path))
         env_default(faces[i].key, path);
   static const char *cjk[] = {"NotoSansCJKjp-Regular.otf", "NotoSansCJK-Regular.ttc", "NotoSansCJKjp-Regular.ttc"};
   for (size_t i = 0; i < sizeof cjk / sizeof cjk[0]; i++)
      if (!path_join(path, sizeof path, fonts, cjk[i]) && exists(path)) {
         env_default("LUNA_FONT_CJK", path);
         break;
      }
   if (!path_join(path, sizeof path, exedir, "syslib-arm64") && directory(path))
      env_default("LUNARIA_SYSLIB_DIR", path);
#ifdef __APPLE__
   if (!path_join(path, sizeof path, exedir, ".deps/lib/moltenvk_icd.json") && exists(path))
      env_default("VK_ICD_FILENAMES", path);
#endif
   return 0;
}
static int keymap_resolve(const char *exedir)
{
   const char *key = getenv("LUNARIA_KEYMAP");
   if (!key || !*key || !strcmp(key, "off"))
      return 0;
   char path[APK_PATH];
   if (!strcmp(key, "genshin") || !strcmp(key, "crossworlds")) {
      char relative[128];
      snprintf(relative, sizeof relative, "keymaps/%s.conf", key);
      if (path_join(path, sizeof path, exedir, relative))
         return -1;
   } else if (absolute_path(key, path, sizeof path))
      return -1;
   if (!exists(path))
      return apk_error("cannot read keymap: %s", path);
   return env_set("LUNARIA_KEYMAP", path, true);
}
static int managed_setup(const char *inst)
{
   char managed[APK_PATH];
   if (path_join(managed, sizeof managed, inst, "assets/bin/Data/Managed"))
      return -1;
   if (!directory(managed))
      return 0;
   static const char *versions[] = {"1.0", "2.0", "4.0", "net_4_x-linux"};
   for (unsigned i = 0; i < sizeof versions / sizeof versions[0]; i++) {
      char relative[128], targetdir[APK_PATH];
      snprintf(relative, sizeof relative, "mono/%s", versions[i]);
      if (path_join(targetdir, sizeof targetdir, managed, relative) || mkdirs(targetdir))
         return -1;
      luna_directory *d = luna_directory_open(managed);
      if (!d)
         return -1;
      luna_directory_entry *e;
      int r = 0;
      while ((e = luna_directory_read(d)))
         if (suffix(e->d_name, ".dll")) {
            char source[APK_PATH], target[APK_PATH];
            if (path_join(source, sizeof source, managed, e->d_name) || path_join(target, sizeof target, targetdir, e->d_name) || materialize(source, target)) {
               r = -1;
               break;
            }
         }
      luna_directory_close(d);
      if (r)
         return -1;
   }
   char cfg[APK_PATH], dir[APK_PATH];
   if (path_join(dir, sizeof dir, inst, "mono-etc/mono") || mkdirs(dir) || path_join(cfg, sizeof cfg, dir, "config"))
      return -1;
   if (!exists(cfg)) {
      FILE *f = open_file(cfg, "wb");
      if (!f)
         return -1;
      int r = fputs("<configuration></configuration>\n", f) < 0 ? -1 : 0;
      if (fclose(f))
         r = -1;
      if (r)
         return -1;
   }
   char mono[APK_PATH];
   if (path_join(mono, sizeof mono, managed, "mono/2.0"))
      return -1;
   char search[APK_PATH * 2];
   snprintf(search, sizeof search, "%s:%s", managed, mono);
   env_set("MONO_PATH", search, true);
   env_set("MONO_CONFIG", cfg, true);
   if (path_join(dir, sizeof dir, inst, "mono-etc"))
      return -1;
   env_set("MONO_CFG_DIR", dir, true);
   if (path_join(cfg, sizeof cfg, managed, "mono/config"))
      return -1;
   if (!exists(cfg)) {
      FILE *f = open_file(cfg, "wb");
      if (!f)
         return -1;
      fputs("<configuration/>\n", f);
      if (fclose(f))
         return -1;
   }
   return 0;
}
static int compare_names(const void *a, const void *b)
{
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static uint64_t hash_bytes(uint64_t h, const void *bytes, size_t n)
{
   const unsigned char *p = bytes;
   for (size_t i = 0; i < n; i++) {
      h ^= p[i];
      h *= UINT64_C(1099511628211);
   }
   return h;
}
static int obb_find(const char *dir, const char *pkg, const char *kind, char *out, size_t cap)
{
   luna_directory *d = luna_directory_open(dir);
   if (!d)
      return errno == ENOENT ? 0 : -1;
   char ending[300];
   snprintf(ending, sizeof ending, ".%s.obb", pkg);
   luna_directory_entry *e;
   int r = 0;
   while ((e = luna_directory_read(d)))
      if (!strncmp(e->d_name, kind, strlen(kind)) && suffix(e->d_name, ending)) {
         r = path_join(out, cap, dir, e->d_name);
         break;
      }
   luna_directory_close(d);
   return r;
}
static int obb_alias(const char *source, const char *dir, const char *name)
{
   char path[APK_PATH];
   if (path_join(path, sizeof path, dir, name) || mkdirs(dir))
      return -1;
   luna_file_info a, b;
   if (!luna_file_infoat(LUNA_AT_FDCWD, source, &a, 0) && !luna_file_infoat(LUNA_AT_FDCWD, path, &b, 0) && a.inode == b.inode && a.device == b.device)
      return 0;
   /* A cache may have been evicted; replace a dangling alias as well. */
   if (!luna_file_infoat(LUNA_AT_FDCWD, path, &b, LUNA_AT_NOFOLLOW) &&
       luna_file_unlinkat(LUNA_AT_FDCWD, path, 0))
      return -1;
   /* Expansion archives are immutable package resources. Prefer sharing the
    * original over copying gigabytes into every guest-visible OBB alias. */
   if (!luna_file_link(source, path) || !luna_file_symlink(source, path))
      return 0;
   return copy_file(source, path);
}
static int obb_setup(const char *input, const char *container, const char *inst, const char *package)
{
   char beside[APK_PATH];
   if (copy_text(beside, sizeof beside, input))
      return -1;
   char *slash = strrchr(beside, '/');
   if (slash)
      * slash = 0;
   else
      strcpy(beside, ".");
   const char *kinds[] = {"main.", "patch."}, *keys[] = {"ANDROID_OBB_MAIN", "ANDROID_OBB_PATCH"};
   for (unsigned i = 0; i < 2; i++) {
      char source[APK_PATH] = "", packed[APK_PATH];
      if (!*env_value(keys[i], "")) {
         if (obb_find(beside, package, kinds[i], source, sizeof source))
            return -1;
         if (!*source && container && *container) {
            if (obb_find(container, package, kinds[i], source, sizeof source))
               return -1;
            char relative[512], d[APK_PATH];
            snprintf(relative, sizeof relative, "Android/obb/%s", package);
            if (!*source && (!path_join(d, sizeof d, container, relative)) && obb_find(d, package, kinds[i], source, sizeof source))
               return -1;
         }
         if (!*source) {
            if (path_join(packed, sizeof packed, inst, i ? "assets/patch.obb.png" : "assets/main.obb.png"))
               return -1;
            if (exists(packed))
               strcpy(source, packed);
         }
         if (*source && env_set(keys[i], source, true))
            return -1;
      }
      const char *file = env_value(keys[i], "");
      if (!*file)
         continue;
      char name[512], external[APK_PATH], relative[512];
      snprintf(name, sizeof name, "%s1.%s.obb", kinds[i], package);
      if (obb_alias(file, getenv("ANDROID_EXTERNAL_OBB_DIR"), name))
         return -1;
      snprintf(relative, sizeof relative, "Android/obb/%s", package);
      if (path_join(external, sizeof external, getenv("ANDROID_EXTERNAL_FILES_DIR"), relative) || obb_alias(file, external, name))
         return -1;
      const char *base = strrchr(file, '/');
      base = base ? base + 1 : file;
      if (obb_alias(file, getenv("ANDROID_EXTERNAL_OBB_DIR"), base) || obb_alias(file, external, base))
         return -1;
   }
   return 0;
}
static void apk_help(void)
{
   puts("Usage: lunaria [--config FILE] [--data-root DIR] [--profile NAME] APP.apk|APP.aab|APP.xapk|APP.apks\n"
        "       lunaria [--list-profiles | --choose-profile | --prepare-profile | --install-only] APP.apk\n"
        "       lunaria MODULE.so\n\n"
        "APK archives are installed once in cache/packages; application data persists in data/.\n"
        "Environment settings override lunaria.conf; command-line options override both.");
}
/* 0: enter the loader, 1: command completed, -1: failure. */
int luna_apk_prepare(int *argc, const char ***argv)
{
   if (*argc >= 2 && (!strcmp((*argv)[1], "--apk-process-arm64") || !strcmp((*argv)[1], "--apk-process-arm32")))
      return 0;
   if (*argc < 2) {
      apk_help();
      return 1;
   }
   bool list = false, prepare = false, install = false, choose = false, profile_requested = false;
   int at = 1;
   for (; at < *argc; at++) {
      const char *arg = (*argv)[at];
      if (!strcmp(arg, "--")) {
         at++;
         break;
      }
      if (*arg != '-')
         break;
      if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
         apk_help();
         return 1;
      }
      if (!strcmp(arg, "--list-profiles")) {
         list = true;
         continue;
      }
      if (!strcmp(arg, "--prepare-profile")) {
         prepare = true;
         continue;
      }
      if (!strcmp(arg, "--install-only")) {
         install = true;
         continue;
      }
      if (!strcmp(arg, "--choose-profile")) {
         choose = true;
         profile_requested = false;
         continue;
      }
      const char *key = NULL, *value = NULL;
      static const struct {
         const char *option, *key;
      } options[] = {
         {"--config", "LUNARIA_CONF"}, {"--data-root", "LUNARIA_DATA_ROOT"}, {"--profile", "LUNARIA_PROFILE"},
      };
      for (size_t i = 0; i < sizeof options / sizeof options[0]; i++) {
         size_t n = strlen(options[i].option);
         if (!strcmp(arg, options[i].option)) {
            if (++at >= *argc)
               return apk_error("%s requires a value", arg);
            key = options[i].key;
            value = (*argv)[at];
            break;
         }
         if (!strncmp(arg, options[i].option, n) && arg[n] == '=') {
            key = options[i].key;
            value = arg + n + 1;
            break;
         }
      }
      if (!key || !value || !*value)
         return apk_error("unknown or incomplete option: %s", arg);
      if (env_set(key, value, true))
         return -1;
      if (!strcmp(key, "LUNARIA_PROFILE")) {
         choose = false;
         profile_requested = true;
      }
   }
   if (at >= *argc || *argc - at != 1)
      return apk_error("specify one APK or module");
   const char *argument = (*argv)[at];
   bool container = suffix(argument, ".xapk") || suffix(argument, ".apks");
   if (!container && !suffix(argument, ".apk") && !suffix(argument, ".aab")) {
      if (at != 1)
         return apk_error("APK options require an APK archive");
      return 0;
   }
   char input[APK_PATH], cwd[APK_PATH], exedir[APK_PATH], conf[APK_PATH], root[APK_PATH], cache_root[APK_PATH], cache[APK_PATH], pkgdir[APK_PATH], inst[APK_PATH], base[APK_PATH], ready[APK_PATH];
   if (!luna_file_realpath(argument, input, sizeof input) || !apk_getcwd(cwd, sizeof cwd) || luna_os_executable_path(exedir, sizeof exedir))
      return apk_error("cannot resolve APK or executable path");
   char *slash = strrchr(exedir, '/');
   if (!slash)
      return -1;
   *slash = 0;
#ifdef __APPLE__
   /* A .app uses its Resources directory as the distribution root. */
   char resources[APK_PATH];
   if (!path_join(resources, sizeof resources, exedir, "../Resources") && directory(resources) && luna_file_realpath(resources, exedir, sizeof exedir)) {}
#endif
   char default_conf[APK_PATH];
   if (path_join(default_conf, sizeof default_conf, cwd, "lunaria.conf") || absolute_path(env_value("LUNARIA_CONF", default_conf), conf, sizeof conf) || env_set("LUNARIA_CONF", conf, true) || config_load(conf, NULL))
      return -1;
   if (absolute_path(env_value("LUNARIA_DATA_ROOT", cwd), root, sizeof root) || mkdirs(root))
      return -1;
   const char *configured_cache = getenv("LUNARIA_CACHE_DIR");
   if (configured_cache && *configured_cache) {
      if (absolute_path(configured_cache, cache_root, sizeof cache_root))
         return -1;
   } else if (path_join(cache_root, sizeof cache_root, root, "cache/packages"))
      return -1;
   luna_file_info info;
   if (luna_file_infoat(LUNA_AT_FDCWD, input, &info, 0))
      return -1;
   uint64_t hash = hash_bytes(UINT64_C(14695981039346656037), input, strlen(input));
   hash = hash_bytes(hash, &info.size, sizeof info.size);
   hash = hash_bytes(hash, &info.modified, sizeof info.modified);
   char key[64];
   snprintf(key, sizeof key, "native-v1-%016" PRIx64, hash);
   if (path_join(cache, sizeof cache, cache_root, key) || mkdirs(cache) || path_join(pkgdir, sizeof pkgdir, cache, "pkg") || path_join(inst, sizeof inst, cache, "inst") || path_join(ready, sizeof ready, cache, ".ready"))
      return -1;
   bool hit = exists(ready) && !env_true("LUNARIA_NO_CACHE");
   fprintf(stderr, "[apk] Preparing %s%s\n", input, hit ? " (cached)" : "");
   fflush(stderr);
   struct zip outer = {0}, apk = {0};
   struct manifest *m = calloc(1, sizeof *m), *split = calloc(1, sizeof *split);
   char **apks = NULL;
   size_t apk_count = 0;
   int result = -1;
   if (!m || !split)
      goto done;
   if (container) {
      if (zip_open(&outer, input))
         goto done;
      /* Keep contained APKs and expansion archives intact. */
      if (zip_extract(&outer, pkgdir, !hit))
         goto done;
      apks = calloc(outer.count ? outer.count : 1, sizeof(*apks));
      if (!apks)
         goto done;
      for (size_t i = 0; i < outer.count; i++)
         if (suffix(outer.entries[i].name, ".apk")) {
            char path[APK_PATH];
            if (path_join(path, sizeof path, pkgdir, outer.entries[i].name))
               goto done;
            apks[apk_count] = strdup(path);
            if (!apks[apk_count])
               goto done;
            apk_count++;
         }
      if (!apk_count) {
         apk_error("container has no APKs");
         goto done;
      }
      qsort(apks, apk_count, sizeof(*apks), compare_names);
      bool found = false;
      for (size_t i = 0; i < apk_count; i++) {
         struct zip candidate = {0};
         if (zip_open(&candidate, apks[i]))
            goto done;
         int r = manifest_zip(&candidate, split);
         zip_close(&candidate);
         if (r)
            goto done;
         if (!*split->split) {
            if (copy_text(base, sizeof base, apks[i]))
               goto done;
            found = true;
            break;
         }
      }
      if (!found) {
         apk_error("container has no base APK");
         goto done;
      }
   } else if (copy_text(base, sizeof base, input))
      goto done;
   if (zip_open(&apk, base) || manifest_zip(&apk, m))
      goto done;
   if (zip_find(&apk, "BundleConfig.pb")) {
      for (size_t i = 0; i < apk.count; i++) {
         const char *name = apk.entries[i].name, *mapped = NULL;
         if (!strncmp(name, "base/", 5)) {
            mapped = name + 5;
            if (!strncmp(mapped, "dex/", 4)) mapped += 4;
            else if (!strncmp(mapped, "root/", 5)) mapped += 5;
         } else {
            const char *module = strchr(name, '/');
            if (module && !strncmp(module + 1, "assets/", 7)) mapped = module + 1;
         }
         if (mapped) {
            char *replacement = strdup(mapped);
            if (!replacement) goto done;
            free(apk.entries[i].name);
            apk.entries[i].name = replacement;
         }
      }
   }
   if (config_load(conf, m->package) || keymap_resolve(exedir))
      goto done;
   /* Package configuration can relocate data independently of the APK cache. */
   if (absolute_path(env_value("LUNARIA_DATA_ROOT", cwd), root, sizeof root) || mkdirs(root) || env_set("LUNARIA_DATA_ROOT", root, true))
      goto done;
   char olddata[APK_PATH], newdata[APK_PATH];
   if (path_join(olddata, sizeof olddata, root, "local/data") || path_join(newdata, sizeof newdata, root, "data"))
      goto done;
   if (directory(olddata) && !exists(newdata) && luna_file_rename(olddata, newdata))
      goto done;
   if (list) {
      result = profiles_list(root, m->package) ? -1 : 1;
      goto done;
   }
   char chosen[65];
   if (choose || (!profile_requested && env_true("LUNARIA_PROFILE_PROMPT"))) {
      if (profiles_list(root, m->package))
         goto done;
      fprintf(stderr, "Profile [default]: ");
      if (!fgets(chosen, sizeof chosen, stdin)) {
         apk_error("profile selection cancelled");
         goto done;
      }
      char *name = trim(chosen);
      if (env_set("LUNARIA_PROFILE", *name ? name : "default", true))
         goto done;
   }
   char profile[65];
   if (copy_text(profile, sizeof profile, env_value("LUNARIA_PROFILE", "default")) || profile_storage(root, m->package, profile))
      goto done;
   if (prepare) {
      char private_dir[APK_PATH];
      if (copy_text(private_dir, sizeof private_dir, getenv("ANDROID_FILES_DIR")))
         goto done;
      char *last = strrchr(private_dir, '/');
      if (last)
         * last = 0;
      printf("profile=%s\nprivate=%s\nexternal=%s\nobb=%s\n", profile, private_dir, getenv("ANDROID_EXTERNAL_FILES_DIR"), getenv("ANDROID_EXTERNAL_OBB_DIR"));
      result = 1;
      goto done;
   }
   if (!hit && exists(ready) && luna_file_unlinkat(LUNA_AT_FDCWD, ready, 0))
      goto done;
   if (zip_extract(&apk, inst, !hit))
      goto done;
   bool arm64 = false, arm32 = false;
   for (size_t i = 0; i < apk.count; i++) {
      const char *n = apk.entries[i].name;
      arm64 |= !strncmp(n, "lib/arm64-v8a/", 14);
      arm32 |= !strncmp(n, "lib/armeabi-v7a/", 16);
   }
   char split_list[APK_META] = "";
   for (size_t i = 0; i < apk_count; i++)
      if (strcmp(apks[i], base)) {
         struct zip z = {0};
         if (zip_open(&z, apks[i]))
            goto done;
         int r = manifest_zip(&z, split);
         if (!r && strcmp(split->package, m->package))
            r = apk_error("split package differs from base");
         if (!r)
            r = zip_extract(&z, inst, false);
         for (size_t k = 0; k < z.count; k++) {
            const char *n = z.entries[k].name;
            arm64 |= !strncmp(n, "lib/arm64-v8a/", 14);
            arm32 |= !strncmp(n, "lib/armeabi-v7a/", 16);
         }
         zip_close(&z);
         if (r)
            goto done;
         char record[APK_PATH + 512];
         int n = snprintf(record, sizeof record, "%s|%s", split->split, apks[i]);
         if (n < 0 || (size_t)n >= sizeof record || append(split_list, sizeof split_list, record, ";"))
            goto done;
      }
   const char *arch = env_value("LUNARIA_ARCH", arm64 ? "arm64-v8a" : arm32 ? "armeabi-v7a" : "arm64-v8a");
   if (strcmp(arch, "arm64-v8a") && strcmp(arch, "armeabi-v7a")) {
      apk_error("unsupported architecture: %s", arch);
      goto done;
   }
   char relative[128], libdir[APK_PATH], staged_apk[APK_PATH];
   snprintf(relative, sizeof relative, "lib/%s", arch);
   if (path_join(libdir, sizeof libdir, inst, relative) || mkdirs(libdir) || path_join(staged_apk, sizeof staged_apk, inst, "base.apk") || materialize(base, staged_apk))
      goto done;
   if (env_set("ANDROID_PACKAGE_NAME", m->package, true) || env_set("ANDROID_PACKAGE_CODE_PATH", inst, true) || env_set("ANDROID_APK_FILE", staged_apk, true) || env_set("ANDROID_NATIVE_LIB_DIR", libdir, true) || env_set("ANDROID_SPLIT_APKS", split_list, true))
      goto done;
   const struct {
      const char *key, *value;
   } metadata[] = {
      {"ANDROID_LAUNCH_ACTIVITY", m->activity}, {"ANDROID_APPLICATION_CLASS", m->application},
      {"ANDROID_SCREEN_ORIENTATION", m->orientation}, {"ANDROID_THEME_RESOURCE", m->theme},
      {"ANDROID_CONTENT_PROVIDERS", m->providers}, {"ANDROID_REQUESTED_PERMISSIONS", m->permissions},
      {"ANDROID_MIN_SDK", m->min_sdk}, {"ANDROID_TARGET_SDK", m->target_sdk},
      {"ANDROID_QUERY_ACTIONS", m->query_actions}, {"ANDROID_QUERY_PACKAGES", m->query_packages},
   };
   for (size_t i = 0; i < sizeof metadata / sizeof metadata[0]; i++) {
      if (*metadata[i].value) {
         if (env_set(metadata[i].key, metadata[i].value, true))
            goto done;
      } else
         luna_os_unsetenv(metadata[i].key);
   }
   luna_os_unsetenv("ANDROID_APK_SIGNER_PKCS7");
   for (size_t i = 0; i < apk.count; i++) {
      struct zip_entry *e = apk.entries + i;
      if (!strncasecmp(e->name, "META-INF/", 9) && (suffix(e->name, ".RSA") || suffix(e->name, ".DSA") || suffix(e->name, ".EC"))) {
         char signer[APK_PATH];
         if (path_join(signer, sizeof signer, inst, ".lunaria-apk-signer.p7b"))
            goto done;
         FILE *f = open_file(signer, "wb");
         if (!f)
            goto done;
         int r = zip_read(&apk, e, f, NULL, 0, NULL);
         if (fclose(f))
            r = -1;
         if (r || env_set("ANDROID_APK_SIGNER_PKCS7", signer, true))
            goto done;
         break;
      }
   }
   fprintf(stderr, "[apk] Preparing runtime files and OBB resources\n");
   fflush(stderr);
   if (managed_setup(inst) || obb_setup(input, container ? pkgdir : NULL, inst, m->package) || font_defaults(exedir))
      goto done;
   if (env_default("LUNARIA_DEX_START", "1") || env_default("LUNARIA_JIT_UI", "1") || env_default("LUNARIA_A64_SELF_SCHED", "1"))
      goto done;
   static char main_module[APK_PATH];
   static const char *newargs[3];
   main_module[0] = 0;
   static const char *engines[] = {"libunity.so", "libUE4.so", "libUnreal.so", "libmain.so"};
   for (size_t i = 0; i < sizeof engines / sizeof engines[0]; i++) {
      char path[APK_PATH];
      if (path_join(path, sizeof path, libdir, engines[i]))
         goto done;
      if (exists(path)) {
         strcpy(main_module, path);
         break;
      }
   }
   if (!*main_module)
      strcpy(main_module, !strcmp(arch, "arm64-v8a") ? "--apk-process-arm64" : "--apk-process-arm32");
   FILE *stamp = open_file(ready, "wb");
   if (!stamp)
      goto done;
   int stamp_error = fputs("native-v1\n", stamp) < 0;
   if (fclose(stamp))
      stamp_error = 1;
   if (stamp_error) {
      luna_file_unlinkat(LUNA_AT_FDCWD, ready, 0);
      goto done;
   }
   fprintf(stderr, "[apk] %s: %s, %s, profile=%s\n[apk] installed view: %s\n", m->package, arch, hit ? "cache hit" : "installed", profile, inst);
   if (install) {
      printf("package=%s\nactivity=%s\narch=%s\ninstalled=%s\nmodule=%s\n", m->package, m->activity, arch, inst, main_module);
      result = 1;
      goto done;
   }
   if (!*m->activity && env_true("LUNARIA_DEX_START")) {
      apk_error("manifest has no launcher Activity");
      goto done;
   }
   newargs[0] = (*argv)[0];
   newargs[1] = main_module;
   newargs[2] = NULL;
   *argv = newargs;
   *argc = 2;
   result = 0;
done:
   zip_close(&outer);
   zip_close(&apk);
   for (size_t i = 0; i < apk_count; i++)
      free(apks[i]);
   free(apks);
   free(m);
   free(split);
   return result;
}
