/* MPL-2.0. Android Bitmap, Matrix and drawable rendering.
 * Implementation section of dvm_runtime.c; shared helpers remain in that
 * translation unit. Keep registration tables in dvm_runtime.c. */

/* ------------------------------------------------------------------------ *
 * android.graphics.Bitmap / BitmapFactory / BitmapDrawable
 * ------------------------------------------------------------------------ *
 * A Bitmap is its pixels: width x height ARGB_8888 words, decoded at
 * BitmapFactory time (stb_image, the decoder the overlay renderer links in).
 * getPixel/setPixel/createBitmap work on those words.  To be seen, a bitmap is
 * written out once as PNG under the emulator's cache and the view that shows
 * it names that file — the overlay draws from files. */

unsigned char *stbi_load_from_memory(const unsigned char *buf, int len,
                                     int *x, int *y, int *comp, int req);
void stbi_image_free(void *p);
void stbi_set_flip_vertically_on_load_thread(int flag);
int stbi_write_png(const char *path, int w, int h, int comp, const void *data,
                   int stride);
unsigned char *stbi_write_png_to_mem(const unsigned char *pixels, int stride,
                                     int x, int y, int n, int *out_len);

struct rt_bitmap {
   int32_t w, h;
   uint32_t *px;            /* ARGB, row-major */
   int32_t density;         /* DisplayMetrics density of the pixels */
   int config_index;
   dvm_ref color_space;
   bool recycled, is_mutable, has_alpha;
   unsigned gen, png_gen;   /* png[] is current when png_gen == gen */
   char png[DVM_PATH_MAX];
   char overlay_key[64];
   unsigned overlay_gen;
};

static void bitmap_destroy(void *data)
{
   struct rt_bitmap *bitmap = data;
   if (!bitmap) return;
   luna_overlay_image_forget(bitmap->overlay_key);
   free(bitmap->px);
   free(bitmap);
}

static struct rt_bitmap *bitmap_of(struct dvm *vm, dvm_ref b)
{
   struct dvm_object *o = b ? dvm__obj(vm, b) : NULL;
   return o ? o->data : NULL;
}

/* The density of the screen the emulator presents: the same number
 * AConfiguration, DisplayMetrics and the property table report, so dp and px
 * mean one thing wherever an app asks. */
static int32_t device_density_dpi(void)
{
   const struct lunaria_screen *screen = lunaria_screen();
   return screen && screen->density > 0 ? screen->density : 160;
}

static dvm_ref bitmap_new(struct dvm *vm, int32_t w, int32_t h)
{
   if (w <= 0 || h <= 0 || (int64_t)w * h > (1 << 28)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "width and height must be > 0");
      return 0;
   }
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/graphics/Bitmap;");
   dvm_ref b = c ? dvm_new_object(vm, c) : 0;
   struct dvm_object *o = b ? dvm__obj(vm, b) : NULL;
   if (!o) return 0;
   struct rt_bitmap *bm = calloc(1, sizeof *bm);
   uint32_t *px = calloc((size_t)w * (size_t)h, sizeof *px);
   if (!bm || !px) {
      free(bm); free(px);
      dvm__throw(vm, "java/lang/OutOfMemoryError", "Bitmap %dx%d", w, h);
      return 0;
   }
   bm->w = w;
   bm->h = h;
   bm->px = px;
   bm->density = device_density_dpi();
   bm->has_alpha = true;
   bm->config_index = 3; /* ARGB_8888 */
   bm->gen = 1;
   o->data = bm;
   o->data_destroy = bitmap_destroy;
   return b;
}

static bool bitmap_live(struct dvm *vm, struct rt_bitmap *bm)
{
   if (bm && !bm->recycled) return true;
   dvm__throw(vm, "java/lang/IllegalStateException",
              "Can't call on a recycled bitmap");
   return false;
}

/* The bitmap as a PNG file the overlay can load; rewritten when the pixels
 * changed since last time. */
static const char *bitmap_png_path(struct dvm *vm, dvm_ref b)
{
   struct rt_bitmap *bm = bitmap_of(vm, b);
   if (!bm || bm->recycled || !bm->px) return NULL;
   if (bm->png[0] && bm->png_gen == bm->gen) return bm->png;
   static unsigned seq;
   char dir[DVM_PATH_MAX];
   if (luna_temp_path(dir, sizeof dir, "bitmaps")) return NULL;
   (void)luna_file_mkdir(dir, 0700);
   if (!bm->png[0])
      snprintf(bm->png, sizeof bm->png, "%s/b%u.png", dir, ++seq);
   uint8_t *rgba = malloc((size_t)bm->w * (size_t)bm->h * 4);
   if (!rgba) return NULL;
   for (size_t i = 0; i < (size_t)bm->w * (size_t)bm->h; ++i) {
      const uint32_t p = bm->px[i];
      rgba[i * 4 + 0] = (uint8_t)(p >> 16);
      rgba[i * 4 + 1] = (uint8_t)(p >> 8);
      rgba[i * 4 + 2] = (uint8_t)p;
      rgba[i * 4 + 3] = (uint8_t)(p >> 24);
   }
   char temporary[DVM_PATH_MAX+8];
   snprintf(temporary,sizeof temporary,"%s.tmp",bm->png);
   const int ok = stbi_write_png(temporary, bm->w, bm->h, 4, rgba, bm->w * 4);
   free(rgba);
   if (!ok || luna_file_rename(temporary,bm->png)) {
      (void)luna_file_unlink(temporary); return NULL;
   }
   bm->png_gen = bm->gen;
   return bm->png;
}

/* Bilinear resample into a new bitmap (createScaledBitmap, density scaling). */
static dvm_ref bitmap_scaled(struct dvm *vm, struct rt_bitmap *src,
                             int32_t w, int32_t h, bool filter)
{
   dvm_ref r = bitmap_new(vm, w, h);
   struct rt_bitmap *dst = bitmap_of(vm, r);
   if (!dst) return 0;
   for (int32_t y = 0; y < h; ++y)
      for (int32_t x = 0; x < w; ++x) {
         const float fx = ((float)x + 0.5f) * (float)src->w / (float)w - 0.5f;
         const float fy = ((float)y + 0.5f) * (float)src->h / (float)h - 0.5f;
         if (!filter) {
            int32_t sx = (int32_t)((float)x * (float)src->w / (float)w);
            int32_t sy = (int32_t)((float)y * (float)src->h / (float)h);
            dst->px[(size_t)y * (size_t)w + (size_t)x] =
               src->px[(size_t)sy * (size_t)src->w + (size_t)sx];
            continue;
         }
         int32_t x0 = (int32_t)floorf(fx), y0 = (int32_t)floorf(fy);
         const float ax = fx - (float)x0, ay = fy - (float)y0;
         int32_t x1 = x0 + 1, y1 = y0 + 1;
         if (x0 < 0) x0 = 0;
         if (y0 < 0) y0 = 0;
         if (x1 >= src->w) x1 = src->w - 1;
         if (y1 >= src->h) y1 = src->h - 1;
         if (x0 >= src->w) x0 = src->w - 1;
         if (y0 >= src->h) y0 = src->h - 1;
         uint32_t out = 0;
         for (int ch = 0; ch < 32; ch += 8) {
            const float p00 = (float)((src->px[(size_t)y0 * src->w + x0] >> ch) & 0xff);
            const float p10 = (float)((src->px[(size_t)y0 * src->w + x1] >> ch) & 0xff);
            const float p01 = (float)((src->px[(size_t)y1 * src->w + x0] >> ch) & 0xff);
            const float p11 = (float)((src->px[(size_t)y1 * src->w + x1] >> ch) & 0xff);
            const float v = (p00 * (1 - ax) + p10 * ax) * (1 - ay) +
                            (p01 * (1 - ax) + p11 * ax) * ay;
            out |= (uint32_t)(v + 0.5f) << ch;
         }
         dst->px[(size_t)y * (size_t)w + (size_t)x] = out;
      }
   dst->density = src->density;
   return r;
}

/* Dependency-free WebP support belongs to the BitmapFactory decoder. */
#define SIMPLEWEBP_DISABLE_STDIO
#define SIMPLEWEBP_IMPLEMENTATION
#include "simplewebp.h"

static bool bitmap_decode_webp(const uint8_t *data, size_t length, bool bounds_only,
                      int *width, int *height, int *components, uint8_t **rgba)
{
   *rgba = NULL;
   if (!data || !length) return false;
   simplewebp *decoder = NULL;
   if (simplewebp_load_from_memory((void *)data, length, NULL, &decoder) != SIMPLEWEBP_NO_ERROR)
      return false;
   size_t w = 0, h = 0;
   simplewebp_get_dimensions(decoder, &w, &h);
   if (!w || !h || w > (1u << 28) / h) {
      simplewebp_unload(decoder);
      return false;
   }
   /* The upstream API exposes dimensions but not alpha metadata. Read its
    * already-parsed flags instead of parsing the image header twice. */
   const bool alpha = simplewebp_is_lossless(decoder) ?
      decoder->decoder.vp8l.has_alpha : decoder->alph_input.userdata != NULL;
   if (!bounds_only) {
      *rgba = malloc(w * h * 4);
      if (!*rgba || simplewebp_decode(decoder, *rgba, NULL) != SIMPLEWEBP_NO_ERROR) {
         free(*rgba);
         *rgba = NULL;
         simplewebp_unload(decoder);
         return false;
      }
   }
   *width = (int)w;
   *height = (int)h;
   *components = alpha ? 4 : 3;
   simplewebp_unload(decoder);
   return true;
}

/* BitmapFactory.Options fields this decoder honours. */
static const struct rt_field rt_bitmap_options_fields[] = {
   { "inJustDecodeBounds", "Z" }, { "inSampleSize", "I" },
   { "inScaled", "Z" }, { "inDensity", "I" }, { "inTargetDensity", "I" },
   { "inMutable", "Z" }, { "inPreferredConfig", "Landroid/graphics/Bitmap$Config;" },
   { "outWidth", "I" }, { "outHeight", "I" },
   { "outMimeType", "Ljava/lang/String;" },
   F_END
};

static bool bitmap_options_init(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   rt_set_int(vm, self, "inScaled", 1);   /* the documented default */
   RETV();
}

static const char *image_mime(const uint8_t *d, size_t n)
{
   if (n >= 8 && !memcmp(d, "\x89PNG", 4)) return "image/png";
   if (n >= 3 && d[0] == 0xff && d[1] == 0xd8) return "image/jpeg";
   if (n >= 6 && !memcmp(d, "GIF8", 4)) return "image/gif";
   if (n >= 2 && d[0] == 'B' && d[1] == 'M') return "image/bmp";
   if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
      return "image/webp";
   return NULL;
}

/* Decodes encoded image bytes.  `src_density` is the density the bytes were
 * made for (0 when unknown / not a resource); with inScaled the result is
 * scaled to the target density, as decodeResource does.  Returns 0 with no
 * exception pending when the data cannot be decoded — BitmapFactory's
 * answer is null, not a throw. */
static dvm_ref bitmap_decode(struct dvm *vm, const uint8_t *data, size_t len,
                             dvm_ref opts, int32_t src_density)
{
   int w = 0, h = 0, comp = 0;
   const char *mime = image_mime(data, len);
   const bool webp = mime && !strcmp(mime, "image/webp");
   const bool bounds_only = opts && rt_get_int(vm, opts, "inJustDecodeBounds");
   stbi_set_flip_vertically_on_load_thread(0);
   uint8_t *rgba = NULL;
   bool decoded;
   if (webp) decoded = bitmap_decode_webp(data, len, bounds_only, &w, &h, &comp, &rgba);
   else {
      rgba = len <= INT_MAX ? stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4) : NULL;
      decoded = rgba != NULL;
   }
   if (!decoded) {
      static int told;
      if (told++ < 8)
         fprintf(stderr, "[bitmap] cannot decode %zu bytes (%s)\n", len,
                 mime ? mime : "unknown format");
      if (opts) {
         rt_set_int(vm, opts, "outWidth", -1);
         rt_set_int(vm, opts, "outHeight", -1);
      }
      return 0;
   }
   int sample = opts ? rt_get_int(vm, opts, "inSampleSize") : 1;
   if (sample < 1) sample = 1;
   /* inSampleSize is rounded down to a power of two. */
   while (sample & (sample - 1)) sample &= sample - 1;
   int32_t ow = (w + sample - 1) / sample, oh = (h + sample - 1) / sample;
   int32_t target = 0;
   if (opts) {
      const bool scaled = rt_get_int(vm, opts, "inScaled") != 0;
      int32_t in_d = rt_get_int(vm, opts, "inDensity");
      target = rt_get_int(vm, opts, "inTargetDensity");
      if (!in_d) in_d = src_density;
      if (scaled && in_d > 0 && target > 0 && in_d != target) {
         ow = (int32_t)((float)ow * (float)target / (float)in_d + 0.5f);
         oh = (int32_t)((float)oh * (float)target / (float)in_d + 0.5f);
      } else {
         target = in_d;
      }
      rt_set_int(vm, opts, "outWidth", ow);
      rt_set_int(vm, opts, "outHeight", oh);
      union dvm_value m = { .l = mime ? dvm_new_string(vm, mime) : 0 };
      (void)dvm_set_field(vm, opts, "outMimeType", "Ljava/lang/String;", m);
      if (rt_get_int(vm, opts, "inJustDecodeBounds")) {
         if (webp) free(rgba); else stbi_image_free(rgba);
         return 0;
      }
   } else if (src_density > 0 && src_density != device_density_dpi()) {
      target = device_density_dpi();
      ow = (int32_t)((float)ow * (float)target / (float)src_density + 0.5f);
      oh = (int32_t)((float)oh * (float)target / (float)src_density + 0.5f);
   }
   dvm_ref full = bitmap_new(vm, w, h);
   struct rt_bitmap *bm = bitmap_of(vm, full);
   if (!bm) { if (webp) free(rgba); else stbi_image_free(rgba); return 0; }
   for (size_t i = 0; i < (size_t)w * (size_t)h; ++i)
      bm->px[i] = (uint32_t)rgba[i * 4 + 3] << 24 |
                  (uint32_t)rgba[i * 4 + 0] << 16 |
                  (uint32_t)rgba[i * 4 + 1] << 8 | rgba[i * 4 + 2];
   bm->has_alpha = comp == 2 || comp == 4;
   if (webp) free(rgba); else stbi_image_free(rgba);
   bm->is_mutable = opts && rt_get_int(vm, opts, "inMutable");
   if (ow == w && oh == h) {
      if (target > 0) bm->density = target;
      return full;
   }
   dvm_pin(vm, full);
   dvm_ref r = bitmap_scaled(vm, bm, ow, oh, true);
   dvm_unpin(vm, full);
   struct rt_bitmap *rb = bitmap_of(vm, r);
   if (rb) {
      rb->has_alpha = bm->has_alpha;
      rb->is_mutable = bm->is_mutable;
      if (target > 0) rb->density = target;
   }
   return r;
}

/* Everything left in an InputStream, through its own read([BII)I. */
static uint8_t *stream_slurp(struct dvm *vm, dvm_ref stream, size_t *len)
{
   *len = 0;
   struct dvm_class *c = stream ? dvm_object_class(vm, stream) : NULL;
   struct dvm_method *rd = c ? dvm_find_method(vm, c, "read", "([BII)I") : NULL;
   if (!rd) return NULL;
   dvm_ref chunk = dvm_new_array(vm, 'B', "B", 16384);
   if (!chunk) return NULL;
   dvm_pin(vm, chunk);
   uint8_t *buf = NULL;
   size_t cap = 0;
   for (;;) {
      union dvm_value a[3] = { { .l = chunk }, { .i = 0 }, { .i = 16384 } }, r;
      if (!dvm_call(vm, rd, stream, a, 3, &r)) { free(buf); buf = NULL; break; }
      if (r.i < 0) break;
      if (!r.i) continue;
      if (*len + (size_t)r.i > cap) {
         cap = (cap ? cap * 2 : 65536);
         while (cap < *len + (size_t)r.i) cap *= 2;
         uint8_t *nb = realloc(buf, cap);
         if (!nb) { free(buf); buf = NULL; break; }
         buf = nb;
      }
      memcpy(buf + *len, dvm_array_data(vm, chunk), (size_t)r.i);
      *len += (size_t)r.i;
   }
   dvm_unpin(vm, chunk);
   return buf;
}

static bool bf_decodeStream(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self;
   if (!ARG(0).l) RETL(0);
   size_t len = 0;
   uint8_t *data = stream_slurp(vm, ARG(0).l, &len);
   if (!data) { if (dvm_exception(vm)) return false; RETL(0); }
   dvm_ref opts = nargs >= 3 ? ARG(2).l : 0;
   dvm_ref r = bitmap_decode(vm, data, len, opts, 0);
   free(data);
   if (dvm_exception(vm)) return false;
   RETL(r);
}

/* Drawable.createFromResourceStream uses the stream's encoded image, not the
 * source name as a file path.  AssetManager streams can come from any APK
 * split and have no corresponding host file.  Decode through the same Bitmap
 * path used by BitmapFactory, then keep that Bitmap in a BitmapDrawable so
 * ImageView and View backgrounds see the same pixels. */
static bool drawable_create_from_resource_stream(struct dvm *vm, dvm_ref self,
                                                  const union dvm_value *args,
                                                  int nargs,
                                                  union dvm_value *out)
{
   (void)self;
   if (nargs < 3 || !ARG(2).l) RETL(0);
   size_t len = 0;
   uint8_t *data = stream_slurp(vm, ARG(2).l, &len);
   if (!data) {
      if (getenv("LUNARIA_TRACE_DRAWABLE"))
         fprintf(stderr, "[drawable] stream read failed (exception=%d)\n",
                 dvm_exception(vm) != 0);
      if (dvm_exception(vm)) return false;
      RETL(0);
   }
   dvm_ref bitmap = bitmap_decode(vm, data, len, 0, 0);
   free(data);
   if (dvm_exception(vm)) return false;
   if (getenv("LUNARIA_TRACE_DRAWABLE")) {
      const char *name = nargs > 3 && ARG(3).l ?
         dvm_string_utf8(vm, ARG(3).l) : NULL;
      fprintf(stderr, "[drawable] stream %s: %zu bytes, bitmap=%u\n",
              name ? name : "(unnamed)", len, (unsigned)bitmap);
   }
   if (!bitmap) RETL(0);
   dvm_pin(vm, bitmap);
   struct dvm_class *klass =
      dvm__class_by_desc(vm, "Landroid/graphics/drawable/BitmapDrawable;");
   dvm_ref drawable = klass ? dvm_new_object(vm, klass) : 0;
   if (drawable) {
      union dvm_value value = { .l = bitmap };
      (void)dvm_set_field(vm, drawable, "bitmap", "Landroid/graphics/Bitmap;",
                          value);
   }
   dvm_unpin(vm, bitmap);
   RETL(drawable);
}

static bool bf_decodeByteArray(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)self;
   struct dvm_object *a = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   if (!a || a->kind != DVM_OBJ_ARRAY) {
      dvm__throw(vm, "java/lang/NullPointerException", "data");
      return false;
   }
   const int32_t off = ARG(1).i, len = ARG(2).i;
   if (off < 0 || len < 0 || (uint32_t)off + (uint32_t)len > a->length) {
      dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException",
                 "decodeByteArray");
      return false;
   }
   uint8_t *copy = malloc(len ? (size_t)len : 1);
   if (!copy) RETL(0);
   memcpy(copy, (const uint8_t *)a->data + off, (size_t)len);
   dvm_ref r = bitmap_decode(vm, copy, (size_t)len, nargs >= 4 ? ARG(3).l : 0, 0);
   free(copy);
   if (dvm_exception(vm)) return false;
   RETL(r);
}

static uint8_t *read_host_file(const char *path, size_t *len)
{
   *len = 0;
   FILE *f = luna_file_fopen(path, "rb");
   if (!f) return NULL;
   uint8_t *buf = NULL;
   size_t cap = 0;
   for (;;) {
      if (*len == cap) {
         cap = cap ? cap * 2 : 65536;
         uint8_t *nb = realloc(buf, cap);
         if (!nb) { free(buf); fclose(f); return NULL; }
         buf = nb;
      }
      size_t n = fread(buf + *len, 1, cap - *len, f);
      if (!n) break;
      *len += n;
   }
   fclose(f);
   return buf;
}

static bool bf_decodeFile(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)self;
   const char *p = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!p) RETL(0);
   char host[DVM_PATH_MAX];
   const char *hp = arm_exec_map_guest_path(p, host, sizeof host);
   size_t len = 0;
   uint8_t *data = hp ? read_host_file(hp, &len) : NULL;
   if (!data) RETL(0);
   dvm_ref r = bitmap_decode(vm, data, len, nargs >= 2 ? ARG(1).l : 0, 0);
   free(data);
   if (dvm_exception(vm)) return false;
   RETL(r);
}

/* The density bucket a resource file came from, by its directory. */
static int32_t resource_density(const char *path)
{
   static const struct { const char *q; int d; } b[] = {
      { "-xxxhdpi", 640 }, { "-xxhdpi", 480 }, { "-xhdpi", 320 },
      { "-hdpi", 240 }, { "-tvdpi", 213 }, { "-mdpi", 160 }, { "-ldpi", 120 },
   };
   if (strstr(path, "-nodpi") || strstr(path, "-anydpi")) return 0;
   for (size_t i = 0; i < sizeof b / sizeof b[0]; ++i)
      if (strstr(path, b[i].q)) return b[i].d;
   return 160;   /* the unqualified directory is mdpi */
}

static const char *ui_drawable_path(int32_t res_id);

static bool bf_decodeResource(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)self;
   const char *path = ui_drawable_path(ARG(1).i);
   size_t len = 0;
   uint8_t *data = path ? read_host_file(path, &len) : NULL;
   if (!data) RETL(0);
   dvm_ref r = bitmap_decode(vm, data, len, nargs >= 3 ? ARG(2).l : 0,
                             resource_density(path));
   free(data);
   if (dvm_exception(vm)) return false;
   RETL(r);
}

static const struct rt_method rt_bitmap_factory[] = {
   SM("decodeStream", "(Ljava/io/InputStream;)Landroid/graphics/Bitmap;",
      bf_decodeStream),
   SM("decodeStream",
      "(Ljava/io/InputStream;Landroid/graphics/Rect;Landroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;",
      bf_decodeStream),
   SM("decodeByteArray", "([BII)Landroid/graphics/Bitmap;", bf_decodeByteArray),
   SM("decodeByteArray",
      "([BIILandroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;",
      bf_decodeByteArray),
   SM("decodeFile", "(Ljava/lang/String;)Landroid/graphics/Bitmap;",
      bf_decodeFile),
   SM("decodeFile",
      "(Ljava/lang/String;Landroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;",
      bf_decodeFile),
   SM("decodeResource",
      "(Landroid/content/res/Resources;I)Landroid/graphics/Bitmap;",
      bf_decodeResource),
   SM("decodeResource",
      "(Landroid/content/res/Resources;ILandroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;",
      bf_decodeResource),
   M_END,
};

static const struct rt_method rt_bitmap_options[] = {
   M("<init>", "()V", bitmap_options_init),
   M_END,
};

/* --- Bitmap methods ------------------------------------------------------ */

static bool bm_getWidth(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   RETI(bm ? bm->w : 0);
}

static bool bm_getHeight(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   RETI(bm ? bm->h : 0);
}

static bool bm_getByteCount(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   if (!strcmp(builtin_name(), "getRowBytes")) RETI(bm ? bm->w * 4 : 0);
   RETI(bm && !bm->recycled ? bm->w * bm->h * 4 : 0);
}

static bool bm_getPixel(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)nargs;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   if (!bitmap_live(vm, bm)) return false;
   const int32_t x = ARG(0).i, y = ARG(1).i;
   if (x < 0 || y < 0 || x >= bm->w || y >= bm->h) {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "x/y must be inside the bitmap");
      return false;
   }
   RETI((int32_t)bm->px[(size_t)y * bm->w + x]);
}

static bool bm_setPixel(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)nargs; (void)out;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   if (!bitmap_live(vm, bm)) return false;
   if (!bm->is_mutable) {
      dvm__throw(vm, "java/lang/IllegalStateException", "Bitmap is immutable");
      return false;
   }
   const int32_t x = ARG(0).i, y = ARG(1).i;
   if (x < 0 || y < 0 || x >= bm->w || y >= bm->h) {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "x/y must be inside the bitmap");
      return false;
   }
   bm->px[(size_t)y * bm->w + x] = ARG(2).u;
   ++bm->gen;
   RETV();
}

/* getPixels / setPixels(int[] pixels, int offset, int stride, x, y, w, h). */
static bool bm_pixels(struct dvm *vm, dvm_ref self,
                      const union dvm_value *args, int nargs,
                      union dvm_value *out)
{
   (void)nargs; (void)out;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   if (!bitmap_live(vm, bm)) return false;
   const bool set = !strcmp(builtin_name(), "setPixels");
   if (set && !bm->is_mutable) {
      dvm__throw(vm, "java/lang/IllegalStateException", "Bitmap is immutable");
      return false;
   }
   struct dvm_object *a = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   const int32_t off = ARG(1).i, stride = ARG(2).i;
   const int32_t x = ARG(3).i, y = ARG(4).i, w = ARG(5).i, h = ARG(6).i;
   if (!a || x < 0 || y < 0 || w < 0 || h < 0 || x + w > bm->w ||
       y + h > bm->h || abs(stride) < w) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "bad region");
      return false;
   }
   int32_t *arr = a->data;
   for (int32_t r = 0; r < h; ++r) {
      const int64_t base = (int64_t)off + (int64_t)r * stride;
      if (base < 0 || base + w > (int64_t)a->length) {
         dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "pixels");
         return false;
      }
      for (int32_t c = 0; c < w; ++c) {
         uint32_t *p = &bm->px[(size_t)(y + r) * bm->w + (size_t)(x + c)];
         if (set) *p = (uint32_t)arr[base + c];
         else arr[base + c] = (int32_t)*p;
      }
   }
   if (set) ++bm->gen;
   RETV();
}

static bool bm_eraseColor(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)nargs; (void)out;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   if (!bitmap_live(vm, bm)) return false;
   if (!bm->is_mutable) {
      dvm__throw(vm, "java/lang/IllegalStateException", "Bitmap is immutable");
      return false;
   }
   for (size_t i = 0; i < (size_t)bm->w * bm->h; ++i) bm->px[i] = ARG(0).u;
   ++bm->gen;
   RETV();
}

static bool bm_recycle(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   if (bm && !bm->recycled) {
      bm->recycled = true;
      luna_overlay_image_forget(bm->overlay_key);bm->overlay_key[0]=0;
      free(bm->px);
      bm->px = NULL;
      if (bm->png[0]) luna_file_unlink(bm->png);
   }
   RETV();
}

static bool bm_flag(struct dvm *vm, dvm_ref self,
                    const union dvm_value *args, int nargs,
                    union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   const char *n = builtin_name();
   if (!strcmp(n, "isRecycled")) RETI(!bm || bm->recycled);
   if (!strcmp(n, "isMutable")) RETI(bm && bm->is_mutable);
   if (!strcmp(n, "hasAlpha")) RETI(bm && bm->has_alpha);
   if (!strcmp(n, "getDensity")) RETI(bm ? bm->density : 0);
   RETI(0);
}

static bool bm_setDensity(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)nargs; (void)out;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   if (bm) bm->density = ARG(0).i;
   RETV();
}

static bool bm_setHasAlpha(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)nargs; (void)out;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   if (bm) bm->has_alpha = ARG(0).i != 0;
   RETV();
}

/* Public configuration metadata is independent of the host raster storage. */
static bool bm_getConfig(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_bitmap *bitmap=bitmap_of(vm,self);
   if(!bitmap_live(vm,bitmap)) return false;
   const char *names[]={"ALPHA_8","RGB_565","ARGB_4444","ARGB_8888","RGBA_F16","HARDWARE"};
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/graphics/Bitmap$Config;");
   for (int i = 0; c && i < c->nsfields; ++i)
      if (!strcmp(c->sfields[i].name, names[bitmap->config_index])) RETL(c->sslots[i].l);
   RETL(0);
}

static bool bm_getColorSpace(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   struct rt_bitmap *bitmap=bitmap_of(vm,self);
   if(!bitmap_live(vm,bitmap)) return false;
   RETL(bitmap->config_index==0 ? 0 : bitmap->color_space ? bitmap->color_space : color_space_named(vm,0));
}

/* --- android.graphics.Matrix --------------------------------------------- */

/* A 3x3 matrix, row-major as getValues() reports it: MSCALE_X, MSKEW_X,
 * MTRANS_X, MSKEW_Y, MSCALE_Y, MTRANS_Y, MPERSP_0..2.  pre* multiplies on the
 * right (M = M x T), post* on the left (M = T x M), as Skia defines them. */
static const struct rt_field rt_matrix_fields[] = {
   { "values", "[F" }, F_END
};

static float *matrix_values(struct dvm *vm, dvm_ref m)
{
   union dvm_value v = { 0 };
   if (!m) return NULL;
   (void)dvm_get_field(vm, m, "values", "[F", &v);
   if (!v.l) {
      v.l = dvm_new_array(vm, 'F', "F", 9);
      if (!v.l) return NULL;
      float *f = dvm_array_data(vm, v.l);
      f[0] = f[4] = f[8] = 1.0f;
      (void)dvm_set_field(vm, m, "values", "[F", v);
   }
   return dvm_array_data(vm, v.l);
}

