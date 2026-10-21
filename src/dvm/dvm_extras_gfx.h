/* android.graphics additions: Canvas shapes, ColorMatrix, Outline, Region,
 * Drawable tinting.  Merged into the builtin tables through rt_extras (see
 * dvm_runtime_define); included from dvm_runtime.c after dvm_graphics.h. */

#ifndef DVM_EXTRAS_GFX_H
#define DVM_EXTRAS_GFX_H

/* --------------------------------------------------------- Canvas shapes *
 *
 * Circles, ovals, arcs, lines and points are paths: the geometry goes into the
 * same luna_path the rasteriser already fills and strokes, so the paint's
 * style, stroke width, caps, shader and filter apply exactly as for drawPath. */

static bool x_canvas_shape(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   const char *name = builtin_name(), *sig = builtin_sig();
   dvm_ref paint = args[nargs - 1].l;
   luna_path path = { 0 };
   bool good = true;
   float rect[4] = { 0 };

   if (!strcmp(name, "drawCircle")) {
      const float radius = ARG(2).f;
      if (radius <= 0) RETV();
      rect[0] = ARG(0).f - radius; rect[1] = ARG(1).f - radius;
      rect[2] = ARG(0).f + radius; rect[3] = ARG(1).f + radius;
      good = luna_path_arc(&path, rect, 0, 360, true) && luna_path_close(&path);
   } else if (!strcmp(name, "drawOval") || !strcmp(name, "drawArc")) {
      int index;
      if (sig[1] == 'L') { if (!canvas_rect_args(vm, args, 1, rect)) return false; index = 1; }
      else { for (int i = 0; i < 4; ++i) rect[i] = args[i].f; index = 4; }
      if (!(rect[0] < rect[2] && rect[1] < rect[3])) RETV();
      if (!strcmp(name, "drawOval"))
         good = luna_path_arc(&path, rect, 0, 360, true) && luna_path_close(&path);
      else {
         const float start = args[index].f;
         float sweep = args[index + 1].f;
         const bool center = args[index + 2].i != 0;
         if (sweep > 360.f) sweep = 360.f;
         if (sweep < -360.f) sweep = -360.f;
         if (center) {
            good = luna_path_move(&path, (rect[0] + rect[2]) * .5f, (rect[1] + rect[3]) * .5f) &&
                   luna_path_arc(&path, rect, start, sweep, false) && luna_path_close(&path);
         } else good = luna_path_arc(&path, rect, start, sweep, true);
      }
   } else if (!strcmp(name, "drawLine")) {
      good = luna_path_move(&path, ARG(0).f, ARG(1).f) && luna_path_line(&path, ARG(2).f, ARG(3).f);
   } else if (!strcmp(name, "drawPoint")) {
      /* A point is a zero-length segment: round caps make it a dot, square a square. */
      good = luna_path_move(&path, ARG(0).f, ARG(1).f) && luna_path_line(&path, ARG(0).f, ARG(1).f);
   } else {
      dvm__throw(vm, "java/lang/InternalError", "Canvas.%s", name);
      return false;
   }
   if (!good) {
      luna_path_clear(&path, true);
      dvm__throw(vm, "java/lang/OutOfMemoryError", "Canvas shape geometry");
      return false;
   }
   if (!strcmp(name, "drawLine") || !strcmp(name, "drawPoint")) {
      /* Lines and points are always stroked, whatever the paint's style says. */
      union dvm_value style = { 0 };
      dvm_ref stroke = 0;
      (void)dvm_get_static(vm, dvm_find_class(vm, "android/graphics/Paint$Style"), "STROKE",
                           "Landroid/graphics/Paint$Style;", &style);
      stroke = style.l;
      union dvm_value saved = { 0 };
      (void)dvm_get_field(vm, paint, "style", "Landroid/graphics/Paint$Style;", &saved);
      (void)dvm_set_field(vm, paint, "style", "Landroid/graphics/Paint$Style;", (union dvm_value){ .l = stroke });
      good = canvas_path_paint(vm, self, &path, paint, out);
      (void)dvm_set_field(vm, paint, "style", "Landroid/graphics/Paint$Style;", saved);
   } else good = canvas_path_paint(vm, self, &path, paint, out);
   luna_path_clear(&path, true);
   return good;
}

/* drawBitmap(bitmap, left, top, paint) is the matrix form with a translation. */
static bool x_canvas_drawBitmapAt(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "bitmap"); return false; }
   dvm_ref matrix = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/Matrix"));
   union dvm_value ignored = { 0 }, shift[2] = { { .f = ARG(1).f }, { .f = ARG(2).f } };
   if (!matrix || !am_vcall(vm, matrix, "<init>", "()V", NULL, 0, &ignored) ||
       !am_vcall(vm, matrix, "setTranslate", "(FF)V", shift, 2, &ignored)) return false;
   dvm_pin(vm, matrix);
   const union dvm_value draw[3] = { ARG(0), { .l = matrix }, ARG(3) };
   const bool good = am_vcall(vm, self, "drawBitmap",
                              "(Landroid/graphics/Bitmap;Landroid/graphics/Matrix;Landroid/graphics/Paint;)V", draw, 3, out);
   dvm_unpin(vm, matrix);
   return good;
}

/* drawPaint(paint): fill the whole clip with the paint. */
static bool x_canvas_drawPaint(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "paint"); return false; }
   union dvm_value bounds = { 0 };
   if (!am_vcall(vm, self, "getClipBounds", "()Landroid/graphics/Rect;", NULL, 0, &bounds) || !bounds.l) return false;
   const union dvm_value rect[2] = { bounds, ARG(0) };
   return am_vcall(vm, self, "drawRect", "(Landroid/graphics/Rect;Landroid/graphics/Paint;)V", rect, 2, out);
}

