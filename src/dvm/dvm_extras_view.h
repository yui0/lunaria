/* android.view / android.widget additions.
 *
 * Methods merged into the builtin View, ViewGroup and widget tables through
 * rt_extras (see dvm_runtime_define).  Included from dvm_runtime.c after the
 * UI layer, so the layout helpers (ui_node, ui_children, ...) are in scope. */

#ifndef DVM_EXTRAS_VIEW_H
#define DVM_EXTRAS_VIEW_H

/* A small helper for the many "store the value, answer it back" settings. */
static bool x_flag_set(struct dvm *vm, dvm_ref self, const char *field, int32_t v)
{
   return dvm_set_field(vm, self, field, "Z", (union dvm_value){ .i = v != 0 });
}

/* ------------------------------------------------- View state save/restore *
 *
 * Fragments, ViewPager2 and RecyclerView keep a view's state across a
 * detach (a tab switch, a back-stack pop) by asking the hierarchy to write it
 * into a SparseArray keyed by view id and to read it back later.  A view that
 * has state overrides onSaveInstanceState()/onRestoreInstanceState(); the
 * base class contributes the id-keyed plumbing and an empty state. */

static dvm_ref x_empty_saved_state(struct dvm *vm)
{
   union dvm_value v = { 0 };
   (void)dvm_get_static(vm, dvm_find_class(vm, "android/view/AbsSavedState"),
                        "EMPTY_STATE", "Landroid/view/AbsSavedState;", &v);
   return v.l;
}

static bool x_view_onSaveInstanceState(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                       int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(x_empty_saved_state(vm));
}

static bool x_view_dispatchSave(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)out; (void)nargs;
   union dvm_value id = { .i = -1 }, disabled = { 0 };
   (void)dvm_get_field(vm, self, "id", "I", &id);
   (void)dvm_get_field(vm, self, "saveDisabled", "Z", &disabled);
   if (id.i != -1 && !disabled.i && ARG(0).l) {
      union dvm_value state = { 0 }, ignored = { 0 };
      if (!am_vcall(vm, self, "onSaveInstanceState", "()Landroid/os/Parcelable;", NULL, 0, &state)) return false;
      if (state.l) {
         const union dvm_value put[2] = { id, state };
         if (!am_vcall(vm, ARG(0).l, "put", "(ILjava/lang/Object;)V", put, 2, &ignored)) return false;
      }
   }
   RETV();
}

static bool x_view_dispatchRestore(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
   (void)out; (void)nargs;
   union dvm_value id = { .i = -1 };
   (void)dvm_get_field(vm, self, "id", "I", &id);
   if (id.i != -1 && ARG(0).l) {
      union dvm_value state = { 0 }, ignored = { 0 };
      if (!am_vcall(vm, ARG(0).l, "get", "(I)Ljava/lang/Object;", &id, 1, &state)) return false;
      if (state.l && !am_vcall(vm, self, "onRestoreInstanceState", "(Landroid/os/Parcelable;)V", &state, 1, &ignored))
         return false;
   }
   RETV();
}

/* ViewGroup: its own state, then each child that has not opted out. */
static bool x_group_dispatch_state(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
   const bool save = !strcmp(builtin_name(), "dispatchSaveInstanceState");
   if (!(save ? x_view_dispatchSave : x_view_dispatchRestore)(vm, self, args, nargs, out)) return false;
   dvm_ref kids[UI_MAX_KIDS];
   const uint32_t nk = ui_children(vm, self, kids, UI_MAX_KIDS);
   for (uint32_t i = 0; i < nk; ++i) {
      union dvm_value skip = { 0 }, ignored = { 0 };
      (void)dvm_get_field(vm, kids[i], "saveFromParentDisabled", "Z", &skip);
      if (skip.i) continue;
      if (!am_vcall(vm, kids[i], builtin_name(), "(Landroid/util/SparseArray;)V", args, 1, &ignored)) return false;
   }
   RETV();
}

static bool x_view_hierarchy_state(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   const bool save = !strcmp(builtin_name(), "saveHierarchyState");
   union dvm_value ignored = { 0 };
   if (!am_vcall(vm, self, save ? "dispatchSaveInstanceState" : "dispatchRestoreInstanceState",
                 "(Landroid/util/SparseArray;)V", args, 1, &ignored)) return false;
   RETV();
}

/* AbsSavedState / View.BaseSavedState: the base of every view's saved state.
 * It carries the superclass's state so a subclass's can nest around it. */
static const struct rt_field rt_x_saved_state_fields[] = {
   { "superState", "Landroid/os/Parcelable;" }, F_END
};
static const struct rt_field rt_x_saved_state_sfields[] = {
   { "EMPTY_STATE", "Landroid/view/AbsSavedState;" }, F_END
};

static bool x_saved_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)out;
   dvm_ref super = x_empty_saved_state(vm);
   if (!strstr(builtin_sig(), "Parcel;") && !strstr(builtin_sig(), "Parcel;Ljava")) {
      if (!ARG(0).l) {
         dvm__throw(vm, "java/lang/IllegalArgumentException", "superState must not be null");
         return false;
      }
      super = ARG(0).l;
   } else if (ARG(0).l) {
      /* Reading back from a Parcel: the writer put the super state first. */
      union dvm_value read = { 0 }, loader = { .l = nargs > 1 ? ARG(1).l : 0 };
      if (dvm_find_method(vm, dvm_object_class(vm, ARG(0).l), "readParcelable",
                          "(Ljava/lang/ClassLoader;)Landroid/os/Parcelable;") &&
          am_vcall(vm, ARG(0).l, "readParcelable", "(Ljava/lang/ClassLoader;)Landroid/os/Parcelable;", &loader, 1, &read) &&
          read.l)
         super = read.l;
   }
   (void)dvm_set_field(vm, self, "superState", "Landroid/os/Parcelable;", (union dvm_value){ .l = super });
   RETV();
}

static bool x_saved_getSuperState(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   return dvm_get_field(vm, self, "superState", "Landroid/os/Parcelable;", out);
}

static bool x_saved_writeToParcel(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   union dvm_value super = { 0 }, ignored = { 0 };
   (void)dvm_get_field(vm, self, "superState", "Landroid/os/Parcelable;", &super);
   if (ARG(0).l && dvm_find_method(vm, dvm_object_class(vm, ARG(0).l), "writeParcelable",
                                   "(Landroid/os/Parcelable;I)V")) {
      const union dvm_value w[2] = { super, ARG(1) };
      if (!am_vcall(vm, ARG(0).l, "writeParcelable", "(Landroid/os/Parcelable;I)V", w, 2, &ignored)) return false;
   }
   RETV();
}

static const struct rt_method rt_x_saved_state[] = {
   M("<init>", "(Landroid/os/Parcelable;)V", x_saved_init),
   M("<init>", "(Landroid/os/Parcel;)V", x_saved_init),
   M("<init>", "(Landroid/os/Parcel;Ljava/lang/ClassLoader;)V", x_saved_init),
   M("getSuperState", "()Landroid/os/Parcelable;", x_saved_getSuperState),
   M("describeContents", "()I", ret_zero),
   M("writeToParcel", "(Landroid/os/Parcel;I)V", x_saved_writeToParcel),
   M_END,
};

/* ------------------------------------------------------------ Nested scroll *
 *
 * ViewParent's nested-scrolling half.  A plain ViewGroup is not a nested
 * scrolling parent: it declines the gesture (false) and ignores the rest, which
 * is what the framework's own defaults do. */

static bool x_group_declines(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   RETI(0);
}

static const struct rt_method rt_x_view_group[] = {
   M("onStartNestedScroll", "(Landroid/view/View;Landroid/view/View;I)Z", x_group_declines),
   M("onNestedScrollAccepted", "(Landroid/view/View;Landroid/view/View;I)V", empty_void),
   M("onStopNestedScroll", "(Landroid/view/View;)V", empty_void),
   M("onNestedScroll", "(Landroid/view/View;IIII)V", empty_void),
   M("onNestedPreScroll", "(Landroid/view/View;II[I)V", empty_void),
   M("onNestedFling", "(Landroid/view/View;FFZ)Z", x_group_declines),
   M("onNestedPreFling", "(Landroid/view/View;FF)Z", x_group_declines),
   M("getNestedScrollAxes", "()I", ret_zero),
   M("dispatchSaveInstanceState", "(Landroid/util/SparseArray;)V", x_group_dispatch_state),
   M("dispatchRestoreInstanceState", "(Landroid/util/SparseArray;)V", x_group_dispatch_state),
   M_END,
};

/* ------------------------------------------------------------- Scrolling *
 *
 * View.canScrollVertically()/Horizontally() are answered from the three
 * compute*Scroll*() hooks, which a scrolling view overrides: offset is where
 * the viewport is, range how much content there is, extent how much of it the
 * viewport shows. */

static int32_t x_view_size(struct dvm *vm, dvm_ref view, bool height)
{
   const struct ui_node *n = ui_node(vm, view);
   if (n) return height ? n->h : n->w;
   return ui_int(vm, view, height ? "bottom" : "right") - ui_int(vm, view, height ? "top" : "left");
}

static bool x_view_scroll_compute(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *name = builtin_name();
   const bool vertical = strstr(name, "Vertical") != NULL;
   if (strstr(name, "Offset")) RETI(ui_int(vm, self, vertical ? "scrollY" : "scrollX"));
   RETI(x_view_size(vm, self, vertical));       /* range and extent: the view's own size */
}

static bool x_view_canScroll(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)nargs;
   const bool vertical = !strcmp(builtin_name(), "canScrollVertically");
   union dvm_value offset = { 0 }, range = { 0 }, extent = { 0 };
   if (!am_vcall(vm, self, vertical ? "computeVerticalScrollOffset" : "computeHorizontalScrollOffset", "()I", NULL, 0, &offset) ||
       !am_vcall(vm, self, vertical ? "computeVerticalScrollRange" : "computeHorizontalScrollRange", "()I", NULL, 0, &range) ||
       !am_vcall(vm, self, vertical ? "computeVerticalScrollExtent" : "computeHorizontalScrollExtent", "()I", NULL, 0, &extent))
      return false;
   const int32_t scrollable = range.i - extent.i;
   if (scrollable == 0) RETI(0);
   RETI(ARG(0).i < 0 ? offset.i > 0 : offset.i < scrollable - 1);
}

/* ScrollView: the content is the single child plus the padding around it. */
static bool x_scrollview_range(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const bool vertical = !strcmp(builtin_name(), "computeVerticalScrollRange");
   dvm_ref kids[UI_MAX_KIDS];
   const uint32_t nk = ui_children(vm, self, kids, UI_MAX_KIDS);
   int32_t pad[4];
   ui_pad(vm, self, pad);
   const int32_t own = x_view_size(vm, self, vertical);
   if (!nk) RETI(own);
   struct ui_lp lp;
   ui_read_lp(vm, kids[0], &lp);
   const int32_t content = x_view_size(vm, kids[0], vertical) +
      (vertical ? lp.mt + lp.mb + pad[1] + pad[3] : lp.ml + lp.mr + pad[0] + pad[2]);
   RETI(content > own ? content : own);
}