static void mat_identity(float out[9])
{static const float identity[9]={1,0,0,0,1,0,0,0,1};memcpy(out,identity,sizeof identity);}
static void mat_mul(float out[9], const float a[9], const float b[9])
{
   float r[9];
   for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
         r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] +
                        a[i * 3 + 2] * b[6 + j];
   memcpy(out, r, sizeof r);
}

static bool mat_invert(const float m[9], float out[9])
{
   const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                     m[1] * (m[3] * m[8] - m[5] * m[6]) +
                     m[2] * (m[3] * m[7] - m[4] * m[6]);
   if (fabsf(det) < 1e-12f) return false;
   const float id = 1.0f / det;
   out[0] = (m[4] * m[8] - m[5] * m[7]) * id;
   out[1] = (m[2] * m[7] - m[1] * m[8]) * id;
   out[2] = (m[1] * m[5] - m[2] * m[4]) * id;
   out[3] = (m[5] * m[6] - m[3] * m[8]) * id;
   out[4] = (m[0] * m[8] - m[2] * m[6]) * id;
   out[5] = (m[2] * m[3] - m[0] * m[5]) * id;
   out[6] = (m[3] * m[7] - m[4] * m[6]) * id;
   out[7] = (m[1] * m[6] - m[0] * m[7]) * id;
   out[8] = (m[0] * m[4] - m[1] * m[3]) * id;
   return true;
}

/* AOSP View coordinate transforms preserve the caller's matrix. Global
 * operations pre-concatenate; local operations post-concatenate inverses. */
static bool view_transform_coordinates(struct dvm *vm, dvm_ref self,
                                         const union dvm_value *args, int nargs,
                                         union dvm_value *out)
{
   dvm_ref target = nargs ? ARG(0).l : 0;
   if (!target) {
      dvm__throw(vm, "java/lang/NullPointerException", "matrix");
      return false;
   }
   const bool local = !strcmp(builtin_name(), "transformMatrixToLocal");
   union dvm_value parent = {0}, own = {0}, ignored = {0};
   (void)dvm_get_field(vm, self, "parent", "Landroid/view/ViewParent;", &parent);
   bool parent_view = parent.l && ui_is_a(vm, parent.l, "android/view/View");
   if (parent_view && !am_vcall(vm, parent.l,
         local ? "transformMatrixToLocal" : "transformMatrixToGlobal",
         "(Landroid/graphics/Matrix;)V", args, 1, &ignored)) return false;
   if (!view_get_matrix(vm, self, NULL, 0, &own)) return false;
   float *m = matrix_values(vm, target), *v = matrix_values(vm, own.l);
   if (!m || !v) return false;
   float shift[9];mat_identity(shift);
   if (parent_view) {
      float direction = local ? 1.f : -1.f;
      shift[2] = direction * rt_get_int(vm, parent.l, "scrollX");
      shift[5] = direction * rt_get_int(vm, parent.l, "scrollY");
      if (local) mat_mul(m, shift, m); else mat_mul(m, m, shift);
   }
   float direction = local ? -1.f : 1.f;
   shift[2] = direction * rt_get_int(vm, self, "left");
   shift[5] = direction * rt_get_int(vm, self, "top");
   if (local) {
      mat_mul(m, shift, m);
      float inverse[9];
      if (mat_invert(v, inverse)) mat_mul(m, inverse, m);
   } else {
      mat_mul(m, m, shift);
      mat_mul(m, m, v);
   }
   RETV();
}

static void mat_map(const float m[9], float x, float y, float *ox, float *oy)
{
   const float w = m[6] * x + m[7] * y + m[8];
   *ox = (m[0] * x + m[1] * y + m[2]) / (w ? w : 1.0f);
   *oy = (m[3] * x + m[4] * y + m[5]) / (w ? w : 1.0f);
}

/* The transform an op names, from its arguments: scale (sx, sy[, px, py]),
 * translate (dx, dy), rotate (deg[, px, py]), skew (kx, ky[, px, py]). */
static void mat_of_op(const char *op, const union dvm_value *args, int nargs,
                      float t[9])
{
   memset(t, 0, 9 * sizeof *t);
   t[0] = t[4] = t[8] = 1.0f;
   float px = 0, py = 0;
   if (strstr(op, "Scale")) {
      t[0] = ARG(0).f; t[4] = ARG(1).f;
      if (nargs >= 4) { px = ARG(2).f; py = ARG(3).f; }
   } else if (strstr(op, "Translate")) {
      t[2] = ARG(0).f; t[5] = ARG(1).f;
      return;
   } else if (strstr(op, "Rotate")) {
      const double r = (double)ARG(0).f * M_PI / 180.0;
      const float c = rotation_snap_zero((float)cos(r)), s = rotation_snap_zero((float)sin(r));
      t[0] = c; t[1] = -s; t[3] = s; t[4] = c;
      if (nargs >= 3) { px = ARG(1).f; py = ARG(2).f; }
   } else if (strstr(op, "Skew")) {
      t[1] = ARG(0).f; t[3] = ARG(1).f;
      if (nargs >= 4) { px = ARG(2).f; py = ARG(3).f; }
   }
   /* About a pivot: T(p) x op x T(-p). */
   if (px || py) {
      t[2] = px - t[0] * px - t[1] * py;
      t[5] = py - t[3] * px - t[4] * py;
   }
}

static bool matrix_op(struct dvm *vm, dvm_ref self,
                      const union dvm_value *args, int nargs,
                      union dvm_value *out)
{
   float *m = matrix_values(vm, self);
   if (!m) return false;
   const char *n = builtin_name();
   if (!strcmp(n, "setConcat") || !strcmp(n, "preConcat") ||
       !strcmp(n, "postConcat")) {
      float *a = ARG(0).l ? matrix_values(vm, ARG(0).l) : NULL;
      m = matrix_values(vm, self);
      if (!a) { dvm__throw(vm, "java/lang/NullPointerException", "matrix"); return false; }
      float r[9];
      if (!strcmp(n, "setConcat")) {
         float *b = ARG(1).l ? matrix_values(vm, ARG(1).l) : NULL;
         a = matrix_values(vm, ARG(0).l);
         m = matrix_values(vm, self);
         if (!b) { dvm__throw(vm, "java/lang/NullPointerException", "b"); return false; }
         mat_mul(r, a, b);
      } else if (n[1] == 'r') {
         mat_mul(r, m, a);
      } else {
         mat_mul(r, a, m);
      }
      memcpy(m, r, sizeof r);
      RETI(1);
   }
   float t[9];
   mat_of_op(n, args, nargs, t);
   if (!strncmp(n, "set", 3)) memcpy(m, t, sizeof t);
   else if (!strncmp(n, "pre", 3)) mat_mul(m, m, t);
   else mat_mul(m, t, m);
   if (!strncmp(n, "set", 3)) RETV();
   RETI(1);
}

static bool matrix_init(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)out;
   float *m = matrix_values(vm, self);
   float *src = nargs && ARG(0).l ? matrix_values(vm, ARG(0).l) : NULL;
   m = matrix_values(vm, self);
   if (m && src) memcpy(m, src, 9 * sizeof *m);
   RETV();
}

static bool matrix_reset(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   float *m = matrix_values(vm, self);
   if (m) { memset(m, 0, 9 * sizeof *m); m[0] = m[4] = m[8] = 1.0f; }
   RETV();
}

/* set(Matrix) / setValues(float[9]) / getValues(float[9]). */
static bool matrix_copy(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)nargs; (void)out;
   const char *n = builtin_name();
   float *m = matrix_values(vm, self);
   if (!strcmp(n, "set")) {
      if (!ARG(0).l) return matrix_reset(vm, self, args, 0, out);
      float *src = matrix_values(vm, ARG(0).l);
      m = matrix_values(vm, self);
      memcpy(m, src, 9 * sizeof *m);
      RETV();
   }
   struct dvm_object *a = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   if (!a || a->length < 9) {
      dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "values");
      return false;
   }
   if (!strcmp(n, "setValues")) memcpy(m, a->data, 9 * sizeof *m);
   else memcpy(a->data, m, 9 * sizeof *m);
   RETV();
}

static bool matrix_isIdentity(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   const float *m = matrix_values(vm, self);
   static const float id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
   RETI(m && !memcmp(m, id, sizeof id));
}

static bool matrix_invert(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)nargs;
   float *m = matrix_values(vm, self), inv[9];
   if (!m || !mat_invert(m, inv)) RETI(0);
   if (ARG(0).l) {
      float *d = matrix_values(vm, ARG(0).l);
      if (d) memcpy(d, inv, sizeof inv);
   }
   RETI(1);
}

/* mapPoints(float[] pts): in place, as x,y pairs. */
static bool matrix_mapPoints(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs; (void)out;
   const float *m = matrix_values(vm, self);
   struct dvm_object *a = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   if (!m || !a) RETV();
   float *p = a->data;
   for (uint32_t i = 0; i + 1 < a->length; i += 2)
      mat_map(m, p[i], p[i + 1], &p[i], &p[i + 1]);
   RETV();
}

static bool matrix_map_rect(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   dvm_ref dst = ARG(0).l, src = nargs > 1 ? ARG(1).l : dst;
   if (!dst || !src) {
      dvm__throw(vm, "java/lang/NullPointerException", "rect");
      return false;
   }
   float bounds[4];
   const char *edges[] = {"left", "top", "right", "bottom"};
   for (int i = 0; i < 4; ++i) bounds[i] = view_transform_value(vm, src, edges[i]);
   const float *m = matrix_values(vm, self);
   if (!m) return false;
   float xs[4], ys[4];
   for (int i = 0; i < 4; ++i)
      mat_map(m, bounds[(i & 1) ? 2 : 0], bounds[(i & 2) ? 3 : 1], &xs[i], &ys[i]);
   float result[] = {xs[0], ys[0], xs[0], ys[0]};
   for (int i = 1; i < 4; ++i) {
      result[0] = fminf(result[0], xs[i]); result[1] = fminf(result[1], ys[i]);
      result[2] = fmaxf(result[2], xs[i]); result[3] = fmaxf(result[3], ys[i]);
   }
   for (int i = 0; i < 4; ++i)
      (void)dvm_set_field(vm, dst, edges[i], "F", (union dvm_value){.f = result[i]});
   RETI(m[6] == 0.f && m[7] == 0.f && m[8] == 1.f &&
        ((m[1] == 0.f && m[3] == 0.f && m[0] != 0.f && m[4] != 0.f) ||
         (m[0] == 0.f && m[4] == 0.f && m[1] != 0.f && m[3] != 0.f)));
}

static const struct rt_method rt_matrix[] = {
   M("<init>", "()V", matrix_init),
   M("<init>", "(Landroid/graphics/Matrix;)V", matrix_init),
   M("reset", "()V", matrix_reset),
   M("set", "(Landroid/graphics/Matrix;)V", matrix_copy),
   M("setValues", "([F)V", matrix_copy),
   M("getValues", "([F)V", matrix_copy),
   M("isIdentity", "()Z", matrix_isIdentity),
   M("invert", "(Landroid/graphics/Matrix;)Z", matrix_invert),
   M("mapPoints", "([F)V", matrix_mapPoints),
   M("mapRect", "(Landroid/graphics/RectF;)Z", matrix_map_rect),
   M("mapRect", "(Landroid/graphics/RectF;Landroid/graphics/RectF;)Z", matrix_map_rect),
   M("setScale", "(FF)V", matrix_op), M("setScale", "(FFFF)V", matrix_op),
   M("preScale", "(FF)Z", matrix_op), M("preScale", "(FFFF)Z", matrix_op),
   M("postScale", "(FF)Z", matrix_op), M("postScale", "(FFFF)Z", matrix_op),
   M("setTranslate", "(FF)V", matrix_op),
   M("preTranslate", "(FF)Z", matrix_op),
   M("postTranslate", "(FF)Z", matrix_op),
   M("setRotate", "(F)V", matrix_op), M("setRotate", "(FFF)V", matrix_op),
   M("preRotate", "(F)Z", matrix_op), M("preRotate", "(FFF)Z", matrix_op),
   M("postRotate", "(F)Z", matrix_op), M("postRotate", "(FFF)Z", matrix_op),
   M("setSkew", "(FF)V", matrix_op), M("setSkew", "(FFFF)V", matrix_op),
   M("preSkew", "(FF)Z", matrix_op), M("postSkew", "(FF)Z", matrix_op),
   M("setConcat", "(Landroid/graphics/Matrix;Landroid/graphics/Matrix;)Z",
     matrix_op),
   M("preConcat", "(Landroid/graphics/Matrix;)Z", matrix_op),
   M("postConcat", "(Landroid/graphics/Matrix;)Z", matrix_op),
   M_END,
};

/* Software Canvas. Bitmap storage is straight ARGB; sampling and blending
 * operate on premultiplied colours so transparent texels cannot bleed RGB. */
struct canvas_clip {
   float inverse[9], rect[4];
   unsigned char *mask;int mask_width,mask_height;
   float bounds[4]; /* conservative device bounds for finite affine maps */
   int operation; /* Region.Op: difference, intersect, union, xor, reverse difference, replace */
   bool singular, bounded, simple;
};
struct canvas_save {
   float matrix[9];
   size_t clips;
};
struct render_command;
struct canvas_state {
   struct render_command *commands;
   size_t command_count, command_capacity;
   bool recording, recording_active;
   int32_t viewport_width, viewport_height;
   struct canvas_save *saved;
   struct canvas_clip *clip;
   size_t count, capacity, clips, clip_capacity;
   bool clip_cache_valid,clip_simple;
   float clip_bounds[4];
};

static void canvas_destroy(void *data)
{
   struct canvas_state *s = data;
   if (!s) return;
   for(size_t i=0;i<s->clips;++i) free(s->clip[i].mask);
   free(s->commands);
   free(s->saved); free(s->clip); free(s);
}

static struct canvas_state *canvas_state(struct dvm *vm, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) return NULL;
   if (!o->data) {
      o->data = calloc(1, sizeof(struct canvas_state));
      o->data_destroy = canvas_destroy;
      if (!o->data) dvm__throw(vm, "java/lang/OutOfMemoryError", "Canvas state");
   }
   return o->data;
}

static void canvas_read_matrix(struct dvm *vm, dvm_ref self, float matrix[9])
{
   const float identity[9] = {1,0,0,0,1,0,0,0,1};
   union dvm_value ref = {0};
   (void)dvm_get_field(vm, self, "matrix", "Landroid/graphics/Matrix;", &ref);
   const float *values = ref.l ? matrix_values(vm, ref.l) : NULL;
   memcpy(matrix, values ? values : identity, 9*sizeof *matrix);
}

static bool canvas_write_matrix(struct dvm *vm, dvm_ref self, const float matrix[9])
{
   union dvm_value ref = {0};
   (void)dvm_get_field(vm, self, "matrix", "Landroid/graphics/Matrix;", &ref);
   if (!ref.l) {
      ref.l = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/Matrix"));
      if (!ref.l) return false;
      (void)dvm_set_field(vm, self, "matrix", "Landroid/graphics/Matrix;", ref);
   }
   float *values = matrix_values(vm, ref.l);
   if (!values) return false;
   memcpy(values, matrix, 9*sizeof *matrix);
   return true;
}

static bool canvas_save_api(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   struct canvas_state *s = canvas_state(vm, self);
   if (!s) return false;
   const char *name = builtin_name();
   if (!strcmp(name, "getSaveCount")) RETI((int32_t)s->count+1);
   if (!strcmp(name, "save")) {
      if (s->count == s->capacity) {
         size_t capacity = s->capacity ? s->capacity*2 : 8;
         struct canvas_save *grown = realloc(s->saved, capacity*sizeof *grown);
         if (!grown) { dvm__throw(vm,"java/lang/OutOfMemoryError","Canvas save stack"); return false; }
         s->saved = grown; s->capacity = capacity;
      }
      canvas_read_matrix(vm, self, s->saved[s->count].matrix);
      s->saved[s->count].clips = s->clips;
      RETI((int32_t)++s->count);
   }
   int32_t target = !strcmp(name,"restoreToCount") ? ARG(0).i : (int32_t)s->count;
   if (target < 1) {
      dvm__throw(vm,"java/lang/IllegalStateException","Underflow in restore"); return false;
   }
   if ((size_t)target > s->count) RETV();
   struct canvas_save *saved = &s->saved[target-1];
   if (!canvas_write_matrix(vm,self,saved->matrix)) return false;
   for(size_t i=saved->clips;i<s->clips;++i) {free(s->clip[i].mask);s->clip[i].mask=NULL;}
   s->clips = saved->clips;
   s->clip_cache_valid=false;
   s->count = (size_t)target-1;
   RETV();
}

static bool canvas_transform(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs;
   float current[9], op[9] = {1,0,0,0,1,0,0,0,1};
   const char *name = builtin_name();
   if (!strcmp(name,"concat")) {
      if (!ARG(0).l) RETV();
      const float *matrix = matrix_values(vm,ARG(0).l);
      if (!matrix) return false;
      memcpy(op,matrix,sizeof op);
   } else if (!strcmp(name,"translate")) { op[2]=ARG(0).f; op[5]=ARG(1).f; }
   else if (!strcmp(name,"scale")) { op[0]=ARG(0).f; op[4]=ARG(1).f; }
   else if (!strcmp(name,"skew")) { op[1]=ARG(0).f; op[3]=ARG(1).f; }
   else {
      float radians=ARG(0).f*(float)(M_PI/180.0);
      op[0]=op[4]=cosf(radians); op[3]=sinf(radians); op[1]=-op[3];
   }
   canvas_read_matrix(vm,self,current);
   mat_mul(current,current,op);
   if (!canvas_write_matrix(vm,self,current)) return false;
   RETV();
}

static void canvas_prepare_clip(struct canvas_state *s)
{
   if(!s || s->clip_cache_valid) return;
   s->clip_simple=true;
   s->clip_bounds[0]=s->clip_bounds[1]=-INFINITY;
   s->clip_bounds[2]=s->clip_bounds[3]=INFINITY;
   for(size_t i=0;i<s->clips;++i) {
      const struct canvas_clip *clip=&s->clip[i];
      if(!clip->simple || clip->operation!=1 || clip->mask) {s->clip_simple=false;break;}
      s->clip_bounds[0]=fmaxf(s->clip_bounds[0],clip->bounds[0]);
      s->clip_bounds[1]=fmaxf(s->clip_bounds[1],clip->bounds[1]);
      s->clip_bounds[2]=fminf(s->clip_bounds[2],clip->bounds[2]);
      s->clip_bounds[3]=fminf(s->clip_bounds[3],clip->bounds[3]);
   }
   s->clip_cache_valid=true;
}

static bool canvas_clip_contains(struct canvas_state *s, float x, float y)
{
   canvas_prepare_clip(s);
   if(s && s->clip_simple) return x>=s->clip_bounds[0] && y>=s->clip_bounds[1] &&
      x<s->clip_bounds[2] && y<s->clip_bounds[3];
   bool result=true;
   for (size_t i=0; s && i<s->clips; ++i) {
      const struct canvas_clip *c=&s->clip[i];
      float lx=0,ly=0;
      if (!c->singular) mat_map(c->inverse,x,y,&lx,&ly);
      bool inside;
      if(c->mask) {
         int px=(int)floorf(x),py=(int)floorf(y);
         inside=px>=0 && py>=0 && px<c->mask_width && py<c->mask_height &&
            c->mask[4*((size_t)py*c->mask_width+px)+3]!=0;
      } else inside=!c->singular && isfinite(lx) && isfinite(ly) &&
         lx>=c->rect[0] && lx<c->rect[2] && ly>=c->rect[1] && ly<c->rect[3];
      switch(c->operation) {
         case 0: result=result && !inside;break;
         case 1: result=result && inside;break;
         case 2: result=result || inside;break;
         case 3: result=result!=inside;break;
         case 4: result=inside && !result;break;
         case 5: result=inside;break;
      }
   }
   return result;
}

/* Intersections cannot affect pixels outside their mapped bounds. Differences
 * do not shrink this conservative rectangle; their exact shape is still
 * tested by canvas_clip_contains. Saved clips carry their bounds with them. */
static void canvas_clip_limits(struct canvas_state *s,
                                 const struct rt_bitmap *bm, int limits[4])
{
   float bounds[4] = {0, 0, (float)bm->w, (float)bm->h};
   for (size_t i = 0; s && i < s->clips; ++i) {
      const struct canvas_clip *c = &s->clip[i];
      float region[4]={0,0,(float)bm->w,(float)bm->h};
      if(c->bounded) memcpy(region,c->bounds,sizeof region);
      if(c->operation==1) {
         bounds[0]=fmaxf(bounds[0],region[0]);bounds[1]=fmaxf(bounds[1],region[1]);
         bounds[2]=fminf(bounds[2],region[2]);bounds[3]=fminf(bounds[3],region[3]);
      } else if(c->operation==2 || c->operation==3) {
         bounds[0]=fminf(bounds[0],region[0]);bounds[1]=fminf(bounds[1],region[1]);
         bounds[2]=fmaxf(bounds[2],region[2]);bounds[3]=fmaxf(bounds[3],region[3]);
      } else if(c->operation>=4) memcpy(bounds,region,sizeof bounds);
      bounds[0]=fmaxf(0,bounds[0]);bounds[1]=fmaxf(0,bounds[1]);
      bounds[2]=fminf((float)bm->w,bounds[2]);bounds[3]=fminf((float)bm->h,bounds[3]);
   }
   if (!(bounds[0] < bounds[2] && bounds[1] < bounds[3])) {
      memset(limits, 0, 4 * sizeof *limits);
      return;
   }
   limits[0] = (int)floorf(bounds[0]); limits[1] = (int)floorf(bounds[1]);
   limits[2] = (int)ceilf(bounds[2]); limits[3] = (int)ceilf(bounds[3]);
}

static bool canvas_rect_args(struct dvm *vm, const union dvm_value *args,
                              int nargs, float rect[4])
{
   if (nargs>=4) {
      const struct dvm_method *method=dvm__builtin_method();
      bool ints=method && method->sig[1]=='I';
      for (int i=0;i<4;++i) rect[i]=ints ? (float)args[i].i : args[i].f;
   } else {
      if (!args[0].l) {dvm__throw(vm,"java/lang/NullPointerException","rect"); return false;}
      bool ints=ui_is_a(vm,args[0].l,"android/graphics/Rect");
      const char *fields[]={"left","top","right","bottom"};
      for (int i=0;i<4;++i) {
         union dvm_value v={0};
         (void)dvm_get_field(vm,args[0].l,fields[i],ints ? "I" : "F",&v);
         rect[i]=ints ? (float)v.i : v.f;
      }
   }
   return true;
}

static bool canvas_clip_operation(struct dvm *vm,const union dvm_value *args,int nargs,int *operation)
{
   *operation=!strncmp(builtin_name(),"clipOut",7) ? 0 : 1;
   if(!strstr(builtin_sig(),"Landroid/graphics/Region$Op;")) return true;
   dvm_ref op=args[nargs-1].l;
   union dvm_value value={0};
   if(op && !dvm_get_field(vm,op,"nativeInt","I",&value)) return false;
   if(lunaria_app_target_sdk()>=28 && (!op || (value.i!=0 && value.i!=1))) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","Only INTERSECT and DIFFERENCE are allowed");return false;
   }
   if(!op) {dvm__throw(vm,"java/lang/NullPointerException","op");return false;}
   if(value.i<0 || value.i>5) {dvm__throw(vm,"java/lang/IllegalArgumentException","Region.Op");return false;}
   *operation=value.i;return true;
}

static bool canvas_clip_rect(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   struct canvas_state *s=canvas_state(vm,self);
   if (!s) return false;
   struct canvas_clip clip={0}; float matrix[9];
   if(!canvas_clip_operation(vm,args,nargs,&clip.operation)) return false;
   if (!canvas_rect_args(vm,args,nargs,clip.rect)) return false;
   canvas_read_matrix(vm,self,matrix);
   clip.singular=!mat_invert(matrix,clip.inverse);
   if (clip.singular || !(clip.rect[0] < clip.rect[2] && clip.rect[1] < clip.rect[3])) {
      clip.bounded = true; /* empty intersection, inert difference */
      clip.simple=true;
   } else if (matrix[6] == 0 && matrix[7] == 0 && matrix[8] != 0) {
      clip.bounded = true;
      for (int i = 0; i < 4; ++i) {
         float x, y;
         mat_map(matrix, clip.rect[(i & 1) ? 2 : 0], clip.rect[(i & 2) ? 3 : 1], &x, &y);
         if (!isfinite(x) || !isfinite(y)) { clip.bounded = false; break; }
         if (!i) { clip.bounds[0] = clip.bounds[2] = x; clip.bounds[1] = clip.bounds[3] = y; }
         else {
            clip.bounds[0] = fminf(clip.bounds[0], x); clip.bounds[1] = fminf(clip.bounds[1], y);
            clip.bounds[2] = fmaxf(clip.bounds[2], x); clip.bounds[3] = fmaxf(clip.bounds[3], y);
         }
      }
      /* Positive axis-aligned maps preserve the rectangle's half-open edges.
       * Reflections, rotation, perspective, paths and differences keep the
       * exact per-clip test. Common nested widget clips reduce to four compares. */
      clip.simple=clip.bounded && matrix[1]==0 && matrix[3]==0 &&
         matrix[0]/matrix[8]>0 && matrix[4]/matrix[8]>0;
   }
   if (s->clips==s->clip_capacity) {
      size_t cap=s->clip_capacity ? s->clip_capacity*2 : 8;
      struct canvas_clip *grown=realloc(s->clip,cap*sizeof *grown);
      if (!grown) {dvm__throw(vm,"java/lang/OutOfMemoryError","Canvas clip"); return false;}
      s->clip=grown; s->clip_capacity=cap;
   }
   s->clip[s->clips++]=clip;
   s->clip_cache_valid=false;
   union dvm_value target={0};
   (void)dvm_get_field(vm,self,"bitmap","Landroid/graphics/Bitmap;",&target);
   struct rt_bitmap *bm=bitmap_of(vm,target.l);
   struct rt_bitmap viewport={.w=s->viewport_width,.h=s->viewport_height};
   if(!bm && s->recording) bm=&viewport;
   if (bm) {
      int limits[4]; canvas_clip_limits(s, bm, limits);
      for (int y=limits[1];y<limits[3];++y) for (int x=limits[0];x<limits[2];++x)
         if (canvas_clip_contains(s,x+0.5f,y+0.5f)) RETI(1);
   }
   RETI(0);
}

static uint32_t canvas_src_over(uint32_t src,uint32_t dst,bool opaque)
{
   unsigned sa=src>>24,da=opaque ? 255 : dst>>24;
   if (sa==255) return src;
   if (!sa) return opaque ? dst|0xff000000u : dst;
   unsigned alpha=sa*255+da*(255-sa);
   uint32_t result=((alpha+127)/255)<<24;
   for(int shift=0;shift<=16;shift+=8) {
      unsigned value=((src>>shift)&255)*sa*255+((dst>>shift)&255)*da*(255-sa);
      result|=((value+alpha/2)/alpha)<<shift;
   }
   return result;
}

/* Coverage interpolates the composited premultiplied result with the
 * destination. Scaling source alpha alone is incorrect for CLEAR/SRC/IN. */
static uint32_t canvas_blend(uint32_t src,uint32_t dst,int mode,unsigned coverage,bool opaque)
{
   if(!coverage) return opaque ? dst|0xff000000u : dst;
   if(mode==3) {
      src=(src&0xffffffu)|((((src>>24)*coverage+127)/255)<<24);
      return canvas_src_over(src,dst,opaque);
   }
   if(opaque) dst|=0xff000000u;
   uint32_t result=luna_blend_argb(src,dst,mode);
   unsigned ra=result>>24,da=dst>>24;
   unsigned alpha=ra*coverage+da*(255-coverage);
   if(!alpha) return opaque ? 0xff000000u : 0;
   uint32_t output=opaque ? 0xff000000u : ((alpha+127)/255)<<24;
   for(int shift=0;shift<=16;shift+=8) {
      unsigned value=((result>>shift)&255)*ra*coverage+((dst>>shift)&255)*da*(255-coverage);
      unsigned divisor=opaque ? 255*255 : alpha;
      output|=((value+divisor/2)/divisor)<<shift;
   }
   return output;
}
static bool canvas_porter_mode(struct dvm *vm,dvm_ref ref,int *mode)
{
   if(!ref) {dvm__throw(vm,"java/lang/NullPointerException","mode");return false;}
   union dvm_value value={0};
   if(!dvm_get_field(vm,ref,"nativeInt","I",&value)) return false;
   if(value.i<0 || value.i>17) {dvm__throw(vm,"java/lang/IllegalArgumentException","PorterDuff mode");return false;}
   *mode=value.i;return true;
}
static bool canvas_paint_mode(struct dvm *vm,dvm_ref paint,int *mode)
{
   *mode=3;if(!paint) return true;
   union dvm_value xfer={0};
   dvm__get_field_from(vm,paint,dvm_find_class(vm,"android/graphics/Paint"),"xfermode","Landroid/graphics/Xfermode;",&xfer);
   *mode=xfermode_value(vm,xfer.l);
   if(*mode<0 || *mode>28) {dvm__throw(vm,"java/lang/IllegalArgumentException","Blend mode");return false;}
   return true;
}