static const struct rt_method rt_x_canvas[] = {
   M("drawCircle", "(FFFLandroid/graphics/Paint;)V", x_canvas_shape),
   M("drawOval", "(Landroid/graphics/RectF;Landroid/graphics/Paint;)V", x_canvas_shape),
   M("drawOval", "(FFFFLandroid/graphics/Paint;)V", x_canvas_shape),
   M("drawArc", "(Landroid/graphics/RectF;FFZLandroid/graphics/Paint;)V", x_canvas_shape),
   M("drawArc", "(FFFFFFZLandroid/graphics/Paint;)V", x_canvas_shape),
   M("drawLine", "(FFFFLandroid/graphics/Paint;)V", x_canvas_shape),
   M("drawPoint", "(FFLandroid/graphics/Paint;)V", x_canvas_shape),
   M("drawBitmap", "(Landroid/graphics/Bitmap;FFLandroid/graphics/Paint;)V", x_canvas_drawBitmapAt),
   M("drawPaint", "(Landroid/graphics/Paint;)V", x_canvas_drawPaint),
   M_END,
};

/* ------------------------------------------------------------------ Color */

static bool x_color_hsv_alpha(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)nargs;
   union dvm_value rgb = { 0 };
   if (!color_hsv_to_color(vm, self, &args[1], 1, &rgb)) return false;
   RETI((int32_t)(((uint32_t)rgb.i & 0x00ffffffu) | ((uint32_t)(ARG(0).i & 0xff) << 24)));
}

static bool x_color_rgb_to_hsv(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)nargs;
   const union dvm_value packed[2] = { { .i = (int32_t)(0xff000000u | ((uint32_t)(ARG(0).i & 0xff) << 16) |
                                                        ((uint32_t)(ARG(1).i & 0xff) << 8) | (uint32_t)(ARG(2).i & 0xff)) },
                                       ARG(3) };
   return color_to_hsv(vm, self, packed, 2, out);
}

static const struct rt_method rt_x_color[] = {
   SM("HSVToColor", "(I[F)I", x_color_hsv_alpha),
   SM("RGBToHSV", "(III[F)V", x_color_rgb_to_hsv),
   M_END,
};

/* ------------------------------------------------------------ ColorMatrix *
 *
 * A 4x5 matrix over (R, G, B, A, 1) in 0..255 units: each output channel is a
 * row's dot product with the straight-alpha input.  Stored as a float[20] so
 * getArray() hands back the live array, as the framework does. */

static float *x_cm(struct dvm *vm, dvm_ref self)
{
   union dvm_value arr = { 0 };
   (void)dvm_get_field(vm, self, "array", "[F", &arr);
   if (!arr.l) {
      arr.l = dvm_new_array(vm, 'F', "F", 20);
      if (!arr.l) return NULL;
      (void)dvm_set_field(vm, self, "array", "[F", arr);
      float *f = dvm_array_data(vm, arr.l);
      memset(f, 0, 20 * sizeof *f);
      f[0] = f[6] = f[12] = f[18] = 1.f;
   }
   return dvm_array_data(vm, arr.l);
}

static const struct rt_field rt_x_color_matrix_fields[] = { { "array", "[F" }, F_END };

static void x_cm_identity(float *m)
{
   memset(m, 0, 20 * sizeof *m);
   m[0] = m[6] = m[12] = m[18] = 1.f;
}

/* out = a * b: apply b first, then a. */
static void x_cm_mul(float out[20], const float a[20], const float b[20])
{
   float t[20];
   for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 5; ++j) {
         float v = j == 4 ? a[i * 5 + 4] : 0.f;
         for (int k = 0; k < 4; ++k) v += a[i * 5 + k] * b[k * 5 + j];
         t[i * 5 + j] = v;
      }
   memcpy(out, t, sizeof t);
}

static bool x_cm_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)out;
   float *m = x_cm(vm, self);
   if (!m) return false;
   if (nargs && ARG(0).l) {
      const char *sig = builtin_sig();
      const float *src;
      if (strchr(sig, '[')) {
         struct dvm_object *a = dvm__obj(vm, ARG(0).l);
         if (!a || a->length < 20) { dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "length=%u; index=19", a ? a->length : 0u); return false; }
         src = a->data;
      } else src = x_cm(vm, ARG(0).l);
      if (!src) return false;
      memmove(m, src, 20 * sizeof *m);
   }
   RETV();
}

static bool x_cm_method(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   const char *name = builtin_name();
   float *m = x_cm(vm, self);
   if (!m) return false;
   if (!strcmp(name, "reset")) { x_cm_identity(m); RETV(); }
   if (!strcmp(name, "set")) {
      const float *src;
      if (strchr(builtin_sig(), '[')) {
         struct dvm_object *a = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
         if (!a || a->length < 20) { dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "src"); return false; }
         src = a->data;
      } else src = ARG(0).l ? x_cm(vm, ARG(0).l) : NULL;
      if (!src) return false;
      memmove(m, src, 20 * sizeof *m);
      RETV();
   }
   if (!strcmp(name, "setScale")) {
      x_cm_identity(m);
      m[0] = ARG(0).f; m[6] = ARG(1).f; m[12] = ARG(2).f; m[18] = ARG(3).f;
      RETV();
   }
   if (!strcmp(name, "setSaturation")) {
      const float s = ARG(0).f, inv = 1.f - s;
      const float r = inv * 0.213f, g = inv * 0.715f, b = inv * 0.072f;
      x_cm_identity(m);
      m[0] = r + s; m[1] = g;     m[2] = b;
      m[5] = r;     m[6] = g + s; m[7] = b;
      m[10] = r;    m[11] = g;    m[12] = b + s;
      RETV();
   }
   if (!strcmp(name, "setRotate")) {
      const float rad = ARG(1).f * (float)(M_PI / 180.0), c = cosf(rad), s = sinf(rad);
      const int axis = ARG(0).i;
      if (axis < 0 || axis > 2) { dvm__throw(vm, "java/lang/RuntimeException", "Invalid axis"); return false; }
      const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
      x_cm_identity(m);
      m[a1 * 6] = c;      m[a1 * 5 + a2] = s;
      m[a2 * 5 + a1] = -s; m[a2 * 6] = c;
      RETV();
   }
   if (!strcmp(name, "setRGB2YUV")) {
      static const float v[20] = { 0.299f, 0.587f, 0.114f, 0, 0,  -0.16874f, -0.33126f, 0.5f, 0, 0,
                                   0.5f, -0.41869f, -0.08131f, 0, 0,  0, 0, 0, 1, 0 };
      memcpy(m, v, sizeof v);
      RETV();
   }
   if (!strcmp(name, "setYUV2RGB")) {
      static const float v[20] = { 1, 0, 1.402f, 0, 0,  1, -0.34414f, -0.71414f, 0, 0,
                                   1, 1.772f, 0, 0, 0,  0, 0, 0, 1, 0 };
      memcpy(m, v, sizeof v);
      RETV();
   }
   /* setConcat(a, b), preConcat(m) = this * m, postConcat(m) = m * this. */
   if (!strcmp(name, "setConcat")) {
      const float *a = ARG(0).l ? x_cm(vm, ARG(0).l) : NULL, *b = ARG(1).l ? x_cm(vm, ARG(1).l) : NULL;
      if (!a || !b) { dvm__throw(vm, "java/lang/NullPointerException", "matrix"); return false; }
      float ac[20], bc[20];
      memcpy(ac, a, sizeof ac); memcpy(bc, b, sizeof bc);
      x_cm_mul(m, ac, bc);
      RETV();
   }
   const float *o = ARG(0).l ? x_cm(vm, ARG(0).l) : NULL;
   if (!o) { dvm__throw(vm, "java/lang/NullPointerException", "matrix"); return false; }
   float oc[20];
   memcpy(oc, o, sizeof oc);
   if (!strcmp(name, "preConcat")) x_cm_mul(m, m, oc); else x_cm_mul(m, oc, m);
   RETV();
}