/* ViewGroup.onInterceptTouchEvent / View.onTouchEvent of ScrollView and
 * HorizontalScrollView.  A press that travels past the touch slop along the
 * view's axis becomes this view's drag: the child that had the press gets a
 * cancel (view_dispatch_touch does that when this returns true) and every
 * following sample scrolls the content by how far the finger moved. */
static bool x_scroll_touch(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   luna_touch_event sample;
   if (!nargs || !dvm_motion_event_read(vm, ARG(0).l, &sample)) RETI(0);
   const bool intercept = !strcmp(builtin_name(), "onInterceptTouchEvent");
   const bool vertical = !ui_is_a(vm, self, "android/widget/HorizontalScrollView");
   const int action = sample.action & 255;
   const float pos = vertical ? sample.y : sample.x;
   union dvm_value dragging = { 0 };
   (void)dvm_get_field(vm, self, "scrollDragging", "Z", &dragging);
   if (action == 0) {
      (void)dvm_set_field(vm, self, "scrollDownX", "F", (union dvm_value){ .f = sample.x });
      (void)dvm_set_field(vm, self, "scrollDownY", "F", (union dvm_value){ .f = sample.y });
      (void)dvm_set_field(vm, self, "scrollLastX", "F", (union dvm_value){ .f = sample.x });
      (void)dvm_set_field(vm, self, "scrollLastY", "F", (union dvm_value){ .f = sample.y });
      (void)dvm_set_field(vm, self, "scrollDragging", "Z", (union dvm_value){ .i = 0 });
      RETI(!intercept && ui_scroll_max(vm, self, vertical) > 0);
   }
   if (action == 1 || action == 3) {
      (void)dvm_set_field(vm, self, "scrollDragging", "Z", (union dvm_value){ .i = 0 });
      RETI(!intercept && dragging.i);
   }
   if (action != 2) RETI(0);
   union dvm_value down = { 0 }, last = { 0 };
   (void)dvm_get_field(vm, self, vertical ? "scrollDownY" : "scrollDownX", "F", &down);
   (void)dvm_get_field(vm, self, vertical ? "scrollLastY" : "scrollLastX", "F", &last);
   if (intercept) {
      if (dragging.i) RETI(1);
      if (ui_scroll_max(vm, self, vertical) <= 0) RETI(0);
      if (fabsf(pos - down.f) <= 8.0f * rt_density()) RETI(0);
      (void)dvm_set_field(vm, self, "scrollDragging", "Z", (union dvm_value){ .i = 1 });
      (void)dvm_set_field(vm, self, vertical ? "scrollLastY" : "scrollLastX", "F", (union dvm_value){ .f = pos });
      RETI(1);
   }
   /* The finger moved (last - pos) pixels the other way: the content follows it. */
   (void)ui_scroll_by(vm, self, vertical, (int32_t)lroundf(last.f - pos));
   (void)dvm_set_field(vm, self, vertical ? "scrollLastY" : "scrollLastX", "F", (union dvm_value){ .f = pos });
   (void)dvm_set_field(vm, self, "scrollDragging", "Z", (union dvm_value){ .i = 1 });
   RETI(1);
}

static const struct rt_method rt_x_scroll_view[] = {
   M("onInterceptTouchEvent", "(Landroid/view/MotionEvent;)Z", x_scroll_touch),
   M("onTouchEvent", "(Landroid/view/MotionEvent;)Z", x_scroll_touch),
   M("computeVerticalScrollRange", "()I", x_scrollview_range),
   M("computeHorizontalScrollRange", "()I", x_scrollview_range),
   M_END,
};

/* getGlobalVisibleRect(): the part of the view that shows on screen, in the
 * coordinates of the window's root.  Each ancestor shifts it by the child's
 * position less its own scroll and clips it to the ancestor's bounds; a view
 * that is detached, or clipped away entirely, has none. */
static bool x_view_globalVisibleRect(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                     int nargs, union dvm_value *out)
{
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "r"); return false; }
   const struct ui_node *n = ui_node(vm, self);
   if (!n || ui_gone(vm, self)) RETI(0);
   int32_t l = 0, t = 0, r = n->w, b = n->h;
   dvm_ref child = self;
   int32_t ox = 0, oy = 0;              /* where the view's origin lands in the root */
   for (int depth = 0; depth < 256; ++depth) {
      dvm_ref parent = ui_obj(vm, child, "parent", "Landroid/view/ViewParent;");
      const struct ui_node *pn = parent ? ui_node(vm, parent) : NULL;
      const struct ui_node *cn = ui_node(vm, child);
      if (!pn || !cn) break;
      const int32_t dx = cn->x - ui_int(vm, parent, "scrollX"), dy = cn->y - ui_int(vm, parent, "scrollY");
      l += dx; r += dx; t += dy; b += dy;
      ox += dx; oy += dy;
      if (l < 0) l = 0;
      if (t < 0) t = 0;
      if (r > pn->w) r = pn->w;
      if (b > pn->h) b = pn->h;
      if (r <= l || b <= t) RETI(0);
      child = parent;
   }
   rt_set_int(vm, ARG(0).l, "left", l);  rt_set_int(vm, ARG(0).l, "top", t);
   rt_set_int(vm, ARG(0).l, "right", r); rt_set_int(vm, ARG(0).l, "bottom", b);
   if (nargs > 1 && ARG(1).l) { rt_set_int(vm, ARG(1).l, "x", ox); rt_set_int(vm, ARG(1).l, "y", oy); }
   RETI(1);
}

/* --------------------------------------------------------- Long click etc. */

static bool x_view_setLongClickListener(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                        int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   (void)dvm_set_field(vm, self, "longClickListener", "Landroid/view/View$OnLongClickListener;", ARG(0));
   /* Giving a view a long-click listener makes it long-clickable. */
   if (ARG(0).l) (void)dvm_set_field(vm, self, "longClickable", "Z", (union dvm_value){ .i = 1 });
   ui_invalidate();
   RETV();
}

static bool x_view_setLongClickable(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   (void)dvm_set_field(vm, self, "longClickable", "Z", (union dvm_value){ .i = ARG(0).i != 0 });
   RETV();
}

static bool x_view_isLongClickable(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "longClickable", "Z", &v);
   RETI(v.i != 0);
}

/* performLongClick(): the listener decides whether the press was handled. */
static bool x_view_performLongClick(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value listener = { 0 }, handled = { 0 };
   (void)dvm_get_field(vm, self, "longClickListener", "Landroid/view/View$OnLongClickListener;", &listener);
   if (listener.l) {
      const union dvm_value arg = { .l = self };
      if (!am_vcall(vm, listener.l, "onLongClick", "(Landroid/view/View;)Z", &arg, 1, &handled)) return false;
   }
   RETI(handled.i != 0);
}

static bool x_view_setLayerType(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   rt_set_int(vm, self, "layerType", ARG(0).i);
   RETV();
}

static bool x_view_getLayerType(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm, self, "layerType"));
}

static bool x_view_setClipToOutline(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   (void)dvm_set_field(vm, self, "clipToOutline", "Z", (union dvm_value){ .i = ARG(0).i != 0 });
   ui_invalidate();
   RETV();
}

static bool x_view_getClipToOutline(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm, self, "clipToOutline") != 0);
}

static bool x_view_uniqueDrawingId(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
   (void)vm; (void)args; (void)nargs;
   RETJ((int64_t)self);        /* stable for the life of the view, and unique among live views */
}

static const struct rt_method rt_x_view[] = {
   M("onSaveInstanceState", "()Landroid/os/Parcelable;", x_view_onSaveInstanceState),
   M("onRestoreInstanceState", "(Landroid/os/Parcelable;)V", empty_void),
   M("dispatchSaveInstanceState", "(Landroid/util/SparseArray;)V", x_view_dispatchSave),
   M("dispatchRestoreInstanceState", "(Landroid/util/SparseArray;)V", x_view_dispatchRestore),
   M("saveHierarchyState", "(Landroid/util/SparseArray;)V", x_view_hierarchy_state),
   M("restoreHierarchyState", "(Landroid/util/SparseArray;)V", x_view_hierarchy_state),
   M("computeVerticalScrollOffset", "()I", x_view_scroll_compute),
   M("computeVerticalScrollRange", "()I", x_view_scroll_compute),
   M("computeVerticalScrollExtent", "()I", x_view_scroll_compute),
   M("computeHorizontalScrollOffset", "()I", x_view_scroll_compute),
   M("computeHorizontalScrollRange", "()I", x_view_scroll_compute),
   M("computeHorizontalScrollExtent", "()I", x_view_scroll_compute),
   M("canScrollVertically", "(I)Z", x_view_canScroll),
   M("canScrollHorizontally", "(I)Z", x_view_canScroll),
   M("getGlobalVisibleRect", "(Landroid/graphics/Rect;)Z", x_view_globalVisibleRect),
   M("getGlobalVisibleRect", "(Landroid/graphics/Rect;Landroid/graphics/Point;)Z", x_view_globalVisibleRect),
   M("setOnLongClickListener", "(Landroid/view/View$OnLongClickListener;)V", x_view_setLongClickListener),
   M("setLongClickable", "(Z)V", x_view_setLongClickable),
   M("isLongClickable", "()Z", x_view_isLongClickable),
   M("performLongClick", "()Z", x_view_performLongClick),
   M("setLayerType", "(ILandroid/graphics/Paint;)V", x_view_setLayerType),
   M("getLayerType", "()I", x_view_getLayerType),
   M("getUniqueDrawingId", "()J", x_view_uniqueDrawingId),
   M("setClipToOutline", "(Z)V", x_view_setClipToOutline),
   M("getClipToOutline", "()Z", x_view_getClipToOutline),
   /* "Show layout bounds" is a developer option this device has off. */
   M("isShowingLayoutBounds", "()Z", ret_false),
   /* The base hook is empty; subclasses override it and call it through super. */
   M("onSizeChanged", "(IIII)V", empty_void),
   M_END,
};


/* -------------------------------------------------------------- ViewConfiguration */

static bool x_vc_doubleTapSlop(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   RETI((int32_t)(100.f * (float)device_density_dpi() / 160.f + .5f));      /* 100 dp */
}

static const struct rt_method rt_x_view_configuration[] = {
   M("getScaledDoubleTapSlop", "()I", x_vc_doubleTapSlop),
   /* A touch device with no hardware keyboard shows no menu shortcut hints. */
   M("shouldShowMenuShortcutsWhenKeyboardPresent", "()Z", ret_false),
   M_END,
};

/* ----------------------------------------------------------- GestureDetector *
 *
 * Turns a stream of MotionEvents into taps, double taps, scrolls, flings and
 * long presses, following the framework's own state machine: the listener is
 * told of a press (onDown) at once, of a tap once the finger lifts inside the
 * touch slop, of scrolls once it leaves it, and of a fling on release at speed.
 * Long press, show-press and the double-tap window are timers on the Handler. */

#define X_TAP_TIMEOUT        100
#define X_LONGPRESS_TIMEOUT  500
#define X_DOUBLE_TAP_TIMEOUT 300
#define X_DOUBLE_TAP_MIN_TIME 40