/* General path drawing uses a local coverage texture; inverse mapping preserves
 * nonuniform Canvas scaling of stroke widths, joins and caps. */
static uint32_t vector_gradient_sample(const struct vector_gradient *,float,float);
static bool shader_matrix(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;union dvm_value stored={0};
   if(builtin_name()[0]=='s') {
      if(ARG(0).l) {
         float *source=matrix_values(vm,ARG(0).l);if(!source) return false;
         stored.l=dvm_new_array(vm,'F',"F",9);if(!stored.l) return false;
         memcpy(dvm_array_data(vm,stored.l),source,9*sizeof(float));
      }
      dvm_set_field(vm,self,"localMatrix","[F",stored);RETV();
   }
   dvm_get_field(vm,self,"localMatrix","[F",&stored);
   float *target=matrix_values(vm,ARG(0).l);if(!target) return false;
   if(stored.l) memcpy(target,dvm_array_data(vm,stored.l),9*sizeof(float));
   else mat_identity(target);
   static const float identity[9]={1,0,0,0,1,0,0,0,1};
   RETI(memcmp(target,identity,sizeof identity)!=0);
}
static void shader_gradient_destroy(void *data) {vector_gradient_free(data);}
static bool linear_gradient_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;struct vector_gradient *g=calloc(1,sizeof *g);
   if(!g) {dvm__throw(vm,"java/lang/OutOfMemoryError","LinearGradient");return false;}
   g->x0=ARG(0).f;g->y0=ARG(1).f;g->x1=ARG(2).f;g->y1=ARG(3).f;
   bool array=strstr(dvm__builtin_method()->sig,"[I")!=NULL;
   uint32_t count=array ? dvm_array_length(vm,ARG(4).l) : 2;
   dvm_ref mode=ARG(6).l;
   if(!mode || count<2 || (array && ARG(5).l && dvm_array_length(vm,ARG(5).l)!=count)) {
      free(g);dvm__throw(vm,!mode ? "java/lang/NullPointerException" : "java/lang/IllegalArgumentException","gradient colors, positions or tile mode");return false;
   }
   g->tile=path_enum_ordinal(vm,mode);
   const uint32_t *colors=array ? dvm_array_data(vm,ARG(4).l) : NULL;
   const float *positions=array && ARG(5).l ? dvm_array_data(vm,ARG(5).l) : NULL;
   for(uint32_t i=0;i<count;++i) {
      uint32_t color=array ? colors[i] : args[4+i].u;
      float position=positions ? positions[i] : (float)i/(float)(count-1);
      if(!vector_gradient_stop(g,color,position)) {vector_gradient_free(g);dvm__throw(vm,"java/lang/IllegalArgumentException","gradient stop");return false;}
   }
   struct dvm_object *o=dvm__obj(vm,self);
   if(o->data_destroy) o->data_destroy(o->data);
   o->data=g;o->data_destroy=shader_gradient_destroy;RETV();
}
static bool bitmap_shader_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;if(!ARG(0).l || !ARG(1).l || !ARG(2).l) {dvm__throw(vm,"java/lang/IllegalArgumentException","bitmap and tile modes required");return false;}
   if(!bitmap_live(vm,bitmap_of(vm,ARG(0).l))) return false;
   dvm_set_field(vm,self,"bitmap","Landroid/graphics/Bitmap;",ARG(0));
   rt_set_int(vm,self,"tileX",path_enum_ordinal(vm,ARG(1).l));rt_set_int(vm,self,"tileY",path_enum_ordinal(vm,ARG(2).l));RETV();
}
static const struct rt_field rt_shader_fields[]={{"localMatrix","[F"},F_END};
static const struct rt_method rt_shader_methods[]={
   M("setLocalMatrix","(Landroid/graphics/Matrix;)V",shader_matrix),M("getLocalMatrix","(Landroid/graphics/Matrix;)Z",shader_matrix),M_END};
static const struct rt_method rt_linear_gradient[]={
   M("<init>","(FFFF[I[FLandroid/graphics/Shader$TileMode;)V",linear_gradient_init),
   M("<init>","(FFFFIILandroid/graphics/Shader$TileMode;)V",linear_gradient_init),M_END};
static const struct rt_field rt_bitmap_shader_fields[]={
   {"bitmap","Landroid/graphics/Bitmap;"},{"tileX","I"},{"tileY","I"},F_END};
static const struct rt_method rt_bitmap_shader[]={
   M("<init>","(Landroid/graphics/Bitmap;Landroid/graphics/Shader$TileMode;Landroid/graphics/Shader$TileMode;)V",bitmap_shader_init),M_END};
static const struct rt_field rt_shader_tile_fields[]={
   {"CLAMP","Landroid/graphics/Shader$TileMode;"},{"REPEAT","Landroid/graphics/Shader$TileMode;"},{"MIRROR","Landroid/graphics/Shader$TileMode;"},F_END};
static const struct rt_method rt_shader_tile[]={
   SM("values","()[Landroid/graphics/Shader$TileMode;",builtin_enum_values),
   SM("valueOf","(Ljava/lang/String;)Landroid/graphics/Shader$TileMode;",builtin_enum_values),M_END};
struct canvas_shader {
   struct vector_gradient *gradient;
   struct rt_bitmap *bitmap;
   float inverse[9];int tile_x,tile_y;unsigned alpha;
   struct color_filter_state filter;
};
static bool canvas_shader_prepare(struct dvm *vm,dvm_ref paint,struct canvas_shader *s)
{
   memset(s,0,sizeof *s);mat_identity(s->inverse);
   union dvm_value shader={0},matrix={0},color={0},filter={0};
   dvm_get_field(vm,paint,"shader","Landroid/graphics/Shader;",&shader);
   if(!shader.l) return true;
   struct dvm_object *o=dvm__obj(vm,shader.l);
   if(object_is_a(vm,shader.l,"Landroid/graphics/LinearGradient;")) s->gradient=o ? o->data : NULL;
   else if(object_is_a(vm,shader.l,"Landroid/graphics/BitmapShader;")) {
      union dvm_value bitmap={0};dvm_get_field(vm,shader.l,"bitmap","Landroid/graphics/Bitmap;",&bitmap);
      s->bitmap=bitmap_of(vm,bitmap.l);if(!bitmap_live(vm,s->bitmap)) return false;
      s->tile_x=ui_int(vm,shader.l,"tileX");s->tile_y=ui_int(vm,shader.l,"tileY");
   }
   if(!s->gradient && !s->bitmap) {dvm__throw(vm,"java/lang/UnsupportedOperationException","Shader implementation");return false;}
   dvm_get_field(vm,shader.l,"localMatrix","[F",&matrix);
   if(matrix.l && !mat_invert(dvm_array_data(vm,matrix.l),s->inverse)) memset(s->inverse,0,sizeof s->inverse);
   dvm_get_field(vm,paint,"color","I",&color);s->alpha=color.u>>24;
   dvm_get_field(vm,paint,"colorFilter","Landroid/graphics/ColorFilter;",&filter);
   return color_filter_prepare(vm,filter.l,&s->filter);
}
static int shader_tile(float value,int extent,int mode)
{
   if(mode==1) {value=fmodf(value,(float)extent);if(value<0) value+=extent;}
   else if(mode==2) {value=fmodf(value,(float)extent*2);if(value<0)value+=extent*2;if(value>=extent)value=extent*2-value;}
   return (int)fmaxf(0,fminf((float)extent-1,floorf(value)));
}
static uint32_t canvas_shader_sample(const struct canvas_shader *s,float x,float y,uint32_t solid)
{
   if(!s->gradient && !s->bitmap) return solid;
   float u,v;mat_map(s->inverse,x,y,&u,&v);if(!isfinite(u) || !isfinite(v)) return 0;
   uint32_t color=s->gradient ? vector_gradient_sample(s->gradient,u,v) :
      s->bitmap->px[(size_t)shader_tile(v,s->bitmap->h,s->tile_y)*s->bitmap->w+shader_tile(u,s->bitmap->w,s->tile_x)];
   color=(color&0xffffffu)|((((color>>24)*s->alpha+127)/255)<<24);
   return color_filter_pixel(&s->filter,color);
}

static bool canvas_path_color(struct dvm *vm,dvm_ref self,const luna_path *path,uint32_t color,
                                 int style,float stroke,int cap,int join,float miter,bool aa,
                                 dvm_ref paint,union dvm_value *out)
{
   int mode;if(!canvas_paint_mode(vm,paint,&mode)) return false;
   struct canvas_shader shader;if(!canvas_shader_prepare(vm,paint,&shader)) return false;
   union dvm_value target={0};(void)dvm_get_field(vm,self,"bitmap","Landroid/graphics/Bitmap;",&target);
   struct rt_bitmap *dst=bitmap_of(vm,target.l);if(!dst) RETV();
   if(!bitmap_live(vm,dst)) return false;
   struct canvas_state *state=canvas_state(vm,self);if(!state) return false;
   float matrix[9],inverse[9];canvas_read_matrix(vm,self,matrix);
   if(!mat_invert(matrix,inverse)) RETV();
   int limits[4];canvas_clip_limits(state,dst,limits);
   bool inverse_fill=(path->fill&2) && style!=1;
   bool hairline=style && stroke<=0,device_coverage=inverse_fill || hairline;
   float bounds[4],scale=1,margin=0;
   luna_path_bounds(path,bounds);
   if(!inverse_fill && !path->first) RETV();
   scale=fmaxf(hypotf(matrix[0],matrix[3]),hypotf(matrix[1],matrix[4]));
   if(!(scale>0) || !isfinite(scale)) RETV();
   if(hairline) stroke=1;
   if(style) margin=stroke*fmaxf(join==0 ? miter : 1,1)/2;
   int width,height;
   float local[9]={scale,0,0,0,scale,0,0,0,1};
   if(device_coverage) {width=dst->w;height=dst->h;memcpy(local,matrix,sizeof local);}
   else {
      bounds[0]=floorf((bounds[0]-margin)*scale)-1;
      bounds[1]=floorf((bounds[1]-margin)*scale)-1;
      bounds[2]=ceilf((bounds[2]+margin)*scale)+1;
      bounds[3]=ceilf((bounds[3]+margin)*scale)+1;
      if(!isfinite(bounds[0]) || !isfinite(bounds[1]) || !isfinite(bounds[2]) || !isfinite(bounds[3])) RETV();
      double w=(double)bounds[2]-bounds[0],h=(double)bounds[3]-bounds[1];
      if(w<=0 || h<=0) RETV();
      if(w>32768 || h>32768 || w*h>(1<<26)) {dvm__throw(vm,"java/lang/OutOfMemoryError","Path coverage");return false;}
      width=(int)w;height=(int)h;local[2]=-bounds[0];local[5]=-bounds[1];
      /* Bound the device scan for affine transforms. */
      if(matrix[6]==0 && matrix[7]==0 && matrix[8]!=0) {
         float box[4]={0};
         for(int i=0;i<4;++i) {
            float x,y;mat_map(matrix,bounds[(i&1) ? 2 : 0]/scale,bounds[(i&2) ? 3 : 1]/scale,&x,&y);
            if(!i) {box[0]=box[2]=x;box[1]=box[3]=y;}
            else {box[0]=fminf(box[0],x);box[1]=fminf(box[1],y);box[2]=fmaxf(box[2],x);box[3]=fmaxf(box[3],y);}
         }
         limits[0]=(int)fmaxf(limits[0],fminf(dst->w,floorf(box[0])));
         limits[1]=(int)fmaxf(limits[1],fminf(dst->h,floorf(box[1])));
         limits[2]=(int)fminf(limits[2],fmaxf(0,ceilf(box[2])));
         limits[3]=(int)fminf(limits[3],fmaxf(0,ceilf(box[3])));
      }
   }
   unsigned char *mask=luna_path_mask(path,width,height,local,style,
      hairline ? 1 : stroke*scale,cap,join,miter,aa);
   if(!mask) {dvm__throw(vm,"java/lang/OutOfMemoryError","Path mask");return false;}
   for(int y=limits[1];y<limits[3];++y) for(int x=limits[0];x<limits[2];++x) {
      if(!canvas_clip_contains(state,x+.5f,y+.5f)) continue;
      float u=x+.5f,v=y+.5f;
      if(!device_coverage) {float px,py;mat_map(inverse,u,v,&px,&py);u=px*scale-bounds[0];v=py*scale-bounds[1];}
      if(!isfinite(u) || !isfinite(v) || u<0 || v<0 || u>=width || v>=height) continue;
      unsigned coverage=mask[4*((size_t)(int)v*width+(int)u)+3];if(!coverage) continue;
      float lx,ly;mat_map(inverse,x+.5f,y+.5f,&lx,&ly);
      uint32_t sample=canvas_shader_sample(&shader,lx,ly,color);
      size_t index=(size_t)y*dst->w+x;dst->px[index]=canvas_blend(sample,dst->px[index],mode,coverage,!dst->has_alpha);
   }
   free(mask);++dst->gen;RETV();
}
static bool canvas_path_paint(struct dvm *vm,dvm_ref self,const luna_path *path,dvm_ref paint,union dvm_value *out)
{
   if(!paint) {dvm__throw(vm,"java/lang/NullPointerException","paint");return false;}
   union dvm_value color={0},stroke={0},flags={0},style={0},cap={0},join={0},miter={0};

   (void)dvm_get_field(vm,paint,"color","I",&color);(void)dvm_get_field(vm,paint,"strokeWidth","F",&stroke);
   (void)dvm_get_field(vm,paint,"flags","I",&flags);(void)dvm_get_field(vm,paint,"style","Landroid/graphics/Paint$Style;",&style);
   (void)dvm_get_field(vm,paint,"strokeCap","Landroid/graphics/Paint$Cap;",&cap);
   (void)dvm_get_field(vm,paint,"strokeJoin","Landroid/graphics/Paint$Join;",&join);
   (void)dvm_get_field(vm,paint,"strokeMiter","F",&miter);
   if(!paint_filter_color(vm,paint,&color.u)) return false;
   return canvas_path_color(vm,self,path,color.u,style.l ? path_enum_ordinal(vm,style.l) : 0,stroke.f,
      cap.l ? path_enum_ordinal(vm,cap.l) : 0,join.l ? path_enum_ordinal(vm,join.l) : 0,miter.f>0 ? miter.f : 4,(flags.i&1)!=0,paint,out);
}
static bool canvas_draw_path(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;luna_path *path=path_of(vm,args[0].l);if(!path) return false;
   return canvas_path_paint(vm,self,path,args[1].l,out);
}
static bool canvas_round_rect(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   float rect[4];int index;
   if(nargs==4) {if(!canvas_rect_args(vm,args,1,rect)) return false;index=1;}
   else {for(int i=0;i<4;++i) rect[i]=args[i].f;index=4;}
   if(!args[nargs-1].l) {dvm__throw(vm,"java/lang/NullPointerException","paint");return false;}
   float radii[8];for(int i=0;i<8;++i) radii[i]=args[index+(i&1)].f;
   luna_path path={0};
   bool good=luna_path_round_rect(&path,rect,radii,false);
   if(!good) dvm__throw(vm,"java/lang/OutOfMemoryError","RoundRect geometry");
   else good=canvas_path_paint(vm,self,&path,args[nargs-1].l,out);
   luna_path_clear(&path,true);return good;
}
static bool canvas_clip_path(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   int operation;if(!canvas_clip_operation(vm,args,nargs,&operation)) return false;
   luna_path *path=path_of(vm,ARG(0).l);if(!path) return false;
   struct canvas_state *state=canvas_state(vm,self);if(!state) return false;
   union dvm_value target={0};(void)dvm_get_field(vm,self,"bitmap","Landroid/graphics/Bitmap;",&target);
   struct rt_bitmap *bitmap=bitmap_of(vm,target.l);
   struct rt_bitmap viewport={.w=state->viewport_width,.h=state->viewport_height};
   if(!bitmap && state->recording) bitmap=&viewport;
   if(!bitmap || bitmap->w<=0 || bitmap->h<=0) RETI(0);
   float matrix[9];canvas_read_matrix(vm,self,matrix);
   struct canvas_clip clip={.mask_width=bitmap->w,.mask_height=bitmap->h,
      .operation=operation};
   clip.mask=luna_path_mask(path,bitmap->w,bitmap->h,matrix,0,0,0,0,4,false);
   if(!clip.mask) {dvm__throw(vm,"java/lang/OutOfMemoryError","Path clip");return false;}
   if(state->clips==state->clip_capacity) {
      size_t capacity=state->clip_capacity ? state->clip_capacity*2 : 8;
      struct canvas_clip *grown=realloc(state->clip,capacity*sizeof *grown);
      if(!grown) {free(clip.mask);dvm__throw(vm,"java/lang/OutOfMemoryError","Path clip");return false;}
      state->clip=grown;state->clip_capacity=capacity;
   }
   /* Bounds describe this path's device mask independently of the previous
    * clip: legacy UNION/REPLACE may expose pixels outside the previous clip. */
   int minx=bitmap->w,miny=bitmap->h,maxx=0,maxy=0;
   for(int y=0;y<bitmap->h;++y) for(int x=0;x<bitmap->w;++x) {
      if(!clip.mask[4*((size_t)y*bitmap->w+x)+3]) continue;
      minx=x<minx ? x : minx;miny=y<miny ? y : miny;maxx=x+1>maxx ? x+1 : maxx;maxy=y+1>maxy ? y+1 : maxy;
   }
   clip.bounded=true;clip.bounds[0]=minx;clip.bounds[1]=miny;clip.bounds[2]=maxx;clip.bounds[3]=maxy;
   state->clip[state->clips++]=clip;state->clip_cache_valid=false;
   int limits[4];canvas_clip_limits(state,bitmap,limits);bool any=false;
   for(int y=limits[1];!any && y<limits[3];++y) for(int x=limits[0];x<limits[2];++x)
      if(canvas_clip_contains(state,x+.5f,y+.5f)) {any=true;break;}
   RETI(any);
}

static bool canvas_fill_color(struct dvm *vm,dvm_ref self,const float *rect,
                               uint32_t color,int style,float stroke,bool aa,int mode,
                               union dvm_value *out)
{
   float inverse[9],matrix[9],half_x=stroke*0.5f,half_y=stroke*0.5f;
   if (rect) {
      if (!(rect[0]<rect[2] && rect[1]<rect[3])) RETV();
      canvas_read_matrix(vm,self,matrix);
      if (!mat_invert(matrix,inverse)) RETV();
      if (stroke<=0) {
         half_x=0.5f*hypotf(inverse[0],inverse[1]);
         half_y=0.5f*hypotf(inverse[3],inverse[4]);
      }
   }
   union dvm_value target={0};
   (void)dvm_get_field(vm,self,"bitmap","Landroid/graphics/Bitmap;",&target);
   struct rt_bitmap *dst=bitmap_of(vm,target.l);
   if (!dst) RETV();
   if (!bitmap_live(vm,dst)) return false;
   struct canvas_state *state=canvas_state(vm,self);
   if (!state) return false;
   int limits[4]; canvas_clip_limits(state, dst, limits);
   int x0=limits[0],y0=limits[1],x1=limits[2],y1=limits[3];
   bool full_pixels=!rect;
   if (rect && matrix[6]==0 && matrix[7]==0 && matrix[8]!=0) {
      float l=rect[0]-(style ? half_x : 0),r=rect[2]+(style ? half_x : 0);
      float t=rect[1]-(style ? half_y : 0),b=rect[3]+(style ? half_y : 0);
      float px[4],py[4];
      mat_map(matrix,l,t,&px[0],&py[0]); mat_map(matrix,r,t,&px[1],&py[1]);
      mat_map(matrix,l,b,&px[2],&py[2]); mat_map(matrix,r,b,&px[3],&py[3]);
      float minx=px[0],maxx=px[0],miny=py[0],maxy=py[0];
      bool finite=true;
      for(int i=0;i<4;++i) {
         finite=finite && isfinite(px[i]) && isfinite(py[i]);
         minx=fminf(minx,px[i]); maxx=fmaxf(maxx,px[i]);
         miny=fminf(miny,py[i]); maxy=fmaxf(maxy,py[i]);
      }
      if (finite) {
         full_pixels=style==0 && matrix[1]==0 && matrix[3]==0 &&
            floorf(minx)==minx && floorf(maxx)==maxx &&
            floorf(miny)==miny && floorf(maxy)==maxy;
         x0=(int)fmaxf(x0,fminf(dst->w,floorf(minx)));
         y0=(int)fmaxf(y0,fminf(dst->h,floorf(miny)));
         x1=(int)fminf(x1,fmaxf(0,ceilf(maxx)));
         y1=(int)fminf(y1,fmaxf(0,ceilf(maxy)));
      }
   }
   canvas_prepare_clip(state);
   if(full_pixels && state->clip_simple) {
      /* Integer-aligned axis rectangles cover complete pixels even with AA.
       * Rectangular clip membership depends on pixel centers, so fractional
       * clips can be resolved once per span rather than once per pixel. */
      x0=(int)fmaxf(x0,fminf(dst->w,ceilf(state->clip_bounds[0]-.5f)));
      y0=(int)fmaxf(y0,fminf(dst->h,ceilf(state->clip_bounds[1]-.5f)));
      x1=(int)fminf(x1,fmaxf(0,ceilf(state->clip_bounds[2]-.5f)));
      y1=(int)fminf(y1,fmaxf(0,ceilf(state->clip_bounds[3]-.5f)));
      bool replace=mode==0 || ((mode==1 || mode==3) && (color>>24)==255);
      uint32_t value=mode==0 ? 0 : color;
      if(!dst->has_alpha) value|=0xff000000u;
      for(int y=y0;y<y1;++y) {
         uint32_t *row=dst->px+(size_t)y*dst->w;
         if(replace) for(int x=x0;x<x1;++x) row[x]=value;
         else for(int x=x0;x<x1;++x) row[x]=canvas_blend(color,row[x],mode,255,!dst->has_alpha);
      }
      ++dst->gen;RETV();
   }
   for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) {
      if (!canvas_clip_contains(state,x+0.5f,y+0.5f)) continue;
      unsigned coverage=255;
      if (rect) {
         unsigned covered=0,n=aa ? 4 : 1;
         for(unsigned sy=0;sy<n;++sy) for(unsigned sx=0;sx<n;++sx) {
            float lx,ly; mat_map(inverse,x+(sx+0.5f)/n,y+(sy+0.5f)/n,&lx,&ly);
            if (!isfinite(lx) || !isfinite(ly)) continue;
            bool fill=lx>=rect[0] && ly>=rect[1] && lx<rect[2] && ly<rect[3];
            bool outer=lx>=rect[0]-half_x && ly>=rect[1]-half_y && lx<rect[2]+half_x && ly<rect[3]+half_y;
            bool inner=lx>=rect[0]+half_x && ly>=rect[1]+half_y && lx<rect[2]-half_x && ly<rect[3]-half_y;
            if (style==0 ? fill : style==1 ? outer && !inner : outer) ++covered;
         }
         if (!covered) continue;
         coverage=(covered*255+n*n/2)/(n*n);
      }
      size_t i=(size_t)y*dst->w+x;
      dst->px[i]=canvas_blend(color,dst->px[i],mode,coverage,!dst->has_alpha);
   }
   ++dst->gen;
   RETV();
}

static bool canvas_fill(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   const char *name=builtin_name();
   bool rect_draw=!strcmp(name,"drawRect");
   float rect[4]={0},stroke=0; uint32_t color=0; int style=0,mode=3; bool aa=false;
   if (rect_draw) {
      if (!canvas_rect_args(vm,args,nargs,rect)) return false;
      dvm_ref paint=nargs>=5 ? ARG(4).l : ARG(1).l;
      if (!paint) {dvm__throw(vm,"java/lang/NullPointerException","paint"); return false;}
      union dvm_value c={0};
      (void)dvm_get_field(vm,paint,"xfermode","Landroid/graphics/Xfermode;",&c);
      if(!canvas_paint_mode(vm,paint,&mode)) return false;
      (void)dvm_get_field(vm,paint,"color","I",&c); color=c.u;
      if(!paint_filter_color(vm,paint,&color)) return false;
      (void)dvm_get_field(vm,paint,"strokeWidth","F",&c); stroke=c.f;
      (void)dvm_get_field(vm,paint,"flags","I",&c); aa=(c.i&1)!=0;
      (void)dvm_get_field(vm,paint,"style","Landroid/graphics/Paint$Style;",&c);
      if (c.l) { (void)dvm_get_field(vm,c.l,"ordinal","I",&c); style=c.i; }
      union dvm_value shader={0};dvm_get_field(vm,paint,"shader","Landroid/graphics/Shader;",&shader);
      if(shader.l) {
         luna_path path={0};
         bool good=luna_path_move(&path,rect[0],rect[1]) && luna_path_line(&path,rect[2],rect[1]) &&
            luna_path_line(&path,rect[2],rect[3]) && luna_path_line(&path,rect[0],rect[3]) && luna_path_close(&path);
         if(good) good=canvas_path_color(vm,self,&path,color,style,stroke,0,0,4,aa,paint,out);
         luna_path_clear(&path,true);return good;
      }
   } else if (!strcmp(name,"drawColor")) {
      color=ARG(0).u;
      if(nargs>1 && !canvas_porter_mode(vm,ARG(1).l,&mode)) return false;
   }
   else {
      int offset=!strcmp(name,"drawRGB") ? 0 : 1;
      color=offset ? (ARG(0).u&255)<<24 : 0xff000000u;
      for(int i=0;i<3;++i) color|=(args[offset+i].u&255)<<(16-8*i);
   }
   return canvas_fill_color(vm,self,rect_draw ? rect : NULL,color,style,stroke,aa,mode,out);
}

static bool canvas_set_bitmap(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   dvm_ref bitmap = nargs ? ARG(0).l : 0;
   if (!bitmap && nargs && !strcmp(builtin_name(), "<init>")) {
      dvm__throw(vm, "java/lang/NullPointerException", "bitmap"); return false;
   }
   if (bitmap) {
      struct rt_bitmap *bm = bitmap_of(vm, bitmap);
      if (!bitmap_live(vm, bm)) return false;
      if (!bm->is_mutable) {
         dvm__throw(vm, "java/lang/IllegalStateException", "Immutable bitmap passed to Canvas");
         return false;
      }
   }
   (void)dvm_set_field(vm, self, "bitmap", "Landroid/graphics/Bitmap;", (union dvm_value){.l = bitmap});
   (void)dvm_set_field(vm, self, "matrix", "Landroid/graphics/Matrix;", (union dvm_value){0});
   struct canvas_state *state=canvas_state(vm,self);
   if (!state) return false;
   for(size_t i=0;i<state->clips;++i) {free(state->clip[i].mask);state->clip[i].mask=NULL;}
   state->count=state->clips=0;
   state->clip_cache_valid=false;
   RETV();
}

static bool canvas_size(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct canvas_state *state=canvas_state(vm,self);
   if(state && state->recording) {
      const char *name=builtin_name();
      RETI(!strcmp(name,"getWidth") ? state->viewport_width :
           !strcmp(name,"getHeight") ? state->viewport_height : 0);
   }
   union dvm_value bitmap = {0};
   (void)dvm_get_field(vm, self, "bitmap", "Landroid/graphics/Bitmap;", &bitmap);
   struct rt_bitmap *bm = bitmap_of(vm, bitmap.l);
   const char *name = builtin_name();
   RETI(!bm ? 0 : !strcmp(name, "getWidth") ? bm->w :
                    !strcmp(name, "getHeight") ? bm->h : bm->density);
}

static bool canvas_matrix(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs, union dvm_value *out)
{
   union dvm_value value = {0};
   (void)dvm_get_field(vm, self, "matrix", "Landroid/graphics/Matrix;", &value);
   bool setting = !strcmp(builtin_name(), "setMatrix");
   dvm_ref src = setting ? ARG(0).l : value.l;
   float data[9] = {1,0,0,0,1,0,0,0,1};
   if (src) {
      float *m = matrix_values(vm, src);
      if (!m) return false;
      memcpy(data, m, sizeof data);
   }
   dvm_ref dst = !setting && nargs ? ARG(0).l :
      dvm_new_object(vm, dvm_find_class(vm, "android/graphics/Matrix"));
   if (!dst) { dvm__throw(vm, "java/lang/NullPointerException", "matrix"); return false; }
   float *m = matrix_values(vm, dst);
   if (!m) return false;
   memcpy(m, data, sizeof data);
   if (setting) {
      (void)dvm_set_field(vm, self, "matrix", "Landroid/graphics/Matrix;", (union dvm_value){.l = dst});
      RETV();
   }
   if (nargs) RETV();
   RETL(dst);
}