static bool x_cm_getArray(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   if (!x_cm(vm, self)) return false;
   return dvm_get_field(vm, self, "array", "[F", out);
}

static const struct rt_method rt_x_color_matrix[] = {
   M("<init>", "()V", x_cm_init), M("<init>", "([F)V", x_cm_init),
   M("<init>", "(Landroid/graphics/ColorMatrix;)V", x_cm_init),
   M("getArray", "()[F", x_cm_getArray),
   M("reset", "()V", x_cm_method),
   M("set", "(Landroid/graphics/ColorMatrix;)V", x_cm_method), M("set", "([F)V", x_cm_method),
   M("setScale", "(FFFF)V", x_cm_method), M("setSaturation", "(F)V", x_cm_method),
   M("setRotate", "(IF)V", x_cm_method),
   M("setRGB2YUV", "()V", x_cm_method), M("setYUV2RGB", "()V", x_cm_method),
   M("setConcat", "(Landroid/graphics/ColorMatrix;Landroid/graphics/ColorMatrix;)V", x_cm_method),
   M("preConcat", "(Landroid/graphics/ColorMatrix;)V", x_cm_method),
   M("postConcat", "(Landroid/graphics/ColorMatrix;)V", x_cm_method),
   M_END,
};

/* ColorMatrixColorFilter keeps its own copy of the 20 values; the draw path
 * reads them in color_filter_prepare(). */
static const struct rt_field rt_x_cmcf_fields[] = { { "colorMatrix", "[F" }, F_END };

static bool x_cmcf_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "matrix"); return false; }
   const float *src;
   if (strchr(builtin_sig(), '[')) {
      struct dvm_object *a = dvm__obj(vm, ARG(0).l);
      if (!a || a->length < 20) { dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "colorMatrix"); return false; }
      src = a->data;
   } else src = x_cm(vm, ARG(0).l);
   if (!src) return false;
   dvm_ref copy = dvm_new_array(vm, 'F', "F", 20);
   if (!copy) return false;
   memcpy(dvm_array_data(vm, copy), src, 20 * sizeof(float));
   (void)dvm_set_field(vm, self, "colorMatrix", "[F", (union dvm_value){ .l = copy });
   RETV();
}

static const struct rt_method rt_x_cmcf[] = {
   M("<init>", "(Landroid/graphics/ColorMatrix;)V", x_cmcf_init), M("<init>", "([F)V", x_cmcf_init),
   M_END,
};

/* ---------------------------------------------------------------- Outline *
 *
 * The shape a view or render node casts and, when clipToOutline is set, clips
 * to: nothing, a rectangle, a rounded rectangle, an oval, or a path. */

enum { X_OUTLINE_EMPTY = 0, X_OUTLINE_RECT, X_OUTLINE_ROUND, X_OUTLINE_OVAL, X_OUTLINE_PATH };

static const struct rt_field rt_x_outline_fields[] = {
   { "shape", "I" }, { "left", "I" }, { "top", "I" }, { "right", "I" }, { "bottom", "I" },
   { "radius", "F" }, { "alpha", "F" }, { "path", "Landroid/graphics/Path;" }, F_END
};

static bool x_outline_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)args; (void)out;
   (void)dvm_set_field(vm, self, "alpha", "F", (union dvm_value){ .f = 0.f });
   if (nargs && ARG(0).l) {            /* Outline(Outline) */
      static const char *const ints[] = { "shape", "left", "top", "right", "bottom" };
      for (int i = 0; i < 5; ++i) rt_set_int(vm, self, ints[i], rt_get_int(vm, ARG(0).l, ints[i]));
      union dvm_value f = { 0 }, p = { 0 };
      (void)dvm_get_field(vm, ARG(0).l, "radius", "F", &f);
      (void)dvm_set_field(vm, self, "radius", "F", f);
      (void)dvm_get_field(vm, ARG(0).l, "alpha", "F", &f);
      (void)dvm_set_field(vm, self, "alpha", "F", f);
      (void)dvm_get_field(vm, ARG(0).l, "path", "Landroid/graphics/Path;", &p);
      (void)dvm_set_field(vm, self, "path", "Landroid/graphics/Path;", p);
   }
   RETV();
}

