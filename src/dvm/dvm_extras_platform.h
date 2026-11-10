/* MPL-2.0. Platform methods that were answered by the "not implemented" stub:
 * each is the real behaviour for this device, not a placeholder.  Included
 * once, before the rt_extras table, after the graphics and UI runtime. */

/* ---- Matrix.setRectToRect (Skia's SkMatrix::setRectToRect) -------------- */
static bool x_rect_floats(struct dvm *vm, dvm_ref r, float v[4])
{
  static const char *const edges[4] = { "left", "top", "right", "bottom" };
  if (!r) {
    dvm__throw(vm, "java/lang/NullPointerException", "rect");
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    union dvm_value f = { 0 };
    (void)dvm_get_field(vm, r, edges[i], "F", &f);
    v[i] = f.f;
  }
  return true;
}

static bool x_matrix_set_rect_to_rect(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                      int nargs, union dvm_value *out)
{
  (void)nargs;
  float src[4], dst[4];
  if (!x_rect_floats(vm, ARG(0).l, src) || !x_rect_floats(vm, ARG(1).l, dst)) return false;
  float *m = matrix_values(vm, self);
  if (!m) return false;
  const float sw = src[2] - src[0], sh = src[3] - src[1], dw = dst[2] - dst[0], dh = dst[3] - dst[1];
  if (!(sw > 0) || !(sh > 0)) {          /* an empty source leaves the identity */
    mat_identity(m);
    RETI(0);
  }
  if (!(dw > 0) || !(dh > 0)) {          /* an empty target collapses everything */
    memset(m, 0, 9 * sizeof *m);
    m[8] = 1.0f;
    RETI(1);
  }
  union dvm_value name = { 0 };
  const char *fit = ARG(2).l && dvm_get_field(vm, ARG(2).l, "name", "Ljava/lang/String;", &name) && name.l
                    ? dvm_string_utf8(vm, name.l) : "FILL";
  float sx = dw / sw, sy = dh / sh;
  bool x_larger = false;
  if (strcmp(fit, "FILL")) {
    if (sx > sy) {
      x_larger = true;
      sx = sy;
    } else sy = sx;
  }
  float tx = dst[0] - src[0] * sx, ty = dst[1] - src[1] * sy;
  if (!strcmp(fit, "CENTER") || !strcmp(fit, "END")) {
    float diff = x_larger ? dw - sw * sx : dh - sh * sy;
    if (!strcmp(fit, "CENTER")) diff /= 2;
    if (x_larger) tx += diff;
    else ty += diff;
  }
  const float values[9] = { sx, 0, tx, 0, sy, ty, 0, 0, 1 };
  memcpy(m, values, sizeof values);
  ui_invalidate();
  RETI(1);
}
static const struct rt_method rt_x_matrix_rect[] = {
  M("setRectToRect", "(Landroid/graphics/RectF;Landroid/graphics/RectF;Landroid/graphics/Matrix$ScaleToFit;)Z",
    x_matrix_set_rect_to_rect),
  M_END,
};

/* ---- ColorStateList.isStateful ------------------------------------------ */
static bool x_csl_is_stateful(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value specs = { 0 };
  (void)dvm_get_field(vm, self, "stateSpecs", "[[I", &specs);
  RETI(specs.l && dvm_array_length(vm, specs.l) > 1);
}
static const struct rt_method rt_x_color_state_more[] = {
  M("isStateful", "()Z", x_csl_is_stateful),
  M_END,
};