static void canvas_sample(const struct rt_bitmap *src, float x, float y,
                           bool filter,const int subset[4], float sample[4])
{
   memset(sample, 0, 4 * sizeof *sample);
   float weights[4] = {1,0,0,0};
   int xs[4], ys[4], count = filter ? 4 : 1;
   if (filter) {
      x -= 0.5f; y -= 0.5f;
      int ix = (int)floorf(x), iy = (int)floorf(y);
      float fx = x - ix, fy = y - iy;
      xs[0] = xs[2] = ix; xs[1] = xs[3] = ix + 1;
      ys[0] = ys[1] = iy; ys[2] = ys[3] = iy + 1;
      weights[0] = (1-fx)*(1-fy); weights[1] = fx*(1-fy);
      weights[2] = (1-fx)*fy; weights[3] = fx*fy;
   } else { xs[0] = (int)floorf(x); ys[0] = (int)floorf(y); }
   for (int i = 0; i < count; ++i) {
      int ix=xs[i]<subset[0] ? subset[0] : xs[i]>=subset[2] ? subset[2]-1 : xs[i];
      int iy=ys[i]<subset[1] ? subset[1] : ys[i]>=subset[3] ? subset[3]-1 : ys[i];
      uint32_t pixel = src->px[(size_t)iy * src->w + ix];
      float alpha = src->has_alpha ? (float)(pixel >> 24) / 255.f : 1.f;
      sample[3] += alpha * weights[i];
      for (int c = 0; c < 3; ++c)
         sample[c] += (float)((pixel >> (16 - 8*c)) & 255) / 255.f * alpha * weights[i];
   }
}

static bool canvas_draw_bitmap(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   bool rectangle=nargs==4;
   if (!ARG(0).l || (!rectangle && !ARG(1).l) || (rectangle && !ARG(2).l)) {
      dvm__throw(vm, "java/lang/NullPointerException", "bitmap, matrix or destination"); return false;
   }
   struct rt_bitmap *src = bitmap_of(vm, ARG(0).l);
   if (!bitmap_live(vm, src)) return false;
   int subset[4]={0,0,src->w,src->h};
   float transform[9]={1,0,0,0,1,0,0,0,1}, inverse[9];
   const float *m;
   if(rectangle) {
      int bounds[4]={0,0,src->w,src->h};const char *edges[]={"left","top","right","bottom"};
      if(ARG(1).l) for(int i=0;i<4;++i) bounds[i]=rt_get_int(vm,ARG(1).l,edges[i]);
      double width=(double)bounds[2]-bounds[0],height=(double)bounds[3]-bounds[1];
      float dest[4];union dvm_value one={.l=ARG(2).l};
      if(!canvas_rect_args(vm,&one,1,dest)) return false;
      if(width<=0 || height<=0 || !(dest[0]<dest[2] && dest[1]<dest[3])) RETV();
      transform[0]=(float)(((double)dest[2]-dest[0])/width);
      transform[4]=(float)(((double)dest[3]-dest[1])/height);
      transform[2]=dest[0]-bounds[0]*transform[0];transform[5]=dest[1]-bounds[1]*transform[4];
      subset[0]=bounds[0]>0 ? bounds[0] : 0;subset[1]=bounds[1]>0 ? bounds[1] : 0;
      subset[2]=bounds[2]<src->w ? bounds[2] : src->w;subset[3]=bounds[3]<src->h ? bounds[3] : src->h;
      if(subset[0]>=subset[2] || subset[1]>=subset[3]) RETV();
   } else {
      m=matrix_values(vm,ARG(1).l);if(!m) return false;
      memcpy(transform,m,sizeof transform);
   }
   dvm_ref paint=args[nargs-1].l;
   union dvm_value target = {0}, current = {0};
   (void)dvm_get_field(vm, self, "bitmap", "Landroid/graphics/Bitmap;", &target);
   (void)dvm_get_field(vm, self, "matrix", "Landroid/graphics/Matrix;", &current);
   if (current.l) {
      m = matrix_values(vm, current.l);
      if (!m) return false;
      mat_mul(transform, m, transform);
   }
   struct rt_bitmap *dst = bitmap_of(vm, target.l);
   if (!dst) RETV();
   if (!bitmap_live(vm, dst)) return false;
   if (!mat_invert(transform, inverse)) RETV();
   for (int i = 0; i < 9; ++i) if (!isfinite(inverse[i])) RETV();
   union dvm_value flags = {0}, color = {.u = 0xff000000u}, xfer = {0}, filter={0};
   if (paint) {
      (void)dvm_get_field(vm, paint, "flags", "I", &flags);
      (void)dvm_get_field(vm, paint, "color", "I", &color);
      (void)dvm_get_field(vm, paint, "xfermode", "Landroid/graphics/Xfermode;", &xfer);
      (void)dvm_get_field(vm, paint, "colorFilter", "Landroid/graphics/ColorFilter;", &filter);
   }
   struct color_filter_state filter_state;
   if(!color_filter_prepare(vm,filter.l,&filter_state)) return false;
   int mode;if(!canvas_paint_mode(vm,paint,&mode)) return false;
   /* Preserve the input when a Canvas draws its own backing bitmap. */
   struct rt_bitmap snapshot = *src;
   if (src == dst) {
      size_t bytes = (size_t)src->w * src->h * sizeof *src->px;
      snapshot.px = malloc(bytes);
      if (!snapshot.px) { dvm__throw(vm, "java/lang/OutOfMemoryError", "Canvas bitmap snapshot"); return false; }
      memcpy(snapshot.px, src->px, bytes);
      src = &snapshot;
   }
   const float paint_alpha = (float)(color.u >> 24) / 255.f;
   struct canvas_state *state=canvas_state(vm,self);
   if (!state) { if(src==&snapshot) free(snapshot.px); return false; }
   int limits[4]; canvas_clip_limits(state, dst, limits);
   int x0=limits[0],y0=limits[1],x1=limits[2],y1=limits[3];
   if (transform[6]==0 && transform[7]==0 && transform[8]!=0) {
      float min_x=INFINITY,min_y=INFINITY,max_x=-INFINITY,max_y=-INFINITY;
      bool finite=true;
      for (int i=0;i<4;++i) {
         float px,py;
         mat_map(transform,(float)subset[(i&1) ? 2 : 0],(float)subset[(i&2) ? 3 : 1],&px,&py);
         finite &= isfinite(px) && isfinite(py);
         min_x=fminf(min_x,px); max_x=fmaxf(max_x,px);
         min_y=fminf(min_y,py); max_y=fmaxf(max_y,py);
      }
      if (finite) {
         x0=(int)floorf(fmaxf(x0,fminf((float)dst->w,min_x)));
         y0=(int)floorf(fmaxf(y0,fminf((float)dst->h,min_y)));
         x1=(int)ceilf(fminf(x1,fmaxf(0,max_x)));
         y1=(int)ceilf(fminf(y1,fmaxf(0,max_y)));
      }
   }
   for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) {
      if (!canvas_clip_contains(state,x+0.5f,y+0.5f)) continue;
      float sx, sy;
      float w = inverse[6]*(x+0.5f) + inverse[7]*(y+0.5f) + inverse[8];
      if (!w) continue;
      mat_map(inverse, x+0.5f, y+0.5f, &sx, &sy);
      if (!isfinite(sx) || !isfinite(sy) || sx < subset[0] || sy < subset[1] || sx >= subset[2] || sy >= subset[3]) continue;
      float sample[4];
      canvas_sample(src, sx, sy, (flags.i & 2) != 0, subset, sample);
      for (int i = 0; i < 4; ++i) sample[i] *= paint_alpha;
      if(filter_state.active) {
         uint32_t input=(uint32_t)lroundf(sample[3]*255)<<24;
         if(sample[3]>0)for(int i=0;i<3;++i)input|=(uint32_t)lroundf(fmaxf(0,fminf(1,sample[i]/sample[3]))*255)<<(16-8*i);
         uint32_t filtered=color_filter_pixel(&filter_state,input);
         sample[3]=(filtered>>24)/255.f;
         for(int i=0;i<3;++i)sample[i]=((filtered>>(16-8*i))&255)/255.f*sample[3];
      }
      uint32_t *pixel = &dst->px[(size_t)y * dst->w + x], old = *pixel;
      if(mode!=3) {
         uint32_t source=(uint32_t)lroundf(sample[3]*255)<<24;
         if(sample[3]>0) for(int c=0;c<3;++c)
            source|=(uint32_t)lroundf(fmaxf(0,fminf(1,sample[c]/sample[3]))*255)<<(16-8*c);
         *pixel=canvas_blend(source,old,mode,255,!dst->has_alpha);continue;
      }
      float da = dst->has_alpha ? (float)(old >> 24) / 255.f : 1.f;
      float alpha = sample[3] + da*(1-sample[3]);
      uint32_t result = (uint32_t)lroundf(alpha*255.f) << 24;
      for (int c = 0; c < 3; ++c) {
         int shift = 16-8*c;
         float value = sample[c] + (float)((old >> shift) & 255)/255.f * da*(1-sample[3]);
         int channel = alpha > 0 ? (int)lroundf(value/alpha*255.f) : 0;
         if (channel > 255) channel = 255;
         if (channel < 0) channel = 0;
         result |= (uint32_t)channel << shift;
      }
      *pixel = result;
   }
   if (src == &snapshot) free(snapshot.px);
   ++dst->gen;
   RETV();
}

static unsigned text_mask_sample(const struct luna_text_bitmap *mask,float x,float y,bool aa)
{
   if (!aa) return mask->coverage[(size_t)(int)y*mask->width+(int)x]>=128 ? 255 : 0;
   x-=.5f;y-=.5f;
   int ix=(int)floorf(x),iy=(int)floorf(y);float fx=x-ix,fy=y-iy,value=0;
   for(int j=0;j<2;++j) for(int i=0;i<2;++i) {
      int sx=ix+i,sy=iy+j;
      if(sx>=0 && sy>=0 && sx<mask->width && sy<mask->height)
         value+=mask->coverage[(size_t)sy*mask->width+sx]*(i ? fx : 1-fx)*(j ? fy : 1-fy);
   }
   return (unsigned)lroundf(value);
}

/* Rasterize at the Canvas scale, then inverse-map the glyph's coverage. Font
 * coordinates are relative to the baseline, not to the top of a text box. */
static bool canvas_text_run(struct dvm *vm,dvm_ref canvas,dvm_ref paint,
                             const struct luna_text_run *run,float x,float baseline,
                             bool apply_align,union dvm_value *out)
{
   union dvm_value target={0},size={0},scale={0},skew={0},flags={0},color={0},align={0},style={0},xfer={0};
   (void)dvm_get_field(vm,canvas,"bitmap","Landroid/graphics/Bitmap;",&target);
   struct rt_bitmap *dst=bitmap_of(vm,target.l);
   (void)dvm_get_field(vm,paint,"textSize","F",&size);
   (void)dvm_get_field(vm,paint,"textScaleX","F",&scale);
   (void)dvm_get_field(vm,paint,"textSkewX","F",&skew);
   (void)dvm_get_field(vm,paint,"flags","I",&flags);
   (void)dvm_get_field(vm,paint,"color","I",&color);
   if(!paint_filter_color(vm,paint,&color.u)) return false;
   (void)dvm_get_field(vm,paint,"textAlign","Landroid/graphics/Paint$Align;",&align);
   (void)dvm_get_field(vm,paint,"style","Landroid/graphics/Paint$Style;",&style);
   (void)dvm_get_field(vm,paint,"xfermode","Landroid/graphics/Xfermode;",&xfer);
   int mode;if(!canvas_paint_mode(vm,paint,&mode)) return false;
   if (style.l && path_enum_ordinal(vm,style.l)!=0) {
      dvm__throw(vm,"java/lang/UnsupportedOperationException","Text stroke");return false;
   }
   if (!dst || !run->count || !size.f || !scale.f) RETV();
   if (!bitmap_live(vm,dst)) return false;
   int alignment=apply_align && align.l ? path_enum_ordinal(vm,align.l) : 0;
   if(alignment==1) x-=run->width*scale.f/2;
   else if(alignment==2) x-=run->width*scale.f;
   float current[9],text[9]={scale.f,skew.f,x,0,1,baseline,0,0,1},transform[9];
   canvas_read_matrix(vm,canvas,current);mat_mul(transform,current,text);
   for(int i=0;i<9;++i) if(!isfinite(transform[i])) RETV();
   float raster_scale=fmaxf(hypotf(transform[0],transform[3]),hypotf(transform[1],transform[4]));
   if(!(raster_scale>0) || !isfinite(raster_scale)) RETV();
   raster_scale=fminf(fmaxf(raster_scale,1),32768/fmaxf(size.f,1));
   struct canvas_state *state=canvas_state(vm,canvas);if(!state) return false;
   int limits[4];canvas_clip_limits(state,dst,limits);
   for(size_t g=0;g<run->count;++g) {
      struct luna_text_bitmap mask={0};
      if(!luna_overlay_raster_glyph(&run->glyphs[g],size.f*raster_scale,&mask)) {
         dvm__throw(vm,"java/lang/RuntimeException","Glyph rasterization unavailable");return false;
      }
      if(!mask.coverage) continue;
      float local[9]={1/raster_scale,0,run->glyphs[g].x+mask.left/raster_scale,
         0,1/raster_scale,run->glyphs[g].y+mask.top/raster_scale,0,0,1},matrix[9],inverse[9];
      mat_mul(matrix,transform,local);
      if(!mat_invert(matrix,inverse)) {luna_overlay_free_glyph(&mask);continue;}
      int box[4];memcpy(box,limits,sizeof box);
      if(matrix[6]==0 && matrix[7]==0 && matrix[8]!=0) {
         float bounds[4]={INFINITY,INFINITY,-INFINITY,-INFINITY};bool finite=true;
         for(int i=0;i<4;++i) {
            float px,py;mat_map(matrix,(i&1) ? mask.width : 0,(i&2) ? mask.height : 0,&px,&py);
            finite &= isfinite(px) && isfinite(py);
            bounds[0]=fminf(bounds[0],px);bounds[1]=fminf(bounds[1],py);
            bounds[2]=fmaxf(bounds[2],px);bounds[3]=fmaxf(bounds[3],py);
         }
         if(finite) {
            box[0]=(int)fmaxf(box[0],fminf(dst->w,floorf(bounds[0])));
            box[1]=(int)fmaxf(box[1],fminf(dst->h,floorf(bounds[1])));
            box[2]=(int)fminf(box[2],fmaxf(0,ceilf(bounds[2])));
            box[3]=(int)fminf(box[3],fmaxf(0,ceilf(bounds[3])));
         }
      }
      for(int py=box[1];py<box[3];++py) for(int px=box[0];px<box[2];++px) {
         if(!canvas_clip_contains(state,px+.5f,py+.5f)) continue;
         float u,v;mat_map(inverse,px+.5f,py+.5f,&u,&v);
         if(!isfinite(u) || !isfinite(v) || u<0 || v<0 || u>=mask.width || v>=mask.height) continue;
         unsigned coverage=text_mask_sample(&mask,u,v,(flags.i&1)!=0);
         size_t index=(size_t)py*dst->w+px;
         dst->px[index]=canvas_blend(color.u,dst->px[index],mode,coverage,!dst->has_alpha);
      }
      luna_overlay_free_glyph(&mask);
   }
   ++dst->gen;RETV();
}

static bool canvas_draw_text(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                              int nargs,union dvm_value *out)
{
   bool array=builtin_sig()[1]=='[',ranged=nargs==6;
   int position=ranged ? 3 : 1;
   dvm_ref paint=ARG(position+2).l;
   if(!paint) {dvm__throw(vm,"java/lang/NullPointerException","paint");return false;}
   struct rt_text_units text;
   if(!text_read_units(vm,ARG(0).l,array,"java/lang/NullPointerException",&text)) return false;
   int64_t start=ranged ? ARG(1).i : 0;
   int64_t end=ranged ? (array ? start+(int64_t)ARG(2).i : ARG(2).i) : (int64_t)text.length;
   if(start<0 || end<start || (uint64_t)end>text.length) {
      free(text.owned);dvm__throw(vm,"java/lang/IndexOutOfBoundsException","text range");return false;
   }
   if(start==end) {free(text.owned);RETV();}
   struct luna_text_run run={0};
   bool good=paint_shape_units(vm,paint,text.data+start,(size_t)(end-start),&run);
   free(text.owned);
   if(good) good=canvas_text_run(vm,self,paint,&run,ARG(position).f,ARG(position+1).f,true,out);
   free(run.glyphs);return good;
}

static bool text_on_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                          int nargs,union dvm_value *out)
{
   (void)nargs;
   dvm_ref canvas=ARG(0).l;
   if(!canvas) {dvm__throw(vm,"java/lang/NullPointerException","canvas");return false;}
   struct textview_layout *layout=textview_current_layout(vm,self);if(!layout) return false;
   const struct ui_node *node=ui_node(vm,self);
   int width=node ? node->w : ui_int(vm,self,"right")-ui_int(vm,self,"left");
   int height=node ? node->h : ui_int(vm,self,"bottom")-ui_int(vm,self,"top");
   int32_t pad[4];ui_pad(vm,self,pad);
   dvm_ref content=ui_obj(vm,self,"text","Ljava/lang/CharSequence;");
   bool hint=layout->source!=content;
   union dvm_value color={.i=ui_int(vm,self,hint ? "hintTextColor" : "textColor")};
   (void)dvm_set_field(vm,layout->paint,"color","I",color);
   int gravity=ui_int(vm,self,"gravity");
   if(!gravity) gravity=ui_is_a(vm,self,"android/widget/Button") ? 0x11 :
      ui_is_a(vm,self,"android/widget/EditText") ? 0x13 : 0x33;
   int horizontal=gravity&7,vertical=gravity&0x70;
   if((gravity&0x800000) && ui_int(vm,self,"layoutDirection")==1)
      horizontal=horizontal==3 ? 5 : horizontal==5 ? 3 : horizontal;
   float y=pad[1],available=height-pad[1]-pad[3];
   if(available>layout->height) {
      if(vertical==0x10) y+=floorf((available-layout->height)/2);
      else if(vertical==0x50) y+=available-layout->height;
   }
   union dvm_value saved={0},ignored={0};
   if(!am_vcall(vm,canvas,"save","()I",NULL,0,&saved)) return false;
   int sx=ui_int(vm,self,"scrollX"),sy=ui_int(vm,self,"scrollY");
   union dvm_value clip[]={{.i=pad[0]+sx},{.i=pad[1]+sy},{.i=width-pad[2]+sx},{.i=height-pad[3]+sy}};
   bool good=am_vcall(vm,canvas,"clipRect","(IIII)Z",clip,4,&ignored);
   for(size_t i=0;good && i<layout->count;++i) {
      float x=pad[0],line_width=layout->lines[i].run.width*layout->scale;
      if(horizontal==1) x+=(width-pad[0]-pad[2]-line_width)/2;
      else if(horizontal==5) x+=width-pad[0]-pad[2]-line_width;
      good=canvas_text_run(vm,canvas,layout->paint,&layout->lines[i].run,x,
         y+layout->baseline+i*layout->line_height,false,out);
   }
   dvm_ref raised=vm->exception;
   if(raised) {dvm_pin(vm,raised);dvm_clear_exception(vm);}
   bool restored=am_vcall(vm,canvas,"restoreToCount","(I)V",&saved,1,&ignored);
   if(raised) {vm->exception=raised;dvm_unpin(vm,raised);}
   return good && restored;
}

static bool color_drawable_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                 int nargs,union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","canvas"); return false;}
   union dvm_value filter={0},bounds={0},color={0};
   (void)dvm_get_field(vm,self,"colorFilter","Landroid/graphics/ColorFilter;",&filter);
   if (!drawable_get_bounds(vm,self,NULL,0,&bounds)) return false;
   (void)dvm_get_field(vm,self,"color","I",&color);
   if (!color_filter_apply(vm,filter.l,color.u,&color.u)) return false;
   float rect[4];
   for (int i=0;i<4;++i) rect[i]=(float)ui_int(vm,bounds.l,layer_edges[i]);
   return canvas_fill_color(vm,ARG(0).l,rect,color.u,0,0,false,3,out);
}