static bool x_outline_set(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)out;
   const char *name = builtin_name(), *sig = builtin_sig();
   if (!strcmp(name, "setEmpty")) {
      rt_set_int(vm, self, "shape", X_OUTLINE_EMPTY);
      (void)dvm_set_field(vm, self, "path", "Landroid/graphics/Path;", (union dvm_value){ 0 });
      RETV();
   }
   if (!strcmp(name, "setPath") || !strcmp(name, "setConvexPath")) {
      if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "path"); return false; }
      rt_set_int(vm, self, "shape", X_OUTLINE_PATH);
      (void)dvm_set_field(vm, self, "path", "Landroid/graphics/Path;", ARG(0));
      RETV();
   }
   int32_t b[4];
   int next;
   if (sig[1] == 'L') {                          /* (Rect[, radius]) */
      if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "rect"); return false; }
      static const char *const e[] = { "left", "top", "right", "bottom" };
      for (int i = 0; i < 4; ++i) b[i] = rt_get_int(vm, ARG(0).l, e[i]);
      next = 1;
   } else { for (int i = 0; i < 4; ++i) b[i] = ARG(i).i; next = 4; }
   const bool round = !strcmp(name, "setRoundRect"), oval = !strcmp(name, "setOval");
   const float radius = round ? ARG(next).f : 0.f;
   if (b[0] >= b[2] || b[1] >= b[3]) {           /* an empty box casts nothing */
      rt_set_int(vm, self, "shape", X_OUTLINE_EMPTY);
      RETV();
   }
   (void)nargs;
   static const char *const f[] = { "left", "top", "right", "bottom" };
   for (int i = 0; i < 4; ++i) rt_set_int(vm, self, f[i], b[i]);
   rt_set_int(vm, self, "shape", oval ? X_OUTLINE_OVAL : round && radius > 0 ? X_OUTLINE_ROUND : X_OUTLINE_RECT);
   (void)dvm_set_field(vm, self, "radius", "F", (union dvm_value){ .f = radius });
   RETV();
}

static bool x_outline_query(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   const int shape = rt_get_int(vm, self, "shape");
   if (!strcmp(name, "isEmpty")) RETI(shape == X_OUTLINE_EMPTY);
   if (!strcmp(name, "canClip")) RETI(shape == X_OUTLINE_RECT || shape == X_OUTLINE_ROUND || shape == X_OUTLINE_OVAL || shape == X_OUTLINE_PATH);
   if (!strcmp(name, "getRadius")) {
      union dvm_value f = { 0 };
      (void)dvm_get_field(vm, self, "radius", "F", &f);
      RETF(shape == X_OUTLINE_ROUND ? f.f : -1.0f);
   }
   if (!strcmp(name, "getAlpha")) {
      union dvm_value f = { 0 };
      (void)dvm_get_field(vm, self, "alpha", "F", &f);
      RETF(f.f);
   }
   if (!strcmp(name, "setAlpha")) {
      (void)dvm_set_field(vm, self, "alpha", "F", ARG(0));
      RETV();
   }
   /* getRect(Rect): only a rectangle (or a round rect) is a rect. */
   if (shape != X_OUTLINE_RECT && shape != X_OUTLINE_ROUND) RETI(0);
   if (ARG(0).l) {
      static const char *const e[] = { "left", "top", "right", "bottom" };
      for (int i = 0; i < 4; ++i) rt_set_int(vm, ARG(0).l, e[i], rt_get_int(vm, self, e[i]));
   }
   RETI(1);
}

static bool x_outline_set_from(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "src"); return false; }
   return x_outline_init(vm, self, args, nargs, out);
}

static const struct rt_method rt_x_outline[] = {
   M("<init>", "()V", x_outline_init), M("<init>", "(Landroid/graphics/Outline;)V", x_outline_init),
   M("set", "(Landroid/graphics/Outline;)V", x_outline_set_from),
   M("setEmpty", "()V", x_outline_set),
   M("setRect", "(IIII)V", x_outline_set), M("setRect", "(Landroid/graphics/Rect;)V", x_outline_set),
   M("setRoundRect", "(IIIIF)V", x_outline_set), M("setRoundRect", "(Landroid/graphics/Rect;F)V", x_outline_set),
   M("setOval", "(IIII)V", x_outline_set), M("setOval", "(Landroid/graphics/Rect;)V", x_outline_set),
   M("setPath", "(Landroid/graphics/Path;)V", x_outline_set), M("setConvexPath", "(Landroid/graphics/Path;)V", x_outline_set),
   M("isEmpty", "()Z", x_outline_query), M("canClip", "()Z", x_outline_query),
   M("getRadius", "()F", x_outline_query), M("getAlpha", "()F", x_outline_query),
   M("setAlpha", "(F)V", x_outline_query), M("getRect", "(Landroid/graphics/Rect;)Z", x_outline_query),
   M_END,
};

/* The Path of an outline's shape, in the coordinates of the outline. */
static bool x_outline_path(struct dvm *vm, dvm_ref outline, luna_path *path)
{
   const int shape = rt_get_int(vm, outline, "shape");
   if (shape == X_OUTLINE_EMPTY) return false;
   if (shape == X_OUTLINE_PATH) {
      union dvm_value p = { 0 };
      (void)dvm_get_field(vm, outline, "path", "Landroid/graphics/Path;", &p);
      luna_path *src = p.l ? path_of(vm, p.l) : NULL;
      return src && luna_path_append(path, src, NULL);
   }
   const float rect[4] = { (float)rt_get_int(vm, outline, "left"), (float)rt_get_int(vm, outline, "top"),
                           (float)rt_get_int(vm, outline, "right"), (float)rt_get_int(vm, outline, "bottom") };
   if (shape == X_OUTLINE_OVAL) return luna_path_arc(path, rect, 0, 360, true) && luna_path_close(path);
   union dvm_value radius = { 0 };
   (void)dvm_get_field(vm, outline, "radius", "F", &radius);
   if (shape == X_OUTLINE_ROUND) {
      const float radii[8] = { radius.f, radius.f, radius.f, radius.f, radius.f, radius.f, radius.f, radius.f };
      return luna_path_round_rect(path, rect, radii, false);
   }
   return luna_path_move(path, rect[0], rect[1]) && luna_path_line(path, rect[2], rect[1]) &&
          luna_path_line(path, rect[2], rect[3]) && luna_path_line(path, rect[0], rect[3]) && luna_path_close(path);
}