struct x_gesture {
   dvm_ref listener, double_tap, handler, velocity;
   dvm_ref down, up;                     /* copies of the current down and previous up events */
   dvm_ref t_long, t_show, t_tap;        /* the timer Runnables */
   int touch_slop_sq, double_tap_touch_slop_sq, double_tap_slop_sq, min_fling, max_fling;
   float down_x, down_y, last_x, last_y;
   bool in_long_press, always_in_tap, always_in_bigger_tap, still_down, defer_confirm, double_tapping;
   bool long_press_enabled;
   bool tap_pending, long_pending, show_pending;
};

static void x_gesture_destroy(void *data) { free(data); }

static const struct rt_field rt_x_gesture_timer_fields[] = {
   { "detector", "Landroid/view/GestureDetector;" }, { "kind", "I" }, F_END
};

static struct x_gesture *x_gesture_of(struct dvm *vm, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(vm, self);
   return o ? o->data : NULL;
}

static void x_gesture_pin(struct dvm *vm, dvm_ref *slot, dvm_ref value)
{
   if (value) dvm_pin(vm, value);
   if (*slot) dvm_unpin(vm, *slot);
   *slot = value;
}

static bool x_gesture_call(struct dvm *vm, dvm_ref target, const char *name, const char *sig,
                           const union dvm_value *a, int n, bool *handled)
{
   union dvm_value r = { 0 };
   if (!target || !dvm_find_method(vm, dvm_object_class(vm, target), name, sig)) return true;
   if (!am_vcall(vm, target, name, sig, a, n, &r)) return false;
   if (handled && sig[strlen(sig) - 1] == 'Z') *handled |= r.i != 0;
   return true;
}

static bool x_gesture_remove(struct dvm *vm, struct x_gesture *g, dvm_ref timer, bool *pending)
{
   if (!*pending) return true;
   *pending = false;
   union dvm_value arg = { .l = timer }, ignored = { 0 };
   return am_vcall(vm, g->handler, "removeCallbacks", "(Ljava/lang/Runnable;)V", &arg, 1, &ignored);
}

static bool x_gesture_post(struct dvm *vm, struct x_gesture *g, dvm_ref timer, int64_t delay, bool *pending)
{
   union dvm_value a[2] = { { .l = timer }, { .j = delay } }, ignored = { 0 };
   *pending = true;
   return am_vcall(vm, g->handler, "postDelayed", "(Ljava/lang/Runnable;J)Z", a, 2, &ignored);
}

static bool x_gesture_cancel_taps(struct dvm *vm, struct x_gesture *g)
{
   if (!x_gesture_remove(vm, g, g->t_long, &g->long_pending) ||
       !x_gesture_remove(vm, g, g->t_show, &g->show_pending) ||
       !x_gesture_remove(vm, g, g->t_tap, &g->tap_pending)) return false;
   g->double_tapping = g->always_in_tap = g->always_in_bigger_tap = g->in_long_press = g->defer_confirm = false;
   return true;
}

static bool x_gesture_cancel(struct dvm *vm, struct x_gesture *g)
{
   if (!x_gesture_cancel_taps(vm, g)) return false;
   if (g->velocity) {
      union dvm_value ignored = { 0 };
      if (!am_vcall(vm, g->velocity, "recycle", "()V", NULL, 0, &ignored)) return false;
      x_gesture_pin(vm, &g->velocity, 0);
   }
   g->still_down = false;
   return true;
}

/* The timers: 1 long press, 2 show press, 3 single-tap window. */
static bool x_gesture_timer_run(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   union dvm_value det = { 0 };
   (void)dvm_get_field(vm, self, "detector", "Landroid/view/GestureDetector;", &det);
   struct x_gesture *g = det.l ? x_gesture_of(vm, det.l) : NULL;
   if (!g) RETV();
   const int kind = rt_get_int(vm, self, "kind");
   const union dvm_value ev = { .l = g->down };
   if (kind == 2) {
      g->show_pending = false;
      if (!x_gesture_call(vm, g->listener, "onShowPress", "(Landroid/view/MotionEvent;)V", &ev, 1, NULL)) return false;
   } else if (kind == 1) {
      g->long_pending = false;
      g->tap_pending = false;                     /* a long press supersedes the pending single tap */
      if (g->t_tap) { (void)x_gesture_remove(vm, g, g->t_tap, &(bool){ true }); }
      g->defer_confirm = false;
      g->in_long_press = true;
      if (!x_gesture_call(vm, g->listener, "onLongPress", "(Landroid/view/MotionEvent;)V", &ev, 1, NULL)) return false;
   } else {
      g->tap_pending = false;
      if (g->double_tap) {
         if (!g->still_down) {
            if (!x_gesture_call(vm, g->double_tap, "onSingleTapConfirmed", "(Landroid/view/MotionEvent;)Z", &ev, 1, NULL)) return false;
         } else g->defer_confirm = true;
      }
   }
   RETV();
}

static const struct rt_method rt_x_gesture_timer[] = {
   M("run", "()V", x_gesture_timer_run), M_END,
};

static dvm_ref x_gesture_timer(struct dvm *vm, dvm_ref detector, int kind)
{
   dvm_ref t = dvm_new_object(vm, dvm_find_class(vm, "lunaria/GestureTimer"));
   if (!t) return 0;
   (void)dvm_set_field(vm, t, "detector", "Landroid/view/GestureDetector;", (union dvm_value){ .l = detector });
   rt_set_int(vm, t, "kind", kind);
   dvm_pin(vm, t);
   return t;
}

static bool x_gesture_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)out;
   const char *sig = builtin_sig();
   dvm_ref listener = 0, handler = 0, context = 0;
   if (!strncmp(sig, "(Landroid/content/Context;", 26)) { context = ARG(0).l; listener = ARG(1).l; if (nargs > 2) handler = ARG(2).l; }
   else { listener = ARG(0).l; if (nargs > 1) handler = ARG(1).l; }
   if (!listener) { dvm__throw(vm, "java/lang/NullPointerException", "OnGestureListener must not be null"); return false; }
   struct x_gesture *g = calloc(1, sizeof *g);
   if (!g) { dvm__throw(vm, "java/lang/OutOfMemoryError", "GestureDetector"); return false; }
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) { free(g); return false; }
   o->data = g;
   o->data_destroy = x_gesture_destroy;
   x_gesture_pin(vm, &g->listener, listener);
   if (dvm_find_method(vm, dvm_object_class(vm, listener), "onSingleTapConfirmed", "(Landroid/view/MotionEvent;)Z"))
      x_gesture_pin(vm, &g->double_tap, listener);
   if (!handler) {
      handler = dvm_new_object(vm, dvm_find_class(vm, "android/os/Handler"));
      union dvm_value ignored = { 0 };
      if (!handler || !am_vcall(vm, handler, "<init>", "()V", NULL, 0, &ignored)) return false;
   }
   x_gesture_pin(vm, &g->handler, handler);
   g->t_long = x_gesture_timer(vm, self, 1);
   g->t_show = x_gesture_timer(vm, self, 2);
   g->t_tap = x_gesture_timer(vm, self, 3);
   if (!g->t_long || !g->t_show || !g->t_tap) return false;
   g->long_press_enabled = true;
   /* Slops and speeds are the scaled ViewConfiguration values (the unscaled ones
    * when there is no Context to scale by). */
   union dvm_value touch = { 0 }, minv = { 0 }, maxv = { 0 }, dbl = { 0 };
   if (context) {
      union dvm_value vc = { 0 };
      const union dvm_value arg = { .l = context };
      if (!x_call_static(vm, "android/view/ViewConfiguration", "get", "(Landroid/content/Context;)Landroid/view/ViewConfiguration;", &arg, 1, &vc) || !vc.l ||
          !am_vcall(vm, vc.l, "getScaledTouchSlop", "()I", NULL, 0, &touch) ||
          !am_vcall(vm, vc.l, "getScaledMinimumFlingVelocity", "()I", NULL, 0, &minv) ||
          !am_vcall(vm, vc.l, "getScaledMaximumFlingVelocity", "()I", NULL, 0, &maxv) ||
          !am_vcall(vm, vc.l, "getScaledDoubleTapSlop", "()I", NULL, 0, &dbl)) return false;
   } else {
      touch.i = 8; minv.i = 50; maxv.i = 8000; dbl.i = 100;
      (void)x_call_static(vm, "android/view/ViewConfiguration", "getTouchSlop", "()I", NULL, 0, &touch);
      (void)x_call_static(vm, "android/view/ViewConfiguration", "getMinimumFlingVelocity", "()I", NULL, 0, &minv);
      (void)x_call_static(vm, "android/view/ViewConfiguration", "getMaximumFlingVelocity", "()I", NULL, 0, &maxv);
   }
   g->touch_slop_sq = touch.i * touch.i;
   g->double_tap_touch_slop_sq = touch.i * touch.i;
   g->double_tap_slop_sq = dbl.i * dbl.i;
   g->min_fling = minv.i;
   g->max_fling = maxv.i;
   RETV();
}

static bool x_gesture_setters(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)nargs;
   struct x_gesture *g = x_gesture_of(vm, self);
   if (!g) RETV();
   const char *name = builtin_name();
   if (!strcmp(name, "setOnDoubleTapListener")) { x_gesture_pin(vm, &g->double_tap, ARG(0).l); RETV(); }
   if (!strcmp(name, "setIsLongpressEnabled")) { g->long_press_enabled = ARG(0).i != 0; RETV(); }
   if (!strcmp(name, "isLongpressEnabled")) RETI(g->long_press_enabled);
   RETV();                                  /* setContextClickListener: stylus/mouse buttons are not delivered */
}

/* A second tap makes a double tap when it follows soon and lands near the first. */
static bool x_gesture_is_double(const struct x_gesture *g, float fx, float fy, int64_t second_time,
                                int64_t first_down_time, int64_t first_up_time, float up_x, float up_y)
{
   (void)first_down_time;
   if (!g->always_in_bigger_tap) return false;
   const int64_t delta = second_time - first_up_time;
   if (delta > X_DOUBLE_TAP_TIMEOUT || delta < X_DOUBLE_TAP_MIN_TIME) return false;
   const int dx = (int)(up_x - fx), dy = (int)(up_y - fy);
   return dx * dx + dy * dy < g->double_tap_slop_sq;
}