static const struct rt_field rt_canvas_fields[] = {
   {"bitmap", "Landroid/graphics/Bitmap;"}, {"matrix", "Landroid/graphics/Matrix;"}, F_END
};
static bool canvas_draw_render_node(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static const struct rt_method rt_canvas[] = {
   M("clipRect","(FFFFLandroid/graphics/Region$Op;)Z",canvas_clip_rect),
   M("clipRect","(IIIILandroid/graphics/Region$Op;)Z",canvas_clip_rect),
   M("clipRect","(Landroid/graphics/Rect;Landroid/graphics/Region$Op;)Z",canvas_clip_rect),
   M("clipRect","(Landroid/graphics/RectF;Landroid/graphics/Region$Op;)Z",canvas_clip_rect),
   M("clipPath","(Landroid/graphics/Path;Landroid/graphics/Region$Op;)Z",canvas_clip_path),
   M("drawRoundRect", "(Landroid/graphics/RectF;FFLandroid/graphics/Paint;)V", canvas_round_rect),
   M("drawRoundRect", "(FFFFFFLandroid/graphics/Paint;)V", canvas_round_rect),
   M("drawRenderNode", "(Landroid/graphics/RenderNode;)V", canvas_draw_render_node),
   M("drawText","(Ljava/lang/String;FFLandroid/graphics/Paint;)V",canvas_draw_text),
   M("drawText","(Ljava/lang/String;IIFFLandroid/graphics/Paint;)V",canvas_draw_text),
   M("drawText","(Ljava/lang/CharSequence;IIFFLandroid/graphics/Paint;)V",canvas_draw_text),
   M("drawText","([CIIFFLandroid/graphics/Paint;)V",canvas_draw_text),
   M("drawPath","(Landroid/graphics/Path;Landroid/graphics/Paint;)V",canvas_draw_path),
   M("clipPath","(Landroid/graphics/Path;)Z",canvas_clip_path),
   M("clipOutPath","(Landroid/graphics/Path;)Z",canvas_clip_path),
   M("save", "()I", canvas_save_api),
   M("save", "(I)I", canvas_save_api),
   M("getSaveCount", "()I", canvas_save_api),
   M("restore", "()V", canvas_save_api),
   M("restoreToCount", "(I)V", canvas_save_api),
   M("translate", "(FF)V", canvas_transform),
   M("scale", "(FF)V", canvas_transform),
   M("rotate", "(F)V", canvas_transform),
   M("skew", "(FF)V", canvas_transform),
   M("concat", "(Landroid/graphics/Matrix;)V", canvas_transform),
   M("clipRect", "(FFFF)Z", canvas_clip_rect),
   M("clipRect", "(IIII)Z", canvas_clip_rect),
   M("clipRect", "(Landroid/graphics/Rect;)Z", canvas_clip_rect),
   M("clipRect", "(Landroid/graphics/RectF;)Z", canvas_clip_rect),
   M("clipOutRect", "(FFFF)Z", canvas_clip_rect),
   M("clipOutRect", "(IIII)Z", canvas_clip_rect),
   M("clipOutRect", "(Landroid/graphics/Rect;)Z", canvas_clip_rect),
   M("clipOutRect", "(Landroid/graphics/RectF;)Z", canvas_clip_rect),
   M("drawColor", "(I)V", canvas_fill),
   M("drawColor", "(ILandroid/graphics/PorterDuff$Mode;)V", canvas_fill),
   M("drawRGB", "(III)V", canvas_fill),
   M("drawARGB", "(IIII)V", canvas_fill),
   M("drawRect", "(FFFFLandroid/graphics/Paint;)V", canvas_fill),
   M("drawRect", "(Landroid/graphics/Rect;Landroid/graphics/Paint;)V", canvas_fill),
   M("drawRect", "(Landroid/graphics/RectF;Landroid/graphics/Paint;)V", canvas_fill),
   M("<init>", "()V", canvas_set_bitmap),
   M("<init>", "(Landroid/graphics/Bitmap;)V", canvas_set_bitmap),
   M("setBitmap", "(Landroid/graphics/Bitmap;)V", canvas_set_bitmap),
   M("getWidth", "()I", canvas_size), M("getHeight", "()I", canvas_size),
   M("getDensity", "()I", canvas_size),
   M("isHardwareAccelerated", "()Z", ret_false),
   M("getMatrix", "()Landroid/graphics/Matrix;", canvas_matrix),
   M("getMatrix", "(Landroid/graphics/Matrix;)V", canvas_matrix),
   M("setMatrix", "(Landroid/graphics/Matrix;)V", canvas_matrix),
   M("drawBitmap", "(Landroid/graphics/Bitmap;Landroid/graphics/Matrix;Landroid/graphics/Paint;)V", canvas_draw_bitmap),
   M("drawBitmap", "(Landroid/graphics/Bitmap;Landroid/graphics/Rect;Landroid/graphics/Rect;Landroid/graphics/Paint;)V", canvas_draw_bitmap),
   M("drawBitmap", "(Landroid/graphics/Bitmap;Landroid/graphics/Rect;Landroid/graphics/RectF;Landroid/graphics/Paint;)V", canvas_draw_bitmap),
   M_END
};
/* RecordingCanvas retains immutable command arguments, while RenderNode child
 * references remain live so a parent's display list follows child updates. */
struct render_command {
   struct dvm_method *method;
   union dvm_value args[8];
   int nargs;
};
static dvm_ref recording_snapshot(struct dvm *vm,dvm_ref ref)
{
   struct dvm_object *src=dvm__obj(vm,ref);
   if(!src) return 0;
   const char *name=src->cls->name;
   bool copy=src->kind==DVM_OBJ_ARRAY || !strcmp(name,"android/graphics/Paint") ||
      !strcmp(name,"android/graphics/Matrix") || !strcmp(name,"android/graphics/Path") ||
      !strcmp(name,"android/graphics/Rect") || !strcmp(name,"android/graphics/RectF");
   if(!copy) return ref;
   dvm_ref result;
   if(src->kind==DVM_OBJ_ARRAY) {
      result=dvm_new_array(vm,src->elem_kind,src->cls->desc+1,src->length);
      if(result) memcpy(dvm_array_data(vm,result),src->data,
         (size_t)src->length*(src->elem_kind=='J' || src->elem_kind=='D' ? 8 :
            src->elem_kind=='B' || src->elem_kind=='Z' ? 1 : src->elem_kind=='C' || src->elem_kind=='S' ? 2 : 4));
      return result;
   }
   result=dvm_new_object(vm,src->cls);if(!result) return 0;
   struct dvm_object *dst=dvm__obj(vm,result);
   if(src->slots) memcpy(dst->slots,src->slots,src->cls->islots*sizeof(*src->slots));
   if(!strcmp(name,"android/graphics/Matrix")) {
      dvm_set_field(vm,result,"values","[F",(union dvm_value){0});
      float *from=matrix_values(vm,ref),*to=matrix_values(vm,result);
      if(!from || !to) return 0;
      memcpy(to,from,9*sizeof(float));
   } else if(!strcmp(name,"android/graphics/Path")) {
      luna_path *from=path_of(vm,ref),*to=path_of(vm,result);
      if(!from || !to || !luna_path_append(to,from,NULL)) return 0;
      to->fill=from->fill;
   }
   return result;
}
static bool recording_canvas_call(struct dvm *vm,dvm_ref self,
   const union dvm_value *args,int nargs,union dvm_value *out)
{
   struct canvas_state *state=canvas_state(vm,self);
   if(!state || !state->recording_active) {
      dvm__throw(vm,"java/lang/IllegalStateException","RecordingCanvas is not recording");return false;
   }
   const char *name=builtin_name(),*sig=builtin_sig();
   if(!strcmp(name,"isHardwareAccelerated")) RETI(1);
   struct dvm_method *method=dvm_find_method(vm,dvm_find_class(vm,"android/graphics/Canvas"),name,sig);
   if(!method) return false;
   bool query=!strncmp(name,"get",3);
   if(query) return dvm_call(vm,method,self,args,nargs,out);
   if(!strcmp(name,"setBitmap") || !strcmp(name,"<init>")) {
      dvm__throw(vm,"java/lang/RuntimeException","Cannot set a bitmap on a RecordingCanvas");return false;
   }
   if(nargs>8) {dvm__throw(vm,"java/lang/IllegalArgumentException","Canvas argument count");return false;}
   if(state->command_count==state->command_capacity) {
      size_t cap=state->command_capacity ? state->command_capacity*2 : 32;
      struct render_command *grown=realloc(state->commands,cap*sizeof(*grown));
      if(!grown) {dvm__throw(vm,"java/lang/OutOfMemoryError","Display list");return false;}
      state->commands=grown;state->command_capacity=cap;
   }
   struct render_command command={.method=method,.nargs=nargs};
   memcpy(command.args,args,(size_t)nargs*sizeof(*args));
   const char *type=sig+1;
   for(int i=0;i<nargs;++i) {
      if(*type=='L' || *type=='[') {
         if(args[i].l) {
            command.args[i].l=recording_snapshot(vm,args[i].l);
            if(!command.args[i].l) {
               if(!dvm_exception(vm)) dvm__throw(vm,"java/lang/OutOfMemoryError","Display list snapshot");
               return false;
            }
         }
         while(*type=='[') ++type;
         if(*type=='L') {while(*type && *type!=';') ++type;}
      }
      ++type;
   }
   /* Save counts and matrices are observable during recording. Raster drawing
    * waits until replay, and cannot modify a target bitmap during recording. */
   bool drawing=!strncmp(name,"draw",4);
   if(!drawing && !dvm_call(vm,method,self,args,nargs,out)) return false;
   state->commands[state->command_count++]=command;
   if(drawing) RETV();
   return true;
}
static const struct rt_method rt_recording_canvas[] = {
   M("clipRect","(FFFFLandroid/graphics/Region$Op;)Z",recording_canvas_call),
   M("clipRect","(IIIILandroid/graphics/Region$Op;)Z",recording_canvas_call),
   M("clipRect","(Landroid/graphics/Rect;Landroid/graphics/Region$Op;)Z",recording_canvas_call),
   M("clipRect","(Landroid/graphics/RectF;Landroid/graphics/Region$Op;)Z",recording_canvas_call),
   M("clipPath","(Landroid/graphics/Path;Landroid/graphics/Region$Op;)Z",recording_canvas_call),
   M("drawRoundRect", "(Landroid/graphics/RectF;FFLandroid/graphics/Paint;)V", recording_canvas_call),
   M("drawRoundRect", "(FFFFFFLandroid/graphics/Paint;)V", recording_canvas_call),
   M("drawRenderNode", "(Landroid/graphics/RenderNode;)V", recording_canvas_call),
   M("drawText","(Ljava/lang/String;FFLandroid/graphics/Paint;)V",recording_canvas_call),
   M("drawText","(Ljava/lang/String;IIFFLandroid/graphics/Paint;)V",recording_canvas_call),
   M("drawText","(Ljava/lang/CharSequence;IIFFLandroid/graphics/Paint;)V",recording_canvas_call),
   M("drawText","([CIIFFLandroid/graphics/Paint;)V",recording_canvas_call),
   M("drawPath","(Landroid/graphics/Path;Landroid/graphics/Paint;)V",recording_canvas_call),
   M("clipPath","(Landroid/graphics/Path;)Z",recording_canvas_call),
   M("clipOutPath","(Landroid/graphics/Path;)Z",recording_canvas_call),
   M("save", "()I", recording_canvas_call),
   M("save", "(I)I", recording_canvas_call),
   M("getSaveCount", "()I", recording_canvas_call),
   M("restore", "()V", recording_canvas_call),
   M("restoreToCount", "(I)V", recording_canvas_call),
   M("translate", "(FF)V", recording_canvas_call),
   M("scale", "(FF)V", recording_canvas_call),
   M("rotate", "(F)V", recording_canvas_call),
   M("skew", "(FF)V", recording_canvas_call),
   M("concat", "(Landroid/graphics/Matrix;)V", recording_canvas_call),
   M("clipRect", "(FFFF)Z", recording_canvas_call),
   M("clipRect", "(IIII)Z", recording_canvas_call),
   M("clipRect", "(Landroid/graphics/Rect;)Z", recording_canvas_call),
   M("clipRect", "(Landroid/graphics/RectF;)Z", recording_canvas_call),
   M("clipOutRect", "(FFFF)Z", recording_canvas_call),
   M("clipOutRect", "(IIII)Z", recording_canvas_call),
   M("clipOutRect", "(Landroid/graphics/Rect;)Z", recording_canvas_call),
   M("clipOutRect", "(Landroid/graphics/RectF;)Z", recording_canvas_call),
   M("drawColor", "(I)V", recording_canvas_call),
   M("drawColor", "(ILandroid/graphics/PorterDuff$Mode;)V", recording_canvas_call),
   M("drawRGB", "(III)V", recording_canvas_call),
   M("drawARGB", "(IIII)V", recording_canvas_call),
   M("drawRect", "(FFFFLandroid/graphics/Paint;)V", recording_canvas_call),
   M("drawRect", "(Landroid/graphics/Rect;Landroid/graphics/Paint;)V", recording_canvas_call),
   M("drawRect", "(Landroid/graphics/RectF;Landroid/graphics/Paint;)V", recording_canvas_call),
   M("<init>", "()V", recording_canvas_call),
   M("<init>", "(Landroid/graphics/Bitmap;)V", recording_canvas_call),
   M("setBitmap", "(Landroid/graphics/Bitmap;)V", recording_canvas_call),
   M("getWidth", "()I", recording_canvas_call), M("getHeight", "()I", recording_canvas_call),
   M("getDensity", "()I", recording_canvas_call),
   M("isHardwareAccelerated", "()Z", recording_canvas_call),
   M("getMatrix", "()Landroid/graphics/Matrix;", recording_canvas_call),
   M("getMatrix", "(Landroid/graphics/Matrix;)V", recording_canvas_call),
   M("setMatrix", "(Landroid/graphics/Matrix;)V", recording_canvas_call),
   M("drawBitmap", "(Landroid/graphics/Bitmap;Landroid/graphics/Matrix;Landroid/graphics/Paint;)V", recording_canvas_call),
   M("drawBitmap", "(Landroid/graphics/Bitmap;Landroid/graphics/Rect;Landroid/graphics/Rect;Landroid/graphics/Paint;)V", recording_canvas_call),
   M("drawBitmap", "(Landroid/graphics/Bitmap;Landroid/graphics/Rect;Landroid/graphics/RectF;Landroid/graphics/Paint;)V", recording_canvas_call),
   M_END
};
static const struct rt_field rt_render_node_fields[]={
   {"recording","Landroid/graphics/RecordingCanvas;"},
   {"displayList","Landroid/graphics/RecordingCanvas;"},
   {"left","I"},{"top","I"},{"right","I"},{"bottom","I"},
   {"translationX","F"},{"translationY","F"},{"scaleX","F"},{"scaleY","F"},
   {"rotationZ","F"},{"pivotX","F"},{"pivotY","F"},{"pivotExplicit","Z"},
   {"clipToBounds","Z"},F_END
};
static bool render_node_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   dvm_set_field(vm,self,"scaleX","F",(union dvm_value){.f=1});
   dvm_set_field(vm,self,"scaleY","F",(union dvm_value){.f=1});
   dvm_set_field(vm,self,"clipToBounds","Z",(union dvm_value){.i=1});RETV();
}
static float render_node_float(struct dvm *vm,dvm_ref self,const char *field)
{union dvm_value value={0};dvm_get_field(vm,self,field,"F",&value);return value.f;}
static void render_node_pivot(struct dvm *vm,dvm_ref self,float *x,float *y)
{
   union dvm_value explicit={0};dvm_get_field(vm,self,"pivotExplicit","Z",&explicit);
   if(!explicit.i) {
      float px=(float)((int32_t)((uint32_t)rt_get_int(vm,self,"right")-(uint32_t)rt_get_int(vm,self,"left")))*.5f;
      float py=(float)((int32_t)((uint32_t)rt_get_int(vm,self,"bottom")-(uint32_t)rt_get_int(vm,self,"top")))*.5f;
      dvm_set_field(vm,self,"pivotX","F",(union dvm_value){.f=px});
      dvm_set_field(vm,self,"pivotY","F",(union dvm_value){.f=py});
   }
   *x=render_node_float(vm,self,"pivotX");*y=render_node_float(vm,self,"pivotY");
}
static void render_node_matrix(struct dvm *vm,dvm_ref self,float matrix[9])
{
   float px,py;render_node_pivot(vm,self,&px,&py);
   float sx=render_node_float(vm,self,"scaleX"),sy=render_node_float(vm,self,"scaleY");
   float radians=render_node_float(vm,self,"rotationZ")*(float)(M_PI/180.0),c=cosf(radians),sin=sinf(radians);
   float local[]={c*sx,-sin*sy,render_node_float(vm,self,"translationX")+px-c*sx*px+sin*sy*py,
      sin*sx,c*sy,render_node_float(vm,self,"translationY")+py-sin*sx*px-c*sy*py,0,0,1};
   memcpy(matrix,local,sizeof local);
}
static bool render_node_transform(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;const char *name=builtin_name();
   if(!strcmp(name,"getMatrix") || !strcmp(name,"hasIdentityMatrix")) {
      float matrix[9];render_node_matrix(vm,self,matrix);
      if(!strcmp(name,"hasIdentityMatrix")) {
         const float identity[]={1,0,0,0,1,0,0,0,1};
         bool same=true;for(int i=0;i<9;++i) same=same && matrix[i]==identity[i];RETI(same);
      }
      if(!args[0].l) {dvm__throw(vm,"java/lang/NullPointerException","outMatrix");return false;}
      float *dst=matrix_values(vm,args[0].l);if(!dst) return false;
      memcpy(dst,matrix,sizeof matrix);RETV();
   }
   if(!strcmp(name,"resetPivot") || !strcmp(name,"isPivotExplicitlySet")) {
      union dvm_value value={0};dvm_get_field(vm,self,"pivotExplicit","Z",&value);
      if(name[0]=='r') dvm_set_field(vm,self,"pivotExplicit","Z",(union dvm_value){0});
      RETI(value.i);
   }
   const char *properties[]={"translationX","translationY","scaleX","scaleY","rotationZ","pivotX","pivotY"};
   const char *suffixes[]={"TranslationX","TranslationY","ScaleX","ScaleY","RotationZ","PivotX","PivotY"};
   for(int i=0;i<7;++i) if(!strcmp(name+3,suffixes[i])) {
      if(i>=5) {float px,py;render_node_pivot(vm,self,&px,&py);}
      float old=render_node_float(vm,self,properties[i]);
      if(name[0]=='g') RETF(old);
      bool changed=old!=args[0].f;
      if(i>=5) {
         union dvm_value explicit={0};dvm_get_field(vm,self,"pivotExplicit","Z",&explicit);
         changed=changed || !explicit.i;
         dvm_set_field(vm,self,"pivotExplicit","Z",(union dvm_value){.i=1});
      }
      dvm_set_field(vm,self,properties[i],"F",args[0]);RETI(changed);
   }
   return false;
}
static bool render_node_position(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;bool changed=false;
   const char *fields[]={"left","top","right","bottom"};
   for(int i=0;i<4;++i) {
      changed|=rt_get_int(vm,self,fields[i])!=args[i].i;
      rt_set_int(vm,self,fields[i],args[i].i);
   }
   RETI(changed);
}
static bool render_node_record(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value current={0},list={0};
   dvm_get_field(vm,self,"recording","Landroid/graphics/RecordingCanvas;",&current);
   dvm_get_field(vm,self,"displayList","Landroid/graphics/RecordingCanvas;",&list);
   const char *name=builtin_name();
   if(!strcmp(name,"hasDisplayList")) RETI(list.l!=0);
   if(!strcmp(name,"discardDisplayList")) {
      dvm_set_field(vm,self,"displayList","Landroid/graphics/RecordingCanvas;",(union dvm_value){0});RETV();
   }
   if(!strcmp(name,"beginRecording")) {
      if(current.l) {dvm__throw(vm,"java/lang/IllegalStateException","Recording already in progress");return false;}
      dvm_ref canvas=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/RecordingCanvas"));
      struct canvas_state *state=canvas_state(vm,canvas);if(!state) return false;
      state->recording=state->recording_active=true;
      state->viewport_width=nargs ? args[0].i : (int32_t)((uint32_t)rt_get_int(vm,self,"right")-(uint32_t)rt_get_int(vm,self,"left"));
      state->viewport_height=nargs ? args[1].i : (int32_t)((uint32_t)rt_get_int(vm,self,"bottom")-(uint32_t)rt_get_int(vm,self,"top"));
      dvm_set_field(vm,self,"recording","Landroid/graphics/RecordingCanvas;",(union dvm_value){.l=canvas});RETL(canvas);
   }
   if(!current.l) {dvm__throw(vm,"java/lang/IllegalStateException","No recording in progress");return false;}
   struct canvas_state *state=canvas_state(vm,current.l);if(!state) return false;
   state->recording_active=false;
   dvm_set_field(vm,self,"displayList","Landroid/graphics/RecordingCanvas;",current);
   dvm_set_field(vm,self,"recording","Landroid/graphics/RecordingCanvas;",(union dvm_value){0});RETV();
}
static bool render_node_clip(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;union dvm_value old={0};dvm_get_field(vm,self,"clipToBounds","Z",&old);
   dvm_set_field(vm,self,"clipToBounds","Z",args[0]);RETI((old.i!=0)!=(args[0].i!=0));
}
static bool canvas_draw_render_node(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;dvm_ref node=args[0].l;
   if(!node) {dvm__throw(vm,"java/lang/NullPointerException","renderNode");return false;}
   union dvm_value list={0};dvm_get_field(vm,node,"displayList","Landroid/graphics/RecordingCanvas;",&list);
   if(!list.l) RETV();
   struct canvas_state *recording=canvas_state(vm,list.l);if(!recording) return false;
   union dvm_value saved={0},ignored={0};
   if(!am_vcall(vm,self,"save","()I",NULL,0,&saved)) return false;
   union dvm_value shift[]={{.f=(float)rt_get_int(vm,node,"left")},{.f=(float)rt_get_int(vm,node,"top")}};
   bool good=am_vcall(vm,self,"translate","(FF)V",shift,2,&ignored);
   float base[9],local[9];canvas_read_matrix(vm,self,base);render_node_matrix(vm,node,local);
   mat_mul(base,base,local);
   if(good) good=canvas_write_matrix(vm,self,base);
   union dvm_value clip={0};dvm_get_field(vm,node,"clipToBounds","Z",&clip);
   if(good && clip.i) {
      union dvm_value rect[]={{.i=0},{.i=0},{.i=(int32_t)((uint32_t)rt_get_int(vm,node,"right")-(uint32_t)rt_get_int(vm,node,"left"))},
         {.i=(int32_t)((uint32_t)rt_get_int(vm,node,"bottom")-(uint32_t)rt_get_int(vm,node,"top"))}};
      good=am_vcall(vm,self,"clipRect","(IIII)Z",rect,4,&ignored);
   }
   float replay_base[9];canvas_read_matrix(vm,self,replay_base);
   for(size_t i=0;good && i<recording->command_count;++i) {
      struct render_command *command=&recording->commands[i];
      union dvm_value values[8];memcpy(values,command->args,sizeof(values));
      if(!strcmp(command->method->name,"restoreToCount")) values[0].i+=saved.i;
      if(!strcmp(command->method->name,"setMatrix")) {
         const float identity[9]={1,0,0,0,1,0,0,0,1};
         float composed[9];
         const float *local=values[0].l ? matrix_values(vm,values[0].l) : identity;
         if(!local) {good=false;break;}
         mat_mul(composed,replay_base,local);
         good=canvas_write_matrix(vm,self,composed);
      } else good=dvm_call(vm,command->method,self,values,command->nargs,&ignored);
   }
   dvm_ref raised=vm->exception;if(raised) dvm_clear_exception(vm);
   bool restored=am_vcall(vm,self,"restoreToCount","(I)V",&saved,1,&ignored);
   if(raised) vm->exception=raised;
   if(!good || !restored) return false;
   RETV();
}
static const struct rt_method rt_render_node[]={
   M("getMatrix","(Landroid/graphics/Matrix;)V",render_node_transform),
   M("hasIdentityMatrix","()Z",render_node_transform),
   M("resetPivot","()Z",render_node_transform),
   M("isPivotExplicitlySet","()Z",render_node_transform),
   M("setTranslationX","(F)Z",render_node_transform),
   M("getTranslationX","()F",render_node_transform),
   M("setTranslationY","(F)Z",render_node_transform),
   M("getTranslationY","()F",render_node_transform),
   M("setScaleX","(F)Z",render_node_transform),
   M("getScaleX","()F",render_node_transform),
   M("setScaleY","(F)Z",render_node_transform),
   M("getScaleY","()F",render_node_transform),
   M("setRotationZ","(F)Z",render_node_transform),
   M("getRotationZ","()F",render_node_transform),
   M("setPivotX","(F)Z",render_node_transform),
   M("getPivotX","()F",render_node_transform),
   M("setPivotY","(F)Z",render_node_transform),
   M("getPivotY","()F",render_node_transform),
   M("<init>","(Ljava/lang/String;)V",render_node_init),
   M("setPosition","(IIII)Z",render_node_position),
   M("setClipToBounds","(Z)Z",render_node_clip),
   M("beginRecording","()Landroid/graphics/RecordingCanvas;",render_node_record),
   M("beginRecording","(II)Landroid/graphics/RecordingCanvas;",render_node_record),
   M("endRecording","()V",render_node_record),
   M("hasDisplayList","()Z",render_node_record),
   M("discardDisplayList","()V",render_node_record),M_END
};


/* View.draw's background, content, children and foreground are distinct
 * paint stages. The scroll offset applies to content and children. */
static bool view_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                       int nargs,union dvm_value *out)
{
   (void)nargs;
   dvm_ref canvas=ARG(0).l;
   if (!canvas) {dvm__throw(vm,"java/lang/NullPointerException","canvas"); return false;}
   const struct ui_node *node=ui_node(vm,self);
   int32_t width=node ? node->w : ui_int(vm,self,"right")-ui_int(vm,self,"left");
   int32_t height=node ? node->h : ui_int(vm,self,"bottom")-ui_int(vm,self,"top");
   dvm_ref bg=ui_obj(vm,self,"background","Landroid/graphics/drawable/Drawable;");
   if (bg) {
      union dvm_value bounds[]={{.i=0},{.i=0},{.i=width},{.i=height}},ignored={0};
      if (!am_vcall(vm,bg,"setBounds","(IIII)V",bounds,4,&ignored) ||
          !am_vcall(vm,bg,"draw","(Landroid/graphics/Canvas;)V",args,1,out)) return false;
   } else {
      int32_t color=ui_int(vm,self,"backgroundColor");
      if (color) {
         float rect[]={0,0,(float)width,(float)height};
         if (!canvas_fill_color(vm,canvas,rect,(uint32_t)color,0,0,false,3,out)) return false;
      }
   }
   float original[9],matrix[9],scroll[9]={1,0,0,0,1,0,0,0,1};
   canvas_read_matrix(vm,canvas,original);
   scroll[2]=-(float)ui_int(vm,self,"scrollX"); scroll[5]=-(float)ui_int(vm,self,"scrollY");
   mat_mul(matrix,original,scroll);
   if (!canvas_write_matrix(vm,canvas,matrix)) return false;
   bool good=true;
   if (!ui_int(vm,self,"willNotDraw"))
      good=am_vcall(vm,self,"onDraw","(Landroid/graphics/Canvas;)V",(union dvm_value *)args,1,out);
   if (good) good=am_vcall(vm,self,"dispatchDraw","(Landroid/graphics/Canvas;)V",(union dvm_value *)args,1,out);
   if (!canvas_write_matrix(vm,canvas,original)) return false;
   if (!good) return false;
   return am_vcall(vm,self,"onDrawForeground","(Landroid/graphics/Canvas;)V",(union dvm_value *)args,1,out);
}

static bool vg_draw_child(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                           int nargs,union dvm_value *out)
{
   (void)nargs;
   dvm_ref canvas=ARG(0).l,child=ARG(1).l;
   if (!canvas || !child) {dvm__throw(vm,"java/lang/NullPointerException","drawChild"); return false;}
   /* View.draw(canvas,parent,time) returns continued Animation work, not
    * whether pixels were drawn. A static child must not sustain traversal. */
   dvm_ref animation=ui_obj(vm,child,"animation","Landroid/view/animation/Animation;");
   bool more=animation && !rt_get_int(vm,animation,"ended"),visible=true;
   union dvm_value saved={0},ignored={0};
   if (!am_vcall(vm,canvas,"save","()I",NULL,0,&saved)) return false;
   float original[9],matrix[9],position[9]={1,0,0,0,1,0,0,0,1};
   canvas_read_matrix(vm,canvas,original);
   const struct ui_node *node=ui_node(vm,child);
   position[2]=node ? node->x : ui_int(vm,child,"left");
   position[5]=node ? node->y : ui_int(vm,child,"top");
   mat_mul(matrix,original,position);
   union dvm_value child_matrix={0};
   bool good=view_get_matrix(vm,child,NULL,0,&child_matrix);
   if (good && child_matrix.l) {
      const float *values=matrix_values(vm,child_matrix.l);
      if (!values) good=false; else mat_mul(matrix,matrix,values);
   }
   if (good) good=canvas_write_matrix(vm,canvas,matrix);
   if (good && !ui_int(vm,self,"clipChildrenOff")) {
      int w=node ? node->w : ui_int(vm,child,"right")-ui_int(vm,child,"left");
      int h=node ? node->h : ui_int(vm,child,"bottom")-ui_int(vm,child,"top");
      union dvm_value clip[]={{.i=0},{.i=0},{.i=w},{.i=h}};
      good=am_vcall(vm,canvas,"clipRect","(IIII)Z",clip,4,&ignored);
      visible=ignored.i!=0;
   }
   bool trace=g_ui_draw_trace_budget!=0;
   uint32_t before=0;struct rt_bitmap *trace_bitmap=NULL;
   if(trace) {
      --g_ui_draw_trace_budget;
      trace_bitmap=bitmap_of(vm,ui_obj(vm,canvas,"bitmap","Landroid/graphics/Bitmap;"));
      if(trace_bitmap && trace_bitmap->px && trace_bitmap->w>0 && trace_bitmap->h>0)
         before=trace_bitmap->px[(size_t)(trace_bitmap->h/2)*trace_bitmap->w+trace_bitmap->w/2];
      int clip[4]={0};struct canvas_state *state=canvas_state(vm,canvas);
      if(state && trace_bitmap) canvas_clip_limits(state,trace_bitmap,clip);
      fprintf(stderr,"[view-draw] begin p=%u %s c=%u %s id=%08x vis=%d box=%d,%d %dx%d visible=%d good=%d clip=%d,%d,%d,%d matrix=%g,%g,%g,%g,%g,%g,%g,%g,%g center=%08x\n",
         (unsigned)self,ui_class_simple_name(vm,self),(unsigned)child,ui_class_simple_name(vm,child),
         (unsigned)ui_int(vm,child,"id"),ui_int(vm,child,"visibility"),node ? node->x : 0,node ? node->y : 0,
         node ? node->w : 0,node ? node->h : 0,visible,good,clip[0],clip[1],clip[2],clip[3],
         matrix[0],matrix[1],matrix[2],matrix[3],matrix[4],matrix[5],matrix[6],matrix[7],matrix[8],before);
   }
   union dvm_value argument={.l=canvas};
   if (good && (visible || more)) good=am_vcall(vm,child,"draw","(Landroid/graphics/Canvas;)V",&argument,1,&ignored);
   if(trace) {
      uint32_t after=0;
      if(trace_bitmap && trace_bitmap->px && trace_bitmap->w>0 && trace_bitmap->h>0)
         after=trace_bitmap->px[(size_t)(trace_bitmap->h/2)*trace_bitmap->w+trace_bitmap->w/2];
      fprintf(stderr,"[view-draw] end p=%u c=%u good=%d center=%08x -> %08x\n",(unsigned)self,(unsigned)child,good,before,after);
   }
   /* Restore host Canvas state even if an app drawing callback throws. */
   dvm_ref raised=vm->exception;
   if (raised) {dvm_pin(vm,raised); dvm_clear_exception(vm);}
   bool restored=am_vcall(vm,canvas,"restoreToCount","(I)V",&saved,1,&ignored);
   if (raised) {vm->exception=raised; dvm_unpin(vm,raised);}
   if (!good || !restored) return false;
   RETI(more);
}

static bool vg_dispatch_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                              int nargs,union dvm_value *out)
{
   (void)nargs;
   struct rt_list *list=list_of(vm,view_children(vm,self,false));
   uint32_t count=list ? list->size : 0;
   dvm_ref *children=count ? malloc((size_t)count*sizeof *children) : NULL;
   if (count && !children) {dvm__throw(vm,"java/lang/OutOfMemoryError","draw children"); return false;}
   for(uint32_t i=0;i<count;++i) {children[i]=list->items[i]; dvm_pin(vm,children[i]);}
   bool good=true,more=false;
   union dvm_value saved={0},ignored={0};
   good=am_vcall(vm,ARG(0).l,"save","()I",NULL,0,&saved);
   if (good && !ui_int(vm,self,"clipPaddingOff")) {
      const struct ui_node *node=ui_node(vm,self);
      int width=node ? node->w : ui_int(vm,self,"right")-ui_int(vm,self,"left");
      int height=node ? node->h : ui_int(vm,self,"bottom")-ui_int(vm,self,"top");
      int32_t pad[4]; ui_pad(vm,self,pad);
      if (pad[0] || pad[1] || pad[2] || pad[3]) {
         int sx=ui_int(vm,self,"scrollX"),sy=ui_int(vm,self,"scrollY");
         union dvm_value clip[]={{.i=pad[0]+sx},{.i=pad[1]+sy},
            {.i=width-pad[2]+sx},{.i=height-pad[3]+sy}};
         good=am_vcall(vm,ARG(0).l,"clipRect","(IIII)Z",clip,4,&ignored);
      }
   }
   for(uint32_t i=0;good && i<count;++i) {
      uint32_t index=i;
      if (ui_int(vm,self,"childrenDrawingOrderEnabled")) {
         union dvm_value order[]={{.i=(int32_t)count},{.i=(int32_t)i}},result={0};
         good=am_vcall(vm,self,"getChildDrawingOrder","(II)I",order,2,&result);
         if (!good) break;
         if (result.i<0 || (uint32_t)result.i>=count) {
            dvm__throw(vm,"java/lang/IndexOutOfBoundsException","child drawing order"); good=false; break;
         }
         index=(uint32_t)result.i;
      }
      if (!children[index] || ui_int(vm,children[index],"visibility")!=0) continue;
      union dvm_value draw[]={ARG(0),{.l=children[index]},{.j=sysclock_ms(CLOCK_MONOTONIC)}};
      good=am_vcall(vm,self,"drawChild","(Landroid/graphics/Canvas;Landroid/view/View;J)Z",draw,3,&ignored);
      if(good) more |= ignored.i!=0;
   }
   for(uint32_t i=0;i<count;++i) dvm_unpin(vm,children[i]);
   free(children);
   dvm_ref raised=vm->exception;
   if (raised) {dvm_pin(vm,raised); dvm_clear_exception(vm);}
   bool restored=!saved.i || am_vcall(vm,ARG(0).l,"restoreToCount","(I)V",&saved,1,&ignored);
   if (raised) {vm->exception=raised; dvm_unpin(vm,raised);}
   if (!good || !restored) return false;
   if(more) return view_invalidate_frame(vm,self,NULL,0,out);
   RETV();
}