/* Clips `canvas` to the outline, in the canvas's current coordinates. */
static bool x_clip_to_outline(struct dvm *vm, dvm_ref canvas, dvm_ref outline)
{
   luna_path path = { 0 };
   if (!x_outline_path(vm, outline, &path)) { luna_path_clear(&path, true); return true; }
   dvm_ref p = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/Path"));
   union dvm_value ignored = { 0 };
   bool good = p && am_vcall(vm, p, "<init>", "()V", NULL, 0, &ignored);
   luna_path *target = good ? path_of(vm, p) : NULL;
   if (target) good = luna_path_append(target, &path, NULL);
   luna_path_clear(&path, true);
   if (!good || !target) return false;
   dvm_pin(vm, p);
   const union dvm_value arg = { .l = p };
   good = am_vcall(vm, canvas, "clipPath", "(Landroid/graphics/Path;)Z", &arg, 1, &ignored);
   dvm_unpin(vm, p);
   return good;
}

/* ViewOutlineProvider: the callback that says what a view's outline is.  The
 * three shared providers are distinct classes so each keeps its own meaning:
 * BOUNDS is the view's box, PADDED_BOUNDS the box inside the padding, and
 * BACKGROUND whatever shape the background drawable reports (the box when it
 * has none). */
static bool x_vop_bounds(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)self; (void)nargs; (void)out;
   if (!ARG(0).l || !ARG(1).l) RETV();
   const struct ui_node *n = ui_node(vm, ARG(0).l);
   int32_t box[4] = { 0, 0, n ? n->w : 0, n ? n->h : 0 };
   if (!strcmp(dvm__builtin_method()->cls->name, "android/view/ViewOutlineProvider$3")) {
      int32_t pad[4];
      ui_pad(vm, ARG(0).l, pad);
      box[0] = pad[0]; box[1] = pad[1]; box[2] -= pad[2]; box[3] -= pad[3];
   }
   union dvm_value rect[4] = { { .i = box[0] }, { .i = box[1] }, { .i = box[2] }, { .i = box[3] } }, ignored = { 0 };
   if (!am_vcall(vm, ARG(1).l, "setRect", "(IIII)V", rect, 4, &ignored)) return false;
   (void)dvm_set_field(vm, ARG(1).l, "alpha", "F", (union dvm_value){ .f = 1.f });
   RETV();
}

static bool x_vop_background(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   dvm_ref bg = ARG(0).l ? ui_obj(vm, ARG(0).l, "background", "Landroid/graphics/drawable/Drawable;") : 0;
   if (bg && ARG(1).l) {
      union dvm_value ignored = { 0 };
      const union dvm_value arg = ARG(1);
      if (!am_vcall(vm, bg, "getOutline", "(Landroid/graphics/Outline;)V", &arg, 1, &ignored)) return false;
      RETV();
   }
   if (ARG(1).l) {
      if (!x_vop_bounds(vm, self, args, nargs, out)) return false;
      (void)dvm_set_field(vm, ARG(1).l, "alpha", "F", (union dvm_value){ .f = 0.f });
   }
   RETV();
}

static const struct rt_method rt_x_vop[] = {
   M("<init>", "()V", empty_void),
   M("getOutline", "(Landroid/view/View;Landroid/graphics/Outline;)V", x_vop_background),
   M_END,
};
static const struct rt_method rt_x_vop_bounds[] = {
   M("getOutline", "(Landroid/view/View;Landroid/graphics/Outline;)V", x_vop_bounds), M_END,
};
static const struct rt_field rt_x_vop_sfields[] = {
   { "BACKGROUND", "Landroid/view/ViewOutlineProvider;" }, { "BOUNDS", "Landroid/view/ViewOutlineProvider;" },
   { "PADDED_BOUNDS", "Landroid/view/ViewOutlineProvider;" }, F_END
};

/* Drawable.getOutline(): a drawable with no shape of its own casts none. */
static bool x_drawable_getOutline(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)self; (void)nargs; (void)out;
   union dvm_value ignored = { 0 };
   if (!ARG(0).l || !am_vcall(vm, ARG(0).l, "setEmpty", "()V", NULL, 0, &ignored)) return ARG(0).l != 0;
   RETV();
}

/* GradientDrawable: a rectangle with its corner radius, or an oval, over its bounds. */
static bool x_gradient_getOutline(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   if (!ARG(0).l) RETV();
   union dvm_value bounds = { 0 }, ignored = { 0 }, radius = { 0 };
   if (!am_vcall(vm, self, "getBounds", "()Landroid/graphics/Rect;", NULL, 0, &bounds) || !bounds.l) return false;
   (void)dvm_get_field(vm, self, "cornerRadius", "F", &radius);
   const int shape = rt_get_int(vm, self, "shape");
   union dvm_value a[2] = { bounds, { .f = radius.f } };
   bool good;
   if (shape == 1) good = am_vcall(vm, ARG(0).l, "setOval", "(Landroid/graphics/Rect;)V", a, 1, &ignored);
   else if (radius.f > 0) good = am_vcall(vm, ARG(0).l, "setRoundRect", "(Landroid/graphics/Rect;F)V", a, 2, &ignored);
   else good = am_vcall(vm, ARG(0).l, "setRect", "(Landroid/graphics/Rect;)V", a, 1, &ignored);
   if (good) (void)dvm_set_field(vm, ARG(0).l, "alpha", "F", (union dvm_value){ .f = (float)(rt_get_int(vm, self, "alpha") ? rt_get_int(vm, self, "alpha") : 255) / 255.f });
   return good;
}

static const struct rt_method rt_x_gradient[] = {
   M("getOutline", "(Landroid/graphics/Outline;)V", x_gradient_getOutline), M_END,
};

/* A view with clipToOutline draws inside its provider's outline (the framework's
 * default provider is BACKGROUND).  Called by the parent with the canvas set up
 * in the child's own coordinates. */