static bool x_gesture_onTouchEvent(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
   (void)nargs;
   struct x_gesture *g = x_gesture_of(vm, self);
   dvm_ref ev = ARG(0).l;
   if (!g) RETI(0);
   if (!ev) { dvm__throw(vm, "java/lang/NullPointerException", "ev"); return false; }
   union dvm_value v = { 0 }, ignored = { 0 };
#define EV(call, sig, dst, ...) do { union dvm_value a_[2] = { __VA_ARGS__ }; if (!am_vcall(vm, ev, call, sig, a_, 1, &dst)) return false; } while (0)
   EV("getAction", "()I", v, { 0 });
   const int action = v.i & 0xff;
   if (!g->velocity) {
      if (!x_call_static(vm, "android/view/VelocityTracker", "obtain", "()Landroid/view/VelocityTracker;", NULL, 0, &v)) return false;
      x_gesture_pin(vm, &g->velocity, v.l);
   }
   const union dvm_value ev_arg = { .l = ev };
   if (!am_vcall(vm, g->velocity, "addMovement", "(Landroid/view/MotionEvent;)V", &ev_arg, 1, &ignored)) return false;

   const bool pointer_up = action == 6;
   int skip = -1;
   if (pointer_up) { EV("getActionIndex", "()I", v, { 0 }); skip = v.i; }
   EV("getPointerCount", "()I", v, { 0 });
   const int count = v.i;
   float sum_x = 0, sum_y = 0;
   for (int i = 0; i < count; ++i) {
      if (i == skip) continue;
      union dvm_value idx = { .i = i };
      EV("getX", "(I)F", v, idx); sum_x += v.f;
      EV("getY", "(I)F", v, idx); sum_y += v.f;
   }
   const int div = pointer_up ? count - 1 : count;
   const float fx = div ? sum_x / (float)div : 0, fy = div ? sum_y / (float)div : 0;
   EV("getEventTime", "()J", v, { 0 });
   const int64_t event_time = v.j;
   EV("getDownTime", "()J", v, { 0 });
   const int64_t down_time = v.j;
   bool handled = false;

   switch (action) {
   case 5:                                           /* ACTION_POINTER_DOWN */
      g->down_x = g->last_x = fx; g->down_y = g->last_y = fy;
      if (!x_gesture_cancel_taps(vm, g)) return false;
      break;
   case 6: {                                         /* ACTION_POINTER_UP */
      g->down_x = g->last_x = fx; g->down_y = g->last_y = fy;
      union dvm_value unit = { .i = 1000 }, cv[2] = { unit, { .f = (float)g->max_fling } }, idx = { 0 };
      if (!am_vcall(vm, g->velocity, "computeCurrentVelocity", "(IF)V", cv, 2, &ignored)) return false;
      EV("getActionIndex", "()I", idx, { 0 });
      union dvm_value id1 = { 0 }, x1 = { 0 }, y1 = { 0 };
      EV("getPointerId", "(I)I", id1, idx);
      if (!am_vcall(vm, g->velocity, "getXVelocity", "(I)F", &id1, 1, &x1) ||
          !am_vcall(vm, g->velocity, "getYVelocity", "(I)F", &id1, 1, &y1)) return false;
      for (int i = 0; i < count; ++i) {
         if (i == idx.i) continue;
         union dvm_value pi = { .i = i }, id2 = { 0 }, x2 = { 0 }, y2 = { 0 };
         EV("getPointerId", "(I)I", id2, pi);
         if (!am_vcall(vm, g->velocity, "getXVelocity", "(I)F", &id2, 1, &x2) ||
             !am_vcall(vm, g->velocity, "getYVelocity", "(I)F", &id2, 1, &y2)) return false;
         if (x1.f * x2.f + y1.f * y2.f < 0) {
            if (!am_vcall(vm, g->velocity, "clear", "()V", NULL, 0, &ignored)) return false;
            break;
         }
      }
      break;
   }
   case 0: {                                         /* ACTION_DOWN */
      if (g->double_tap) {
         const bool had_tap = g->tap_pending;
         if (!x_gesture_remove(vm, g, g->t_tap, &g->tap_pending)) return false;
         if (g->down && g->up && had_tap) {
            union dvm_value ux = { 0 }, uy = { 0 }, ut = { 0 }, dt = { 0 };
            if (!am_vcall(vm, g->up, "getX", "()F", NULL, 0, &ux) || !am_vcall(vm, g->up, "getY", "()F", NULL, 0, &uy) ||
                !am_vcall(vm, g->up, "getEventTime", "()J", NULL, 0, &ut) || !am_vcall(vm, g->down, "getDownTime", "()J", NULL, 0, &dt)) return false;
            if (x_gesture_is_double(g, fx, fy, down_time, dt.j, ut.j, ux.f, uy.f)) {
               g->double_tapping = true;
               const union dvm_value d = { .l = g->down };
               if (!x_gesture_call(vm, g->double_tap, "onDoubleTap", "(Landroid/view/MotionEvent;)Z", &d, 1, &handled) ||
                   !x_gesture_call(vm, g->double_tap, "onDoubleTapEvent", "(Landroid/view/MotionEvent;)Z", &ev_arg, 1, &handled)) return false;
            } else if (!x_gesture_post(vm, g, g->t_tap, X_DOUBLE_TAP_TIMEOUT, &g->tap_pending)) return false;
         } else if (!x_gesture_post(vm, g, g->t_tap, X_DOUBLE_TAP_TIMEOUT, &g->tap_pending)) return false;
      }
      g->down_x = g->last_x = fx; g->down_y = g->last_y = fy;
      union dvm_value copy = { 0 };
      if (!x_call_static(vm, "android/view/MotionEvent", "obtain", "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", &ev_arg, 1, &copy)) return false;
      x_gesture_pin(vm, &g->down, copy.l);
      g->always_in_tap = g->always_in_bigger_tap = true;
      g->still_down = true;
      g->in_long_press = false;
      g->defer_confirm = false;
      if (g->long_press_enabled) {
         if (!x_gesture_remove(vm, g, g->t_long, &g->long_pending) ||
             !x_gesture_post(vm, g, g->t_long, (down_time + X_TAP_TIMEOUT + X_LONGPRESS_TIMEOUT) - event_time > 0
                             ? (down_time + X_TAP_TIMEOUT + X_LONGPRESS_TIMEOUT) - event_time : 0, &g->long_pending)) return false;
      }
      if (!x_gesture_post(vm, g, g->t_show, (down_time + X_TAP_TIMEOUT) - event_time > 0 ? (down_time + X_TAP_TIMEOUT) - event_time : 0, &g->show_pending)) return false;
      if (!x_gesture_call(vm, g->listener, "onDown", "(Landroid/view/MotionEvent;)Z", &ev_arg, 1, &handled)) return false;
      break;
   }
   case 2: {                                         /* ACTION_MOVE */
      if (g->in_long_press) break;
      const float scroll_x = g->last_x - fx, scroll_y = g->last_y - fy;
      if (g->double_tapping) {
         if (!x_gesture_call(vm, g->double_tap, "onDoubleTapEvent", "(Landroid/view/MotionEvent;)Z", &ev_arg, 1, &handled)) return false;
      } else if (g->always_in_tap) {
         const int dx = (int)(fx - g->down_x), dy = (int)(fy - g->down_y);
         const int distance = dx * dx + dy * dy;
         if (distance > g->touch_slop_sq) {
            union dvm_value s[4] = { { .l = g->down }, { .l = ev }, { .f = scroll_x }, { .f = scroll_y } };
            if (!x_gesture_call(vm, g->listener, "onScroll", "(Landroid/view/MotionEvent;Landroid/view/MotionEvent;FF)Z", s, 4, &handled)) return false;
            g->last_x = fx; g->last_y = fy;
            g->always_in_tap = false;
            if (!x_gesture_remove(vm, g, g->t_tap, &g->tap_pending) || !x_gesture_remove(vm, g, g->t_show, &g->show_pending) ||
                !x_gesture_remove(vm, g, g->t_long, &g->long_pending)) return false;
         }
         if (distance > g->double_tap_touch_slop_sq) g->always_in_bigger_tap = false;
      } else if (fabsf(scroll_x) >= 1 || fabsf(scroll_y) >= 1) {
         union dvm_value s[4] = { { .l = g->down }, { .l = ev }, { .f = scroll_x }, { .f = scroll_y } };
         if (!x_gesture_call(vm, g->listener, "onScroll", "(Landroid/view/MotionEvent;Landroid/view/MotionEvent;FF)Z", s, 4, &handled)) return false;
         g->last_x = fx; g->last_y = fy;
      }
      break;
   }
   case 1: {                                         /* ACTION_UP */
      g->still_down = false;
      union dvm_value current_up = { 0 };
      if (!x_call_static(vm, "android/view/MotionEvent", "obtain", "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", &ev_arg, 1, &current_up)) return false;
      dvm_pin(vm, current_up.l);
      if (g->double_tapping) {
         if (!x_gesture_call(vm, g->double_tap, "onDoubleTapEvent", "(Landroid/view/MotionEvent;)Z", &ev_arg, 1, &handled)) return false;
      } else if (g->in_long_press) {
         if (!x_gesture_remove(vm, g, g->t_tap, &g->tap_pending)) return false;
         g->in_long_press = false;
      } else if (g->always_in_tap) {
         if (!x_gesture_call(vm, g->listener, "onSingleTapUp", "(Landroid/view/MotionEvent;)Z", &ev_arg, 1, &handled)) return false;
         if (g->defer_confirm && g->double_tap &&
             !x_gesture_call(vm, g->double_tap, "onSingleTapConfirmed", "(Landroid/view/MotionEvent;)Z", &ev_arg, 1, NULL)) return false;
      } else {
         union dvm_value unit = { .i = 1000 }, cv[2] = { unit, { .f = (float)g->max_fling } }, pid = { 0 }, id0 = { .i = 0 }, vy = { 0 }, vx = { 0 };
         if (!am_vcall(vm, g->velocity, "computeCurrentVelocity", "(IF)V", cv, 2, &ignored)) return false;
         EV("getPointerId", "(I)I", pid, id0);
         if (!am_vcall(vm, g->velocity, "getYVelocity", "(I)F", &pid, 1, &vy) ||
             !am_vcall(vm, g->velocity, "getXVelocity", "(I)F", &pid, 1, &vx)) return false;
         if (fabsf(vy.f) > (float)g->min_fling || fabsf(vx.f) > (float)g->min_fling) {
            union dvm_value f[4] = { { .l = g->down }, { .l = ev }, { .f = vx.f }, { .f = vy.f } };
            if (!x_gesture_call(vm, g->listener, "onFling", "(Landroid/view/MotionEvent;Landroid/view/MotionEvent;FF)Z", f, 4, &handled)) return false;
         }
      }
      if (g->up) { union dvm_value r = { 0 }; (void)am_vcall(vm, g->up, "recycle", "()V", NULL, 0, &r); }
      x_gesture_pin(vm, &g->up, current_up.l);
      dvm_unpin(vm, current_up.l);
      if (g->velocity) {
         if (!am_vcall(vm, g->velocity, "recycle", "()V", NULL, 0, &ignored)) return false;
         x_gesture_pin(vm, &g->velocity, 0);
      }
      g->double_tapping = false;
      g->defer_confirm = false;
      if (!x_gesture_remove(vm, g, g->t_show, &g->show_pending) || !x_gesture_remove(vm, g, g->t_long, &g->long_pending)) return false;
      break;
   }
   case 3:                                           /* ACTION_CANCEL */
      if (!x_gesture_cancel(vm, g)) return false;
      break;
   default: break;
   }
#undef EV
   RETI(handled);
}