/* ---- Paint: letter spacing, shadow layer, linear text ------------------- */
static bool x_paint_letter_spacing(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
  (void)nargs;
  if (!strcmp(builtin_name(), "getLetterSpacing")) {
    union dvm_value v = { 0 };
    (void)dvm_get_field(vm, self, "letterSpacing", "F", &v);
    RETF(v.f);
  }
  (void)out;
  (void)dvm_set_field(vm, self, "letterSpacing", "F", ARG(0));
  RETV();
}
static bool x_paint_shadow(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
  (void)nargs; (void)out;
  (void)dvm_set_field(vm, self, "shadowRadius", "F", ARG(0));
  (void)dvm_set_field(vm, self, "shadowDx", "F", ARG(1));
  (void)dvm_set_field(vm, self, "shadowDy", "F", ARG(2));
  (void)dvm_set_field(vm, self, "shadowColor", "I", ARG(3));
  RETV();
}
static bool x_paint_linear_text(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  (void)nargs; (void)out;
  union dvm_value flags = { 0 };
  (void)dvm_get_field(vm, self, "flags", "I", &flags);
  flags.i = ARG(0).i ? flags.i | 0x40 : flags.i & ~0x40;
  (void)dvm_set_field(vm, self, "flags", "I", flags);
  RETV();
}
static const struct rt_method rt_x_paint_more[] = {
  M("setLetterSpacing", "(F)V", x_paint_letter_spacing),
  M("getLetterSpacing", "()F", x_paint_letter_spacing),
  M("setShadowLayer", "(FFFI)V", x_paint_shadow),
  M("setLinearText", "(Z)V", x_paint_linear_text),
  M_END,
};

/* ---- TextView: compound drawable padding, auto-size configuration ------- */
static bool x_text_drawable_padding(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
  (void)nargs;
  if (!strcmp(builtin_name(), "getCompoundDrawablePadding")) {
    union dvm_value v = { 0 };
    (void)dvm_get_field(vm, self, "drawablePadding", "I", &v);
    RETI(v.i);
  }
  (void)out;
  (void)dvm_set_field(vm, self, "drawablePadding", "I", ARG(0));
  ui_layout_changed();
  ui_invalidate();
  RETV();
}
static bool x_text_autosize_step(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, self, "autoSizeStep", "I", &v);
  RETI(v.i ? v.i : -1);
}
static bool x_text_autosize_config(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
  (void)nargs; (void)out;
  (void)dvm_set_field(vm, self, "autoSizeMin", "I", ARG(0));
  (void)dvm_set_field(vm, self, "autoSizeMax", "I", ARG(1));
  (void)dvm_set_field(vm, self, "autoSizeStep", "I", ARG(2));
  ui_layout_changed();
  ui_invalidate();
  RETV();
}
static const struct rt_method rt_x_text_view_more[] = {
  M("setCompoundDrawablePadding", "(I)V", x_text_drawable_padding),
  M("getCompoundDrawablePadding", "()I", x_text_drawable_padding),
  M("getAutoSizeStepGranularity", "()I", x_text_autosize_step),
  M("setAutoSizeTextTypeUniformWithConfiguration", "(IIII)V", x_text_autosize_config),
  M_END,
};

/* ---- Intent, WifiManager, ActivityManager, Debug, Activity -------------- */
static bool x_intent_data_string(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value data = { 0 };
  if (!am_vcall(vm, self, "getData", "()Landroid/net/Uri;", NULL, 0, &data)) return false;
  if (!data.l) RETL(0);
  return am_vcall(vm, data.l, "toString", "()Ljava/lang/String;", NULL, 0, out);
}
static const struct rt_method rt_x_intent_more[] = {
  M("getDataString", "()Ljava/lang/String;", x_intent_data_string),
  M("getClipData", "()Landroid/content/ClipData;", ret_null),   /* none is ever attached */
  M("getSelector", "()Landroid/content/Intent;", ret_null),
  M_END,
};
static const struct rt_method rt_x_wifi_more[] = {
  M("isWifiEnabled", "()Z", ret_true),
  M_END,
};
static bool x_current_user(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs;
  RETI(0);   /* the owner: this device has one user */
}
static const struct rt_method rt_x_activity_manager_user[] = {
  SM("getCurrentUser", "()I", x_current_user),
  M_END,
};
static bool x_thread_cpu_ns(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs;
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  RETJ((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec);
}
static const struct rt_method rt_x_debug_cpu[] = {
  SM("threadCpuTimeNanos", "()J", x_thread_cpu_ns),
  M_END,
};
static const struct rt_method rt_x_activity_child[] = {
  M("isChild", "()Z", ret_false),   /* activities here are never embedded in an ActivityGroup */
  M_END,
};
static const struct rt_method rt_x_vm_policy_builder[] = {
  M("<init>", "(Landroid/os/StrictMode$VmPolicy;)V", empty_void),
  M_END,
};