static bool x_view_clip_to_outline(struct dvm *vm, dvm_ref view, dvm_ref canvas)
{
   if (!rt_get_int(vm, view, "clipToOutline")) return true;
   dvm_ref provider = ui_obj(vm, view, "outlineProvider", "Landroid/view/ViewOutlineProvider;");
   if (!provider) {
      union dvm_value v = { 0 };
      (void)dvm_get_static(vm, dvm_find_class(vm, "android/view/ViewOutlineProvider"), "BACKGROUND",
                           "Landroid/view/ViewOutlineProvider;", &v);
      provider = v.l;
   }
   if (!provider) return true;
   dvm_ref outline = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/Outline"));
   union dvm_value ignored = { 0 };
   if (!outline || !am_vcall(vm, outline, "<init>", "()V", NULL, 0, &ignored)) return false;
   dvm_pin(vm, outline);
   const union dvm_value args[2] = { { .l = view }, { .l = outline } };
   bool good = am_vcall(vm, provider, "getOutline", "(Landroid/view/View;Landroid/graphics/Outline;)V", args, 2, &ignored);
   if (good) good = x_clip_to_outline(vm, canvas, outline);
   dvm_unpin(vm, outline);
   return good;
}

/* RenderNode's outline state. */
static const struct rt_field rt_x_render_node_fields[] = {
   { "outline", "Landroid/graphics/Outline;" }, { "clipToOutline", "Z" },
   { "hasOverlappingRendering", "Z" }, { "usesCompositingLayer", "Z" }, F_END
};

static bool x_rn_set(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   union dvm_value old = { 0 };
   const char *field = !strcmp(name, "setOutline") ? "outline" : !strcmp(name, "setClipToOutline") ? "clipToOutline" :
                       !strcmp(name, "setHasOverlappingRendering") ? "hasOverlappingRendering" : "usesCompositingLayer";
   if (!strcmp(name, "setOutline")) {
      (void)dvm_get_field(vm, self, "outline", "Landroid/graphics/Outline;", &old);
      /* A copy: the caller reuses its Outline for the next frame. */
      dvm_ref copy = 0;
      if (ARG(0).l) {
         copy = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/Outline"));
         union dvm_value ignored = { 0 }, src = ARG(0);
         if (!copy || !am_vcall(vm, copy, "<init>", "(Landroid/graphics/Outline;)V", &src, 1, &ignored)) return false;
      }
      (void)dvm_set_field(vm, self, "outline", "Landroid/graphics/Outline;", (union dvm_value){ .l = copy });
      RETI(old.l != copy);
   }
   (void)dvm_get_field(vm, self, field, "Z", &old);
   (void)dvm_set_field(vm, self, field, "Z", (union dvm_value){ .i = ARG(0).i != 0 });
   RETI((old.i != 0) != (ARG(0).i != 0));
}

static const struct rt_method rt_x_render_node[] = {
   M("setOutline", "(Landroid/graphics/Outline;)Z", x_rn_set),
   M("setClipToOutline", "(Z)Z", x_rn_set),
   M("setHasOverlappingRendering", "(Z)Z", x_rn_set),
   M("setUseCompositingLayer", "(ZLandroid/graphics/Paint;)Z", x_rn_set),
   M_END,
};

/* --------------------------------------------------------------- Region *
 *
 * A set of integer rectangles.  Rectangles are kept disjoint by splitting on
 * every operation, which makes union, intersect, difference and xor the same
 * walk with a different keep rule. */

struct x_rect { int32_t l, t, r, b; };
struct x_region { struct x_rect *r; size_t n; };

static void x_region_destroy(void *data)
{
   struct x_region *g = data;
   if (!g) return;
   free(g->r);
   free(g);
}

static struct x_region *x_region_of(struct dvm *vm, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) return NULL;
   if (!o->data) {
      o->data = calloc(1, sizeof(struct x_region));
      o->data_destroy = x_region_destroy;
   }
   return o->data;
}

static int x_cmp_i32(const void *a, const void *b)
{
   const int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
   return x < y ? -1 : x > y;
}

static bool x_region_has(const struct x_region *g, int32_t x, int32_t y)
{
   for (size_t i = 0; g && i < g->n; ++i)
      if (x >= g->r[i].l && x < g->r[i].r && y >= g->r[i].t && y < g->r[i].b) return true;
   return false;
}

/* result = a OP b, with `op` the Region.Op ordinal: 0 DIFFERENCE, 1 INTERSECT,
 * 2 UNION, 3 XOR, 4 REVERSE_DIFFERENCE, 5 REPLACE. */
static bool x_region_combine(struct x_region *out, const struct x_region *a, const struct x_region *b, int op)
{
   if (op == 5) {                                /* REPLACE */
      struct x_rect *copy = b->n ? malloc(b->n * sizeof *copy) : NULL;
      if (b->n && !copy) return false;
      if (b->n) memcpy(copy, b->r, b->n * sizeof *copy);
      free(out->r); out->r = copy; out->n = b->n;
      return true;
   }
   const size_t cap = (a->n + b->n) * 2 + 2;
   int32_t *xs = malloc(cap * sizeof *xs), *ys = malloc(cap * sizeof *ys);
   if (!xs || !ys) { free(xs); free(ys); return false; }
   size_t nx = 0, ny = 0;
   for (size_t i = 0; i < a->n; ++i) { xs[nx++] = a->r[i].l; xs[nx++] = a->r[i].r; ys[ny++] = a->r[i].t; ys[ny++] = a->r[i].b; }
   for (size_t i = 0; i < b->n; ++i) { xs[nx++] = b->r[i].l; xs[nx++] = b->r[i].r; ys[ny++] = b->r[i].t; ys[ny++] = b->r[i].b; }
   qsort(xs, nx, sizeof *xs, x_cmp_i32);
   qsort(ys, ny, sizeof *ys, x_cmp_i32);
   struct x_rect *res = NULL;
   size_t nres = 0, rcap = 0;
   for (size_t j = 0; j + 1 < ny; ++j) {
      if (ys[j] == ys[j + 1]) continue;
      for (size_t i = 0; i + 1 < nx; ++i) {
         if (xs[i] == xs[i + 1]) continue;
         const bool ia = x_region_has(a, xs[i], ys[j]), ib = x_region_has(b, xs[i], ys[j]);
         const bool keep = op == 0 ? ia && !ib : op == 1 ? ia && ib : op == 2 ? ia || ib : op == 3 ? ia != ib : ib && !ia;
         if (!keep) continue;
         if (nres == rcap) {
            rcap = rcap ? rcap * 2 : 16;
            struct x_rect *grown = realloc(res, rcap * sizeof *grown);
            if (!grown) { free(res); free(xs); free(ys); return false; }
            res = grown;
         }
         /* Extend the previous cell in the row when they touch, to keep the set small. */
         if (nres && res[nres - 1].t == ys[j] && res[nres - 1].b == ys[j + 1] && res[nres - 1].r == xs[i])
            res[nres - 1].r = xs[i + 1];
         else res[nres++] = (struct x_rect){ xs[i], ys[j], xs[i + 1], ys[j + 1] };
      }
   }
   free(xs); free(ys);
   free(out->r);
   out->r = res; out->n = nres;
   return true;
}