static const struct rt_method rt_x_gesture_detector[] = {
   M("<init>", "(Landroid/content/Context;Landroid/view/GestureDetector$OnGestureListener;)V", x_gesture_init),
   M("<init>", "(Landroid/content/Context;Landroid/view/GestureDetector$OnGestureListener;Landroid/os/Handler;)V", x_gesture_init),
   M("<init>", "(Landroid/view/GestureDetector$OnGestureListener;)V", x_gesture_init),
   M("<init>", "(Landroid/view/GestureDetector$OnGestureListener;Landroid/os/Handler;)V", x_gesture_init),
   M("onTouchEvent", "(Landroid/view/MotionEvent;)Z", x_gesture_onTouchEvent),
   M("setOnDoubleTapListener", "(Landroid/view/GestureDetector$OnDoubleTapListener;)V", x_gesture_setters),
   M("setContextClickListener", "(Landroid/view/GestureDetector$OnContextClickListener;)V", x_gesture_setters),
   M("setIsLongpressEnabled", "(Z)V", x_gesture_setters), M("isLongpressEnabled", "()Z", x_gesture_setters),
   M_END,
};

/* SimpleOnGestureListener: every callback present and doing nothing. */
static const struct rt_method rt_x_simple_gesture_listener[] = {
   M("<init>", "()V", empty_void),
   M("onSingleTapUp", "(Landroid/view/MotionEvent;)Z", ret_false),
   M("onLongPress", "(Landroid/view/MotionEvent;)V", empty_void),
   M("onScroll", "(Landroid/view/MotionEvent;Landroid/view/MotionEvent;FF)Z", ret_false),
   M("onFling", "(Landroid/view/MotionEvent;Landroid/view/MotionEvent;FF)Z", ret_false),
   M("onShowPress", "(Landroid/view/MotionEvent;)V", empty_void),
   M("onDown", "(Landroid/view/MotionEvent;)Z", ret_false),
   M("onDoubleTap", "(Landroid/view/MotionEvent;)Z", ret_false),
   M("onDoubleTapEvent", "(Landroid/view/MotionEvent;)Z", ret_false),
   M("onSingleTapConfirmed", "(Landroid/view/MotionEvent;)Z", ret_false),
   M("onContextClick", "(Landroid/view/MotionEvent;)Z", ret_false),
   M_END,
};


/* A List field of any object, created on first use when asked. */
static dvm_ref x_if_list_for(struct dvm *vm, dvm_ref self, const char *field, bool create)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, field, "Ljava/util/List;", &v);
   if (!v.l && create) {
      v.l = coll_new_list(vm, "Ljava/util/ArrayList;");
      (void)dvm_set_field(vm, self, field, "Ljava/util/List;", v);
   }
   return v.l;
}

/* ------------------------------------------------------------ View flags etc */

static bool x_flag_bool(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   const char *field = strstr(name, "TransitionGroup") ? "transitionGroup" :
                       strstr(name, "TouchscreenBlocksFocus") ? "touchscreenBlocksFocus" : "keyboardCluster";
   if (!strncmp(name, "set", 3)) {
      if (!x_flag_set(vm, self, field, ARG(0).i)) return false;
      RETV();
   }
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, field, "Z", &v);
   RETI(v.i != 0);
}

/* isLayoutDirectionResolved(): resolution happens when the view joins a window. */
static bool x_view_layoutDirectionResolved(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                           int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   return am_vcall(vm, self, "isAttachedToWindow", "()Z", NULL, 0, out);
}

static bool x_view_getForeground(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   return dvm_get_field(vm, self, "foreground", "Landroid/graphics/drawable/Drawable;", out);
}

/* The state of one View only, without its children: what a ViewGroup that saves
 * its children separately (RecyclerView) uses for itself. */
static bool x_group_freeze_self(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   return (!strcmp(builtin_name(), "dispatchFreezeSelfOnly") ? x_view_dispatchSave : x_view_dispatchRestore)
          (vm, self, args, nargs, out);
}

/* A drawable's tint: the list and mode a view or ImageView was given, applied
 * to the drawable it draws.  Clearing the list removes the tint again. */
static bool x_apply_tint(struct dvm *vm, dvm_ref drawable, dvm_ref list, dvm_ref mode)
{
   if (!drawable) return true;
   union dvm_value ignored = { 0 }, l = { .l = list }, m = { .l = mode };
   if (!am_vcall(vm, drawable, "setTintList", "(Landroid/content/res/ColorStateList;)V", &l, 1, &ignored)) return false;
   if (mode && !am_vcall(vm, drawable, "setTintMode", "(Landroid/graphics/PorterDuff$Mode;)V", &m, 1, &ignored)) return false;
   return true;
}

static bool x_view_apply_background_tint(struct dvm *vm, dvm_ref view)
{
   union dvm_value list = { 0 }, mode = { 0 };
   (void)dvm_get_field(vm, view, "bgTintList", "Landroid/content/res/ColorStateList;", &list);
   (void)dvm_get_field(vm, view, "bgTintMode", "Landroid/graphics/PorterDuff$Mode;", &mode);
   if (!list.l && !mode.l) return true;
   return x_apply_tint(vm, ui_obj(vm, view, "background", "Landroid/graphics/drawable/Drawable;"), list.l, mode.l);
}

static bool x_view_backgroundTint(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   const bool mode = strstr(name, "Mode") != NULL;
   const char *field = mode ? "bgTintMode" : "bgTintList";
   const char *type = mode ? "Landroid/graphics/PorterDuff$Mode;" : "Landroid/content/res/ColorStateList;";
   if (!strncmp(name, "get", 3)) return dvm_get_field(vm, self, field, type, out);
   (void)dvm_set_field(vm, self, field, type, ARG(0));
   if (!x_view_apply_background_tint(vm, self)) return false;
   ui_invalidate();
   RETV();
}

static bool x_image_tint_alpha(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   dvm_ref drawable = ui_obj(vm, self, "drawable", "Landroid/graphics/drawable/Drawable;");
   union dvm_value list = { 0 }, mode = { 0 }, ignored = { 0 };
   if (!strcmp(name, "setImageAlpha")) {
      const int32_t a = ARG(0).i & 0xff;
      rt_set_int(vm, self, "imageAlpha", a);
      (void)x_flag_set(vm, self, "imageAlphaSet", 1);
      if (drawable) {
         const union dvm_value arg = { .i = a };
         if (!am_vcall(vm, drawable, "setAlpha", "(I)V", &arg, 1, &ignored)) return false;
      }
      ui_invalidate();
      RETV();
   }
   if (!strcmp(name, "getImageAlpha")) RETI(rt_get_int(vm, self, "imageAlphaSet") ? rt_get_int(vm, self, "imageAlpha") : 255);
   if (!strcmp(name, "getImageTintList")) return dvm_get_field(vm, self, "imageTintList", "Landroid/content/res/ColorStateList;", out);
   if (!strcmp(name, "getImageTintMode")) return dvm_get_field(vm, self, "imageTintMode", "Landroid/graphics/PorterDuff$Mode;", out);
   if (!strcmp(name, "setImageTintList"))
      (void)dvm_set_field(vm, self, "imageTintList", "Landroid/content/res/ColorStateList;", ARG(0));
   else (void)dvm_set_field(vm, self, "imageTintMode", "Landroid/graphics/PorterDuff$Mode;", ARG(0));
   (void)dvm_get_field(vm, self, "imageTintList", "Landroid/content/res/ColorStateList;", &list);
   (void)dvm_get_field(vm, self, "imageTintMode", "Landroid/graphics/PorterDuff$Mode;", &mode);
   if (!x_apply_tint(vm, drawable, list.l, mode.l)) return false;
   ui_invalidate();
   RETV();
}

/* ------------------------------------------------------------------ TextView */

/* AllCaps: the text the layout shows is the upper-cased copy; getText() still
 * answers what was set.  The copy is kept per view and rebuilt only when the
 * source changes, so the layout cache keyed on it stays warm. */
static dvm_ref x_caps_source(struct dvm *vm, dvm_ref view, dvm_ref source)
{
   union dvm_value from = { 0 }, to = { 0 };
   (void)dvm_get_field(vm, view, "capsFrom", "Ljava/lang/CharSequence;", &from);
   (void)dvm_get_field(vm, view, "capsTo", "Ljava/lang/CharSequence;", &to);
   if (from.l == source && to.l) return to.l;
   union dvm_value text = { .l = source }, upper = { 0 };
   if (!dvm_string_utf8(vm, source) && !am_vcall(vm, source, "toString", "()Ljava/lang/String;", NULL, 0, &text)) return 0;
   if (!am_vcall(vm, text.l, "toUpperCase", "()Ljava/lang/String;", NULL, 0, &upper) || !upper.l) return 0;
   (void)dvm_set_field(vm, view, "capsFrom", "Ljava/lang/CharSequence;", (union dvm_value){ .l = source });
   (void)dvm_set_field(vm, view, "capsTo", "Ljava/lang/CharSequence;", upper);
   return upper.l;
}

static bool x_text_setAllCaps(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   if (!x_flag_set(vm, self, "allCaps", ARG(0).i)) return false;
   ui_layout_changed();
   ui_invalidate();
   RETV();
}

static bool x_text_ellipsize(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)nargs;
   if (!strcmp(builtin_name(), "getEllipsize")) return dvm_get_field(vm, self, "ellipsize", "Landroid/text/TextUtils$TruncateAt;", out);
   (void)dvm_set_field(vm, self, "ellipsize", "Landroid/text/TextUtils$TruncateAt;", ARG(0));
   ui_layout_changed();
   ui_invalidate();
   RETV();
}

static bool x_text_getTextColors(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value list = { 0 };
   (void)dvm_get_field(vm, self, "textColors", "Landroid/content/res/ColorStateList;", &list);
   if (list.l) RETL(list.l);
   /* A plain setTextColor(int): a one-colour list. */
   dvm_ref cl = dvm_new_object(vm, dvm_find_class(vm, "android/content/res/ColorStateList"));
   union dvm_value color = { .i = rt_get_int(vm, self, "textColor") }, ignored = { 0 };
   if (!cl) return false;
   if (!x_call_static(vm, "android/content/res/ColorStateList", "valueOf", "(I)Landroid/content/res/ColorStateList;", &color, 1, &list) || !list.l)
      return false;
   (void)ignored;
   RETL(list.l);
}

/* onCheckIsTextEditor(): an input type means the view takes text from the IME. */
static bool x_text_isEditor(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(ui_is_a(vm, self, "android/widget/EditText") && rt_get_int(vm, self, "inputType") != 0);
}

/* setTextAppearance(): the style's text attributes — size, colours, style,
 * family, all-caps — applied to this view.  Only what the style actually sets
 * is changed. */