static uint32_t gd_sample(const uint32_t *,uint32_t,const float *,float,uint32_t);
static uint32_t vector_gradient_sample(const struct vector_gradient *g,float x,float y)
{
 float t;
 if(g->type==1) t=hypotf(x-g->cx,y-g->cy)/g->radius;
 else if(g->type==2) {t=atan2f(y-g->cy,x-g->cx)/(float)(2*M_PI);if(t<0) t+=1;}
 else {
  float dx=g->x1-g->x0,dy=g->y1-g->y0,length=dx*dx+dy*dy;
  t=length>0 ? ((x-g->x0)*dx+(y-g->y0)*dy)/length : 1;
 }
 if(g->tile==1) t-=floorf(t);
 else if(g->tile==2) {t=fmodf(t,2);if(t<0) t+=2;if(t>1) t=2-t;}
 return gd_sample(g->colors,(uint32_t)g->count,g->positions,t,0);
}
static unsigned vector_alpha_byte(float value)
{ return (unsigned)floorf(fmaxf(0,fminf(1,value))*255+.5f); }
static bool vector_render_nodes(const struct vector_node *node,int width,int height,
                                  const float group[9],const float viewport[9],float viewport_scale,
                                  const unsigned char *inherited_clip,uint32_t *pixels)
{
   size_t count=(size_t)width*height;unsigned char *clip=NULL;
   for(;node;node=node->next) {
      if(!node->kind) {
         float nested[9];mat_mul(nested,group,node->matrix);
         if(!vector_render_nodes(node->children,width,height,nested,viewport,viewport_scale,
            clip ? clip : inherited_clip,pixels)) {free(clip);return false;}
         continue;
      }
      float matrix[9];mat_mul(matrix,viewport,group);
      if(node->kind==2) {
         unsigned char *mask=luna_path_mask(&node->path,width,height,matrix,0,0,0,0,4,false);
         if(!mask) {free(clip);return false;}
         if(!clip) {
            clip=malloc(count);if(!clip) {free(mask);return false;}
            if(inherited_clip) memcpy(clip,inherited_clip,count);else memset(clip,255,count);
         }
         for(size_t i=0;i<count;++i) clip[i]=(unsigned char)((clip[i]*mask[4*i+3]+127)/255);
         free(mask);continue;
      }
      luna_path trimmed={.fill=node->path.fill};const luna_path *path=&node->path;
      if(node->trim_start!=0 || node->trim_end!=1) {
         if(!luna_path_trim(&trimmed,path,node->trim_start,node->trim_end,node->trim_offset)) {free(clip);luna_path_clear(&trimmed,true);return false;}
         path=&trimmed;
      }
      /* Android's vector stroke scale is |cross(Mx,My)|/max(|Mx|,|My|),
       * multiplied by the smaller viewport scale. It is not an axis average. */
      float maximum=fmaxf(hypotf(group[0],group[3]),hypotf(group[1],group[4]));
      float stroke_scale=maximum>0 ? fabsf(group[0]*group[4]-group[1]*group[3])/maximum*viewport_scale : 0;
      for(int pass=0;pass<2;++pass) {
         uint32_t color=pass ? node->stroke : node->fill;
         const struct vector_gradient *gradient=pass ? node->stroke_gradient : node->fill_gradient;
         unsigned alpha=vector_alpha_byte(pass ? node->stroke_alpha : node->fill_alpha);
         if((!gradient && !(color>>24)) || !alpha || (pass && !(node->stroke_width>0 && stroke_scale>0))) continue;
         float shader_inverse[9];if(gradient && !mat_invert(matrix,shader_inverse)) continue;
         unsigned char *mask=luna_path_mask(path,width,height,matrix,pass,node->stroke_width*stroke_scale,
            node->cap,node->join,node->miter,true);
         if(!mask) {free(clip);luna_path_clear(&trimmed,true);return false;}
         const unsigned char *active_clip=clip ? clip : inherited_clip;
         for(size_t i=0;i<count;++i) {
            unsigned coverage=mask[4*i+3];if(active_clip) coverage=(coverage*active_clip[i]+127)/255;
            if(!coverage) continue;
            uint32_t sample=color;
            if(gradient) {float x,y;mat_map(shader_inverse,(float)(i%(size_t)width)+.5f,(float)(i/(size_t)width)+.5f,&x,&y);sample=vector_gradient_sample(gradient,x,y);}
            sample=(sample&0xffffffu)|((((sample>>24)*alpha+127)/255)<<24);
            uint32_t source=(sample&0xffffffu)|((((sample>>24)*coverage+127)/255)<<24);
            pixels[i]=canvas_src_over(source,pixels[i],false);
         }
         free(mask);
      }
      luna_path_clear(&trimmed,true);
   }
   free(clip);return true;
}
static bool vector_property(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;struct vector_data *vector=vector_of(vm,self);
   const char *name=builtin_name();
   if(!strcmp(name,"<init>")) {
      if(!vector) {vector=calloc(1,sizeof *vector);if(!vector) {dvm__throw(vm,"java/lang/OutOfMemoryError","Vector");return false;}
         if (!vector_attach(vm,self,vector)) return false;vector->alpha=1;vector->width=vector->height=-1;}
      return drawable_init(vm,self,NULL,0,out);
   }
   if(!strcmp(name,"getIntrinsicWidth") || !strcmp(name,"getIntrinsicHeight")) {
      int value=vector ? (!strcmp(name,"getIntrinsicWidth") ? vector->width : vector->height) : -1;
      int target=ui_int(vm,self,"vectorDensity");
      if (vector && value>0 && target>0 && vector->density>0)
         value=(int)(((int64_t)value*target+vector->density/2)/vector->density);
      RETI(value);
   }
   if(!strcmp(name,"getAlpha")) RETI(vector ? (int)(fmaxf(0,fminf(1,vector->alpha))*255) : 255);
   if(!strcmp(name,"setAlpha")) {if(vector) vector->alpha=fmaxf(0,fminf(1,ARG(0).i/255.f));ui_invalidate();RETV();}
   if(!strcmp(name,"isAutoMirrored")) RETI(vector && vector->mirrored);
   if(!strcmp(name,"setAutoMirrored")) {if(vector) vector->mirrored=ARG(0).i!=0;ui_invalidate();RETV();}
   if(!strcmp(name,"setTint")) {
      if(vector) {vector->tint=true;vector->tint_color=ARG(0).u;}
      (void)dvm_set_field(vm,vector_state_ref(vm,self),"vectorTint","Landroid/content/res/ColorStateList;",(union dvm_value){0});
   } else if(!strcmp(name,"setTintList")) {
      if(vector) vector->tint=ARG(0).l!=0;
      (void)dvm_set_field(vm,vector_state_ref(vm,self),"vectorTint","Landroid/content/res/ColorStateList;",ARG(0));
   } else if(!strcmp(name,"setTintMode")) {
      (void)dvm_set_field(vm,vector_state_ref(vm,self),"vectorTintMode","Landroid/graphics/PorterDuff$Mode;",ARG(0));
   }
   ui_invalidate();RETV();
}
static bool vector_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;struct vector_data *vector=vector_of(vm,self);
   if(!vector || !vector->root.children) RETV();
   dvm_ref bounds=ui_obj(vm,self,"bounds","Landroid/graphics/Rect;");if(!bounds) RETV();
   int left=ui_int(vm,bounds,"left"),top=ui_int(vm,bounds,"top");
   int width=ui_int(vm,bounds,"right")-left,height=ui_int(vm,bounds,"bottom")-top;
   if(width<=0 || height<=0) RETV();
   float current[9];canvas_read_matrix(vm,ARG(0).l,current);
   float sx=fabsf(current[0]),sy=fabsf(current[4]);
   if(current[1]!=0 || current[3]!=0) sx=sy=1; /* AOSP cache sizing for skew/rotation. */
   int cache_w=(int)fminf(2048,ceilf(width*sx)),cache_h=(int)fminf(2048,ceilf(height*sy));
   if(cache_w<=0 || cache_h<=0) RETV();
   dvm_ref cached=ui_obj(vm,self,"vectorCache","Landroid/graphics/Bitmap;");
   struct rt_bitmap *bitmap=bitmap_of(vm,cached);
   if(!bitmap || bitmap->w!=cache_w || bitmap->h!=cache_h || bitmap->recycled) {
      cached=bitmap_new(vm,cache_w,cache_h);if(!cached) return false;
      (void)dvm_set_field(vm,self,"vectorCache","Landroid/graphics/Bitmap;",(union dvm_value){.l=cached});
      bitmap=bitmap_of(vm,cached);bitmap->density=0;bitmap->is_mutable=true;
      const float identity[]={1,0,0,0,1,0,0,0,1};
      float viewport[]={cache_w/vector->viewport_width,0,0,0,cache_h/vector->viewport_height,0,0,0,1};
      if(!vector_render_nodes(vector->root.children,cache_w,cache_h,identity,viewport,
         fminf(viewport[0],viewport[4]),NULL,bitmap->px)) {
         (void)dvm_set_field(vm,self,"vectorCache","Landroid/graphics/Bitmap;",(union dvm_value){0});
         dvm__throw(vm,"java/lang/OutOfMemoryError","Vector raster");return false;
      }
      ++bitmap->gen;
   }
   /* Tint/filter affect the completed vector, rather than each overlapping path. */
   union dvm_value tint_list={0},mode={0},filter={0};
   dvm_get_field(vm,self,"colorFilter","Landroid/graphics/ColorFilter;",&filter);
   (void)dvm_get_field(vm,vector_state_ref(vm,self),"vectorTint","Landroid/content/res/ColorStateList;",&tint_list);
   (void)dvm_get_field(vm,vector_state_ref(vm,self),"vectorTintMode","Landroid/graphics/PorterDuff$Mode;",&mode);
   uint32_t tint=vector->tint_color;
   if(tint_list.l) {
      dvm_ref states=ui_obj(vm,self,"stateSet","[I");struct dvm_object *array=states ? dvm__obj(vm,states) : NULL;
      union dvm_value def={0};(void)dvm_get_field(vm,tint_list.l,"color","I",&def);
      tint=(uint32_t)color_state_lookup(vm,tint_list.l,array ? array->data : NULL,array ? array->length : 0,def.i);
   }
   if(vector->tint && !filter.l && mode.l && path_enum_ordinal(vm,mode.l)!=5) {
      dvm__throw(vm,"java/lang/UnsupportedOperationException","Vector tint mode");return false;
   }
   dvm_ref shown=cached;
   if(vector->tint && !filter.l) {
      shown=ui_obj(vm,self,"vectorTintCache","Landroid/graphics/Bitmap;");struct rt_bitmap *tinted=bitmap_of(vm,shown);
      if(!tinted || tinted->w!=cache_w || tinted->h!=cache_h || tinted->recycled) {
         shown=bitmap_new(vm,cache_w,cache_h);if(!shown) return false;
         (void)dvm_set_field(vm,self,"vectorTintCache","Landroid/graphics/Bitmap;",(union dvm_value){.l=shown});
         tinted=bitmap_of(vm,shown);tinted->is_mutable=true;tinted->density=0;rt_set_int(vm,self,"vectorTintGeneration",0);
      }
      if((unsigned)ui_int(vm,self,"vectorTintGeneration")!=bitmap->gen || (uint32_t)ui_int(vm,self,"vectorTintColor")!=tint) {
         for(size_t i=0;i<(size_t)cache_w*cache_h;++i)
            tinted->px[i]=(tint&0xffffffu)|((((tint>>24)*(bitmap->px[i]>>24)+127)/255)<<24);
         rt_set_int(vm,self,"vectorTintGeneration",(int32_t)bitmap->gen);++tinted->gen;
         rt_set_int(vm,self,"vectorTintColor",(int32_t)tint);
      }
   }
   dvm_ref paint=ui_obj(vm,self,"vectorPaint","Landroid/graphics/Paint;");
   if(!paint) {
      paint=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/Paint"));if(!paint) return false;
      (void)dvm_set_field(vm,self,"vectorPaint","Landroid/graphics/Paint;",(union dvm_value){.l=paint});
      union dvm_value flags={.i=2},ignored={0};if(!paint_init(vm,paint,&flags,1,&ignored)) return false;
   }
   union dvm_value alpha={.i=(int)vector_alpha_byte(vector->alpha)},ignored={0};
   if(!paint_alpha(vm,paint,&alpha,1,&ignored)) return false;
   dvm_set_field(vm,paint,"colorFilter","Landroid/graphics/ColorFilter;",filter);
   dvm_ref transform=ui_obj(vm,self,"vectorMatrix","Landroid/graphics/Matrix;");
   if(!transform) {transform=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/Matrix"));if(!transform) return false;
      (void)dvm_set_field(vm,self,"vectorMatrix","Landroid/graphics/Matrix;",(union dvm_value){.l=transform});}
   float *matrix=matrix_values(vm,transform);if(!matrix) return false;
   const float placed[]={width/(float)cache_w,0,(float)left,0,height/(float)cache_h,(float)top,0,0,1};
   memcpy(matrix,placed,sizeof placed);
   if(vector->mirrored && ui_int(vm,self,"layoutDirection")==1) {matrix[0]=-matrix[0];matrix[2]=left+width;}
   union dvm_value draw[]={{.l=shown},{.l=transform},{.l=paint}};
   return canvas_draw_bitmap(vm,ARG(0).l,draw,3,out);
}
static bool vector_constant_state(struct dvm *vm,dvm_ref self,
                                   const union dvm_value *args,int nargs,union dvm_value *out)
{ (void)args;(void)nargs;RETL(vector_state_ref(vm,self)); }

static bool vector_mutate(struct dvm *vm,dvm_ref self,
                          const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   struct vector_data *source=vector_of(vm,self);
   if (source && !ui_int(vm,self,"vectorMutated")) {
      dvm_ref old=vector_state_ref(vm,self);
      dvm_pin(vm,old);
      struct vector_data *copy=vector_copy(source);
      if (!copy) {dvm_unpin(vm,old);dvm__throw(vm,"java/lang/OutOfMemoryError","Vector state copy");return false;}
      int density=source->density;
      if (!vector_attach(vm,self,copy)) {dvm_unpin(vm,old);return false;}
      copy->density=density;
      const char *names[]={"vectorTint","vectorTintMode"};
      const char *sigs[]={"Landroid/content/res/ColorStateList;","Landroid/graphics/PorterDuff$Mode;"};
      for (int i=0;i<2;++i) {
         union dvm_value value={0};dvm_get_field(vm,old,names[i],sigs[i],&value);
         dvm_set_field(vm,vector_state_ref(vm,self),names[i],sigs[i],value);
      }
      dvm_unpin(vm,old);rt_set_int(vm,self,"vectorMutated",1);
   }
   RETL(self);
}

static bool vector_new_drawable(struct dvm *vm,dvm_ref self,
                                const union dvm_value *args,int nargs,union dvm_value *out)
{
   dvm_ref drawable=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/drawable/VectorDrawable"));
   if (!drawable) return false;
   dvm_pin(vm,drawable);
   bool good=dvm_set_field(vm,drawable,"vectorState","Landroid/graphics/drawable/VectorDrawable$VectorDrawableState;",
                           (union dvm_value){.l=self});
   if (good && nargs>0 && ARG(0).l) {
      union dvm_value metrics={0};
      good=am_vcall(vm,ARG(0).l,"getDisplayMetrics","()Landroid/util/DisplayMetrics;",NULL,0,&metrics);
      if (good && metrics.l) rt_set_int(vm,drawable,"vectorDensity",ui_int(vm,metrics.l,"densityDpi"));
   }
   if (good) good=drawable_init(vm,drawable,NULL,0,out);
   dvm_unpin(vm,drawable);
   if (!good) return false;
   RETL(drawable);
}
/* ConstantState's concrete overloads dispatch to the subclass factory. */
static bool drawable_state_new(struct dvm *vm,dvm_ref self,
                               const union dvm_value *args,int nargs,union dvm_value *out)
{
   if (nargs>1) return am_vcall(vm,self,"newDrawable","(Landroid/content/res/Resources;)Landroid/graphics/drawable/Drawable;",args,1,out);
   return am_vcall(vm,self,"newDrawable","()Landroid/graphics/drawable/Drawable;",NULL,0,out);
}
static const struct rt_method rt_drawable_constant_state[]={
 {"newDrawable","()Landroid/graphics/drawable/Drawable;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
 M("newDrawable","(Landroid/content/res/Resources;)Landroid/graphics/drawable/Drawable;",drawable_state_new),
 M("newDrawable","(Landroid/content/res/Resources;Landroid/content/res/Resources$Theme;)Landroid/graphics/drawable/Drawable;",drawable_state_new),
 M("<init>","()V",nop_void),M("getChangingConfigurations","()I",ret_zero),
 M("canApplyTheme","()Z",ret_false),M_END};
static const struct rt_method rt_vector_state[]={
 M("newDrawable","()Landroid/graphics/drawable/Drawable;",vector_new_drawable),
 M("newDrawable","(Landroid/content/res/Resources;)Landroid/graphics/drawable/Drawable;",vector_new_drawable),
 M("newDrawable","(Landroid/content/res/Resources;Landroid/content/res/Resources$Theme;)Landroid/graphics/drawable/Drawable;",vector_new_drawable),M_END};
static const struct rt_field rt_vector_state_fields[]={
 {"vectorTint","Landroid/content/res/ColorStateList;"},{"vectorTintMode","Landroid/graphics/PorterDuff$Mode;"},F_END};

static const struct rt_method rt_vector_drawable[]={
 M("getConstantState","()Landroid/graphics/drawable/Drawable$ConstantState;",vector_constant_state),
 M("mutate","()Landroid/graphics/drawable/Drawable;",vector_mutate),
 M("<init>","()V",vector_property),M("draw","(Landroid/graphics/Canvas;)V",vector_draw),
 M("getIntrinsicWidth","()I",vector_property),M("getIntrinsicHeight","()I",vector_property),
 M("getAlpha","()I",vector_property),M("setAlpha","(I)V",vector_property),
 M("setAutoMirrored","(Z)V",vector_property),M("isAutoMirrored","()Z",vector_property),
 M("setTint","(I)V",vector_property),M("setTintList","(Landroid/content/res/ColorStateList;)V",vector_property),
 M("setTintMode","(Landroid/graphics/PorterDuff$Mode;)V",vector_property),M_END};
static const struct rt_field rt_vector_drawable_fields[]={
 {"vectorCache","Landroid/graphics/Bitmap;"},{"vectorTintCache","Landroid/graphics/Bitmap;"},
 {"vectorPaint","Landroid/graphics/Paint;"},{"vectorMatrix","Landroid/graphics/Matrix;"},
 {"vectorState","Landroid/graphics/drawable/VectorDrawable$VectorDrawableState;"},
 {"vectorMutated","I"},{"vectorDensity","I"},{"vectorTintGeneration","I"},{"vectorTintColor","I"},F_END};

/* ImageView placement follows configureBounds; endpoint rounding is Java's
 * floor(x + .5), including negative center/crop translations. */
static bool image_configure(struct dvm *vm,dvm_ref self,dvm_ref *drawable,
                              float matrix[9],bool *transformed)
{
   const float identity[9]={1,0,0,0,1,0,0,0,1};
   memcpy(matrix,identity,sizeof identity); *transformed=false;
   union dvm_value d={0},dw={0},dh={0},vw={0},vh={0},ignored={0};
   if (!image_get_drawable(vm,self,NULL,0,&d)) return false;
   *drawable=d.l;
   if (!d.l) return true;
   if (!am_vcall(vm,d.l,"getIntrinsicWidth","()I",NULL,0,&dw) ||
       !am_vcall(vm,d.l,"getIntrinsicHeight","()I",NULL,0,&dh) ||
       !view_get_width(vm,self,NULL,0,&vw) || !view_get_height(vm,self,NULL,0,&vh)) return false;
   int32_t pad[4]; ui_pad(vm,self,pad);
   int width=vw.i-pad[0]-pad[2],height=vh.i-pad[1]-pad[3];
   int type=rt_get_int(vm,self,"scaleType")-1; if (type<0) type=3;
   union dvm_value bounds[]={{.i=0},{.i=0},{.i=dw.i},{.i=dh.i}};
   if (dw.i<=0 || dh.i<=0 || type==1) {bounds[2].i=width;bounds[3].i=height;}
   else if (type==0) {
      union dvm_value raw={0};
      if (!image_matrix_storage(vm,self,NULL,0,&raw)) return false;
      const float *values=matrix_values(vm,raw.l); if (!values) return false;
      memcpy(matrix,values,9*sizeof *matrix);
   } else if (width!=dw.i || height!=dh.i) {
      float sx=(float)width/dw.i,sy=(float)height/dh.i;
      float scale=fminf(sx,sy),dx=0,dy=0;
      if (type==5) scale=1;
      else if (type==6) scale=fmaxf(sx,sy);
      else if (type==7) scale=fminf(1,scale);
      if (type>=5) {dx=floorf((width-dw.i*scale)*.5f+.5f);dy=floorf((height-dh.i*scale)*.5f+.5f);}
      else {float align=type==2 ? 0 : type==4 ? 1 : .5f;dx=(width-dw.i*scale)*align;dy=(height-dh.i*scale)*align;}
      matrix[0]=matrix[4]=scale;matrix[2]=dx;matrix[5]=dy;
   }
   if (!am_vcall(vm,d.l,"setBounds","(IIII)V",bounds,4,&ignored)) return false;
   for (int i=0;i<9;++i) if (matrix[i]!=identity[i]) *transformed=true;
   if (type!=0 && transformed[0]) {
      union dvm_value raw={0};
      if (!image_matrix_storage(vm,self,NULL,0,&raw)) return false;
      float *values=matrix_values(vm,raw.l); if (!values) return false;
      memcpy(values,matrix,9*sizeof *matrix);
   }
   return true;
}

static bool image_get_matrix(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                              int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   float matrix[9];bool transformed;dvm_ref drawable;
   if (!image_configure(vm,self,&drawable,matrix,&transformed)) return false;
   if (!transformed) {
      dvm_ref identity=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/Matrix"));
      if (!identity) return false;
      RETL(identity);
   }
   return image_matrix_storage(vm,self,NULL,0,out);
}

static bool image_on_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                            int nargs,union dvm_value *out)
{
   (void)nargs;
   float matrix[9];bool transformed;dvm_ref drawable;
   if (!image_configure(vm,self,&drawable,matrix,&transformed)) return false;
   if (!drawable) RETV();
   union dvm_value intrinsic={0};
   if (!am_vcall(vm,drawable,"getIntrinsicWidth","()I",NULL,0,&intrinsic)) return false;
   if (!intrinsic.i) RETV();
   if (!am_vcall(vm,drawable,"getIntrinsicHeight","()I",NULL,0,&intrinsic)) return false;
   if (!intrinsic.i) RETV();
   int32_t pad[4];ui_pad(vm,self,pad);
   if (!transformed && !pad[0] && !pad[1])
      return am_vcall(vm,drawable,"draw","(Landroid/graphics/Canvas;)V",args,1,out);
   union dvm_value saved={0},ignored={0};
   if (!am_vcall(vm,ARG(0).l,"save","()I",NULL,0,&saved)) return false;
   bool good=true;
   if (rt_get_int(vm,self,"cropToPadding")) {
      union dvm_value w={0},h={0};
      good=view_get_width(vm,self,NULL,0,&w) && view_get_height(vm,self,NULL,0,&h);
      int x=ui_int(vm,self,"scrollX"),y=ui_int(vm,self,"scrollY");
      union dvm_value clip[]={{.i=x+pad[0]},{.i=y+pad[1]},{.i=x+w.i-pad[2]},{.i=y+h.i-pad[3]}};
      if (good) good=am_vcall(vm,ARG(0).l,"clipRect","(IIII)Z",clip,4,&ignored);
   }
   float current[9],shift[9]={1,0,0,0,1,0,0,0,1};
   shift[2]=pad[0];shift[5]=pad[1];
   canvas_read_matrix(vm,ARG(0).l,current);mat_mul(current,current,shift);mat_mul(current,current,matrix);
   if (good) good=canvas_write_matrix(vm,ARG(0).l,current);
   if (good) good=am_vcall(vm,drawable,"draw","(Landroid/graphics/Canvas;)V",args,1,out);
   dvm_ref raised=vm->exception;
   if (raised) {dvm_pin(vm,raised);dvm_clear_exception(vm);}
   bool restored=am_vcall(vm,ARG(0).l,"restoreToCount","(I)V",&saved,1,&ignored);
   if (raised) {vm->exception=raised;dvm_unpin(vm,raised);}
   return good && restored;
}

/* Host widgets retain their existing rendering. A guest draw override owns
 * its subtree: call the real View pipeline and publish its resulting pixels. */
static const char *ui_capture_custom_view(struct dvm *vm,dvm_ref view)
{
   struct dvm_class *cls=dvm_object_class(vm,view);
   const char *hooks[]={"draw","onDraw","dispatchDraw"};
   bool custom=false;
   for(size_t i=0;i<sizeof hooks/sizeof *hooks;++i) {
      struct dvm_method *m=cls ? dvm_find_method(vm,cls,hooks[i],"(Landroid/graphics/Canvas;)V") : NULL;
      if (m && m->has_code) custom=true;
   }
   const struct ui_node *node=ui_node(vm,view);
   if (!custom || !node || node->w<=0 || node->h<=0 || ui_int(vm,view,"visibility")) return NULL;
   dvm_ref bitmap=ui_obj(vm,view,"drawingBitmap","Landroid/graphics/Bitmap;");
   struct rt_bitmap *bm=bitmap_of(vm,bitmap);
   if (!bm || bm->w!=node->w || bm->h!=node->h) {
      bitmap=bitmap_new(vm,node->w,node->h);
      if (!bitmap) return NULL;
      bm=bitmap_of(vm,bitmap);
      bm->is_mutable=true;
      (void)dvm_set_field(vm,view,"drawingBitmap","Landroid/graphics/Bitmap;",(union dvm_value){.l=bitmap});
   }
   dvm_ref canvas=ui_obj(vm,view,"drawingCanvas","Landroid/graphics/Canvas;");
   if (!canvas) {
      canvas=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/Canvas"));
      if (!canvas) return NULL;
      (void)dvm_set_field(vm,view,"drawingCanvas","Landroid/graphics/Canvas;",(union dvm_value){.l=canvas});
   }
   memset(bm->px,0,(size_t)bm->w*bm->h*sizeof *bm->px); ++bm->gen;
   uint64_t draw_started=ui_render_ns();
   union dvm_value target={.l=bitmap},argument={.l=canvas},ignored={0};
   if (!canvas_set_bitmap(vm,canvas,&target,1,&ignored) ||
       !am_vcall(vm,view,"draw","(Landroid/graphics/Canvas;)V",&argument,1,&ignored)) return NULL;
   uint64_t publish_started=ui_render_ns();
   if(!bm->overlay_key[0]) {
      static uint64_t sequence;
      snprintf(bm->overlay_key,sizeof bm->overlay_key,"lunaria-bitmap-%llu",(unsigned long long)++sequence);
   }
   if(bm->overlay_gen!=bm->gen) {
      if(!luna_overlay_image_argb(bm->overlay_key,bm->w,bm->h,bm->px)) {
         dvm__throw(vm,"java/lang/OutOfMemoryError","Bitmap upload");return NULL;
      }
      bm->overlay_gen=bm->gen;
   }
   if(draw_started) {
      ++g_ui_render_prof.draws;g_ui_render_prof.draw_ns+=publish_started-draw_started;
      g_ui_render_prof.publish_ns+=ui_render_ns()-publish_started;
   }
   return bm->overlay_key;
}

/* createBitmap(src, x, y, w, h, matrix, filter): the w x h region, mapped
 * through the matrix into the bounding box of its image (Bitmap.createBitmap
 * does exactly this), sampled back through the inverse. */
static dvm_ref bitmap_transformed(struct dvm *vm, dvm_ref srcref, int32_t x,
                                  int32_t y, int32_t w, int32_t h,
                                  const float m[9], bool filter)
{
   float xs[4], ys[4];
   mat_map(m, 0, 0, &xs[0], &ys[0]);
   mat_map(m, (float)w, 0, &xs[1], &ys[1]);
   mat_map(m, 0, (float)h, &xs[2], &ys[2]);
   mat_map(m, (float)w, (float)h, &xs[3], &ys[3]);
   float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
   for (int i = 1; i < 4; ++i) {
      if (xs[i] < minx) minx = xs[i];
      if (xs[i] > maxx) maxx = xs[i];
      if (ys[i] < miny) miny = ys[i];
      if (ys[i] > maxy) maxy = ys[i];
   }
   const int32_t dw = (int32_t)(maxx - minx + 0.5f), dh = (int32_t)(maxy - miny + 0.5f);
   float inv[9];
   if (dw <= 0 || dh <= 0 || !mat_invert(m, inv)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "width and height must be > 0");
      return 0;
   }
   dvm_ref r = bitmap_new(vm, dw, dh);
   struct rt_bitmap *dst = bitmap_of(vm, r);
   struct rt_bitmap *src = bitmap_of(vm, srcref);
   if (!dst || !src) return r;
   for (int32_t dy = 0; dy < dh; ++dy)
      for (int32_t dx = 0; dx < dw; ++dx) {
         float sx, sy;
         mat_map(inv, (float)dx + 0.5f + minx, (float)dy + 0.5f + miny, &sx, &sy);
         sx -= 0.5f; sy -= 0.5f;
         uint32_t pix = 0;
         if (sx > -1.0f && sy > -1.0f && sx < (float)w && sy < (float)h) {
            int32_t x0 = (int32_t)floorf(sx), y0 = (int32_t)floorf(sy);
            if (!filter) {
               x0 = (int32_t)(sx + 0.5f); y0 = (int32_t)(sy + 0.5f);
               if (x0 < 0) x0 = 0;
               if (y0 < 0) y0 = 0;
               if (x0 >= w) x0 = w - 1;
               if (y0 >= h) y0 = h - 1;
               pix = src->px[(size_t)(y + y0) * src->w + (size_t)(x + x0)];
            } else {
               const float ax = sx - (float)x0, ay = sy - (float)y0;
               int32_t x1 = x0 + 1, y1 = y0 + 1;
               if (x0 < 0) x0 = 0;
               if (y0 < 0) y0 = 0;
               if (x1 >= w) x1 = w - 1;
               if (y1 >= h) y1 = h - 1;
               const uint32_t *row0 = &src->px[(size_t)(y + y0) * src->w + x];
               const uint32_t *row1 = &src->px[(size_t)(y + y1) * src->w + x];
               for (int ch = 0; ch < 32; ch += 8) {
                  const float v =
                     ((float)((row0[x0] >> ch) & 0xff) * (1 - ax) +
                      (float)((row0[x1] >> ch) & 0xff) * ax) * (1 - ay) +
                     ((float)((row1[x0] >> ch) & 0xff) * (1 - ax) +
                      (float)((row1[x1] >> ch) & 0xff) * ax) * ay;
                  pix |= (uint32_t)(v + 0.5f) << ch;
               }
            }
         }
         dst->px[(size_t)dy * dw + dx] = pix;
      }
   dst->density = src->density;
   return r;
}

/* createBitmap(w, h, config) / (source) / (source, x, y, w, h[, matrix,
 * filter]) / (int[] colors, w, h, config); createScaledBitmap. */