static bool x_region_rect_of(struct dvm *vm, dvm_ref rect, struct x_rect *r)
{
   if (!rect) return false;
   *r = (struct x_rect){ rt_get_int(vm, rect, "left"), rt_get_int(vm, rect, "top"),
                         rt_get_int(vm, rect, "right"), rt_get_int(vm, rect, "bottom") };
   return true;
}

static bool x_region_set_rect(struct x_region *g, struct x_rect r)
{
   free(g->r); g->r = NULL; g->n = 0;
   if (r.l >= r.r || r.t >= r.b) return true;
   g->r = malloc(sizeof *g->r);
   if (!g->r) return false;
   g->r[0] = r; g->n = 1;
   return true;
}

static bool x_region_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)out;
   struct x_region *g = x_region_of(vm, self);
   if (!g) return false;
   const char *sig = builtin_sig();
   bool good = true;
   struct x_rect r;
   if (nargs == 4) good = x_region_set_rect(g, (struct x_rect){ ARG(0).i, ARG(1).i, ARG(2).i, ARG(3).i });
   else if (nargs == 1 && strstr(sig, "Rect;")) { if (x_region_rect_of(vm, ARG(0).l, &r)) good = x_region_set_rect(g, r); }
   else if (nargs == 1 && ARG(0).l) {
      struct x_region *o = x_region_of(vm, ARG(0).l);
      static const struct x_region empty = { 0 };
      good = o && x_region_combine(g, &empty, o, 5);
   }
   if (!good) { dvm__throw(vm, "java/lang/OutOfMemoryError", "Region"); return false; }
   RETV();
}

/* op / union / intersect / xor / difference against a Rect, four ints or a Region. */
static bool x_region_op(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   const char *name = builtin_name(), *sig = builtin_sig();
   struct x_region *g = x_region_of(vm, self);
   if (!g) return false;
   struct x_region operand = { 0 }, scratch = { 0 };
   const struct x_region *b = &operand;
   int op;
   int next = 0;
   if (!strcmp(name, "op")) {
      /* op(Rect|Region|IIII, Region.Op), or the (Rect, Region, Op) form. */
      op = rt_get_int(vm, args[nargs - 1].l, "ordinal");
      next = 0;
   } else {
      op = !strcmp(name, "union") ? 2 : !strcmp(name, "xor") ? 3 : !strcmp(name, "intersect") ? 1 : 0;
      if (!strcmp(name, "set")) op = 5;
   }
   bool good = true;
   struct x_rect r;
   if (!strncmp(sig, "(IIII", 5)) good = x_region_set_rect(&operand, (struct x_rect){ ARG(next).i, ARG(next + 1).i, ARG(next + 2).i, ARG(next + 3).i });
   else if (!strncmp(sig, "(Landroid/graphics/Rect;", 24)) {
      if (!x_region_rect_of(vm, ARG(next).l, &r)) { dvm__throw(vm, "java/lang/NullPointerException", "r"); return false; }
      good = x_region_set_rect(&operand, r);
      if (good && !strcmp(name, "op") && strstr(sig, "Landroid/graphics/Region;")) {
         /* (Rect, Region, Op): this = rect OP region. */
         struct x_region *other = x_region_of(vm, ARG(1).l);
         if (!other) { free(operand.r); return false; }
         good = x_region_combine(&scratch, &operand, other, op);
         if (good) { free(g->r); g->r = scratch.r; g->n = scratch.n; scratch.r = NULL; }
         free(operand.r); free(scratch.r);
         if (!good) { dvm__throw(vm, "java/lang/OutOfMemoryError", "Region"); return false; }
         RETI(g->n != 0);
      }
   } else {
      struct x_region *other = ARG(next).l ? x_region_of(vm, ARG(next).l) : NULL;
      if (!other) { dvm__throw(vm, "java/lang/NullPointerException", "region"); return false; }
      b = other;
   }
   if (good) good = x_region_combine(g, g, b, op);
   free(operand.r);
   if (!good) { dvm__throw(vm, "java/lang/OutOfMemoryError", "Region"); return false; }
   RETI(g->n != 0);
}

static bool x_region_query(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   struct x_region *g = x_region_of(vm, self);
   if (!g) return false;
   if (!strcmp(name, "isEmpty")) RETI(g->n == 0);
   if (!strcmp(name, "isRect")) RETI(g->n == 1);
   if (!strcmp(name, "setEmpty")) { free(g->r); g->r = NULL; g->n = 0; RETV(); }
   if (!strcmp(name, "contains")) RETI(x_region_has(g, ARG(0).i, ARG(1).i));
   if (!strcmp(name, "translate")) { for (size_t i = 0; i < g->n; ++i) { g->r[i].l += ARG(0).i; g->r[i].r += ARG(0).i; g->r[i].t += ARG(1).i; g->r[i].b += ARG(1).i; } RETV(); }
   /* getBounds() / getBounds(Rect): the box around every rectangle. */
   struct x_rect box = { 0, 0, 0, 0 };
   for (size_t i = 0; i < g->n; ++i) {
      if (!i) box = g->r[i];
      else { if (g->r[i].l < box.l) box.l = g->r[i].l; if (g->r[i].t < box.t) box.t = g->r[i].t;
             if (g->r[i].r > box.r) box.r = g->r[i].r; if (g->r[i].b > box.b) box.b = g->r[i].b; }
   }
   dvm_ref rect = nargs && ARG(0).l ? ARG(0).l : dvm_new_object(vm, dvm_find_class(vm, "android/graphics/Rect"));
   if (!rect) return false;
   rt_set_int(vm, rect, "left", box.l); rt_set_int(vm, rect, "top", box.t);
   rt_set_int(vm, rect, "right", box.r); rt_set_int(vm, rect, "bottom", box.b);
   if (nargs) RETI(g->n != 0);
   RETL(rect);
}