static bool x_text_setTextAppearance(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                     int nargs, union dvm_value *out)
{
   (void)out;
   dvm_ref context = nargs > 1 ? ARG(0).l : 0;
   const int32_t resid = ARG(nargs - 1).i;
   if (!context) {
      union dvm_value c = { 0 };
      if (!am_vcall(vm, self, "getContext", "()Landroid/content/Context;", NULL, 0, &c)) return false;
      context = c.l;
   }
   if (!context || !resid) RETV();
   union dvm_value resources = { 0 };
   if (!am_vcall(vm, context, "getResources", "()Landroid/content/res/Resources;", NULL, 0, &resources)) return false;
   static const char *const names[] = { "textSize", "textStyle", "textColor", "textColorHint", "textAllCaps", "fontFamily" };
   dvm_ref attrs = dvm_new_array(vm, 'I', "I", 6);
   if (!attrs) return false;
   dvm_pin(vm, attrs);
   int32_t *ids = dvm_array_data(vm, attrs);
   for (int i = 0; i < 6; ++i) {
      union dvm_value n[3] = { { .l = dvm_new_string(vm, names[i]) }, { .l = dvm_new_string(vm, "attr") }, { .l = dvm_new_string(vm, "android") } }, id = { 0 };
      if (!am_vcall(vm, resources.l, "getIdentifier", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", n, 3, &id)) { dvm_unpin(vm, attrs); return false; }
      ids[i] = id.i;
   }
   const union dvm_value ask[2] = { { .i = resid }, { .l = attrs } };
   union dvm_value ta = { 0 }, ignored = { 0 };
   bool good = am_vcall(vm, context, "obtainStyledAttributes", "(I[I)Landroid/content/res/TypedArray;", ask, 2, &ta) && ta.l;
   dvm_unpin(vm, attrs);
   if (!good) return false;
   dvm_pin(vm, ta.l);
   for (int i = 0; good && i < 6; ++i) {
      const union dvm_value idx = { .i = i };
      union dvm_value has = { 0 };
      good = am_vcall(vm, ta.l, "hasValue", "(I)Z", &idx, 1, &has);
      if (!good || !has.i) continue;
      union dvm_value v = { 0 };
      const union dvm_value pair[2] = { idx, { .i = -1 } };
      switch (i) {
      case 0:                                                    /* textSize, in px */
         good = am_vcall(vm, ta.l, "getDimensionPixelSize", "(II)I", pair, 2, &v);
         if (good && v.i >= 0) { const union dvm_value s[2] = { { .i = 0 }, { .f = (float)v.i } }; good = am_vcall(vm, self, "setTextSize", "(IF)V", s, 2, &ignored); }
         break;
      case 1: {                                                  /* textStyle: normal/bold/italic */
         good = am_vcall(vm, ta.l, "getInt", "(II)I", pair, 2, &v);
         if (good && v.i >= 0) {
            union dvm_value face = { 0 };
            good = am_vcall(vm, self, "getTypeface", "()Landroid/graphics/Typeface;", NULL, 0, &face);
            const union dvm_value t[2] = { face, { .i = v.i } };
            if (good) good = am_vcall(vm, self, "setTypeface", "(Landroid/graphics/Typeface;I)V", t, 2, &ignored);
         }
         break;
      }
      case 2:                                                    /* textColor */
         good = am_vcall(vm, ta.l, "getColorStateList", "(I)Landroid/content/res/ColorStateList;", &idx, 1, &v);
         if (good && v.l) good = am_vcall(vm, self, "setTextColor", "(Landroid/content/res/ColorStateList;)V", &v, 1, &ignored);
         break;
      case 3: {                                                  /* textColorHint: its default colour */
         good = am_vcall(vm, ta.l, "getColorStateList", "(I)Landroid/content/res/ColorStateList;", &idx, 1, &v);
         if (good && v.l) {
            union dvm_value def = { 0 };
            (void)dvm_get_field(vm, v.l, "color", "I", &def);
            good = am_vcall(vm, self, "setHintTextColor", "(I)V", &def, 1, &ignored);
         }
         break;
      }
      case 4: {                                                  /* textAllCaps */
         const union dvm_value b[2] = { idx, { .i = 0 } };
         good = am_vcall(vm, ta.l, "getBoolean", "(IZ)Z", b, 2, &v);
         if (good) good = x_text_setAllCaps(vm, self, &v, 1, &ignored);
         break;
      }
      default: {                                                 /* fontFamily */
         good = am_vcall(vm, ta.l, "getString", "(I)Ljava/lang/String;", &idx, 1, &v);
         if (good && v.l) {
            union dvm_value face = { 0 }, style = { .i = 0 };
            const union dvm_value create[2] = { v, style };
            if (x_call_static(vm, "android/graphics/Typeface", "create", "(Ljava/lang/String;I)Landroid/graphics/Typeface;", create, 2, &face) && face.l)
               good = am_vcall(vm, self, "setTypeface", "(Landroid/graphics/Typeface;)V", &face, 1, &ignored);
            else dvm_clear_exception(vm);
         }
         break;
      }
      }
   }
   (void)am_vcall(vm, ta.l, "recycle", "()V", NULL, 0, &ignored);
   dvm_unpin(vm, ta.l);
   if (!good) return false;
   RETV();
}

static const struct rt_method rt_x_view_flags[] = {
   M("setTransitionGroup", "(Z)V", x_flag_bool), M("isTransitionGroup", "()Z", x_flag_bool),
   M("setTouchscreenBlocksFocus", "(Z)V", x_flag_bool), M("getTouchscreenBlocksFocus", "()Z", x_flag_bool),
   M("dispatchFreezeSelfOnly", "(Landroid/util/SparseArray;)V", x_group_freeze_self),
   M("dispatchThawSelfOnly", "(Landroid/util/SparseArray;)V", x_group_freeze_self),
   M_END,
};

static const struct rt_method rt_x_view_more[] = {
   M("setKeyboardNavigationCluster", "(Z)V", x_flag_bool), M("isKeyboardNavigationCluster", "()Z", x_flag_bool),
   M("isLayoutDirectionResolved", "()Z", x_view_layoutDirectionResolved),
   M("getForeground", "()Landroid/graphics/drawable/Drawable;", x_view_getForeground),
   M("setBackgroundTintList", "(Landroid/content/res/ColorStateList;)V", x_view_backgroundTint),
   M("getBackgroundTintList", "()Landroid/content/res/ColorStateList;", x_view_backgroundTint),
   M("setBackgroundTintMode", "(Landroid/graphics/PorterDuff$Mode;)V", x_view_backgroundTint),
   M("getBackgroundTintMode", "()Landroid/graphics/PorterDuff$Mode;", x_view_backgroundTint),
   M("onCheckIsTextEditor", "()Z", ret_false),
   M_END,
};

static const struct rt_method rt_x_image_view[] = {
   M("setImageAlpha", "(I)V", x_image_tint_alpha), M("getImageAlpha", "()I", x_image_tint_alpha),
   M("setImageTintList", "(Landroid/content/res/ColorStateList;)V", x_image_tint_alpha),
   M("getImageTintList", "()Landroid/content/res/ColorStateList;", x_image_tint_alpha),
   M("setImageTintMode", "(Landroid/graphics/PorterDuff$Mode;)V", x_image_tint_alpha),
   M("getImageTintMode", "()Landroid/graphics/PorterDuff$Mode;", x_image_tint_alpha),
   M_END,
};

static const struct rt_method rt_x_text_view[] = {
   M("setAllCaps", "(Z)V", x_text_setAllCaps),
   M("setEllipsize", "(Landroid/text/TextUtils$TruncateAt;)V", x_text_ellipsize),
   M("getEllipsize", "()Landroid/text/TextUtils$TruncateAt;", x_text_ellipsize),
   M("getTextColors", "()Landroid/content/res/ColorStateList;", x_text_getTextColors),
   M("onCheckIsTextEditor", "()Z", x_text_isEditor),
   M("setTextAppearance", "(I)V", x_text_setTextAppearance),
   M("setTextAppearance", "(Landroid/content/Context;I)V", x_text_setTextAppearance),
   M_END,
};


/* -------------------------------------------------------------- ViewTreeObserver *
 *
 * The listeners that are not about layout: scrolling, drawing, focus.  They
 * are kept in lists on the observer and fired by the code that causes the event
 * (scrollTo, the draw pass, a focus change).  Touch-mode listeners are kept
 * but never fire: a touch screen stays in touch mode. */

static const char *x_vto_list(const char *name)
{
   if (strstr(name, "Scroll")) return "scrollListeners";
   if (strstr(name, "Draw")) return "drawListeners";
   if (strstr(name, "GlobalFocus")) return "focusListeners";
   if (strstr(name, "WindowFocus")) return "windowFocusListeners";
   return "touchModeListeners";
}

static bool x_vto_listener(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   const char *name = builtin_name();
   const bool remove = !strncmp(name, "remove", 6);
   if (!ARG(0).l) {
      if (remove) RETV();
      dvm__throw(vm, "java/lang/NullPointerException", "listener");
      return false;
   }
   dvm_ref list = x_if_list_for(vm, self, x_vto_list(name), !remove);
   union dvm_value ignored = { 0 };
   if (remove) {
      if (list && !list_remove_obj(vm, list, args, 1, &ignored)) return false;
      RETV();
   }
   if (!list || !vto_register(vm, self) || !list_add(vm, list, args, 1, &ignored)) return false;
   RETV();
}

/* Calls `method` on every listener of every registered observer, from a
 * snapshot: a listener may add or remove listeners while it runs. */
static void x_vto_fire(struct dvm *vm, const char *field, const char *method, const char *sig,
                       const union dvm_value *a, int n)
{
   const size_t count = vm->nlayout_observers;
   if (!count) return;
   dvm_ref *observers = malloc(count * sizeof *observers);
   if (!observers) return;
   memcpy(observers, vm->layout_observers, count * sizeof *observers);
   for (size_t i = 0; i < count; ++i) dvm_pin(vm, observers[i]);
   for (size_t i = 0; i < count; ++i) {
      union dvm_value list = { 0 };
      (void)dvm_get_field(vm, observers[i], field, "Ljava/util/List;", &list);
      struct rt_list *items = list.l ? list_of(vm, list.l) : NULL;
      const uint32_t listeners = items ? items->size : 0;
      dvm_ref *snapshot = listeners ? malloc((size_t)listeners * sizeof *snapshot) : NULL;
      if (listeners && !snapshot) continue;
      if (listeners) memcpy(snapshot, items->items, (size_t)listeners * sizeof *snapshot);
      for (uint32_t j = 0; j < listeners; ++j) dvm_pin(vm, snapshot[j]);
      for (uint32_t j = 0; j < listeners; ++j) {
         struct dvm_class *c = dvm_object_class(vm, snapshot[j]);
         struct dvm_method *m = c ? dvm_find_method(vm, c, method, sig) : NULL;
         union dvm_value ignored = { 0 };
         if (m && !dvm_call(vm, m, snapshot[j], a, n, &ignored)) ui_report_exception(vm, method);
      }
      for (uint32_t j = 0; j < listeners; ++j) dvm_unpin(vm, snapshot[j]);
      free(snapshot);
   }
   for (size_t i = 0; i < count; ++i) dvm_unpin(vm, observers[i]);
   free(observers);
}

static const struct rt_method rt_x_view_tree_observer[] = {
   M("addOnScrollChangedListener", "(Landroid/view/ViewTreeObserver$OnScrollChangedListener;)V", x_vto_listener),
   M("removeOnScrollChangedListener", "(Landroid/view/ViewTreeObserver$OnScrollChangedListener;)V", x_vto_listener),
   M("addOnDrawListener", "(Landroid/view/ViewTreeObserver$OnDrawListener;)V", x_vto_listener),
   M("removeOnDrawListener", "(Landroid/view/ViewTreeObserver$OnDrawListener;)V", x_vto_listener),
   M("addOnGlobalFocusChangeListener", "(Landroid/view/ViewTreeObserver$OnGlobalFocusChangeListener;)V", x_vto_listener),
   M("removeOnGlobalFocusChangeListener", "(Landroid/view/ViewTreeObserver$OnGlobalFocusChangeListener;)V", x_vto_listener),
   M("addOnWindowFocusChangeListener", "(Landroid/view/ViewTreeObserver$OnWindowFocusChangeListener;)V", x_vto_listener),
   M("removeOnWindowFocusChangeListener", "(Landroid/view/ViewTreeObserver$OnWindowFocusChangeListener;)V", x_vto_listener),
   M("addOnTouchModeChangeListener", "(Landroid/view/ViewTreeObserver$OnTouchModeChangeListener;)V", x_vto_listener),
   M("removeOnTouchModeChangeListener", "(Landroid/view/ViewTreeObserver$OnTouchModeChangeListener;)V", x_vto_listener),
   /* The old name of removeOnGlobalLayoutListener(). */
   M("removeGlobalOnLayoutListener", "(Landroid/view/ViewTreeObserver$OnGlobalLayoutListener;)V", vto_listener_change),
   M_END,
};

/* ----------------------------------------------------- Window / Accessibility */

static bool x_window_frameMetrics(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out);

static const struct rt_method rt_x_window[] = {
   /* Frame metrics come from the GPU compositor's frame timeline, which a
    * software window does not have: the listeners are kept and hear no frames. */
   M("addOnFrameMetricsAvailableListener", "(Landroid/view/Window$OnFrameMetricsAvailableListener;Landroid/os/Handler;)V", x_window_frameMetrics),
   M("removeOnFrameMetricsAvailableListener", "(Landroid/view/Window$OnFrameMetricsAvailableListener;)V", x_window_frameMetrics),
   /* The options-menu panel is rebuilt the next time it is shown; there is no
    * menu bar here that shows it. */
   M("invalidatePanelMenu", "(I)V", empty_void),
   M_END,
};

static bool x_window_frameMetrics(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   const bool remove = !strncmp(builtin_name(), "remove", 6);
   dvm_ref list = x_if_list_for(vm, self, "frameMetricsListeners", !remove);
   union dvm_value ignored = { 0 };
   if (remove) { if (list && !list_remove_obj(vm, list, args, 1, &ignored)) return false; RETV(); }
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "listener"); return false; }
   if (!list || !list_add(vm, list, args, 1, &ignored)) return false;
   RETV();
}