static bool bm_create(struct dvm *vm, dvm_ref self,
                      const union dvm_value *args, int nargs,
                      union dvm_value *out)
{
   (void)self;
   const char *sig = builtin_sig();
   const char *name = builtin_name();
   if (!strcmp(name, "createScaledBitmap")) {
      struct rt_bitmap *src = bitmap_of(vm, ARG(0).l);
      if (!bitmap_live(vm, src)) return false;
      if (ARG(1).i == src->w && ARG(2).i == src->h) RETL(ARG(0).l);
      RETL(bitmap_scaled(vm, src, ARG(1).i, ARG(2).i, ARG(3).i != 0));
   }
   bool metrics=sig && !strncmp(sig,"(Landroid/util/DisplayMetrics;",30);
   if (metrics || (sig && !strncmp(sig, "(II", 3))) {
      int first=metrics ? 1 : 0;
      int32_t width=args[first].i,height=args[first+1].i;
      dvm_ref config=args[first+2].l;
      if(width<=0 || height<=0) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","width and height must be > 0");return false;
      }
      union dvm_value ordinal={0};
      if(config) dvm_get_field(vm,config,"ordinal","I",&ordinal);
      if(config && ordinal.i==5) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","Cannot create mutable HARDWARE bitmap");return false;
      }
      bool explicit_space=strstr(sig,"Landroid/graphics/ColorSpace;")!=NULL;
      dvm_ref space=explicit_space ? args[first+4].l : color_space_named(vm,0);
      if(!space && (!config || ordinal.i!=0)) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","Cannot create bitmap without a color space");return false;
      }
      if(!config) {dvm__throw(vm,"java/lang/NullPointerException","config");return false;}
      if(space && (!dvm__class_assignable(vm,dvm_object_class(vm,space),dvm_find_class(vm,"android/graphics/ColorSpace$Rgb")))) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","Color space must be RGB");return false;
      }
      if(ordinal.i<0 || ordinal.i>4) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","Unknown Bitmap config");return false;
      }
      bool alpha=nargs>first+3 ? args[first+3].i!=0 : true;
      dvm_ref b = bitmap_new(vm,width,height);
      struct rt_bitmap *bm = bitmap_of(vm, b);
      if (!bm) return false;
      bm->is_mutable = true;
      bm->config_index=ordinal.i==2 ? 3 : ordinal.i; /* ARGB_4444 is upgraded by Android. */
      bm->has_alpha=ordinal.i==1 ? false : alpha;
      bm->color_space=ordinal.i==0 ? 0 : ordinal.i==4 && space && color_space_id(vm,space)==0 ? color_space_named(vm,2) :
         ordinal.i==4 && space && color_space_id(vm,space)==1 ? color_space_named(vm,3) : space;
      if(metrics && args[0].l) bm->density=rt_get_int(vm,args[0].l,"densityDpi");
      if(!bm->has_alpha) for(size_t i=0;i<(size_t)width*height;++i) bm->px[i]=0xff000000u;
      RETL(b);
   }
   if (sig && !strncmp(sig, "([I", 3)) {           /* (colors[, off, stride], w, h, cfg) */
      struct dvm_object *a = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
      const bool strided = nargs >= 6;
      const int32_t off = strided ? ARG(1).i : 0;
      const int32_t stride = strided ? ARG(2).i : ARG(1).i;
      const int32_t w = strided ? ARG(3).i : ARG(1).i;
      const int32_t h = strided ? ARG(4).i : ARG(2).i;
      if (!a) { dvm__throw(vm, "java/lang/NullPointerException", "colors"); return false; }
      dvm_ref b = bitmap_new(vm, w, h);
      struct rt_bitmap *bm = bitmap_of(vm, b);
      if (!bm) return false;
      a = dvm__obj(vm, ARG(0).l);
      for (int32_t r = 0; r < h; ++r)
         for (int32_t c = 0; c < w; ++c) {
            const int64_t i = (int64_t)off + (int64_t)r * stride + c;
            if (i < 0 || i >= (int64_t)a->length) {
               dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "colors");
               return false;
            }
            bm->px[(size_t)r * w + c] = ((uint32_t *)a->data)[i];
         }
      RETL(b);
   }
   struct rt_bitmap *src = bitmap_of(vm, ARG(0).l);
   if (!bitmap_live(vm, src)) return false;
   int32_t x = 0, y = 0, w = src->w, h = src->h;
   if (nargs >= 5) { x = ARG(1).i; y = ARG(2).i; w = ARG(3).i; h = ARG(4).i; }
   if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > src->w || y + h > src->h) {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "x + width must be <= bitmap.width()");
      return false;
   }
   if (nargs >= 6 && ARG(5).l) {
      const float *mv = matrix_values(vm, ARG(5).l);
      static const float id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
      if (mv && memcmp(mv, id, sizeof id)) {
         float mcopy[9];
         memcpy(mcopy, mv, sizeof mcopy);
         dvm_pin(vm, ARG(0).l);
         dvm_ref r = bitmap_transformed(vm, ARG(0).l, x, y, w, h, mcopy,
                                        nargs >= 7 && ARG(6).i);
         dvm_unpin(vm, ARG(0).l);
         struct rt_bitmap *rb = bitmap_of(vm, r);
         src = bitmap_of(vm, ARG(0).l);
         if (rb && src) rb->has_alpha = true;
         if (!r) return false;
         RETL(r);
      }
   }
   dvm_ref b = bitmap_new(vm, w, h);
   struct rt_bitmap *bm = bitmap_of(vm, b);
   src = bitmap_of(vm, ARG(0).l);
   if (!bm) return false;
   for (int32_t r = 0; r < h; ++r)
      memcpy(&bm->px[(size_t)r * w], &src->px[(size_t)(y + r) * src->w + x],
             (size_t)w * 4);
   bm->density = src->density;
   bm->has_alpha = src->has_alpha;
   bm->config_index=src->config_index;bm->color_space=src->color_space;
   RETL(b);
}

static bool bm_copy(struct dvm *vm, dvm_ref self,
                    const union dvm_value *args, int nargs,
                    union dvm_value *out)
{
   (void)nargs;
   struct rt_bitmap *src = bitmap_of(vm, self);
   if (!bitmap_live(vm, src)) return false;
   dvm_ref b = bitmap_new(vm, src->w, src->h);
   struct rt_bitmap *bm = bitmap_of(vm, b);
   src = bitmap_of(vm, self);
   if (!bm) return false;
   memcpy(bm->px, src->px, (size_t)src->w * src->h * 4);
   bm->density = src->density;
   bm->has_alpha = src->has_alpha;
   bm->config_index=src->config_index;bm->color_space=src->color_space;
   bm->is_mutable = ARG(1).i != 0;
   RETL(b);
}

int stbi_write_jpg_to_func(void (*function)(void *, void *, int), void *context,
                           int width, int height, int components, const void *data, int quality);
struct bitmap_encoded { unsigned char *data; size_t size, capacity; bool failed; };
static void bitmap_encode_write(void *context, void *data, int length)
{
   struct bitmap_encoded *buffer = context;
   if (buffer->failed || length <= 0) return;
   if ((size_t)length > (size_t)INT32_MAX - buffer->size) { buffer->failed = true; return; }
   size_t need = buffer->size + (size_t)length;
   if (need > buffer->capacity) {
      size_t capacity = buffer->capacity ? buffer->capacity : 4096;
      while (capacity < need) capacity = capacity > (size_t)INT32_MAX/2 ? need : capacity*2;
      void *grown = realloc(buffer->data, capacity);
      if (!grown) { buffer->failed = true; return; }
      buffer->data = grown; buffer->capacity = capacity;
   }
   memcpy(buffer->data + buffer->size, data, (size_t)length);
   buffer->size = need;
}

/* Bitmap.compress writes the chosen image format to the supplied stream. */
static bool bm_compress(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)nargs;
   struct rt_bitmap *bm = bitmap_of(vm, self);
   if (!bitmap_live(vm, bm)) return false;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "format"); return false; }
   if (ARG(1).i < 0 || ARG(1).i > 100) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "quality must be 0..100"); return false;
   }
   union dvm_value fname = { 0 };
   if (ARG(0).l) (void)dvm_get_field(vm, ARG(0).l, "name", "Ljava/lang/String;", &fname);
   const char *fmt = fname.l ? dvm_string_utf8(vm, fname.l) : "PNG";
   if (!ARG(2).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "stream");
      return false;
   }
   if (strcmp(fmt, "PNG") && strcmp(fmt, "JPEG")) {
      fprintf(stderr, "[bitmap] compress(%s): no encoder for this format\n", fmt);
      RETI(0);
   }
   uint8_t *rgba = malloc((size_t)bm->w * bm->h * 4);
   if (!rgba) RETI(0);
   for (size_t i = 0; i < (size_t)bm->w * bm->h; ++i) {
      const uint32_t p = bm->px[i];
      rgba[i * 4 + 0] = (uint8_t)(p >> 16); rgba[i * 4 + 1] = (uint8_t)(p >> 8);
      rgba[i * 4 + 2] = (uint8_t)p; rgba[i * 4 + 3] = (uint8_t)(p >> 24);
   }
   int len = 0;
   uint8_t *png;
   if (!strcmp(fmt, "JPEG")) {
      struct bitmap_encoded encoded = {0};
      int encoded_ok = stbi_write_jpg_to_func(bitmap_encode_write, &encoded,
         bm->w, bm->h, 4, rgba, ARG(1).i ? ARG(1).i : 1);
      if (!encoded_ok || encoded.failed) { free(encoded.data); free(rgba); RETI(0); }
      png = encoded.data; len = (int)encoded.size;
   } else png = stbi_write_png_to_mem(rgba, bm->w * 4, bm->w, bm->h, 4, &len);
   free(rgba);
   if (!png || len <= 0) { free(png); RETI(0); }
   dvm_ref arr = dvm_new_array(vm, 'B', "B", (uint32_t)len);
   if (arr) memcpy(dvm_array_data(vm, arr), png, (size_t)len);
   free(png);
   struct dvm_class *sc = dvm_object_class(vm, ARG(2).l);
   struct dvm_method *wr = sc ? dvm_find_method(vm, sc, "write", "([B)V") : NULL;
   if (!arr || !wr) RETI(0);
   union dvm_value a[1] = { { .l = arr } }, ign;
   if (!dvm_call(vm, wr, ARG(2).l, a, 1, &ign)) return false;
   RETI(1);
}

static const struct rt_method rt_bitmap[] = {
   M("getWidth", "()I", bm_getWidth),
   M("getHeight", "()I", bm_getHeight),
   M("getByteCount", "()I", bm_getByteCount),
   M("getAllocationByteCount", "()I", bm_getByteCount),
   M("getRowBytes", "()I", bm_getByteCount),
   M("getPixel", "(II)I", bm_getPixel),
   M("setPixel", "(III)V", bm_setPixel),
   M("getPixels", "([IIIIIII)V", bm_pixels),
   M("setPixels", "([IIIIIII)V", bm_pixels),
   M("eraseColor", "(I)V", bm_eraseColor),
   M("recycle", "()V", bm_recycle),
   M("isRecycled", "()Z", bm_flag),
   M("isMutable", "()Z", bm_flag),
   M("hasAlpha", "()Z", bm_flag),
   M("getDensity", "()I", bm_flag),
   M("setDensity", "(I)V", bm_setDensity),
   M("setHasAlpha", "(Z)V", bm_setHasAlpha),
   M("getConfig", "()Landroid/graphics/Bitmap$Config;", bm_getConfig),
   M("getColorSpace", "()Landroid/graphics/ColorSpace;", bm_getColorSpace),
   M("copy", "(Landroid/graphics/Bitmap$Config;Z)Landroid/graphics/Bitmap;", bm_copy),
   M("compress",
     "(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z",
     bm_compress),
   SM("createBitmap", "(IILandroid/graphics/Bitmap$Config;Z)Landroid/graphics/Bitmap;", bm_create),
   SM("createBitmap", "(IILandroid/graphics/Bitmap$Config;ZLandroid/graphics/ColorSpace;)Landroid/graphics/Bitmap;", bm_create),
   SM("createBitmap", "(Landroid/util/DisplayMetrics;IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;", bm_create),
   SM("createBitmap", "(Landroid/util/DisplayMetrics;IILandroid/graphics/Bitmap$Config;Z)Landroid/graphics/Bitmap;", bm_create),
   SM("createBitmap", "(Landroid/util/DisplayMetrics;IILandroid/graphics/Bitmap$Config;ZLandroid/graphics/ColorSpace;)Landroid/graphics/Bitmap;", bm_create),
   SM("createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;",
      bm_create),
   SM("createBitmap", "(Landroid/graphics/Bitmap;)Landroid/graphics/Bitmap;",
      bm_create),
   SM("createBitmap", "(Landroid/graphics/Bitmap;IIII)Landroid/graphics/Bitmap;",
      bm_create),
   SM("createBitmap",
      "(Landroid/graphics/Bitmap;IIIILandroid/graphics/Matrix;Z)Landroid/graphics/Bitmap;",
      bm_create),
   SM("createBitmap", "([IIILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;",
      bm_create),
   SM("createBitmap",
      "([IIIIILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;",
      bm_create),
   SM("createScaledBitmap", "(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;",
      bm_create),
   M_END,
};

static const struct rt_field rt_path_fill_sfields[]={
 {"WINDING","Landroid/graphics/Path$FillType;"},{"EVEN_ODD","Landroid/graphics/Path$FillType;"},
 {"INVERSE_WINDING","Landroid/graphics/Path$FillType;"},{"INVERSE_EVEN_ODD","Landroid/graphics/Path$FillType;"},F_END};
static const struct rt_field rt_region_op_fields[]={{"nativeInt","I"},F_END};
static const struct rt_field rt_region_op_sfields[]={
 {"DIFFERENCE","Landroid/graphics/Region$Op;"},{"INTERSECT","Landroid/graphics/Region$Op;"},
 {"UNION","Landroid/graphics/Region$Op;"},{"XOR","Landroid/graphics/Region$Op;"},
 {"REVERSE_DIFFERENCE","Landroid/graphics/Region$Op;"},{"REPLACE","Landroid/graphics/Region$Op;"},F_END};
static const struct rt_field rt_path_direction_sfields[]={
 {"CW","Landroid/graphics/Path$Direction;"},{"CCW","Landroid/graphics/Path$Direction;"},F_END};
static const struct rt_field rt_scale_type_sfields[] = {
   { "MATRIX", "Landroid/widget/ImageView$ScaleType;" },
   { "FIT_XY", "Landroid/widget/ImageView$ScaleType;" },
   { "FIT_START", "Landroid/widget/ImageView$ScaleType;" },
   { "FIT_CENTER", "Landroid/widget/ImageView$ScaleType;" },
   { "FIT_END", "Landroid/widget/ImageView$ScaleType;" },
   { "CENTER", "Landroid/widget/ImageView$ScaleType;" },
   { "CENTER_CROP", "Landroid/widget/ImageView$ScaleType;" },
   { "CENTER_INSIDE", "Landroid/widget/ImageView$ScaleType;" },
   F_END
};

/* KeyCharacterMap.deviceHasKey(keyCode): the handset's physical keys are
 * power and the volume rocker (KEYCODE_VOLUME_UP 24, _DOWN 25, POWER 26);
 * back/home/menu are on-screen gestures, not keys. */
static bool kcm_deviceHasKey(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   const int32_t k = ARG(0).i;
   RETI(k == 24 || k == 25 || k == 26);
}

static bool kcm_deviceHasKeys(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_object *a = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   const uint32_t n = a ? a->length : 0;
   dvm_ref r = dvm_new_array(vm, 'Z', "Z", n);
   a = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   uint8_t *d = r ? dvm_array_data(vm, r) : NULL;
   for (uint32_t i = 0; d && a && i < n; ++i) {
      const int32_t k = ((int32_t *)a->data)[i];
      d[i] = k == 24 || k == 25 || k == 26;
   }
   RETL(r);
}

static const struct rt_method rt_key_character_map[] = {
   SM("deviceHasKey", "(I)Z", kcm_deviceHasKey),
   SM("deviceHasKeys", "([I)[Z", kcm_deviceHasKeys),
   M_END,
};

static const struct rt_field rt_bitmap_config_sfields[] = {
   { "ALPHA_8", "Landroid/graphics/Bitmap$Config;" },
   { "RGB_565", "Landroid/graphics/Bitmap$Config;" },
   { "ARGB_4444", "Landroid/graphics/Bitmap$Config;" },
   { "ARGB_8888", "Landroid/graphics/Bitmap$Config;" },
   { "RGBA_F16", "Landroid/graphics/Bitmap$Config;" },
   { "HARDWARE", "Landroid/graphics/Bitmap$Config;" },
   F_END
};

static const struct rt_field rt_bitmap_format_sfields[] = {
   { "JPEG", "Landroid/graphics/Bitmap$CompressFormat;" },
   { "PNG", "Landroid/graphics/Bitmap$CompressFormat;" },
   { "WEBP", "Landroid/graphics/Bitmap$CompressFormat;" },
   { "WEBP_LOSSY", "Landroid/graphics/Bitmap$CompressFormat;" },
   { "WEBP_LOSSLESS", "Landroid/graphics/Bitmap$CompressFormat;" },
   F_END
};

/* --- BitmapDrawable ------------------------------------------------------ */

static bool bd_get_paint(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static int32_t bd_scaled_dimension(int32_t size,int32_t source,int32_t target)
{
   if (!source || !target || source==target) return size;
   uint32_t bits=(uint32_t)size*(uint32_t)target+(uint32_t)(source/2-(source<0 && source%2));
   int32_t rounded;
   memcpy(&rounded,&bits,sizeof rounded);
   return rounded==INT32_MIN && source==-1 ? INT32_MIN : rounded/source;
}
static void bd_compute_size(struct dvm *vm,dvm_ref self)
{
   struct rt_bitmap *bm=bitmap_of(vm,ui_obj(vm,self,"bitmap","Landroid/graphics/Bitmap;"));
   int32_t target=ui_int(vm,self,"targetDensity");
   union dvm_value width={.i=bm ? bd_scaled_dimension(bm->w,bm->density,target) : -1};
   union dvm_value height={.i=bm ? bd_scaled_dimension(bm->h,bm->density,target) : -1};
   (void)dvm_set_field(vm,self,"bitmapWidth","I",width);
   (void)dvm_set_field(vm,self,"bitmapHeight","I",height);
}
static bool bd_init(struct dvm *vm, dvm_ref self,
                    const union dvm_value *args, int nargs,
                    union dvm_value *out)
{
   union dvm_value ignored = {0};
   if (!drawable_init(vm, self, NULL, 0, &ignored)) return false;
   bool resource_ctor=strstr(builtin_sig(),"Landroid/content/res/Resources;")!=NULL;
   union dvm_value b={.l=nargs>=2 ? ARG(1).l : nargs && !resource_ctor ? ARG(0).l : 0};
   (void)dvm_set_field(vm,self,"bitmap","Landroid/graphics/Bitmap;",b);
   (void)dvm_set_field(vm,self,"gravity","I",(union dvm_value){.i=0x77});
   int32_t density=160;
   if (resource_ctor && ARG(0).l) {
      union dvm_value metrics={0};
      if (!am_vcall(vm,ARG(0).l,"getDisplayMetrics","()Landroid/util/DisplayMetrics;",NULL,0,&metrics)) return false;
      density=ui_int(vm,metrics.l,"densityDpi");
      if (!density) density=160;
   }
   (void)dvm_set_field(vm,self,"targetDensity","I",(union dvm_value){.i=density});
   (void)dvm_set_field(vm,self,"tileModeX","I",(union dvm_value){.i=-1});
   (void)dvm_set_field(vm,self,"tileModeY","I",(union dvm_value){.i=-1});
   bd_compute_size(vm,self);
   union dvm_value paint={0};
   if (!bd_get_paint(vm,self,NULL,0,&paint)) return false;
   RETV();
}

static bool bd_getBitmap(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value b = { 0 };
   (void)dvm_get_field(vm, self, "bitmap", "Landroid/graphics/Bitmap;", &b);
   RETL(b.l);
}

/* The intrinsic size is the bitmap's, scaled from its density to the
 * display's, as BitmapDrawable computes it. */
static bool bd_intrinsic(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(ui_int(vm,self,strstr(builtin_name(),"Width") ? "bitmapWidth" : "bitmapHeight"));
}

/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* Private C BitmapDrawable placement and software Canvas bridge. */
static bool bd_get_paint(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                         int nargs,union dvm_value *out)
{
   (void)args; (void)nargs;
   dvm_ref paint=ui_obj(vm,self,"paint","Landroid/graphics/Paint;");
   if (!paint) {
      paint=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/Paint"));
      if (!paint) return false;
      (void)dvm_set_field(vm,self,"paint","Landroid/graphics/Paint;",(union dvm_value){.l=paint});
      union dvm_value flags={.i=6},ignored={0};
      if (!paint_init(vm,paint,&flags,1,&ignored)) return false;
   }
   RETL(paint);
}
static bool bd_color_filter(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value paint={0},filter={0};if(!bd_get_paint(vm,self,NULL,0,&paint))return false;
   if(builtin_name()[0]=='s'){
      filter=nargs ? ARG(0) : filter;
      dvm_set_field(vm,paint.l,"colorFilter","Landroid/graphics/ColorFilter;",filter);
      dvm_set_field(vm,self,"colorFilter","Landroid/graphics/ColorFilter;",filter);
      ui_invalidate();RETV();
   }
   dvm_get_field(vm,paint.l,"colorFilter","Landroid/graphics/ColorFilter;",&filter);RETL(filter.l);
}
static bool bd_property(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                        int nargs,union dvm_value *out)
{
   (void)nargs;
   const char *name=builtin_name();
   if (!strcmp(name,"setBitmap")) {
      if (ui_obj(vm,self,"bitmap","Landroid/graphics/Bitmap;")!=ARG(0).l) {
         (void)dvm_set_field(vm,self,"bitmap","Landroid/graphics/Bitmap;",ARG(0));
         bd_compute_size(vm,self); ui_invalidate();
      }
      RETV();
   }
   if (!strcmp(name,"setTargetDensity")) {
      int32_t density=ARG(0).i;
      if (strstr(builtin_sig(),"DisplayMetrics")) {
         if (!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","metrics"); return false;}
         density=ui_int(vm,ARG(0).l,"densityDpi");
      } else if (strstr(builtin_sig(),"Canvas")) {
         union dvm_value result={0};
         if (!am_vcall(vm,ARG(0).l,"getDensity","()I",NULL,0,&result)) return false;
         density=result.i;
      }
      if (!density) density=160;
      if (density!=ui_int(vm,self,"targetDensity")) {
         (void)dvm_set_field(vm,self,"targetDensity","I",(union dvm_value){.i=density});
         bd_compute_size(vm,self); ui_invalidate();
      }
      RETV();
   }
   if (strstr(name,"Gravity") || strstr(name,"Mirrored")) {
      bool gravity=strstr(name,"Gravity")!=NULL;
      const char *field=gravity ? "gravity" : "autoMirrored", *type=gravity ? "I" : "Z";
      union dvm_value old={0};
      (void)dvm_get_field(vm,self,field,type,&old);
      if (name[0]!='s') RETI(old.i);
      if (old.i!=ARG(0).i) { (void)dvm_set_field(vm,self,field,type,ARG(0)); ui_invalidate(); }
      RETV();
   }
   union dvm_value paint={0},value={0};
   if (!bd_get_paint(vm,self,NULL,0,&paint)) return false;
   if (strstr(name,"Alpha")) {
      (void)dvm_get_field(vm,paint.l,"color","I",&value);
      if (name[0]!='s') RETI(value.u>>24);
      value.u=(value.u&0x00ffffffu) | ((ARG(0).u&255u)<<24);
      (void)dvm_set_field(vm,paint.l,"color","I",value);
   } else {
      int bit=strstr(name,"AntiAlias") ? 1 : strstr(name,"FilterBitmap") ? 2 : 4;
      (void)dvm_get_field(vm,paint.l,"flags","I",&value);
      if (name[0]!='s') RETI((value.i&bit)!=0);
      value.i=ARG(0).i ? value.i|bit : value.i&~bit;
      (void)dvm_set_field(vm,paint.l,"flags","I",value);
   }
   ui_invalidate(); RETV();
}
static bool bd_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                    int nargs,union dvm_value *out)
{
   (void)nargs;
   dvm_ref bitmap=ui_obj(vm,self,"bitmap","Landroid/graphics/Bitmap;");
   if (!bitmap) RETV();
   if (!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","canvas"); return false;}
   struct rt_bitmap *bm=bitmap_of(vm,bitmap);
   if (!bitmap_live(vm,bm)) return false;
   if (ui_int(vm,self,"tileModeX")!=-1 || ui_int(vm,self,"tileModeY")!=-1) {
      dvm__throw(vm,"java/lang/UnsupportedOperationException","BitmapDrawable tiles are not implemented");
      return false;
   }
   union dvm_value bounds={0},paint={0},iw={0},ih={0},ignored={0};
   if (!drawable_get_bounds(vm,self,NULL,0,&bounds) || !bd_get_paint(vm,self,NULL,0,&paint)) return false;
   iw.i=ui_int(vm,self,"bitmapWidth"); ih.i=ui_int(vm,self,"bitmapHeight");
   dvm_ref rect=ui_obj(vm,self,"drawRect","Landroid/graphics/Rect;");
   if (!rect) {
      rect=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/Rect"));
      if (!rect) return false;
      (void)dvm_set_field(vm,self,"drawRect","Landroid/graphics/Rect;",(union dvm_value){.l=rect});
   }
   union dvm_value place[]={{.i=ui_int(vm,self,"gravity")},iw,ih,bounds,{.l=rect},
      {.i=ui_int(vm,self,"layoutDirection")}};
   if (!gravity_apply(vm,0,place,6,&ignored)) return false;
   int32_t left=ui_int(vm,rect,"left"),top=ui_int(vm,rect,"top");
   int32_t right=ui_int(vm,rect,"right"),bottom=ui_int(vm,rect,"bottom");
   if (right<=left || bottom<=top || bm->w<=0 || bm->h<=0) RETV();
   dvm_ref matrix=ui_obj(vm,self,"drawMatrix","Landroid/graphics/Matrix;");
   if (!matrix) {
      matrix=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/Matrix"));
      if (!matrix) return false;
      (void)dvm_set_field(vm,self,"drawMatrix","Landroid/graphics/Matrix;",(union dvm_value){.l=matrix});
   }
   float *values=matrix_values(vm,matrix);
   if (!values) return false;
   bool mirror=ui_int(vm,self,"autoMirrored") && ui_int(vm,self,"layoutDirection")==1;
   float width=(float)((int64_t)right-left),height=(float)((int64_t)bottom-top);
   memset(values,0,9*sizeof *values);
   values[0]=(mirror ? -width : width)/bm->w; values[4]=height/bm->h;
   values[2]=(float)(mirror ? right : left); values[5]=(float)top; values[8]=1;
   union dvm_value draw[]={{.l=bitmap},{.l=matrix},paint};
   return canvas_draw_bitmap(vm,ARG(0).l,draw,3,out);
}
static const struct rt_field rt_bitmap_drawable_fields[] = {
   {"bitmap","Landroid/graphics/Bitmap;"}, {"paint","Landroid/graphics/Paint;"},
   {"drawMatrix","Landroid/graphics/Matrix;"}, {"drawRect","Landroid/graphics/Rect;"},
   {"targetDensity","I"},{"bitmapWidth","I"},{"bitmapHeight","I"},
   {"gravity","I"},{"autoMirrored","Z"},
   {"tileModeX","I"},{"tileModeY","I"},F_END
};


/* ---- android.graphics.NinePatch / NinePatchDrawable --------------------
 *
 * A bitmap with a chunk that says which columns and rows stretch.  The chunk
 * is the serialised Res_png_9patch an app (or aapt) hands to the constructor:
 * wasDeserialized, numXDivs, numYDivs, numColors as bytes, two skipped
 * words, the four paddings, a skipped word — 32 bytes — then the x divs,
 * the y divs and the colours, all native-endian int32.  Divs come in pairs,
 * [start, end) of a stretchable run. */
static dvm_ref np_obj(struct dvm *vm, dvm_ref o, const char *name,
                      const char *type)
{
   union dvm_value v = { 0 };
   if (o) (void)dvm_get_field(vm, o, name, type, &v);
   return v.l;
}

struct ninepatch_chunk {
   int32_t nx, ny;
   const int32_t *x, *y;
   int32_t pad[4];                  /* left, right, top, bottom */
};

static bool ninepatch_parse(struct dvm *vm, dvm_ref chunk,
                            struct ninepatch_chunk *c)
{
   struct dvm_object *a = chunk ? dvm__obj(vm, chunk) : NULL;
   if (!a || a->length < 32) return false;
   const uint8_t *b = a->data;
   c->nx = b[1];
   c->ny = b[2];
   if (32u + 4u * (uint32_t)(c->nx + c->ny) > a->length) return false;
   memcpy(c->pad, b + 12, sizeof c->pad);
   c->x = (const int32_t *)(b + 32);
   c->y = c->x + c->nx;
   return true;
}

static bool np_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)out;
   union dvm_value v = { .l = nargs > 0 ? ARG(0).l : 0 };
   (void)dvm_set_field(vm, self, "bitmap", "Landroid/graphics/Bitmap;", v);
   v.l = nargs > 1 ? ARG(1).l : 0;
   (void)dvm_set_field(vm, self, "chunk", "[B", v);
   v.l = nargs > 2 ? ARG(2).l : 0;
   (void)dvm_set_field(vm, self, "name", "Ljava/lang/String;", v);
   RETV();
}

static bool np_get_bitmap(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(np_obj(vm, self, "bitmap", "Landroid/graphics/Bitmap;"));
}

static bool np_get_width(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_bitmap *bm = bitmap_of(vm, np_obj(vm, self, "bitmap",
                                                   "Landroid/graphics/Bitmap;"));
   RETI(bm ? bm->w : 0);
}

static bool np_get_height(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_bitmap *bm = bitmap_of(vm, np_obj(vm, self, "bitmap",
                                                   "Landroid/graphics/Bitmap;"));
   RETI(bm ? bm->h : 0);
}

static bool np_is_chunk(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   struct ninepatch_chunk c;
   RETI(ninepatch_parse(vm, ARG(0).l, &c) ? 1 : 0);
}

static const struct rt_field rt_ninepatch_fields[] = {
   { "bitmap", "Landroid/graphics/Bitmap;" }, { "chunk", "[B" },
   { "name", "Ljava/lang/String;" }, F_END
};