static const struct rt_method rt_x_region[] = {
   M("<init>", "()V", x_region_init), M("<init>", "(Landroid/graphics/Region;)V", x_region_init),
   M("<init>", "(Landroid/graphics/Rect;)V", x_region_init), M("<init>", "(IIII)V", x_region_init),
   M("set", "(Landroid/graphics/Region;)Z", x_region_op), M("set", "(Landroid/graphics/Rect;)Z", x_region_op),
   M("set", "(IIII)Z", x_region_op),
   M("union", "(Landroid/graphics/Rect;)Z", x_region_op), M("xor", "(Landroid/graphics/Rect;)Z", x_region_op),
   M("intersect", "(Landroid/graphics/Rect;)Z", x_region_op), M("intersect", "(IIII)Z", x_region_op),
   M("op", "(Landroid/graphics/Rect;Landroid/graphics/Region$Op;)Z", x_region_op),
   M("op", "(IIIILandroid/graphics/Region$Op;)Z", x_region_op),
   M("op", "(Landroid/graphics/Region;Landroid/graphics/Region$Op;)Z", x_region_op),
   M("op", "(Landroid/graphics/Rect;Landroid/graphics/Region;Landroid/graphics/Region$Op;)Z", x_region_op),
   M("isEmpty", "()Z", x_region_query), M("isRect", "()Z", x_region_query),
   M("setEmpty", "()V", x_region_query), M("contains", "(II)Z", x_region_query),
   M("translate", "(II)V", x_region_query),
   M("getBounds", "()Landroid/graphics/Rect;", x_region_query), M("getBounds", "(Landroid/graphics/Rect;)Z", x_region_query),
   M_END,
};

/* -------------------------------------------------------------- Drawables *
 *
 * Tinting is a colour filter the drawable applies when it draws: the tint
 * colour, composited by the tint mode (SRC_IN unless the caller says so). */

static const struct rt_field rt_x_drawable_tint_fields[] = {
   { "tintList", "Landroid/content/res/ColorStateList;" }, { "tintColor", "I" },
   { "tintMode", "Landroid/graphics/PorterDuff$Mode;" }, { "tintSet", "Z" }, F_END
};

static bool x_drawable_apply_tint(struct dvm *vm, dvm_ref self)
{
   union dvm_value set = { 0 }, list = { 0 }, color = { 0 }, mode = { 0 };
   (void)dvm_get_field(vm, self, "tintSet", "Z", &set);
   if (!set.i) return true;
   (void)dvm_get_field(vm, self, "tintList", "Landroid/content/res/ColorStateList;", &list);
   (void)dvm_get_field(vm, self, "tintMode", "Landroid/graphics/PorterDuff$Mode;", &mode);
   union dvm_value filter = { 0 }, ignored = { 0 };
   if (list.l) {
      union dvm_value def = { 0 };
      (void)dvm_get_field(vm, list.l, "color", "I", &def);
      static const int32_t enabled[1] = { 0x0101009e /* state_enabled */ };
      color.i = color_state_lookup(vm, list.l, enabled, 1, def.i);
   } else {
      (void)dvm_get_field(vm, self, "tintColor", "I", &color);
   }
   if (!mode.l) (void)dvm_get_static(vm, dvm_find_class(vm, "android/graphics/PorterDuff$Mode"), "SRC_IN",
                                     "Landroid/graphics/PorterDuff$Mode;", &mode);
   if (list.l || color.i) {
      filter.l = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/PorterDuffColorFilter"));
      const union dvm_value init[2] = { color, mode };
      if (!filter.l || !am_vcall(vm, filter.l, "<init>", "(ILandroid/graphics/PorterDuff$Mode;)V", init, 2, &ignored)) return false;
   }
   return am_vcall(vm, self, "setColorFilter", "(Landroid/graphics/ColorFilter;)V", &filter, 1, &ignored);
}

static bool x_drawable_tint(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   const char *name = builtin_name();
   (void)dvm_set_field(vm, self, "tintSet", "Z", (union dvm_value){ .i = 1 });
   if (!strcmp(name, "setTint")) {
      (void)dvm_set_field(vm, self, "tintList", "Landroid/content/res/ColorStateList;", (union dvm_value){ 0 });
      (void)dvm_set_field(vm, self, "tintColor", "I", ARG(0));
   } else if (!strcmp(name, "setTintList")) {
      (void)dvm_set_field(vm, self, "tintList", "Landroid/content/res/ColorStateList;", ARG(0));
      if (!ARG(0).l) (void)dvm_set_field(vm, self, "tintColor", "I", (union dvm_value){ 0 });
   } else {
      (void)dvm_set_field(vm, self, "tintMode", "Landroid/graphics/PorterDuff$Mode;", ARG(0));
   }
   if (!x_drawable_apply_tint(vm, self)) return false;
   ui_invalidate();
   RETV();
}

static bool x_drawable_getOutline(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out);

static const struct rt_method rt_x_drawable[] = {
   M("getOutline", "(Landroid/graphics/Outline;)V", x_drawable_getOutline),
   M("setTint", "(I)V", x_drawable_tint),
   M("setTintList", "(Landroid/content/res/ColorStateList;)V", x_drawable_tint),
   M("setTintMode", "(Landroid/graphics/PorterDuff$Mode;)V", x_drawable_tint),
   /* A drawable animates through state changes; the base class has nothing to
    * finish, and the animated ones override it. */
   M("jumpToCurrentState", "()V", empty_void),
   M_END,
};

#endif