/* AccessibilityManager's state is fixed here (accessibility and touch
 * exploration are off), so a listener is registered and never called. */
static bool x_a11y_listener(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   const bool remove = !strncmp(name, "remove", 6);
   const char *field = strstr(name, "TouchExploration") ? "touchListeners" : "stateListeners";
   dvm_ref list = x_if_list_for(vm, self, field, !remove);
   union dvm_value ignored = { 0 }, had = { 0 };
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "listener"); return false; }
   if (remove) {
      if (!list) RETI(0);
      if (!am_vcall(vm, list, "contains", "(Ljava/lang/Object;)Z", args, 1, &had)) return false;
      if (!list_remove_obj(vm, list, args, 1, &ignored)) return false;
      RETI(had.i);
   }
   if (!list || !list_add(vm, list, args, 1, &ignored)) return false;
   RETI(1);
}

static const struct rt_method rt_x_accessibility_manager[] = {
   M("addAccessibilityStateChangeListener", "(Landroid/view/accessibility/AccessibilityManager$AccessibilityStateChangeListener;)Z", x_a11y_listener),
   M("removeAccessibilityStateChangeListener", "(Landroid/view/accessibility/AccessibilityManager$AccessibilityStateChangeListener;)Z", x_a11y_listener),
   M("addTouchExplorationStateChangeListener", "(Landroid/view/accessibility/AccessibilityManager$TouchExplorationStateChangeListener;)Z", x_a11y_listener),
   M("removeTouchExplorationStateChangeListener", "(Landroid/view/accessibility/AccessibilityManager$TouchExplorationStateChangeListener;)Z", x_a11y_listener),
   M_END,
};

static const struct rt_field rt_x_a11y_fields[] = {
   { "stateListeners", "Ljava/util/List;" }, { "touchListeners", "Ljava/util/List;" }, F_END
};

/* AccessibilityNodeProvider: a virtual view tree a view can expose.  Every
 * request the base class gets is one it has nothing to answer. */
static const struct rt_method rt_x_a11y_provider[] = {
   M("<init>", "()V", empty_void),
   M("createAccessibilityNodeInfo", "(I)Landroid/view/accessibility/AccessibilityNodeInfo;", ret_null),
   M("performAction", "(IILandroid/os/Bundle;)Z", ret_false),
   M("findAccessibilityNodeInfosByText", "(Ljava/lang/String;I)Ljava/util/List;", ret_null),
   M("findFocus", "(I)Landroid/view/accessibility/AccessibilityNodeInfo;", ret_null),
   M_END,
};

/* ------------------------------------------------------------ Text input */

/* CursorAnchorInfo: where the cursor, the composing text and the characters
 * are, handed to the input method so it can place its candidate window. */
static const struct rt_field rt_x_cursor_info_fields[] = {
   { "selectionStart", "I" }, { "selectionEnd", "I" }, { "composingStart", "I" },
   { "composingText", "Ljava/lang/CharSequence;" }, { "matrix", "Landroid/graphics/Matrix;" },
   { "markerHorizontal", "F" }, { "markerTop", "F" }, { "markerBaseline", "F" }, { "markerBottom", "F" },
   { "markerFlags", "I" }, F_END
};

static bool x_cursor_builder(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   if (!strcmp(name, "<init>")) {
      rt_set_int(vm, self, "selectionStart", -1);
      rt_set_int(vm, self, "selectionEnd", -1);
      rt_set_int(vm, self, "composingStart", -1);
      RETV();
   }
   if (!strcmp(name, "setSelectionRange")) { rt_set_int(vm, self, "selectionStart", ARG(0).i); rt_set_int(vm, self, "selectionEnd", ARG(1).i); }
   else if (!strcmp(name, "setMatrix")) (void)dvm_set_field(vm, self, "matrix", "Landroid/graphics/Matrix;", ARG(0));
   else if (!strcmp(name, "setComposingText")) {
      rt_set_int(vm, self, "composingStart", ARG(0).i);
      (void)dvm_set_field(vm, self, "composingText", "Ljava/lang/CharSequence;", ARG(1));
   } else if (!strcmp(name, "setInsertionMarkerLocation")) {
      (void)dvm_set_field(vm, self, "markerHorizontal", "F", ARG(0)); (void)dvm_set_field(vm, self, "markerTop", "F", ARG(1));
      (void)dvm_set_field(vm, self, "markerBaseline", "F", ARG(2)); (void)dvm_set_field(vm, self, "markerBottom", "F", ARG(3));
      rt_set_int(vm, self, "markerFlags", ARG(4).i);
   } else if (!strcmp(name, "build")) {
      dvm_ref info = dvm_new_object(vm, dvm_find_class(vm, "android/view/inputmethod/CursorAnchorInfo"));
      if (!info) return false;
      static const char *const ints[] = { "selectionStart", "selectionEnd", "composingStart", "markerFlags" };
      for (int i = 0; i < 4; ++i) rt_set_int(vm, info, ints[i], rt_get_int(vm, self, ints[i]));
      static const char *const floats[] = { "markerHorizontal", "markerTop", "markerBaseline", "markerBottom" };
      for (int i = 0; i < 4; ++i) { union dvm_value f = { 0 }; (void)dvm_get_field(vm, self, floats[i], "F", &f); (void)dvm_set_field(vm, info, floats[i], "F", f); }
      union dvm_value t = { 0 }, m = { 0 };
      (void)dvm_get_field(vm, self, "composingText", "Ljava/lang/CharSequence;", &t);
      (void)dvm_set_field(vm, info, "composingText", "Ljava/lang/CharSequence;", t);
      (void)dvm_get_field(vm, self, "matrix", "Landroid/graphics/Matrix;", &m);
      (void)dvm_set_field(vm, info, "matrix", "Landroid/graphics/Matrix;", m);
      RETL(info);
   }
   RETL(self);                      /* addCharacterBounds and the setters that follow the call chain */
}

static bool x_cursor_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *name = builtin_name();
   if (!strcmp(name, "getSelectionStart")) RETI(rt_get_int(vm, self, "selectionStart"));
   if (!strcmp(name, "getSelectionEnd")) RETI(rt_get_int(vm, self, "selectionEnd"));
   if (!strcmp(name, "getComposingTextStart")) RETI(rt_get_int(vm, self, "composingStart"));
   if (!strcmp(name, "getComposingText")) return dvm_get_field(vm, self, "composingText", "Ljava/lang/CharSequence;", out);
   if (!strcmp(name, "getMatrix")) return dvm_get_field(vm, self, "matrix", "Landroid/graphics/Matrix;", out);
   if (!strcmp(name, "getInsertionMarkerFlags")) RETI(rt_get_int(vm, self, "markerFlags"));
   const char *field = !strcmp(name, "getInsertionMarkerHorizontal") ? "markerHorizontal" :
                       !strcmp(name, "getInsertionMarkerTop") ? "markerTop" :
                       !strcmp(name, "getInsertionMarkerBaseline") ? "markerBaseline" : "markerBottom";
   return dvm_get_field(vm, self, field, "F", out);
}

static const struct rt_method rt_x_cursor_builder[] = {
   M("<init>", "()V", x_cursor_builder),
   M("setSelectionRange", "(II)Landroid/view/inputmethod/CursorAnchorInfo$Builder;", x_cursor_builder),
   M("setMatrix", "(Landroid/graphics/Matrix;)Landroid/view/inputmethod/CursorAnchorInfo$Builder;", x_cursor_builder),
   M("setComposingText", "(ILjava/lang/CharSequence;)Landroid/view/inputmethod/CursorAnchorInfo$Builder;", x_cursor_builder),
   M("setInsertionMarkerLocation", "(FFFFI)Landroid/view/inputmethod/CursorAnchorInfo$Builder;", x_cursor_builder),
   M("addCharacterBounds", "(IFFFFI)Landroid/view/inputmethod/CursorAnchorInfo$Builder;", x_cursor_builder),
   M("addVisibleLineBounds", "(FFFF)Landroid/view/inputmethod/CursorAnchorInfo$Builder;", x_cursor_builder),
   M("build", "()Landroid/view/inputmethod/CursorAnchorInfo;", x_cursor_builder),
   M_END,
};

static const struct rt_method rt_x_cursor_info[] = {
   M("getSelectionStart", "()I", x_cursor_get), M("getSelectionEnd", "()I", x_cursor_get),
   M("getComposingTextStart", "()I", x_cursor_get), M("getComposingText", "()Ljava/lang/CharSequence;", x_cursor_get),
   M("getMatrix", "()Landroid/graphics/Matrix;", x_cursor_get),
   M("getInsertionMarkerHorizontal", "()F", x_cursor_get), M("getInsertionMarkerTop", "()F", x_cursor_get),
   M("getInsertionMarkerBaseline", "()F", x_cursor_get), M("getInsertionMarkerBottom", "()F", x_cursor_get),
   M("getInsertionMarkerFlags", "()I", x_cursor_get),
   M_END,
};