static const struct rt_method rt_ninepatch[] = {
   M("<init>", "(Landroid/graphics/Bitmap;[B)V", np_init),
   M("<init>", "(Landroid/graphics/Bitmap;[BLjava/lang/String;)V", np_init),
   M("getBitmap", "()Landroid/graphics/Bitmap;", np_get_bitmap),
   M("getWidth", "()I", np_get_width),
   M("getHeight", "()I", np_get_height),
   SM("isNinePatchChunk", "([B)Z", np_is_chunk),
   M_END,
};

static dvm_ref npd_patch(struct dvm *vm, dvm_ref self)
{
   return np_obj(vm, self, "ninePatch", "Landroid/graphics/NinePatch;");
}

/* NinePatchDrawable(Resources, NinePatch) / (NinePatch), and the form that
 * builds its NinePatch from a bitmap and a chunk. */
static bool npd_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)out;
   union dvm_value np = { 0 };
   if (nargs >= 3) {
      struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/graphics/NinePatch;");
      np.l = c ? dvm_new_object(vm, c) : 0;
      if (np.l) {
         union dvm_value a[3] = { ARG(1), ARG(2), { .l = nargs > 4 ? ARG(4).l : 0 } };
         union dvm_value ignored = { 0 };
         (void)np_init(vm, np.l, a, 3, &ignored);
      }
   } else {
      np.l = nargs > 1 ? ARG(1).l : nargs > 0 ? ARG(0).l : 0;
      /* (Resources, NinePatch) and (NinePatch): take whichever is one. */
      struct dvm_class *want = dvm__class_by_desc(vm, "Landroid/graphics/NinePatch;");
      struct dvm_class *got = np.l ? dvm_object_class(vm, np.l) : NULL;
      if (nargs > 1 && !(got && want && dvm__class_assignable(vm, got, want)))
         np.l = 0;
   }
   (void)dvm_set_field(vm, self, "ninePatch", "Landroid/graphics/NinePatch;", np);
   RETV();
}

static bool npd_intrinsic_w(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   return np_get_width(vm, npd_patch(vm, self), args, nargs, out);
}

static bool npd_intrinsic_h(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   return np_get_height(vm, npd_patch(vm, self), args, nargs, out);
}

/* getPadding(Rect): the chunk's content insets; true when there are any. */
static bool npd_get_padding(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)nargs;
   struct ninepatch_chunk c;
   dvm_ref np = npd_patch(vm, self);
   const bool have = np && ninepatch_parse(vm, np_obj(vm, np, "chunk", "[B"), &c);
   dvm_ref r = ARG(0).l;
   if (r) {
      rt_set_int(vm, r, "left", have ? c.pad[0] : 0);
      rt_set_int(vm, r, "right", have ? c.pad[1] : 0);
      rt_set_int(vm, r, "top", have ? c.pad[2] : 0);
      rt_set_int(vm, r, "bottom", have ? c.pad[3] : 0);
   }
   RETI(have && (c.pad[0] | c.pad[1] | c.pad[2] | c.pad[3]) ? 1 : 0);
}

static const struct rt_field rt_ninepatch_drawable_fields[] = {
   { "ninePatch", "Landroid/graphics/NinePatch;" }, F_END
};

static const struct rt_method rt_ninepatch_drawable[] = {
   M("<init>", "(Landroid/graphics/NinePatch;)V", npd_init),
   M("<init>", "(Landroid/content/res/Resources;Landroid/graphics/NinePatch;)V",
     npd_init),
   M("<init>", "(Landroid/content/res/Resources;Landroid/graphics/Bitmap;[B"
     "Landroid/graphics/Rect;Ljava/lang/String;)V", npd_init),
   M("getIntrinsicWidth", "()I", npd_intrinsic_w),
   M("getIntrinsicHeight", "()I", npd_intrinsic_h),
   M("getMinimumWidth", "()I", npd_intrinsic_w),
   M("getMinimumHeight", "()I", npd_intrinsic_h),
   M("getPadding", "(Landroid/graphics/Rect;)Z", npd_get_padding),
   M_END,
};

/* The drawable as CSS border-image: the bitmap, cut where the chunk's
 * stretchable runs begin and end, with the middle filled.  A chunk with more
 * than one run per axis is cut at its outermost edges. */
static bool ninepatch_border_image_css(struct dvm *vm, dvm_ref drawable,
                                       char *out, size_t n)
{
   dvm_ref np = npd_patch(vm, drawable);
   dvm_ref bitmap = np_obj(vm, np, "bitmap", "Landroid/graphics/Bitmap;");
   struct rt_bitmap *bm = bitmap_of(vm, bitmap);
   struct ninepatch_chunk c;
   if (!bm || !ninepatch_parse(vm, np_obj(vm, np, "chunk", "[B"), &c) ||
       c.nx < 2 || c.ny < 2)
      return false;
   const char *png = bitmap_png_path(vm, bitmap);
   if (!png) return false;
   const int32_t l = c.x[0], r = bm->w - c.x[c.nx - 1];
   const int32_t t = c.y[0], b = bm->h - c.y[c.ny - 1];
   snprintf(out, n, "border-image-source:url('%s');"
            "border-image-slice:%d %d %d %d fill;"
            "border-image-width:%dpx %dpx %dpx %dpx;",
            png, t, r, b, l, t, r, b, l);
   return true;
}

static const struct rt_method rt_bitmap_drawable[] = {
   M("setColorFilter","(Landroid/graphics/ColorFilter;)V",bd_color_filter),
   M("getColorFilter","()Landroid/graphics/ColorFilter;",bd_color_filter),
   M("<init>","()V",bd_init),
   M("<init>","(Landroid/content/res/Resources;)V",bd_init),
   M("draw","(Landroid/graphics/Canvas;)V",bd_draw),
   M("getPaint","()Landroid/graphics/Paint;",bd_get_paint),
   M("setBitmap","(Landroid/graphics/Bitmap;)V",bd_property),
   M("setTargetDensity","(I)V",bd_property),
   M("setTargetDensity","(Landroid/util/DisplayMetrics;)V",bd_property),
   M("setTargetDensity","(Landroid/graphics/Canvas;)V",bd_property),
   M("setGravity","(I)V",bd_property), M("getGravity","()I",bd_property),
   M("setAutoMirrored","(Z)V",bd_property), M("isAutoMirrored","()Z",bd_property),
   M("setAlpha","(I)V",bd_property), M("getAlpha","()I",bd_property),
   M("setFilterBitmap","(Z)V",bd_property), M("isFilterBitmap","()Z",bd_property),
   M("setAntiAlias","(Z)V",bd_property), M("hasAntiAlias","()Z",bd_property),
   M("setDither","(Z)V",bd_property), M("hasDither","()Z",bd_property),
   M("<init>", "(Landroid/graphics/Bitmap;)V", bd_init),
   M("<init>", "(Landroid/content/res/Resources;Landroid/graphics/Bitmap;)V",
     bd_init),
   M("getBitmap", "()Landroid/graphics/Bitmap;", bd_getBitmap),
   M("getIntrinsicWidth", "()I", bd_intrinsic),
   M("getIntrinsicHeight", "()I", bd_intrinsic),
   M_END,
};

/* ImageView.setImageBitmap(b) is setImageDrawable(new BitmapDrawable(b)). */
static bool image_set_bitmap(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs; (void)out;
   dvm_ref d = 0;
   if (ARG(0).l) {
      struct dvm_class *c =
         dvm__class_by_desc(vm, "Landroid/graphics/drawable/BitmapDrawable;");
      d = c ? dvm_new_object(vm, c) : 0;
      if (d) {
         dvm_pin(vm,d);
         union dvm_value resources={0},ignored={0};
         bool good=am_vcall(vm,self,"getResources","()Landroid/content/res/Resources;",NULL,0,&resources);
         union dvm_value constructor[]={resources,ARG(0)};
         if (good) good=am_vcall(vm,d,"<init>",
            "(Landroid/content/res/Resources;Landroid/graphics/Bitmap;)V",constructor,2,&ignored);
         dvm_unpin(vm,d);
         if (!good) return false;
      }
   }
   union dvm_value v = { .l = d };
   return image_set_drawable(vm,self,&v,1,out);
}

/* --- GradientDrawable ---------------------------------------------------- */

/* A shape: RECTANGLE 0, OVAL 1, LINE 2, RING 3; a solid colour or a list of
 * gradient colours; corner radius; stroke.  The view layer renders these as
 * the box's background, radius and border. */
static bool gd_setColors(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);

static bool gd_init(struct dvm *vm, dvm_ref self,
                    const union dvm_value *args, int nargs,
                    union dvm_value *out)
{
   if (!drawable_init(vm,self,NULL,0,out)) return false;
   rt_set_int(vm,self,"alpha",255);
   rt_set_int(vm,self,"width",-1); rt_set_int(vm,self,"height",-1);
   rt_set_int(vm,self,"strokeWidth",-1); rt_set_int(vm,self,"color",0);
   if (nargs >= 2) {         /* (Orientation, int[] colors) */
      if (!gd_setColors(vm,self,args+1,1,out)) return false;
      (void)dvm_set_field(vm, self, "orientation",
                          "Landroid/graphics/drawable/GradientDrawable$Orientation;",
                          ARG(0));
   }
   RETV();
}

static bool gd_set_int(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)nargs; (void)out;
   const char *n = builtin_name();
   const char *field = !strcmp(n, "setShape") ? "shape"
                     : !strcmp(n, "setGradientType") ? "gradientType"
                     : !strcmp(n, "setAlpha") ? "alpha" : NULL;
   if (field) rt_set_int(vm, self, field, ARG(0).i);
   ui_invalidate();
   RETV();
}

static bool gd_setCornerRadius(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)nargs; (void)out;
   union dvm_value v = { .f = fmaxf(0,ARG(0).f) };
   (void)dvm_set_field(vm, self, "cornerRadius", "F", v);
   (void)dvm_set_field(vm,self,"cornerRadii","[F",(union dvm_value){0});
   ui_invalidate();
   RETV();
}

/* The renderer reads all four pairs; null clears corner rounding. */
static bool gd_setCornerRadii(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)nargs; (void)out;
   if (!ARG(0).l) (void)dvm_set_field(vm,self,"cornerRadius","F",(union dvm_value){0});
   (void)dvm_set_field(vm, self, "cornerRadii", "[F", ARG(0));
   ui_invalidate();
   RETV();
}

static bool gd_setStroke(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)nargs; (void)out;
   rt_set_int(vm, self, "strokeWidth", ARG(0).i);
   rt_set_int(vm, self, "strokeColor", ARG(1).i);
   (void)dvm_set_field(vm,self,"dashWidth","F",(union dvm_value){.f=nargs>=4 ? ARG(2).f : 0});
   (void)dvm_set_field(vm,self,"dashGap","F",(union dvm_value){.f=nargs>=4 ? ARG(3).f : 0});
   ui_invalidate();
   RETV();
}

static bool gd_setColors(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   dvm_ref colors=0;
   if (ARG(0).l) {
      uint32_t n=dvm_array_length(vm,ARG(0).l);
      colors=dvm_new_array(vm,'I',"I",n);
      if (!colors) return false;
      if (n) memcpy(dvm_array_data(vm,colors),dvm_array_data(vm,ARG(0).l),(size_t)n*sizeof(int32_t));
   }
   (void)dvm_set_field(vm,self,"colors","[I",(union dvm_value){.l=colors});
   (void)dvm_set_field(vm,self,"positions","[F",(union dvm_value){.l=nargs>1 ? ARG(1).l : 0});
   ui_invalidate();
   RETV();
}

static bool gd_setOrientation(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)nargs; (void)out;
   (void)dvm_set_field(vm, self, "orientation",
                       "Landroid/graphics/drawable/GradientDrawable$Orientation;",
                       ARG(0));
   RETV();
}

static bool gd_get_int(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *n = builtin_name();
   RETI(rt_get_int(vm, self, !strcmp(n, "getShape") ? "shape"
                           : !strcmp(n, "getGradientType") ? "gradientType"
                           : "alpha"));
}

static bool gd_getCornerRadius(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "cornerRadius", "F", &v);
   *out = v;
   return true;
}

static bool gd_setSize(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)nargs; (void)out;
   rt_set_int(vm, self, "width", ARG(0).i);
   rt_set_int(vm, self, "height", ARG(1).i);
   RETV();
}

static const struct rt_field rt_gradient_drawable_fields[] = {
   { "shape", "I" }, { "gradientType", "I" }, { "alpha", "I" },
   { "cornerRadius", "F" }, { "cornerRadii", "[F" },
   { "strokeWidth", "I" }, { "strokeColor", "I" }, { "colors", "[I" },
   { "orientation", "Landroid/graphics/drawable/GradientDrawable$Orientation;" },
   { "width", "I" }, { "height", "I" }, { "dashWidth", "F" }, { "dashGap", "F" },
   { "positions", "[F" },
   F_END
};

/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. https://mozilla.org/MPL/2.0/
 */
/* GradientDrawable's bitmap Canvas renderer. Uses the Canvas matrix, clip and
 * source-over compositor already shared by the other drawable renderers. */
static bool gd_intrinsic(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                          int nargs,union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm,self,!strcmp(builtin_name(),"getIntrinsicWidth") ? "width" : "height"));
}
static uint32_t gd_alpha(uint32_t color,unsigned alpha)
{ return (color&0xffffffu)|((((color>>24)*(alpha+(alpha>>7)))>>8)<<24); }

/* Distance to an ellipse boundary; offset strokes follow the curve's normal,
 * rather than substituting an ellipse with enlarged axes. */
static float gd_ellipse_distance(float x,float y,float a,float b)
{
   x=fabsf(x); y=fabsf(y);
   if (a<b) {float v=a; a=b; b=v; v=x; x=y; y=v;}
   double q=(double)x*x/((double)a*a)+(double)y*y/((double)b*b),cx,cy;
   if (fabsf(a-b)<0.00001f) return hypotf(x,y)-a;
   double ratio=((double)a/b)*((double)a/b),z0=(double)x/a,z1=(double)y/b;
   if (z1>1e-12) {
      double low=z1-1,high=q<1 ? 0 : hypot(ratio*z0,z1)-1;
      for (int i=0;i<48;++i) {
         double mid=(low+high)*0.5,u=ratio*z0/(mid+ratio),v=z1/(mid+1);
         if (u*u+v*v>1) low=mid; else high=mid;
      }
      double root=(low+high)*0.5;
      cx=ratio*x/(root+ratio); cy=y/(root+1);
   } else {
      double limit=(double)a-(double)b*b/a;
      if (x<limit) {double f=x/limit; cx=a*f; cy=b*sqrt(fmax(0,1-f*f));}
      else {cx=a; cy=0;}
   }
   double distance=hypot(cx-x,cy-y);
   return (float)(q<=1 ? -distance : distance);
}
static bool gd_inside(const float box[4],const float radii[8],int shape,
                        float x,float y,float offset)
{
   float l=box[0]-offset,t=box[1]-offset,r=box[2]+offset,b=box[3]+offset;
   if (!(l<r && t<b) || x<l || x>=r || y<t || y>=b) return false;
   if (shape==1) {
      float a=(box[2]-box[0])*0.5f,bb=(box[3]-box[1])*0.5f;
      float dx=x-(box[0]+box[2])*0.5f,dy=y-(box[1]+box[3])*0.5f;
      if (fabsf(offset)<0.000001f) return dx*dx/(a*a)+dy*dy/(bb*bb)<=1;
      return gd_ellipse_distance(dx,dy,a,bb)<=offset;
   }
   for (int i=0;i<4;++i) {
      float rx=radii[i*2],ry=radii[i*2+1];
      if (!(rx>0 && ry>0)) continue;
      bool right=i==1 || i==2,bottom=i>=2;
      float cx=right ? box[2]-rx : box[0]+rx,cy=bottom ? box[3]-ry : box[1]+ry;
      if ((right ? x>cx : x<cx) && (bottom ? y>cy : y<cy)) {
         float dx=x-cx,dy=y-cy;
         if (fabsf(offset)<0.000001f) return dx*dx/(rx*rx)+dy*dy/(ry*ry)<=1;
         return gd_ellipse_distance(dx,dy,rx,ry)<=offset;
      }
   }
   return true;
}
static uint32_t gd_sample(const uint32_t *colors,uint32_t count,const float *positions,
                            float position,uint32_t solid)
{
   if (!colors) return solid;
   position=fmaxf(0,fminf(1,position));
   uint32_t i=0;
   while (i+1<count && position>(positions ? positions[i+1] : (float)(i+1)/(count-1))) ++i;
   if (i+1==count) return colors[i];
   float start=positions ? positions[i] : (float)i/(count-1);
   float end=positions ? positions[i+1] : (float)(i+1)/(count-1);
   float f=end>start ? fmaxf(0,fminf(1,(position-start)/(end-start))) : 1;
   uint32_t color=0;
   for (int shift=0;shift<=24;shift+=8) {
      float a=(float)((colors[i]>>shift)&255),b=(float)((colors[i+1]>>shift)&255);
      color|=(uint32_t)lroundf(a+(b-a)*f)<<shift;
   }
   return color;
}
static bool gd_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                      int nargs,union dvm_value *out)
{
   (void)nargs;
   dvm_ref canvas=ARG(0).l;
   if (!canvas) {dvm__throw(vm,"java/lang/NullPointerException","canvas"); return false;}
   union dvm_value bounds={0};
   if (!drawable_get_bounds(vm,self,NULL,0,&bounds)) return false;
   float stroke=fmaxf(0,rt_get_int(vm,self,"strokeWidth")),half=stroke*0.5f,box[4];
   for (int i=0;i<4;++i) box[i]=(float)ui_int(vm,bounds.l,layer_edges[i])+(i<2 ? half : -half);
   if (!(box[0]<box[2] && box[1]<box[3])) RETV();
   int shape=rt_get_int(vm,self,"shape"),type=rt_get_int(vm,self,"gradientType");
   if (shape==3 || ui_float(vm,self,"dashWidth")>0) {
      dvm__throw(vm,"java/lang/UnsupportedOperationException","Canvas ring or dashed GradientDrawable rendering is not implemented"); return false;
   }
   if (shape<0 || shape>3) RETV();
   union dvm_value filter={0},colors_ref={0},positions_ref={0},radii_ref={0},orientation={0},target={0};
   (void)dvm_get_field(vm,self,"colorFilter","Landroid/graphics/ColorFilter;",&filter);
   struct color_filter_state filter_state;
   if(!color_filter_prepare(vm,filter.l,&filter_state)) return false;
   (void)dvm_get_field(vm,self,"colors","[I",&colors_ref);
   (void)dvm_get_field(vm,self,"positions","[F",&positions_ref);
   const uint32_t *colors=dvm_array_data(vm,colors_ref.l);
   uint32_t count=colors_ref.l ? dvm_array_length(vm,colors_ref.l) : 0;
   const float *positions=dvm_array_data(vm,positions_ref.l);
   if (colors && (count<2 || (positions && dvm_array_length(vm,positions_ref.l)!=count))) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","gradient colors or positions"); return false;
   }
   float radii[8],radius=ui_float(vm,self,"cornerRadius"),w=box[2]-box[0],h=box[3]-box[1];
   radius=fminf(fmaxf(0,radius),fminf(w,h)*0.5f);
   for (int i=0;i<8;++i) radii[i]=radius;
   (void)dvm_get_field(vm,self,"cornerRadii","[F",&radii_ref);
   if (radii_ref.l && shape==0) {
      if (dvm_array_length(vm,radii_ref.l)<8) {dvm__throw(vm,"java/lang/ArrayIndexOutOfBoundsException","radii needs eight values"); return false;}
      const float *r=dvm_array_data(vm,radii_ref.l); float scale=1;
      for (int i=0;i<8;++i) radii[i]=fmaxf(0,r[i]);
      const int pairs[4][2]={{0,2},{6,4},{1,7},{3,5}};
      for (int i=0;i<4;++i) {
         float sum=radii[pairs[i][0]]+radii[pairs[i][1]],extent=i<2 ? w : h;
         if (sum>extent) scale=fminf(scale,extent/sum);
      }
      for (int i=0;i<8;++i) radii[i]*=scale;
   }
   (void)dvm_get_field(vm,self,"orientation","Landroid/graphics/drawable/GradientDrawable$Orientation;",&orientation);
   int ord=orientation.l ? ui_int(vm,orientation.l,"ordinal") : 0;
   static const int endpoints[8][4]={{0,0,0,1},{1,0,0,1},{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,1,1}};
   if (ord<0 || ord>=8) {dvm__throw(vm,"java/lang/IllegalArgumentException","gradient orientation"); return false;}
   float x0=box[0]+endpoints[ord][0]*w,y0=box[1]+endpoints[ord][1]*h;
   float dx=(endpoints[ord][2]-endpoints[ord][0])*w,dy=(endpoints[ord][3]-endpoints[ord][1])*h;
   if (colors && type!=0) {dvm__throw(vm,"java/lang/UnsupportedOperationException","Canvas radial or sweep GradientDrawable rendering is not implemented"); return false;}
   (void)dvm_get_field(vm,canvas,"bitmap","Landroid/graphics/Bitmap;",&target);
   struct rt_bitmap *dst=bitmap_of(vm,target.l);
   if (!dst) RETV();
   if (!bitmap_live(vm,dst)) return false;
   struct canvas_state *clip=canvas_state(vm,canvas); if (!clip) return false;
   float matrix[9],inverse[9]; canvas_read_matrix(vm,canvas,matrix);
   if (!mat_invert(matrix,inverse)) RETV();
   int left=0,top=0,right=dst->w,bottom=dst->h;
   if (matrix[6]==0 && matrix[7]==0 && matrix[8]!=0) {
      float minx=INFINITY,miny=INFINITY,maxx=-INFINITY,maxy=-INFINITY;
      for (int i=0;i<4;++i) {
         float px,py; mat_map(matrix,box[i&1 ? 2 : 0]+(i&1 ? half : -half),
            box[i&2 ? 3 : 1]+(i&2 ? half : -half),&px,&py);
         minx=fminf(minx,px); maxx=fmaxf(maxx,px); miny=fminf(miny,py); maxy=fmaxf(maxy,py);
      }
      left=(int)fmaxf(0,fminf(dst->w,floorf(minx))); right=(int)fmaxf(0,fminf(dst->w,ceilf(maxx)));
      top=(int)fmaxf(0,fminf(dst->h,floorf(miny))); bottom=(int)fmaxf(0,fminf(dst->h,ceilf(maxy)));
   }
   int limits[4]; canvas_clip_limits(clip, dst, limits);
   if (left < limits[0]) left = limits[0];
   if (top < limits[1]) top = limits[1];
   if (right > limits[2]) right = limits[2];
   if (bottom > limits[3]) bottom = limits[3];
   unsigned alpha=(unsigned)rt_get_int(vm,self,"alpha")&255;
   uint32_t solid=(uint32_t)rt_get_int(vm,self,"color"),line=(uint32_t)rt_get_int(vm,self,"strokeColor");
   bool layer=shape!=2 && stroke>0 && (gd_alpha(line,alpha)>>24)>0 && (gd_alpha(line,alpha)>>24)<255 && alpha<255;
   uint32_t paint_line=color_filter_pixel(&filter_state,layer ? line : gd_alpha(line,alpha));
   /* The union of the two central strips lies entirely inside a rounded
    * rectangle. If a whole device pixel maps inside either strip, all 16
    * coverage samples have the same solid color; evaluate/blend it once.
    * Perspective and boundary pixels retain the exact sampling path below. */
   const bool solid_interior=shape==0 && !colors && stroke<=0 &&
      inverse[6]==0 && inverse[7]==0 && inverse[8]!=0;
   float half_x=0,half_y=0;
   if(solid_interior) {
      half_x=(fabsf(inverse[0])+fabsf(inverse[1]))/(2*fabsf(inverse[8]));
      half_y=(fabsf(inverse[3])+fabsf(inverse[4]))/(2*fabsf(inverse[8]));
   }
   const float inner_left=box[0]+fmaxf(radii[0],radii[6]);
   const float inner_right=box[2]-fmaxf(radii[2],radii[4]);
   const float inner_top=box[1]+fmaxf(radii[1],radii[3]);
   const float inner_bottom=box[3]-fmaxf(radii[5],radii[7]);
   const uint32_t paint_solid=color_filter_pixel(&filter_state,gd_alpha(solid,alpha));
   for (int y=top;y<bottom;++y) for (int x=left;x<right;++x) {
      if (!canvas_clip_contains(clip,x+0.5f,y+0.5f)) continue;
      if(solid_interior) {
         float lx,ly;mat_map(inverse,x+.5f,y+.5f,&lx,&ly);
         if(lx-half_x>=box[0] && lx+half_x<=box[2] &&
            ly-half_y>=box[1] && ly+half_y<=box[3] &&
            ((lx-half_x>=inner_left && lx+half_x<=inner_right) ||
             (ly-half_y>=inner_top && ly+half_y<=inner_bottom))) {
            size_t pixel=(size_t)y*dst->w+x;
            dst->px[pixel]=canvas_src_over(paint_solid,dst->px[pixel],!dst->has_alpha);
            continue;
         }
      }
      float accum[4]={0};
      for (int sy=0;sy<4;++sy) for (int sx=0;sx<4;++sx) {
         float lx,ly; mat_map(inverse,x+(sx+0.5f)*0.25f,y+(sy+0.5f)*0.25f,&lx,&ly);
         if (!isfinite(lx) || !isfinite(ly)) continue;
         uint32_t sample=0;
         if (shape!=2 && gd_inside(box,radii,shape,lx,ly,0)) {
            float pos=colors ? ((lx-x0)*dx+(ly-y0)*dy)/(dx*dx+dy*dy) : 0;
            sample=gd_sample(colors,count,positions,pos,solid);
            if(!layer) sample=gd_alpha(sample,alpha);
            sample=color_filter_pixel(&filter_state,sample);
         }
         /* A fill-only drawable has no stroke boundary to evaluate. In
          * particular, avoid solving rounded/elliptical offsets for every
          * subpixel when those results cannot contribute to the image. */
         if (stroke>0) {
            bool edge=shape==2 ? lx>=box[0] && lx<box[2] && fabsf(ly-(box[1]+box[3])*0.5f)<half :
               gd_inside(box,radii,shape,lx,ly,half) && !gd_inside(box,radii,shape,lx,ly,-half);
            if (edge) sample=canvas_src_over(paint_line,sample,false);
         }
         if (layer) sample=gd_alpha(sample,alpha);
         float a=(float)(sample>>24)/255.f; accum[3]+=a;
         for (int i=0;i<3;++i) accum[i]+=((sample>>(16-i*8))&255)*a;
      }
      if (!(accum[3]>0)) continue;
      uint32_t src=(uint32_t)lroundf(accum[3]*255.f/16.f)<<24;
      for (int i=0;i<3;++i) src|=(uint32_t)lroundf(accum[i]/accum[3])<<(16-i*8);
      size_t pixel=(size_t)y*dst->w+x; dst->px[pixel]=canvas_src_over(src,dst->px[pixel],!dst->has_alpha);
   }
   ++dst->gen;
   RETV();
}

static const struct rt_method rt_gradient_drawable_full[] = {
   M("<init>", "()V", gd_init),
   M("<init>",
     "(Landroid/graphics/drawable/GradientDrawable$Orientation;[I)V", gd_init),
   M("setColor", "(I)V", gradient_set_color),
   M("setShape", "(I)V", gd_set_int),
   M("getShape", "()I", gd_get_int),
   M("setGradientType", "(I)V", gd_set_int),
   M("getGradientType", "()I", gd_get_int),
   M("setAlpha", "(I)V", gd_set_int),
   M("getAlpha", "()I", gd_get_int),
   M("setCornerRadius", "(F)V", gd_setCornerRadius),
   M("getCornerRadius", "()F", gd_getCornerRadius),
   M("setCornerRadii", "([F)V", gd_setCornerRadii),
   M("setStroke", "(II)V", gd_setStroke),
   M("setStroke", "(IIFF)V", gd_setStroke),
   M("setColors", "([I)V", gd_setColors),
   M("setColors", "([I[F)V", gd_setColors),
   M("draw", "(Landroid/graphics/Canvas;)V", gd_draw),
   M("getIntrinsicWidth", "()I", gd_intrinsic),
   M("getIntrinsicHeight", "()I", gd_intrinsic),
   M("setOrientation",
     "(Landroid/graphics/drawable/GradientDrawable$Orientation;)V",
     gd_setOrientation),
   M("setSize", "(II)V", gd_setSize),
   M_END,
};

static const struct rt_field rt_gd_orientation_sfields[] = {
   { "TOP_BOTTOM", "Landroid/graphics/drawable/GradientDrawable$Orientation;" },
   { "TR_BL", "Landroid/graphics/drawable/GradientDrawable$Orientation;" },
   { "RIGHT_LEFT", "Landroid/graphics/drawable/GradientDrawable$Orientation;" },
   { "BR_TL", "Landroid/graphics/drawable/GradientDrawable$Orientation;" },
   { "BOTTOM_TOP", "Landroid/graphics/drawable/GradientDrawable$Orientation;" },
   { "BL_TR", "Landroid/graphics/drawable/GradientDrawable$Orientation;" },
   { "LEFT_RIGHT", "Landroid/graphics/drawable/GradientDrawable$Orientation;" },
   { "TL_BR", "Landroid/graphics/drawable/GradientDrawable$Orientation;" },
   F_END
};