/* Editable.Factory: how an EditText builds the buffer it edits in. */
static bool x_editable_factory_instance(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                        int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   static dvm_ref shared;
   RETL(runtime_singleton(vm, "Landroid/text/Editable$Factory;", &shared));
}

static bool x_editable_factory_new(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   dvm_ref buffer = dvm_new_object(vm, dvm_find_class(vm, "android/text/SpannableStringBuilder"));
   union dvm_value ignored = { 0 };
   if (!buffer || !am_vcall(vm, buffer, "<init>", "(Ljava/lang/CharSequence;)V", args, 1, &ignored)) return false;
   RETL(buffer);
}

static const struct rt_method rt_x_editable_factory[] = {
   M("<init>", "()V", empty_void),
   SM("getInstance", "()Landroid/text/Editable$Factory;", x_editable_factory_instance),
   M("newEditable", "(Ljava/lang/CharSequence;)Landroid/text/Editable;", x_editable_factory_new),
   M_END,
};

/* -------------------------------------------------------------- Resources */

/* TypedValue.complexToDimension*(data, metrics): the packed dimension scaled by
 * this display's density (and, for sp, its scaled density). */
static float x_complex_to_dimension(struct dvm *vm, int32_t data, dvm_ref metrics)
{
   static const float radix[] = { 1.0f, 1.0f / 128.0f, 1.0f / 32768.0f, 1.0f / 8388608.0f };
   const float value = (float)(data >> 8) * radix[((uint32_t)data >> 4) & 3u];
   union dvm_value density = { 0 }, scaled = { 0 }, xdpi = { 0 };
   (void)dvm_get_field(vm, metrics, "density", "F", &density);
   (void)dvm_get_field(vm, metrics, "scaledDensity", "F", &scaled);
   (void)dvm_get_field(vm, metrics, "xdpi", "F", &xdpi);
   switch ((uint32_t)data & 15u) {
   case 0: return value;
   case 1: return value * density.f;
   case 2: return value * (scaled.f ? scaled.f : density.f);
   case 3: return value * xdpi.f * (1.0f / 72);
   case 4: return value * xdpi.f;
   case 5: return value * xdpi.f * (1.0f / 25.4f);
   default: return value;
   }
}

static bool x_typed_value_complex(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   if (!ARG(1).l) { dvm__throw(vm, "java/lang/NullPointerException", "metrics"); return false; }
   const float f = x_complex_to_dimension(vm, ARG(0).i, ARG(1).l);
   const char *name = builtin_name();
   if (!strcmp(name, "complexToDimension")) RETF(f);
   if (!strcmp(name, "complexToDimensionPixelOffset")) RETI((int32_t)f);
   const int32_t rounded = (int32_t)(f >= 0 ? f + .5f : f - .5f);
   RETI(rounded ? rounded : (f > 0 ? 1 : f < 0 ? -1 : 0));
}

static const struct rt_method rt_x_typed_value[] = {
   SM("complexToDimension", "(ILandroid/util/DisplayMetrics;)F", x_typed_value_complex),
   SM("complexToDimensionPixelOffset", "(ILandroid/util/DisplayMetrics;)I", x_typed_value_complex),
   SM("complexToDimensionPixelSize", "(ILandroid/util/DisplayMetrics;)I", x_typed_value_complex),
   M_END,
};

static bool x_res_dimension_offset(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
   union dvm_value d = { 0 };
   if (!res_get_dimension(vm, self, args, nargs, &d)) return false;
   RETI((int32_t)d.f);                      /* offset truncates; pixel size rounds */
}

/* getFloat(id): a <dimen format="float"> or <item type="dimen"> resource's number. */
static bool x_res_float(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   int32_t value = 0;
   const char *string = NULL;
   if (!res_value_resolved((uint32_t)ARG(0).i, &value, &string)) {
      dvm__throw(vm, "android/content/res/Resources$NotFoundException", "Resource ID #0x%x", (unsigned)ARG(0).i);
      return false;
   }
   float f;
   memcpy(&f, &value, sizeof f);
   RETF(f);
}

static const struct rt_method rt_x_resources[] = {
   M("getDimensionPixelOffset", "(I)I", x_res_dimension_offset),
   M("getFloat", "(I)F", x_res_float),
   M_END,
};

static bool x_typed_array_textArray(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)nargs;
   const union dvm_value id_args[2] = { ARG(0), { .i = 0 } };
   union dvm_value id = { 0 }, res = { 0 };
   if (!am_vcall(vm, self, "getResourceId", "(II)I", id_args, 2, &id)) return false;
   if (!id.i) RETL(0);
   (void)dvm_get_field(vm, self, "resources", "Landroid/content/res/Resources;", &res);
   if (!res.l) RETL(0);
   return am_vcall(vm, res.l, "getTextArray", "(I)[Ljava/lang/CharSequence;", &id, 1, out);
}

static const struct rt_method rt_x_typed_array[] = {
   /* Configuration changes this array's values depend on: none are tracked. */
   M("getChangingConfigurations", "()I", ret_zero),
   M("getTextArray", "(I)[Ljava/lang/CharSequence;", x_typed_array_textArray),
   M_END,
};

/* ----------------------------------------------------------------- Scrolling */

/* OverScroller.getCurrVelocity(): the speed, in px/s, the scroll has right now
 * (the derivative of the position the scroller is easing along). */
static bool x_scroller_currVelocity(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   if (rt_get_int(vm, self, "finished")) RETF(0.f);
   union dvm_value st = { 0 };
   (void)dvm_get_field(vm, self, "start", "J", &st);
   const int32_t dur = rt_get_int(vm, self, "duration");
   if (dur <= 0) RETF(0.f);
   const float t = (float)(scroller_now_ms() - st.j) / (float)dur;
   if (t < 0.f || t >= 1.f) RETF(0.f);
   const float eps = 1.0f / 512, end = scroller_viscous(1.0f);
   const float slope = (scroller_viscous(t + eps > 1.f ? 1.f : t + eps) - scroller_viscous(t - eps < 0.f ? 0.f : t - eps)) /
                       ((t + eps > 1.f ? 1.f : t + eps) - (t - eps < 0.f ? 0.f : t - eps)) / end;
   const float dx = (float)(rt_get_int(vm, self, "finalX") - rt_get_int(vm, self, "startX"));
   const float dy = (float)(rt_get_int(vm, self, "finalY") - rt_get_int(vm, self, "startY"));
   RETF(sqrtf(dx * dx + dy * dy) * slope * 1000.f / (float)dur);
}

static const struct rt_method rt_x_overscroller[] = {
   M("getCurrVelocity", "()F", x_scroller_currVelocity),
   M_END,
};

/* ScrollView.fullScroll(FOCUS_UP/FOCUS_DOWN): to the top or the bottom, true
 * when that moved anything. */
static bool x_scrollview_fullScroll(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)nargs;
   const bool down = ARG(0).i == 130;                 /* View.FOCUS_DOWN */
   union dvm_value range = { 0 }, extent = { 0 };
   if (!am_vcall(vm, self, "computeVerticalScrollRange", "()I", NULL, 0, &range) ||
       !am_vcall(vm, self, "computeVerticalScrollExtent", "()I", NULL, 0, &extent)) return false;
   const int32_t target = down ? (range.i - extent.i > 0 ? range.i - extent.i : 0) : 0;
   const int32_t sx = ui_int(vm, self, "scrollX"), sy = ui_int(vm, self, "scrollY");
   if (sy == target) RETI(0);
   const union dvm_value to[2] = { { .i = sx }, { .i = target } };
   union dvm_value ignored = { 0 };
   if (!am_vcall(vm, self, "scrollTo", "(II)V", to, 2, &ignored)) return false;
   RETI(1);
}

static const struct rt_method rt_x_scroll_view2[] = {
   M("fullScroll", "(I)Z", x_scrollview_fullScroll),
   M_END,
};

/* ViewAnimator.setInAnimation(Context, resId): the animation the resource names. */
static bool x_animator_animationRes(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)nargs;
   union dvm_value anim = { 0 }, ignored = { 0 };
   if (!x_call_static(vm, "android/view/animation/AnimationUtils", "loadAnimation",
                      "(Landroid/content/Context;I)Landroid/view/animation/Animation;", args, 2, &anim)) return false;
   const bool in = !strcmp(builtin_name(), "setInAnimation");
   (void)out;
   return am_vcall(vm, self, in ? "setInAnimation" : "setOutAnimation", "(Landroid/view/animation/Animation;)V", &anim, 1, &ignored);
}

static const struct rt_method rt_x_view_animator[] = {
   M("setInAnimation", "(Landroid/content/Context;I)V", x_animator_animationRes),
   M("setOutAnimation", "(Landroid/content/Context;I)V", x_animator_animationRes),
   M_END,
};

/* RelativeLayout.setGravity()/getGravity(): where the children sit as a group
 * when no rule places them. */
static bool x_relative_gravity(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)nargs;
   if (!strcmp(builtin_name(), "getGravity")) RETI(rt_get_int(vm, self, "gravity"));
   rt_set_int(vm, self, "gravity", ARG(0).i);
   ui_layout_changed();
   ui_invalidate();
   (void)out;
   RETV();
}

static const struct rt_method rt_x_relative_layout[] = {
   M("setGravity", "(I)V", x_relative_gravity), M("getGravity", "()I", x_relative_gravity),
   M("setHorizontalGravity", "(I)V", x_relative_gravity), M("setVerticalGravity", "(I)V", x_relative_gravity),
   M_END,
};

/* ------------------------------------------------------------ ValueAnimator */

static bool x_va_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *name = builtin_name();
   if (!strcmp(name, "getRepeatCount")) RETI(rt_get_int(vm, self, "repeatCount"));
   if (!strcmp(name, "getRepeatMode")) RETI(rt_get_int(vm, self, "repeatMode") ? rt_get_int(vm, self, "repeatMode") : 1);
   if (!strcmp(name, "getInterpolator")) return dvm_get_field(vm, self, "interpolator", "Landroid/animation/TimeInterpolator;", out);
   if (!strcmp(name, "isPaused")) RETI(0);
   /* getTotalDuration(): the repeats multiply the duration; infinite is -1. */
   union dvm_value d = { 0 }, delay = { 0 };
   (void)dvm_get_field(vm, self, "duration", "J", &d);
   (void)dvm_get_field(vm, self, "delay", "J", &delay);
   const int32_t repeat = rt_get_int(vm, self, "repeatCount");
   if (repeat == -1) RETJ(-1);
   RETJ(delay.j + d.j * (int64_t)(repeat + 1));
}

static const struct rt_method rt_x_value_animator[] = {
   M("getRepeatCount", "()I", x_va_get), M("getRepeatMode", "()I", x_va_get),
   M("getInterpolator", "()Landroid/animation/TimeInterpolator;", x_va_get),
   M("isPaused", "()Z", x_va_get), M("getTotalDuration", "()J", x_va_get),
   M_END,
};

#endif
