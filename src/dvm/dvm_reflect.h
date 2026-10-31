/* MPL-2.0. java.lang reflection, Class and class loaders.
 * Implementation section of dvm_runtime.c; shared helpers remain in that
 * translation unit. Keep registration tables in dvm_runtime.c. */

/* java.lang.reflect.Method.  Library code probes for optional platform APIs
 * with getMethod()/invoke() and catches NoSuchMethodException when they are
 * absent — Google Play services does exactly that.  With no reflection at all
 * getMethod() answered null and the caller then invoked through it, turning a
 * routine "feature not present" into a NullPointerException nothing catches.
 * The Method object records the class and name; invoke() dispatches through
 * the same lookup the interpreter uses. */
static dvm_ref runtime_singleton(struct dvm *vm, const char *desc,
                                 dvm_ref *slot);

/* The overload named by a Class[] of parameter types, or NULL when it cannot
 * be told apart.  Same reason as reflect_param_sig() below: getMethod() is
 * given the parameter types and picking the first method of that name calls a
 * different one. */
static bool reflect_param_sig(struct dvm *vm, dvm_ref types, char *buf,
                              size_t cap);

static struct dvm_method *reflect_find_overload(struct dvm *vm,
                                                struct dvm_class *cls,
                                                const char *name, dvm_ref types)
{
   char sig[512];
   if (!cls || !name || !reflect_param_sig(vm, types, sig, sizeof sig))
      return NULL;
   const size_t plen = strcspn(sig, ")") + 1u;   /* "(...)" including the ')' */
   for (struct dvm_class *c = cls; c; c = c->super)
      for (int i = 0; i < c->nmethods; ++i) {
         struct dvm_method *m = &c->methods[i];
         if (!m->name || strcmp(m->name, name)) continue;
         if (m->sig && !strncmp(m->sig, sig, plen)) return m;
      }
   return NULL;
}

static bool cl_getMethod(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   struct dvm_object *co = dvm__obj(vm, self);
   struct dvm_class *target = co ? co->klass : NULL;
   const char *name = dvm_string_utf8(vm, ARG(0).l);
   /* A host-backed class has no method table the VM can enumerate, so it must
    * not be reported as missing the method — the stub layer may well implement
    * it.  Only a dex-defined class can answer "absent" with authority. */
   bool known = target && name &&
                (target->external || dvm_find_method(vm, target, name, NULL));
   if (!known) {
      dvm__throw(vm, "java/lang/NoSuchMethodException", "%s.%s",
                 target ? target->name : "?", name ? name : "?");
      return false;
   }
   struct dvm_class *mc = dvm__class_by_desc(vm, "Ljava/lang/reflect/Method;");
   dvm_ref m = mc ? dvm_new_object(vm, mc) : 0;
   if (!m) RETL(0);
   union dvm_value v = { .l = self };
   (void)dvm_set_field(vm, m, "owner", "Ljava/lang/Class;", v);
   v.l = ARG(0).l;
   (void)dvm_set_field(vm, m, "name", "Ljava/lang/String;", v);
   v.l = nargs > 1 ? ARG(1).l : 0;
   dvm_pin(vm, v.l);
   (void)dvm_set_field(vm, m, "paramTypes", "[Ljava/lang/Class;", v);
   struct dvm_method *found = (target && name)
      ? reflect_find_overload(vm, target, name, nargs > 1 ? ARG(1).l : 0)
      : NULL;
   if (!found && target && name) found = dvm_find_method(vm, target, name, NULL);
   if (found) {
      v.i = (int32_t)found->access;
      (void)dvm_set_field(vm, m, "access", "I", v);
      if (found->sig) {
         v.l = dvm_new_string(vm, found->sig);
         (void)dvm_set_field(vm, m, "sig", "Ljava/lang/String;", v);
      }
   }
   RETL(m);
}

static dvm_ref reflect_field_object(struct dvm *vm, struct dvm_class *owner,
                                    const struct dvm_field *fd)
{
   struct dvm_class *fc = dvm__class_by_desc(vm, "Ljava/lang/reflect/Field;");
   dvm_ref field = fc ? dvm_new_object(vm, fc) : 0;
   if (!field) return 0;
   union dvm_value v = { .l = dvm_class_object(vm, owner) };
   (void)dvm_set_field(vm, field, "owner", "Ljava/lang/Class;", v);
   v.l = dvm_new_string(vm, fd->name ? fd->name : "");
   (void)dvm_set_field(vm, field, "name", "Ljava/lang/String;", v);
   v.l = dvm_new_string(vm, fd->type ? fd->type : "Ljava/lang/Object;");
   (void)dvm_set_field(vm, field, "sig", "Ljava/lang/String;", v);
   v.i = (int32_t)fd->access;
   (void)dvm_set_field(vm, field, "access", "I", v);
   return field;
}

static struct dvm_field *reflect_find_public_field(struct dvm_class *cls,
                                                   const char *name, unsigned depth)
{
   if (!cls || depth > 128) return NULL;
   for (int i = 0; i < cls->nifields + cls->nsfields; ++i) {
      struct dvm_field *field = i < cls->nifields ? &cls->ifields[i] : &cls->sfields[i-cls->nifields];
      if ((field->access & DEX_ACC_PUBLIC) && field->name && !strcmp(field->name,name)) return field;
   }
   for (int i = 0; i < cls->nifaces; ++i) {
      struct dvm_field *field = reflect_find_public_field(cls->ifaces[i],name,depth+1);
      if (field) return field;
   }
   return reflect_find_public_field(cls->super,name,depth+1);
}

static bool cl_getField(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *co = dvm__obj(vm, self);
   if (!ARG(0).l) { dvm__throw(vm,"java/lang/NullPointerException","field name"); return false; }
   if (!co || !co->klass) RETL(0);
   const char *name = dvm_string_utf8(vm, ARG(0).l);
   struct dvm_class *c = co->klass;
   const struct dvm_field *found = NULL;
   if (!strcmp(builtin_name(),"getDeclaredField")) {
      for (int i = 0; name && !found && i < c->nifields; ++i)
         if (c->ifields[i].name && !strcmp(c->ifields[i].name,name)) found = &c->ifields[i];
      for (int i = 0; name && !found && i < c->nsfields; ++i)
         if (c->sfields[i].name && !strcmp(c->sfields[i].name,name)) found = &c->sfields[i];
   } else if (name) found = reflect_find_public_field(c,name,0);
   if (!found) {
      dvm__throw(vm,"java/lang/NoSuchFieldException","%s.%s",c->name,name ? name : "");
      return false;
   }
   RETL(reflect_field_object(vm,found->cls,found));
}

/* Visit each declaring type once, including shared interface ancestors.
 * Field names are not deduplicated: a subclass can hide a public field, and
 * getFields must still return both declarations. No class initialization is
 * required to enumerate metadata. */
static bool cl_getFields(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *co = dvm__obj(vm, self);
   struct dvm_class *root = co ? co->klass : NULL;
   struct dvm_class **types = NULL;
   size_t count = 0, cap = 0;
   uint32_t fields = 0;
   if (root && !root->is_array && !root->is_primitive) {
      cap = 8;
      types = malloc(cap * sizeof *types);
      if (!types) goto oom;
      types[count++] = root;
   }
   for (size_t i = 0; i < count; ++i) {
      struct dvm_class *c = types[i];
      for (int j = 0; j < c->nifields + c->nsfields; ++j) {
         const struct dvm_field *fd = j < c->nifields ? &c->ifields[j]
                                                     : &c->sfields[j - c->nifields];
         if (fd->access & DEX_ACC_PUBLIC) {
            if (fields == UINT32_MAX) goto oom;
            ++fields;
         }
      }
      for (int j = 0; j <= c->nifaces; ++j) {
         struct dvm_class *next = j < c->nifaces ? c->ifaces[j]
                                  : c->is_interface ? NULL : c->super;
         if (!next) continue;
         size_t seen = 0;
         while (seen < count && types[seen] != next) ++seen;
         if (seen != count) continue;
         if (count == cap) {
            if (cap > SIZE_MAX / (2 * sizeof *types)) goto oom;
            size_t grown = cap * 2;
            struct dvm_class **p = realloc(types, grown * sizeof *types);
            if (!p) goto oom;
            types = p; cap = grown;
         }
         types[count++] = next;
      }
   }
   dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/lang/reflect/Field;", fields);
   if (!arr) { free(types); return false; }
   dvm_ref *items = dvm_array_data(vm, arr);
   uint32_t at = 0;
   for (size_t i = 0; i < count; ++i) {
      struct dvm_class *c = types[i];
      for (int j = 0; j < c->nifields + c->nsfields; ++j) {
         const struct dvm_field *fd = j < c->nifields ? &c->ifields[j]
                                                     : &c->sfields[j - c->nifields];
         if (!(fd->access & DEX_ACC_PUBLIC)) continue;
         dvm_ref field = reflect_field_object(vm, c, fd);
         if (!field) { free(types); return false; }
         items[at++] = field;
      }
   }
   free(types);
   RETL(arr);
oom:
   free(types);
   dvm__throw(vm, "java/lang/OutOfMemoryError", "public fields");
   return false;
}

/* Every field this class declares, static ones included — the field-by-field
 * walk a serializer does before it can map an object at all.  An empty array
 * would have said "this class has no state". */
static bool cl_getDeclaredFields(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *co = dvm__obj(vm, self);
   struct dvm_class *cls = co ? co->klass : NULL;
   int ni = cls ? cls->nifields : 0, ns = cls ? cls->nsfields : 0;
   if (vm->trace)
      fprintf(stderr, "[dvm] Class.getDeclaredFields %s -> %d instance, %d static\n",
              cls && cls->name ? cls->name : "?", ni, ns);
   dvm_ref arr =
      dvm_new_array(vm, 'L', "Ljava/lang/reflect/Field;", (uint32_t)(ni + ns));
   dvm_ref *items = arr ? (dvm_ref *)dvm_array_data(vm, arr) : NULL;
   for (int i = 0; items && i < ni + ns; ++i) {
      const struct dvm_field *fd = i < ni ? &cls->ifields[i]
                                          : &cls->sfields[i - ni];
      dvm_ref field = reflect_field_object(vm, cls, fd);
      if (!field) return false;
      items[i] = field;
   }
   RETL(arr);
}

static bool rm_getName(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &v);
   RETL(v.l);
}

/* The descriptor letter a primitive java.lang.Class stands for.  The
 * primitives are modelled as builtin classes named exactly "int", "long", …
 * so Integer.TYPE and int.class are the same object the platform hands out. */
static const char *rt_prim_desc(const char *name)
{
   static const struct { const char *n, *d; } t[] = {
      { "int", "I" },     { "long", "J" },   { "short", "S" },
      { "byte", "B" },    { "char", "C" },   { "boolean", "Z" },
      { "float", "F" },   { "double", "D" }, { "void", "V" },
   };
   for (size_t i = 0; i < sizeof t / sizeof t[0]; ++i)
      if (!strcmp(name, t[i].n)) return t[i].d;
   if (name[0] && !name[1] && strchr("IJSBCZFDV", name[0])) return name;
   return NULL;
}

/* Unbox a reflective argument into the primitive the callee declares.
 *
 * Method.invoke() and Constructor.newInstance() take Object[], and Java
 * applies a widening primitive conversion on the way in: an Integer is a
 * legal argument for a `long` parameter.  Copying the box's raw slot instead
 * left the top half of a 64-bit register holding whatever sat next to a
 * 32-bit box, so `invoke(stream, "skip", 16)` skipped 6,594 bytes — the whole
 * rest of the stream — and AppsFlyer's decrypted dex came out empty.  The
 * conversion is the fix; the box knows what it holds, the signature says what
 * is wanted. */
static bool reflect_unbox(struct dvm *vm, dvm_ref ref, char want,
                          union dvm_value *out)
{
   struct dvm_object *bo = ref ? dvm__obj(vm, ref) : NULL;
   if (!bo || !bo->slots) return false;
   struct dvm_class *c = dvm_object_class(vm, ref);
   const char *n = (c && c->name) ? c->name : "";
   const union dvm_value v = bo->slots[0];
   char have = 'I';                     /* Integer/Short/Byte/Character/Boolean */
   if (!strcmp(n, "java/lang/Long"))        have = 'J';
   else if (!strcmp(n, "java/lang/Float"))  have = 'F';
   else if (!strcmp(n, "java/lang/Double")) have = 'D';
   const int64_t iv = have == 'J' ? v.j
                    : have == 'F' ? (int64_t)v.f
                    : have == 'D' ? (int64_t)v.d : (int64_t)v.i;
   const double dv = have == 'J' ? (double)v.j
                   : have == 'F' ? (double)v.f
                   : have == 'D' ? v.d : (double)v.i;
   memset(out, 0, sizeof *out);
   switch (want) {
   case 'J': out->j = iv; break;
   case 'D': out->d = dv; break;
   case 'F': out->f = (float)dv; break;
   default:  out->i = (int32_t)iv; break;
   }
   return true;
}

static bool class_desc_of(struct dvm *vm, dvm_ref cref, char *buf, size_t sz)
{
   struct dvm_object *o = dvm__obj(vm, cref);
   if (!o || !o->klass || !o->klass->name) return false;
   const char *prim = rt_prim_desc(o->klass->name);
   if (prim) { snprintf(buf, sz, "%s", prim); return true; }
   if (o->klass->name[0] == '[') { snprintf(buf, sz, "%s", o->klass->name);
                                   return true; }
   snprintf(buf, sz, "L%s;", o->klass->name);
   return true;
}

/* LUNARIA_DVM_REFLECT=1 — every Method.invoke(), with what it was handed and
 * what it answered.
 *
 * Obfuscated SDKs fold platform answers into their decryption keys: they ask
 * the framework reflectively for a constant nobody else ever reads, and a
 * value that is merely plausible rather than the one AOSP computes takes the
 * whole class down in <clinit>.  Drawable.resolveOpacity(3, 7) was one; the
 * ViewConfiguration timeouts were another.  Nothing in the failure names the
 * call, so this prints the reflective boundary itself — the one place where
 * "what did the app ask the platform, and what did we say" is answerable. */
static bool rt_reflect_trace(void)
{
   static _Atomic(const char *) slot;
   static _Atomic(int) known;      /* 0 = cold, 1 = on, 2 = off */
   int k = atomic_load_explicit(&known, memory_order_relaxed);
   if (!k) {
      const char *v = getenv("LUNARIA_DVM_REFLECT");
      atomic_store_explicit(&slot, v, memory_order_relaxed);
      k = (v && *v && strcmp(v, "0")) ? 1 : 2;
      atomic_store_explicit(&known, k, memory_order_relaxed);
   }
   return k == 1;
}

/* One argument or result, as a short human-readable token. */
static void rt_reflect_val(struct dvm *vm, dvm_ref r, char *buf, size_t sz)
{
   if (!r) { snprintf(buf, sz, "null"); return; }
   struct dvm_object *o = dvm__obj(vm, r);
   const char *cn = (o && o->cls && o->cls->name) ? o->cls->name : "?";
   if (!strcmp(cn, "java/lang/String")) {
      snprintf(buf, sz, "\"%s\"", dvm_string_utf8(vm, r));
      return;
   }
   if (o && o->slots && o->cls && o->cls->islots) {
      /* A box holds its value in slot 0, but only java.lang.Long fills all 64
       * bits of it — printing the others as a long shows the caller's stack
       * junk in the upper half and makes a correct answer look wrong. */
      if (!strcmp(cn, "java/lang/Long") || !strcmp(cn, "java/lang/Double")) {
         snprintf(buf, sz, "%lld", (long long)o->slots[0].j);
         return;
      }
      if (!strcmp(cn, "java/lang/Integer") || !strcmp(cn, "java/lang/Short") ||
          !strcmp(cn, "java/lang/Byte")    || !strcmp(cn, "java/lang/Boolean") ||
          !strcmp(cn, "java/lang/Character")) {
         snprintf(buf, sz, "%d", (int)o->slots[0].i);
         return;
      }
   }
   snprintf(buf, sz, "%s@%lx", cn, (unsigned long)r);
}

static void rt_reflect_log(struct dvm *vm, const struct dvm_class *cls,
                           const char *name, const char *sig,
                           const uint32_t *slots, int n, dvm_ref result)
{
   char line[512];
   int at = snprintf(line, sizeof line, "[reflect] %s.%s%s(",
                     cls && cls->name ? cls->name : "?", name ? name : "?",
                     sig ? sig : "");
   for (int i = 0; i < n && at > 0 && at < (int)sizeof line; ++i) {
      char v[128];
      rt_reflect_val(vm, slots[i], v, sizeof v);
      at += snprintf(line + at, sizeof line - (size_t)at, "%s%s",
                     i ? ", " : "", v);
   }
   char rv[128];
   rt_reflect_val(vm, result, rv, sizeof rv);
   if (at > 0 && at < (int)sizeof line)
      snprintf(line + at, sizeof line - (size_t)at, ") -> %s", rv);
   fprintf(stderr, "%s\n", line);
}

/* Reflection's wrapper exception, with the failure it wraps kept.
 *
 * InvocationTargetException exists precisely to carry the exception the called
 * method threw; getTargetException()/getCause() are how every caller gets at
 * it, and unwrapping is the standard idiom for rethrowing the real failure
 * (`Throwable c = t.getCause(); if (c != null) throw c;`).  Discarding the
 * target and reporting only the method name turned every reflective failure
 * into the same contentless wrapper — including the one that took AppsFlyer's
 * <clinit> down, where nothing in the process could say what had failed. */
static bool reflect_target_failed(struct dvm *vm, const char *what)
{
   const dvm_ref target = dvm_exception(vm);
   dvm_pin(vm, target);
   dvm_clear_exception(vm);
   dvm__throw(vm, "java/lang/reflect/InvocationTargetException", "%s", what);
   const dvm_ref ite = dvm_exception(vm);
   if (ite && target) {
      union dvm_value v = { .l = target };
      (void)dvm_set_field(vm, ite, "cause", "Ljava/lang/Throwable;", v);
   }
   return false;
}

static bool rm_invoke(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   union dvm_value ownerv = { 0 }, namev = { 0 }, ptypesv = { 0 };
   (void)dvm_get_field(vm, self, "owner", "Ljava/lang/Class;", &ownerv);
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &namev);
   (void)dvm_get_field(vm, self, "paramTypes", "[Ljava/lang/Class;", &ptypesv);
   const char *name = dvm_string_utf8(vm, namev.l);
   const bool is_static = (rt_get_int(vm, self, "access") & 0x0008) != 0;

   dvm_ref recv = ARG(0).l;
   struct dvm_class *cls = NULL;
   if (is_static) {
      /* Static Method.invoke(null, …) — and also invoke(SomeClass.class, …),
       * which some SDKs use.  The receiver is never the Class meta-object;
       * dispatch goes to the method's declaring class. */
      struct dvm_object *co = dvm__obj(vm, ownerv.l);
      cls = co ? co->klass : NULL;
      recv = 0;
   } else if (recv) {
      /* Dispatch through the receiver's Java type.  A Class meta-object keeps
       * the represented type in object->klass, but its runtime type is still
       * java.lang.Class.  Using the represented type here turned a reflective
       * Class.getResource() into AFa1hSDK.getResource() and guaranteed an
       * unresolved call. */
      cls = dvm_object_class(vm, recv);
   }
   if (!cls) {
      struct dvm_object *co = dvm__obj(vm, ownerv.l);
      cls = co ? co->klass : NULL;
   }
   /* The trailing Object[] carries the arguments. */
   uint32_t slots[16];
   int n = 0;
   dvm_ref arr = (nargs > 1) ? ARG(1).l : 0;
   uint32_t len = arr ? dvm_array_length(vm, arr) : 0;
   const dvm_ref *items = arr ? dvm_array_data(vm, arr) : NULL;
   for (; items && (uint32_t)n < len && n < 16; ++n) slots[n] = items[n];

   /* The parameter descriptor getMethod() was given.  With it the right
    * overload can be picked and a boxed argument handed over as the primitive
    * the callee declares; without it every parameter has to be guessed as
    * Object, which is how a reflective call landed on an unrelated method. */
   char prefix[256];
   int at = 0;
   prefix[at++] = '(';
   bool have_types = ptypesv.l != 0;
   uint32_t np = ptypesv.l ? dvm_array_length(vm, ptypesv.l) : 0;
   const dvm_ref *ptv = ptypesv.l ? dvm_array_data(vm, ptypesv.l) : NULL;
   char pdesc[16][64];
   memset(pdesc, 0, sizeof pdesc);
   for (uint32_t i = 0; have_types && ptv && i < np; ++i) {
      if (i >= 16 || !class_desc_of(vm, ptv[i], pdesc[i], sizeof pdesc[i])) {
         have_types = false;
         break;
      }
      at += snprintf(prefix + at, sizeof prefix - (size_t)at, "%s", pdesc[i]);
      if (at >= (int)sizeof prefix - 2) { have_types = false; break; }
   }
   prefix[at] = '\0';
   if (have_types) { prefix[at++] = ')'; prefix[at] = '\0'; }

   struct dvm_method *m = NULL;
   if (cls && name && have_types) {
      size_t plen = strlen(prefix);
      for (struct dvm_class *k = cls; k && !m; k = k->super)
         for (int i = 0; i < k->nmethods; ++i)
            if (k->methods[i].name && !strcmp(k->methods[i].name, name) &&
                k->methods[i].sig &&
                !strncmp(k->methods[i].sig, prefix, plen)) {
               m = &k->methods[i];
               break;
            }
   }
   if (!m) m = (cls && name) ? dvm_find_method(vm, cls, name, NULL) : NULL;
   if (!m) {
      /* No dex definition: hand it to the host stubs, the same way the
       * interpreter does for a call into a host-backed class.  Reflection
       * carries no descriptor, so synthesise one of the right arity. */
      if (!cls || !name) {
         dvm__throw(vm, "java/lang/reflect/InvocationTargetException", "%s",
                    name ? name : "?");
         return false;
      }
      char sig[320];
      if (have_types) {
         snprintf(sig, sizeof sig, "%sLjava/lang/Object;", prefix);
      } else {
         int k = 0;
         sig[k++] = '(';
         for (int i = 0; i < n && k < 280; ++i) {
            memcpy(sig + k, "Ljava/lang/Object;", 18);
            k += 18;
         }
         sig[k] = '\0';
         strcat(sig, ")Ljava/lang/Object;");
      }
      union dvm_value ret = { 0 };
      if (!dvm__call_out(vm, cls, name, sig, is_static, false, recv, slots, n,
                         &ret)) {
         char what[256];
         snprintf(what, sizeof what, "%s.%s%s", cls->name, name, sig);
         return reflect_target_failed(vm, what);
      }
      if (rt_reflect_trace())
         rt_reflect_log(vm, cls, name, sig, slots, n, ret.l);
      *out = ret;
      return true;
   }

   union dvm_value call_args[16];
   memset(call_args, 0, sizeof call_args);
   for (int i = 0; i < n; ++i) {
      /* A primitive parameter arrives boxed — Method.invoke() takes Object[].
       * Passing the box reference through as if it were the number is how a
       * reflective call ended up with an arbitrary integer.
       *
       * getMethod(name, Class...) names the types, but plenty of code reaches
       * a Method another way (getDeclaredMethods() and a name match is the
       * common one) and then paramTypes is empty.  The method that was
       * actually resolved knows its own signature, so use that when the
       * caller did not supply the types — otherwise a `long` parameter gets an
       * object handle, which is an enormous positive number.  AppsFlyer skips
       * a header with `invoke(stream, "skip", n)`; with the handle for n the
       * skip ran past the end of its asset and the decrypting stream threw
       * "unexpected block size". */
      char pk = 0;
      if (have_types && (uint32_t)i < np && i < 16 && pdesc[i][0])
         pk = pdesc[i][0];
      else if (m->sig) {
         char pd[64];
         if (dvm__sig_param(m->sig, i, pd, sizeof pd)) pk = pd[0];
      }
      if (pk && pk != 'L' && pk != '[' &&
          reflect_unbox(vm, slots[i], pk, &call_args[i]))
         continue;
      call_args[i].l = slots[i];
   }

   /* The same line the host-stub path logs, so LUNARIA_DVM_REFLECT shows a
    * reflective call whether or not the target has a dex body. */
   if (rt_reflect_trace())
      rt_reflect_log(vm, m->cls, m->name, m->sig, slots, n, 0);
   union dvm_value ret = { 0 };
   if (!dvm_call(vm, m, recv, call_args, n, &ret)) {
      char what[256];
      snprintf(what, sizeof what, "%s.%s%s",
               m->cls && m->cls->name ? m->cls->name : "?", m->name,
               m->sig ? m->sig : "");
      return reflect_target_failed(vm, what);
   }
   /* invoke() answers Object, so a primitive result comes back boxed. */
   dvm_ref boxed = 0;
   switch (m->ret_kind) {
   case 'I': boxed = box_make(vm, "Ljava/lang/Integer;", ret); break;
   case 'J': boxed = box_make(vm, "Ljava/lang/Long;", ret); break;
   case 'S': boxed = box_make(vm, "Ljava/lang/Short;", ret); break;
   case 'B': boxed = box_make(vm, "Ljava/lang/Byte;", ret); break;
   case 'C': boxed = box_make(vm, "Ljava/lang/Character;", ret); break;
   case 'Z': boxed = box_make(vm, "Ljava/lang/Boolean;", ret); break;
   case 'F': boxed = box_make(vm, "Ljava/lang/Float;", ret); break;
   case 'D': boxed = box_make(vm, "Ljava/lang/Double;", ret); break;
   case 'V': ret.l = 0; break;
   default: break;   /* already a reference */
   }
   if (boxed) ret.l = boxed;
   if (rt_reflect_trace())
      rt_reflect_log(vm, m->cls, m->name, m->sig, slots, n, ret.l);
   *out = ret;
   return true;
}

static const struct rt_field rt_method_fields[] = {
   { "owner", "Ljava/lang/Class;" }, { "name", "Ljava/lang/String;" },
   /* getMethod() is handed the parameter types; dropping them left invoke()
    * unable to tell one overload from another, or a boxed argument from the
    * primitive the callee actually takes. */
   { "paramTypes", "[Ljava/lang/Class;" },
   /* getDeclaredMethods() records the dex signature and access flags so
    * ReflectionHelper (and any other reflective probe) can call
    * getReturnType()/getParameterTypes()/getModifiers() for real, instead
    * of matching against a Method that only knew its name. */
   { "sig", "Ljava/lang/String;" },
   { "access", "I" },
   F_END
};
static const struct rt_field rt_accessible_fields[] = {
   { "accessible", "Z" }, F_END
};

/* One type descriptor → Class.  Primitives are the single-letter classes
 * ("I", "Z", …) already installed as is_primitive; reference and array
 * descriptors go through the normal class table. */
static dvm_ref class_for_type_desc(struct dvm *vm, const char *desc, size_t len)
{
   if (!desc || !len) return 0;
   char buf[512];
   if (len >= sizeof buf) return 0;
   memcpy(buf, desc, len);
   buf[len] = '\0';
   return class_object_for(vm, dvm__class_by_desc(vm, buf));
}

static const char *type_desc_skip(const char *p)
{
   if (!p || !*p) return p;
   if (*p == '[') {
      while (*p == '[') ++p;
      if (*p == 'L') { while (*p && *p != ';') ++p; if (*p == ';') ++p; }
      else if (*p) ++p;
      return p;
   }
   if (*p == 'L') { while (*p && *p != ';') ++p; if (*p == ';') ++p; return p; }
   return p + 1;
}

static bool rm_getDeclaringClass(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "owner", "Ljava/lang/Class;", &v);
   RETL(v.l);
}

static bool rm_getModifiers(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm, self, "access"));
}

static bool rm_getReturnType(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value sv = { 0 };
   (void)dvm_get_field(vm, self, "sig", "Ljava/lang/String;", &sv);
   const char *sig = dvm_string_utf8(vm, sv.l);
   if (!sig) RETL(0);
   const char *r = strchr(sig, ')');
   if (!r || !r[1]) RETL(0);
   ++r;
   const char *end = type_desc_skip(r);
   RETL(class_for_type_desc(vm, r, (size_t)(end - r)));
}

static bool rm_getParameterTypes(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   /* Prefer the Class[] getMethod() already stored — that is authoritative
    * for a Method built by name lookup.  getDeclaredMethods() leaves it
    * empty and fills `sig` instead. */
   union dvm_value pv = { 0 };
   if (dvm_get_field(vm, self, "paramTypes", "[Ljava/lang/Class;", &pv) &&
       pv.l)
      RETL(pv.l);
   union dvm_value sv = { 0 };
   (void)dvm_get_field(vm, self, "sig", "Ljava/lang/String;", &sv);
   const char *sig = dvm_string_utf8(vm, sv.l);
   if (!sig || sig[0] != '(') RETL(dvm_new_array(vm, 'L', "Ljava/lang/Class;", 0));
   const char *p = sig + 1;
   const char *ends[32];
   const char *starts[32];
   int n = 0;
   while (*p && *p != ')' && n < 32) {
      starts[n] = p;
      p = type_desc_skip(p);
      ends[n] = p;
      ++n;
   }
   dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/lang/Class;", (uint32_t)n);
   dvm_ref *items = arr ? (dvm_ref *)dvm_array_data(vm, arr) : NULL;
   for (int i = 0; items && i < n; ++i)
      items[i] = class_for_type_desc(vm, starts[i], (size_t)(ends[i] - starts[i]));
   RETL(arr);
}

/* AccessibleObject.setAccessible / isAccessible, shared by Method, Field and
 * Constructor.  It suppresses the access checks — which this VM does not
 * perform in the first place, so the only observable part is the flag itself,
 * and that has to read back as what was set. */
static bool ao_setAccessible(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs;
   union dvm_value v = { .i = ARG(0).i != 0 };
   (void)dvm_set_field(vm, self, "accessible", "Z", v);
   RETV();
}

static bool ao_isAccessible(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "accessible", "Z", &v);
   RETI(v.i ? 1 : 0);
}

static bool rf_isSynthetic(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out);
static bool rf_isVarArgs(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI((rt_get_int(vm, self, "access") & DEX_ACC_VARARGS) != 0);
}
static bool rm_isBridge(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI((rt_get_int(vm, self, "access") & DEX_ACC_BRIDGE) != 0);
}
static bool ao_setAccessibleAll(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)self; (void)nargs;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "array"); return false; }
   const dvm_ref *items = dvm_array_data(vm, ARG(0).l);
   const uint32_t n = dvm_array_length(vm, ARG(0).l);
   union dvm_value v = { .i = ARG(1).i != 0 };
   for (uint32_t i = 0; items && i < n; ++i)
      if (items[i]) (void)dvm_set_field(vm, items[i], "accessible", "Z", v);
   RETV();
}
static const struct rt_method rt_accessible_object[]={
   M("setAccessible","(Z)V",ao_setAccessible),
   SM("setAccessible","([Ljava/lang/reflect/AccessibleObject;Z)V",ao_setAccessibleAll),
   M("isAccessible","()Z",ao_isAccessible),M_END
};
static const struct rt_method rt_executable[]={
   {"getName","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M("isSynthetic","()Z",rf_isSynthetic), M("isVarArgs","()Z",rf_isVarArgs), M_END
};
static const struct rt_method rt_reflect_member[]={
   {"getName","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getDeclaringClass","()Ljava/lang/Class;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getModifiers","()I",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"isSynthetic","()Z",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},M_END
};

static bool rm_getAnnotations(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out);
static bool rm_getAnnotation(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out);

static bool rm_isAnnotationPresent(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   union dvm_value r = { 0 };
   if (!rm_getAnnotation(vm, self, args, nargs, &r)) return false;
   RETI(r.l != 0);
}
static bool rm_getParameterAnnotations(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out);
static bool rm_isDefault(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out);
static bool rm_generic(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out);

static const struct rt_method rt_reflect_method[] = {
   M("getName", "()Ljava/lang/String;", rm_getName),
   M("getDeclaringClass", "()Ljava/lang/Class;", rm_getDeclaringClass),
   M("getModifiers", "()I", rm_getModifiers),
   M("getReturnType", "()Ljava/lang/Class;", rm_getReturnType),
   M("getParameterTypes", "()[Ljava/lang/Class;", rm_getParameterTypes),
   M("isAccessible", "()Z", ao_isAccessible),
   M("invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",
     rm_invoke),
   M("setAccessible", "(Z)V", ao_setAccessible),
   M("getAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",
     rm_getAnnotation),
   M("isAnnotationPresent", "(Ljava/lang/Class;)Z", rm_isAnnotationPresent),
   M("getAnnotations", "()[Ljava/lang/annotation/Annotation;", rm_getAnnotations),
   M("getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;",
     rm_getAnnotations),
   M("getParameterAnnotations", "()[[Ljava/lang/annotation/Annotation;",
     rm_getParameterAnnotations),
   M("getGenericParameterTypes", "()[Ljava/lang/reflect/Type;", rm_generic),
   M("getGenericReturnType", "()Ljava/lang/reflect/Type;", rm_generic),
   M("isDefault", "()Z", rm_isDefault),
   M("isBridge", "()Z", rm_isBridge),
   M_END,
};

/* java.lang.reflect.Proxy — AdvertisingIdClient (and similar AIDL clients)
 * builds a stub with newProxyInstance(loader, {IFace}, handler) and then
 * invoke-interfaces through it.  Without a real Proxy the call notes
 * unresolved and returns null; the client then NPEs or wraps a bogus
 * InvocationTargetException.  Each proxy is a fresh $ProxyN class that
 * extends Proxy and lists the requested interfaces so check-cast /
 * instanceof work through the normal class_assignable path. */

static bool class_extends_proxy(struct dvm_class *c)
{
   for (; c; c = c->super)
      if (c->name && !strcmp(c->name, "java/lang/reflect/Proxy"))
         return true;
   return false;
}

static bool class_is_generated_proxy(struct dvm_class *c)
{
   /* Proxy itself is not; only classes newProxyInstance minted. */
   return c && c->generated_proxy;
}

static bool reflect_proxy_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   if(!nargs || !ARG(0).l) {
      dvm__throw(vm,"java/lang/NullPointerException","InvocationHandler"); return false;
   }
   if(!dvm_set_field(vm,self,"h","Ljava/lang/reflect/InvocationHandler;",ARG(0))) return false;
   RETV();
}

static struct dvm_class *proxy_make_class(struct dvm *vm, dvm_ref loader, dvm_ref ifaces_arr)
{
   struct dvm_object *array=dvm__obj(vm,ifaces_arr);
   if(!array) { dvm__throw(vm,"java/lang/NullPointerException","interfaces"); return NULL; }
   if(array->kind!=DVM_OBJ_ARRAY || array->length>65535) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","invalid interfaces"); return NULL;
   }
   const uint32_t count=array->length;
   const dvm_ref *entries=array->data;
   const char *package=NULL;
   size_t package_len=0;
   for(uint32_t i=0;i<count;++i) {
      struct dvm_object *co=dvm__obj(vm,entries[i]);
      if(!co) { dvm__throw(vm,"java/lang/NullPointerException","interface"); return NULL; }
      if(co->kind!=DVM_OBJ_CLASS || !co->klass || !co->klass->is_interface) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","not an interface: %s",
                    co->klass ? co->klass->name : co->cls ? co->cls->name : "null");
         return NULL;
      }
      for(uint32_t j=0;j<i;++j) if(entries[j]==entries[i]) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","repeated interface"); return NULL;
      }
      if(!(co->klass->access & DEX_ACC_PUBLIC)) {
         const char *slash=strrchr(co->klass->desc,'/');
         size_t length=slash ? (size_t)(slash-co->klass->desc)+1 : 1;
         if(package && (length!=package_len || memcmp(package,co->klass->desc,length))) {
            dvm__throw(vm,"java/lang/IllegalArgumentException","non-public interfaces in different packages"); return NULL;
         }
         package=co->klass->desc;package_len=length;
      }
   }
   for(int i=0;i<vm->nclasses;++i) {
      struct dvm_class *cached=vm->classes[i];
      if(!class_is_generated_proxy(cached) || !cached->methods || cached->nmethods!=1 ||
         cached->defining_loader!=loader ||
         cached->nifaces!=(int)count) continue;
      uint32_t j=0;
      while(j<count && cached->ifaces[j]==dvm__obj(vm,entries[j])->klass) ++j;
      if(j==count) return cached;
   }
   static int seq;
   char *desc=malloc(package_len+49);
   if(!desc) { dvm__throw(vm,"java/lang/OutOfMemoryError","proxy name"); return NULL; }
   if(package) memcpy(desc,package,package_len);
   else { desc[0]='L';package_len=1; }
   snprintf(desc+package_len,48,"$Proxy%d;",seq++);

   struct dvm_class *proxy =
      dvm__class_by_desc(vm, "Ljava/lang/reflect/Proxy;");
   if (!proxy || !dvm_init_class(vm, proxy)) { free(desc);return NULL; }

   struct dvm_class *c = dvm__register_builtin(vm, desc);
   free(desc);
   if (!c) return NULL;
   c->super = proxy;
   c->generated_proxy=true;
   c->access = DEX_ACC_FINAL | (package ? 0 : DEX_ACC_PUBLIC);
   c->defining_loader=loader;
   if(loader) dvm_pin(vm,loader); /* class metadata lives for this VM's lifetime */
   c->init_state = 2;
   c->islots = proxy->islots;
   c->nifields = 0;
   c->ifields = NULL;
   c->methods=calloc(1,sizeof *c->methods);
   if(!c->methods) { dvm__throw(vm,"java/lang/OutOfMemoryError","proxy constructor"); return NULL; }
   c->nmethods=1;
   c->methods[0]=(struct dvm_method){.cls=c,.name="<init>",
      .sig=strdup("(Ljava/lang/reflect/InvocationHandler;)V"),.access=DEX_ACC_PUBLIC | DEX_ACC_CONSTRUCTOR,
      .builtin=reflect_proxy_init,.arg_slots=1,.arg_count=1,.ret_kind='V'};
   if(!c->methods[0].sig) { dvm__throw(vm,"java/lang/OutOfMemoryError","proxy signature"); return NULL; }

   uint32_t n = ifaces_arr ? dvm_array_length(vm, ifaces_arr) : 0;
   const dvm_ref *items = ifaces_arr ? dvm_array_data(vm, ifaces_arr) : NULL;
   if (n > 0 && items) {
      c->ifaces = calloc((size_t)n, sizeof *c->ifaces);
      if(!c->ifaces) { dvm__throw(vm,"java/lang/OutOfMemoryError","proxy interfaces"); return NULL; }
      if (c->ifaces) {
         for (uint32_t i = 0; i < n; ++i) {
            struct dvm_object *co = dvm__obj(vm, items[i]);
            if (co && co->kind == DVM_OBJ_CLASS && co->klass)
               c->ifaces[c->nifaces++] = co->klass;
         }
      }
   }
   return c;
}

static dvm_ref proxy_make_method(struct dvm *vm, struct dvm_class *decl,
                                 const char *name, const char *sig)
{
   struct dvm_class *mc = dvm__class_by_desc(vm, "Ljava/lang/reflect/Method;");
   dvm_ref m = mc ? dvm_new_object(vm, mc) : 0;
   if (!m) return 0;
   union dvm_value v = { .l = class_object_for(vm, decl) };
   (void)dvm_set_field(vm, m, "owner", "Ljava/lang/Class;", v);
   v.l = dvm_new_string(vm, name ? name : "");
   (void)dvm_set_field(vm, m, "name", "Ljava/lang/String;", v);
   if (sig) {
      v.l = dvm_new_string(vm, sig);
      (void)dvm_set_field(vm, m, "sig", "Ljava/lang/String;", v);
   }
   struct dvm_method *found =
      (decl && name) ? dvm_find_method(vm, decl, name, sig) : NULL;
   if (found) {
      v.i = (int32_t)found->access;
      (void)dvm_set_field(vm, m, "access", "I", v);
   }
   /* Parameter Class[] so Method.invoke / getParameterTypes stay consistent. */
   int np = dvm_sig_arg_count(sig);
   if (np < 0) np = 0;
   dvm_ref pt = dvm_new_array(vm, 'L', "Ljava/lang/Class;", (uint32_t)np);
   dvm_ref *pti = pt ? (dvm_ref *)dvm_array_data(vm, pt) : NULL;
   for (int i = 0; pti && i < np; ++i) {
      char one[128];
      if (!dvm__sig_param(sig, i, one, sizeof one)) break;
      pti[i] = class_object_for(vm, dvm__class_by_desc(vm, one));
   }
   v.l = pt;
   dvm_pin(vm, pt);
   (void)dvm_set_field(vm, m, "paramTypes", "[Ljava/lang/Class;", v);
   return m;
}

static dvm_ref proxy_box_arg(struct dvm *vm, char kind, union dvm_value v)
{
   switch (kind) {
   case 'Z': return box_make(vm, "Ljava/lang/Boolean;", v);
   case 'B': return box_make(vm, "Ljava/lang/Byte;", v);
   case 'C': return box_make(vm, "Ljava/lang/Character;", v);
   case 'S': return box_make(vm, "Ljava/lang/Short;", v);
   case 'I': return box_make(vm, "Ljava/lang/Integer;", v);
   case 'J': return box_make(vm, "Ljava/lang/Long;", v);
   case 'F': return box_make(vm, "Ljava/lang/Float;", v);
   case 'D': return box_make(vm, "Ljava/lang/Double;", v);
   default:  return v.l;
   }
}

bool dvm_proxy_try_invoke(struct dvm *vm, dvm_ref self, struct dvm_class *cls,
                          const char *name, const char *sig,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o || !o->cls || !class_extends_proxy(o->cls)) return false;
   /* Do not re-enter for Proxy's own statics / reflective helpers. */
   if (cls && cls->name && !strcmp(cls->name, "java/lang/reflect/Proxy"))
      return false;

   union dvm_value hv = { 0 };
   if (!dvm_get_field(vm, self, "h", "Ljava/lang/reflect/InvocationHandler;",
                      &hv) ||
       !hv.l) {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "proxy without handler");
      return true;
   }

   struct dvm_class *decl = cls;
   if (!decl) decl = o->cls;
   dvm_ref method = proxy_make_method(vm, decl, name, sig);
   int np = dvm_sig_arg_count(sig);
   if (np < 0) np = 0;
   if (nargs < np) np = nargs;
   dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/lang/Object;", (uint32_t)np);
   dvm_ref *items = arr ? (dvm_ref *)dvm_array_data(vm, arr) : NULL;
   for (int i = 0; items && i < np; ++i) {
      char one[128];
      char kind = 'L';
      if (dvm__sig_param(sig, i, one, sizeof one))
         kind = dvm__kind_of(one);
      items[i] = proxy_box_arg(vm, kind, args[i]);
   }

   struct dvm_class *hc = dvm_object_class(vm, hv.l);
   if (getenv("LUNARIA_DVM_PROXY"))
      fprintf(stderr, "[proxy] invoke %s.%s%s handler=%s@%u\n",
              decl && decl->name ? decl->name : "?", name, sig,
              hc && hc->name ? hc->name : "?", hv.l);
   struct dvm_method *inv =
      hc ? dvm_find_method(
              vm, hc, "invoke",
              "(Ljava/lang/Object;Ljava/lang/reflect/Method;[Ljava/lang/Object;)"
              "Ljava/lang/Object;")
         : NULL;
   if (!inv)
      inv = dvm_find_method(
          vm, dvm__class_by_desc(vm, "Ljava/lang/reflect/InvocationHandler;"),
          "invoke",
          "(Ljava/lang/Object;Ljava/lang/reflect/Method;[Ljava/lang/Object;)"
          "Ljava/lang/Object;");
   if (!inv) {
      dvm__throw(vm, "java/lang/AbstractMethodError",
                 "InvocationHandler.invoke");
      return true;
   }

   union dvm_value call_args[3] = {
      { .l = self }, { .l = method }, { .l = arr }
   };
   union dvm_value ret = { 0 };
   if (!dvm_call(vm, inv, hv.l, call_args, 3, &ret))
      return true; /* exception already set */

   char rk = dvm_sig_return_kind(sig);
   memset(out, 0, sizeof *out);
   if (rk == 'V') return true;
   if (rk == 'L' || rk == '[') {
      out->l = ret.l;
      return true;
   }
   if (!ret.l) return true; /* null unbox → zero, matching a missing value */
   struct dvm_object *bo = dvm__obj(vm, ret.l);
   if (bo && bo->slots) *out = bo->slots[0];
   return true;
}

static bool proxy_newProxyInstance(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)self;
   if (nargs < 3) {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "newProxyInstance args");
      return false;
   }
   dvm_ref ifaces = ARG(1).l;
   dvm_ref handler = ARG(2).l;
   if (!ifaces || !handler) {
      dvm__throw(vm, "java/lang/NullPointerException", "newProxyInstance");
      return false;
   }
   struct dvm_class *pc = proxy_make_class(vm, ARG(0).l, ifaces);
   if (!pc) {
      if(!dvm_exception(vm)) dvm__throw(vm, "java/lang/InternalError", "proxy class");
      return false;
   }
   dvm_ref obj = dvm_new_object(vm, pc);
   if (!obj) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "proxy");
      return false;
   }
   union dvm_value v = { .l = handler };
   (void)dvm_set_field(vm, obj, "h", "Ljava/lang/reflect/InvocationHandler;",
                       v);
   if (getenv("LUNARIA_DVM_PROXY"))
      fprintf(stderr, "[proxy] newProxyInstance %s ifaces=%u h=%u -> %u\n",
              pc->name, dvm_array_length(vm, ifaces), handler, obj);
   RETL(obj);
}

static bool proxy_getProxyClass(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)self;
   if(nargs<2) { dvm__throw(vm,"java/lang/IllegalArgumentException","getProxyClass args"); return false; }
   struct dvm_class *c=proxy_make_class(vm,ARG(0).l,ARG(1).l);
   if(!c) return false;
   RETL(dvm_class_object(vm,c));
}

static bool proxy_getInvocationHandler(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)self; (void)nargs;
   dvm_ref proxy = ARG(0).l;
   struct dvm_object *o = dvm__obj(vm, proxy);
   if (!o || !class_is_generated_proxy(o->cls)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "not a proxy instance");
      return false;
   }
   union dvm_value hv = { 0 };
   (void)dvm_get_field(vm, proxy, "h", "Ljava/lang/reflect/InvocationHandler;",
                       &hv);
   RETL(hv.l);
}

static bool proxy_isProxyClass(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_object *co = dvm__obj(vm, ARG(0).l);
   RETI(co && co->kind == DVM_OBJ_CLASS &&
        class_is_generated_proxy(co->klass));
}

static const struct rt_field rt_proxy_fields[] = {
   { "h", "Ljava/lang/reflect/InvocationHandler;" },
   F_END
};

static const struct rt_method rt_proxy[] = {
   M("<init>","(Ljava/lang/reflect/InvocationHandler;)V",reflect_proxy_init),
   SM("getProxyClass","(Ljava/lang/ClassLoader;[Ljava/lang/Class;)Ljava/lang/Class;",proxy_getProxyClass),
   SM("newProxyInstance",
      "(Ljava/lang/ClassLoader;[Ljava/lang/Class;"
      "Ljava/lang/reflect/InvocationHandler;)Ljava/lang/Object;",
      proxy_newProxyInstance),
   SM("getInvocationHandler",
      "(Ljava/lang/Object;)Ljava/lang/reflect/InvocationHandler;",
      proxy_getInvocationHandler),
   SM("isProxyClass", "(Ljava/lang/Class;)Z", proxy_isProxyClass),
   M_END,
};

static const struct rt_method rt_runnable[] = {
   M("run","()V",NULL), M_END,
};

/* The primitive kind the Field accessor overload the caller named works in.
 * Field.get/set take and answer Object, so the value crosses boxed; getInt,
 * setLong and the rest take and answer the primitive directly. */
static char rf_accessor_kind(void)
{
   struct dvm_method *m = dvm__builtin_method();
   const char *n = m ? m->name : NULL;
   if (!n) return 'L';
   if (!strcmp(n, "get") || !strcmp(n, "set")) return 'L';
   const char *suffix = n + (n[0] == 'g' ? 3 : 3);   /* past "get"/"set" */
   if (!strcmp(suffix, "Int"))     return 'I';
   if (!strcmp(suffix, "Long"))    return 'J';
   if (!strcmp(suffix, "Short"))   return 'S';
   if (!strcmp(suffix, "Byte"))    return 'B';
   if (!strcmp(suffix, "Char"))    return 'C';
   if (!strcmp(suffix, "Boolean")) return 'Z';
   if (!strcmp(suffix, "Float"))   return 'F';
   if (!strcmp(suffix, "Double"))  return 'D';
   return 'L';
}

/* The widening Field.set performs between the value's primitive type and the
 * field's. */
static union dvm_value rf_widen(union dvm_value v, char from, char to)
{
   const int64_t i = from == 'J' ? v.j
                   : from == 'F' ? (int64_t)v.f
                   : from == 'D' ? (int64_t)v.d : (int64_t)v.i;
   const double d = from == 'J' ? (double)v.j
                  : from == 'F' ? (double)v.f
                  : from == 'D' ? v.d : (double)v.i;
   union dvm_value r = { 0 };
   switch (to) {
   case 'J': r.j = i; break;
   case 'D': r.d = d; break;
   case 'F': r.f = (float)d; break;
   default:  r.i = (int32_t)i; break;
   }
   return r;
}

/* A primitive field read through Field.get(), which answers Object. */
static dvm_ref rf_box(struct dvm *vm, char kind, union dvm_value v)
{
   switch (kind) {
   case 'I': return box_make(vm, "Ljava/lang/Integer;", v);
   case 'J': return box_make(vm, "Ljava/lang/Long;", v);
   case 'S': return box_make(vm, "Ljava/lang/Short;", v);
   case 'B': return box_make(vm, "Ljava/lang/Byte;", v);
   case 'C': return box_make(vm, "Ljava/lang/Character;", v);
   case 'Z': return box_make(vm, "Ljava/lang/Boolean;", v);
   case 'F': return box_make(vm, "Ljava/lang/Float;", v);
   case 'D': return box_make(vm, "Ljava/lang/Double;", v);
   default:  return v.l;
   }
}

static bool rf_get(struct dvm *vm, dvm_ref self,
                   const union dvm_value *args, int nargs,
                   union dvm_value *out)
{
   union dvm_value owner = { 0 }, namev = { 0 };
   (void)dvm_get_field(vm, self, "owner", "Ljava/lang/Class;", &owner);
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &namev);
   struct dvm_object *co = dvm__obj(vm, owner.l);
   struct dvm_class *target = co ? co->klass : NULL;
   const char *name = dvm_string_utf8(vm, namev.l);
   dvm_ref receiver = nargs > 0 ? ARG(0).l : 0;
   /* Reading a static field initializes its class, exactly as a `sget` would.
    * Reflection is not a back door around that: a class reached only through
    * Class.getField()/Field.get() would otherwise be seen with every static
    * still at its default. */
   if (target) (void)dvm_init_class(vm, target);
   /* sun.misc.Unsafe hands out its process singleton through theUnsafe.  Seed
    * the slot on first read so the ordinary static-field path below answers
    * it, and getDeclaredFields()/getDeclaredField() see the same field. */
   if (target && name && !strcmp(target->name, "sun/misc/Unsafe") &&
       !strcmp(name, "theUnsafe") && target->nsfields > 0 && target->sslots &&
       !target->sslots[0].l) {
      static dvm_ref unsafe;
      target->sslots[0].l = runtime_singleton(vm, "Lsun/misc/Unsafe;", &unsafe);
   }
   /* Field.get() answers Object, so a primitive field comes back boxed; the
      typed getters answer the primitive itself. */
   const char want = rf_accessor_kind();
   for (struct dvm_class *c = target; c; c = c->super) {
      for (int i = 0; i < c->nsfields; ++i) {
         struct dvm_field *f = &c->sfields[i];
         if (name && f->name && !strcmp(name, f->name)) {
            union dvm_value v = c->sslots[f->slot];
            if (f->kind == 'L') RETL(v.l);
            if (want == 'L') RETL(rf_box(vm, f->kind, v));
            *out = rf_widen(v, f->kind, want);
            return true;
         }
      }
      if (receiver) {
         for (int i = 0; i < c->nifields; ++i) {
            struct dvm_field *f = &c->ifields[i];
            if (!name || !f->name || strcmp(name, f->name)) continue;
            union dvm_value v = { 0 };
            struct dvm_object *object = dvm__obj(vm, receiver);
            if (!object || !object->slots || f->slot >= object->cls->islots ||
                !dvm__class_assignable(vm, object->cls, c)) {
               dvm__throw(vm, "java/lang/IllegalArgumentException", "field receiver");
               return false;
            }
            v = object->slots[f->slot];
            if (f->kind == 'L') RETL(v.l);
            if (want == 'L') RETL(rf_box(vm, f->kind, v));
            *out = rf_widen(v, f->kind, want);
            return true;
         }
      }
   }
   RETL(0);
}

static bool rf_getDeclaringClass(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value owner = { 0 };
   (void)dvm_get_field(vm, self, "owner", "Ljava/lang/Class;", &owner);
   RETL(owner.l);
}

/* A reflected Field must retain its dex type descriptor.  Serializers do not
 * infer it from the current value (which is frequently null); Field.getType
 * is metadata, and both primitive and array descriptors have to round-trip. */
static bool rf_getType(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value sig = { 0 };
   (void)dvm_get_field(vm, self, "sig", "Ljava/lang/String;", &sig);
   const char *desc = sig.l ? dvm_string_utf8(vm, sig.l) : NULL;
   RETL(desc ? class_for_type_desc(vm, desc, strlen(desc)) : 0);
}

static bool rf_getGenericType(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out);

static bool rf_isSynthetic(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   /* ACC_SYNTHETIC has the same 0x1000 value in dex and Java reflection. */
   RETI((rt_get_int(vm, self, "access") & 0x1000) != 0);
}

/* ACC_ENUM (0x4000) is the same bit in dex and in Java reflection. */
static bool rf_isEnumConstant(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI((rt_get_int(vm, self, "access") & 0x4000) != 0);
}

/* Field.set(): the write half of rf_get().  A Field table that can only read
 * makes every configure-by-reflection path (a test double, a feature flag a
 * library flips on its own static) silently do nothing. */
static bool rf_set(struct dvm *vm, dvm_ref self,
                   const union dvm_value *args, int nargs,
                   union dvm_value *out)
{
   (void)out;
   union dvm_value owner = { 0 }, namev = { 0 };
   (void)dvm_get_field(vm, self, "owner", "Ljava/lang/Class;", &owner);
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &namev);
   struct dvm_object *co = dvm__obj(vm, owner.l);
   struct dvm_class *target = co ? co->klass : NULL;
   const char *name = dvm_string_utf8(vm, namev.l);
   dvm_ref receiver = nargs > 0 ? ARG(0).l : 0;
   union dvm_value value = nargs > 1 ? ARG(1) : (union dvm_value){ 0 };
   if (target) (void)dvm_init_class(vm, target);
   /* Field.set() is handed the value boxed — that is what its Object parameter
    * means — and storing the box's *reference* into an int field is how a
    * reflective deserialiser (gson binding EnvConfig.env here) filled every
    * primitive with a handle instead of the number it had just parsed. */
   const char given = rf_accessor_kind();
   for (struct dvm_class *c = target; c; c = c->super) {
      for (int i = 0; i < c->nsfields; ++i) {
         struct dvm_field *f = &c->sfields[i];
         if (!name || !f->name || strcmp(name, f->name)) continue;
         union dvm_value v = value;
         if (f->kind == 'L') dvm_pin(vm, v.l);
         else if (given == 'L') {
            if (!reflect_unbox(vm, value.l, f->kind, &v)) {
               dvm__throw(vm, "java/lang/IllegalArgumentException",
                          "%s.%s is %c", c->name ? c->name : "?", f->name,
                          f->kind);
               return false;
            }
         } else v = rf_widen(value, given, f->kind);
         c->sslots[f->slot] = v;
         RETV();
      }
      if (receiver) {
         for (int i = 0; i < c->nifields; ++i) {
            struct dvm_field *f = &c->ifields[i];
            if (!name || !f->name || strcmp(name, f->name)) continue;
            union dvm_value v = value;
            if (f->kind == 'L') dvm_pin(vm, v.l);
            else if (given == 'L') {
               if (!reflect_unbox(vm, value.l, f->kind, &v)) {
                  dvm__throw(vm, "java/lang/IllegalArgumentException",
                             "%s.%s is %c", c->name ? c->name : "?", f->name,
                             f->kind);
                  return false;
               }
            } else v = rf_widen(value, given, f->kind);
            struct dvm_object *object = dvm__obj(vm, receiver);
            if (!object || !object->slots || f->slot >= object->cls->islots ||
                !dvm__class_assignable(vm, object->cls, c)) {
               dvm__throw(vm, "java/lang/IllegalArgumentException", "field receiver");
               return false;
            }
            object->slots[f->slot] = v;
            RETV();
         }
      }
   }
   RETV();
}

/* Annotations declared on a field.
 *
 * Field had none, so getAnnotation() was unresolved and answered null.  A
 * reflective deserialiser reads the *name to look for in the document* from
 * exactly this annotation — gson's @SerializedName — and with null it falls
 * back to the field's own name: "app_key" in the JSON never reached `appKey`,
 * and every renamed field stayed at its default.
 *
 * The Field records the class it was asked of, which is not always the class
 * that declares it, so walk up to the declaring class first. */
static bool class_declares_field(struct dvm_class *c, const char *name)
{
   for (int i = 0; i < c->nifields; ++i)
      if (c->ifields[i].name && !strcmp(c->ifields[i].name, name)) return true;
   for (int i = 0; i < c->nsfields; ++i)
      if (c->sfields[i].name && !strcmp(c->sfields[i].name, name)) return true;
   return false;
}

static dvm_ref field_annotation(struct dvm *vm, dvm_ref self, dvm_ref ann_ref)
{
   union dvm_value owner = { 0 }, namev = { 0 };
   (void)dvm_get_field(vm, self, "owner", "Ljava/lang/Class;", &owner);
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &namev);
   struct dvm_object *co = dvm__obj(vm, owner.l);
   struct dvm_object *ao = dvm__obj(vm, ann_ref);
   struct dvm_class *target = co ? co->klass : NULL;
   struct dvm_class *ann = ao ? ao->klass : NULL;
   const char *name = namev.l ? dvm_string_utf8(vm, namev.l) : NULL;
   if (!target || !ann || !ann->desc || !name) return 0;

   for (struct dvm_class *c = target; c; c = c->super) {
      if (!class_declares_field(c, name)) continue;
      uint32_t encoded_off;
      if (!c->dex || !dex_field_annotation(c->dex, c->class_def_idx, name,
                                           ann->desc, &encoded_off))
         return 0;
      dvm_ref ref = dvm_new_object(vm, ann);
      struct dvm_object *obj = dvm__obj(vm, ref);
      if (!obj) return 0;
      obj->annotation_dex = c->dex;
      obj->annotation_off = encoded_off;
      return ref;
   }
   return 0;
}

static bool rf_getAnnotation(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "annotationClass");
      return false;
   }
   RETL(field_annotation(vm, self, ARG(0).l));
}

static bool rf_isAnnotationPresent(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "annotationClass");
      return false;
   }
   RETI(field_annotation(vm, self, ARG(0).l) != 0);
}

static bool rf_getAnnotations(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out);

static const struct rt_method rt_reflect_field[] = {
   M("getName", "()Ljava/lang/String;", rm_getName),
   M("getDeclaringClass", "()Ljava/lang/Class;", rf_getDeclaringClass),
   M("getType", "()Ljava/lang/Class;", rf_getType),
   M("getGenericType", "()Ljava/lang/reflect/Type;", rf_getGenericType),
   M("getModifiers", "()I", rm_getModifiers),
   M("isSynthetic", "()Z", rf_isSynthetic),
   M("isEnumConstant", "()Z", rf_isEnumConstant),
   M("setAccessible", "(Z)V", ao_setAccessible),
   M("isAccessible", "()Z", ao_isAccessible),
   M("getAnnotations", "()[Ljava/lang/annotation/Annotation;", rf_getAnnotations),
   M("getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;", rf_getAnnotations),
   M("getAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",
     rf_getAnnotation),
   M("getDeclaredAnnotation",
     "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;", rf_getAnnotation),
   M("isAnnotationPresent", "(Ljava/lang/Class;)Z", rf_isAnnotationPresent),
   M("get", "(Ljava/lang/Object;)Ljava/lang/Object;", rf_get),
   M("getInt", "(Ljava/lang/Object;)I", rf_get),
   M("getLong", "(Ljava/lang/Object;)J", rf_get),
   M("getShort", "(Ljava/lang/Object;)S", rf_get),
   M("getByte", "(Ljava/lang/Object;)B", rf_get),
   M("getChar", "(Ljava/lang/Object;)C", rf_get),
   M("getBoolean", "(Ljava/lang/Object;)Z", rf_get),
   M("getFloat", "(Ljava/lang/Object;)F", rf_get),
   M("getDouble", "(Ljava/lang/Object;)D", rf_get),
   M("set", "(Ljava/lang/Object;Ljava/lang/Object;)V", rf_set),
   M("setInt", "(Ljava/lang/Object;I)V", rf_set),
   M("setLong", "(Ljava/lang/Object;J)V", rf_set),
   M("setShort", "(Ljava/lang/Object;S)V", rf_set),
   M("setByte", "(Ljava/lang/Object;B)V", rf_set),
   M("setChar", "(Ljava/lang/Object;C)V", rf_set),
   M("setBoolean", "(Ljava/lang/Object;Z)V", rf_set),
   M("setFloat", "(Ljava/lang/Object;F)V", rf_set),
   M("setDouble", "(Ljava/lang/Object;D)V", rf_set),
   M("getAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",
     ret_zero),
   M_END,
};

/* RecordComponent.getAccessor: the record's no-argument method of that name. */
static bool rc_getAccessor(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value owner = { 0 }, name = { 0 };
   (void)dvm_get_field(vm, self, "owner", "Ljava/lang/Class;", &owner);
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &name);
   union dvm_value a[2] = { { .l = name.l }, { .l = 0 } };
   return cl_getMethod(vm, owner.l, a, 2, out);
}

static const struct rt_method rt_reflect_record_component[] = {
   M("getName", "()Ljava/lang/String;", rm_getName),
   M("getDeclaringRecord", "()Ljava/lang/Class;", rf_getDeclaringClass),
   M("getType", "()Ljava/lang/Class;", rf_getType),
   M("getGenericType", "()Ljava/lang/reflect/Type;", rf_getGenericType),
   M("getAccessor", "()Ljava/lang/reflect/Method;", rc_getAccessor),
   M_END,
};

/* sun.misc.Unsafe is used by desugared java.util.concurrent itself.  A field
 * offset is represented by its reflected Field handle; an array offset is a
 * byte offset from arrayBaseOffset() (0 here) scaled by arrayIndexScale(),
 * exactly as on a device.  It used to be the raw element index, with the scale
 * reported as 1 — but a caller does not read elements at the offsets it was
 * given, it computes them: rx's SpscArrayQueue accepts only a 4- or 8-byte
 * reference scale and threw "Unknown pointer size" out of <clinit> for every
 * queue class it has.  Guest bytecode is single-threaded within a slice, so
 * compare/read/write is atomic with respect to every other guest operation. */

/* Element index for the byte offset a caller derived from arrayIndexScale().
 * Negative or unaligned offsets name no element; -1 keeps them out of the
 * payload rather than truncating them into it. */
static int64_t unsafe_array_index(const struct dvm_object *o, int64_t offset)
{
   int w = dvm__elem_width(o->elem_kind);
   if (offset < 0 || offset % w) return -1;
   return offset / w;
}
static struct dvm_field *unsafe_reflected_field(struct dvm *vm, dvm_ref ref)
{
   union dvm_value owner = { 0 }, namev = { 0 }, typev = {0};
   if (!dvm_get_field(vm, ref, "owner", "Ljava/lang/Class;", &owner) ||
       !dvm_get_field(vm, ref, "name", "Ljava/lang/String;", &namev))
      return NULL;
   struct dvm_object *co = dvm__obj(vm, owner.l);
   const char *name = dvm_string_utf8(vm, namev.l);
   (void)dvm_get_field(vm, ref, "sig", "Ljava/lang/String;", &typev);
   const char *type = typev.l ? dvm_string_utf8(vm, typev.l) : NULL;
   for (struct dvm_class *c = co ? co->klass : NULL; c; c = c->super)
      for (int i = 0; name && i < c->nifields; ++i)
         if (c->ifields[i].name && !strcmp(name, c->ifields[i].name) &&
             (!type || !strcmp(type, c->ifields[i].type)))
            return &c->ifields[i];
   return NULL;
}

/* The logical object layout is a header followed by the VM's value slots.
 * Offsets describe that layout, not the lifetime or allocation order of a
 * java.lang.reflect.Field. Inherited fields retain their superclass slots. */
#define UNSAFE_OBJECT_HEADER 16u
static struct dvm_field *unsafe_field(struct dvm *vm, dvm_ref object, int64_t offset)
{
   if (offset < UNSAFE_OBJECT_HEADER ||
       (uint64_t)(offset - UNSAFE_OBJECT_HEADER) % sizeof(union dvm_value)) return NULL;
   uint64_t slot = (uint64_t)(offset - UNSAFE_OBJECT_HEADER) / sizeof(union dvm_value);
   struct dvm_class *cls = dvm_object_class(vm, object);
   if (!cls || slot >= cls->islots) return NULL;
   for (struct dvm_class *c = cls; c; c = c->super)
      for (int i = 0; i < c->nifields; ++i)
         if (c->ifields[i].slot == slot) return &c->ifields[i];
   return NULL;
}

static bool unsafe_objectFieldOffset(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)self; (void)nargs;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "field"); return false; }
   union dvm_value access = {0};
   (void)dvm_get_field(vm, ARG(0).l, "access", "I", &access);
   struct dvm_field *field = (access.i & DEX_ACC_STATIC) ? NULL : unsafe_reflected_field(vm, ARG(0).l);
   if (!field) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "valid for instance fields only");
      return false;
   }
   RETJ(UNSAFE_OBJECT_HEADER + (uint64_t)field->slot * sizeof(union dvm_value));
}

/* Reads and writes at an offset objectFieldOffset() handed out — the accessors
 * protobuf's generated schemas use for every field of every message.  They were
 * missing, so a probe that asked "does Unsafe have getLong?" was answered by
 * the permissive Class.getMethod() and the call itself then did nothing and
 * returned zero, which reads as a message whose fields are all default. */
static bool unsafe_get_prim(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_field *f = unsafe_field(vm, ARG(0).l, ARG(1).j);
   union dvm_value value = { 0 };
   if (f) (void)dvm__get_field_from(vm, ARG(0).l, f->cls, f->name, f->type, &value);
   *out = value;
   return true;
}

static bool unsafe_put_prim(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_field *f = unsafe_field(vm, ARG(0).l, ARG(1).j);
   if (f) (void)dvm__set_field_from(vm, ARG(0).l, f->cls, f->name, f->type, args[2]);
   RETV();
}

/* Primitive Unsafe access is byte-addressed, even when the accessor width is
 * wider than the array element (getInt(byte[], off) is a common hash-table
 * operation).  Validate the complete byte range and use memcpy so unaligned
 * Android accesses remain defined on the host.  Object/CAS accessors below
 * still operate on typed array elements. */
static bool unsafe_array_or_field_get(struct dvm *vm, dvm_ref self,
                                      const union dvm_value *args, int nargs,
                                      union dvm_value *out, char kind)
{
   struct dvm_object *o = dvm__obj(vm, ARG(0).l);
   if (o && o->kind == DVM_OBJ_ARRAY && o->data) {
      int64_t offset = ARG(1).j;
      size_t width = (kind == 'J' || kind == 'D') ? 8u
                   : (kind == 'I' || kind == 'F') ? 4u
                   : (kind == 'S' || kind == 'C') ? 2u : 1u;
      size_t bytes = (size_t)o->length * (size_t)dvm__elem_width(o->elem_kind);
      if (offset < 0 || (uint64_t)offset > bytes ||
          width > bytes - (size_t)offset) RETJ(0);
      const uint8_t *p = (const uint8_t *)o->data + (size_t)offset;
      union dvm_value v = { 0 };
      switch (kind) {
         case 'B': { int8_t x; memcpy(&x, p, 1); v.i = x; break; }
         case 'Z': { uint8_t x; memcpy(&x, p, 1); v.i = x != 0; break; }
         case 'S': { int16_t x; memcpy(&x, p, 2); v.i = x; break; }
         case 'C': { uint16_t x; memcpy(&x, p, 2); v.i = x; break; }
         case 'I': memcpy(&v.i, p, 4); break;
         case 'J': memcpy(&v.j, p, 8); break;
         case 'F': memcpy(&v.f, p, 4); break;
         case 'D': memcpy(&v.d, p, 8); break;
         default: break;
      }
      *out = v;
      return true;
   }
   return unsafe_get_prim(vm, self, args, nargs, out);
}

static bool unsafe_array_or_field_put(struct dvm *vm, dvm_ref self,
                                      const union dvm_value *args, int nargs,
                                      union dvm_value *out, char kind)
{
   struct dvm_object *o = dvm__obj(vm, ARG(0).l);
   if (o && o->kind == DVM_OBJ_ARRAY && o->data) {
      int64_t offset = ARG(1).j;
      size_t width = (kind == 'J' || kind == 'D') ? 8u
                   : (kind == 'I' || kind == 'F') ? 4u
                   : (kind == 'S' || kind == 'C') ? 2u : 1u;
      size_t bytes = (size_t)o->length * (size_t)dvm__elem_width(o->elem_kind);
      if (offset < 0 || (uint64_t)offset > bytes ||
          width > bytes - (size_t)offset) RETV();
      uint8_t *p = (uint8_t *)o->data + (size_t)offset;
      switch (kind) {
         case 'B': { int8_t x = (int8_t)args[2].i; memcpy(p, &x, 1); break; }
         case 'Z': { uint8_t x = args[2].i ? 1 : 0; memcpy(p, &x, 1); break; }
         case 'S': { int16_t x = (int16_t)args[2].i; memcpy(p, &x, 2); break; }
         case 'C': { uint16_t x = (uint16_t)args[2].i; memcpy(p, &x, 2); break; }
         case 'I': memcpy(p, &args[2].i, 4); break;
         case 'J': memcpy(p, &args[2].j, 8); break;
         case 'F': memcpy(p, &args[2].f, 4); break;
         case 'D': memcpy(p, &args[2].d, 8); break;
         default: break;
      }
      RETV();
   }
   return unsafe_put_prim(vm, self, args, nargs, out);
}

#define UNSAFE_ACCESSOR(name, kind)                                           \
   static bool unsafe_get_##name(struct dvm *vm, dvm_ref self,                \
                                 const union dvm_value *args, int nargs,      \
                                 union dvm_value *out)                        \
   { return unsafe_array_or_field_get(vm, self, args, nargs, out, kind); }    \
   static bool unsafe_put_##name(struct dvm *vm, dvm_ref self,                \
                                 const union dvm_value *args, int nargs,      \
                                 union dvm_value *out)                        \
   { return unsafe_array_or_field_put(vm, self, args, nargs, out, kind); }

UNSAFE_ACCESSOR(boolean, 'Z')
UNSAFE_ACCESSOR(byte,    'B')
UNSAFE_ACCESSOR(short,   'S')
UNSAFE_ACCESSOR(char,    'C')
UNSAFE_ACCESSOR(int,     'I')
UNSAFE_ACCESSOR(long,    'J')
UNSAFE_ACCESSOR(float,   'F')
UNSAFE_ACCESSOR(double,  'D')

static bool unsafe_getUnsafe(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   static dvm_ref unsafe;
   RETL(runtime_singleton(vm, "Lsun/misc/Unsafe;", &unsafe));
}

/* allocateInstance(): an object with every field at its default, no
 * constructor run — which is exactly what a deserializer wants. */
static bool unsafe_allocateInstance(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_object *co = dvm__obj(vm, ARG(0).l);
   RETL(co && co->klass ? dvm_new_object(vm, co->klass) : 0);
}

static bool unsafe_arrayBaseOffset(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   RETI(0);
}

static bool unsafe_arrayIndexScale(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_object *co = dvm__obj(vm, ARG(0).l);
   struct dvm_class *k = co ? co->klass : NULL;
   /* 0 is what a device answers for a class that is not an array type. */
   RETI(k && k->is_array ? dvm__elem_width(k->elem_kind) : 0);
}

static bool unsafe_cas_int(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)self; (void)nargs;
   dvm_ref object = ARG(0).l;
   struct dvm_object *o = dvm__obj(vm, object);
   if (o && o->kind == DVM_OBJ_ARRAY) {
      int64_t index = unsafe_array_index(o, ARG(1).j);
      int32_t *items = dvm_array_data(vm, object);
      if (index < 0 || (uint64_t)index >= dvm_array_length(vm, object)) RETI(0);
      if (items[index] != ARG(2).i) RETI(0);
      items[index] = ARG(3).i;
      RETI(1);
   }
   struct dvm_field *f = unsafe_field(vm, object, ARG(1).j);
   union dvm_value current = { 0 };
   if (!f || !dvm__get_field_from(vm, object, f->cls, f->name, f->type, &current)) RETI(0);
   if (current.i != ARG(2).i) RETI(0);
   union dvm_value update = { .i = ARG(3).i };
   (void)dvm__set_field_from(vm, object, f->cls, f->name, f->type, update);
   RETI(1);
}

static bool unsafe_cas_long(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_field *f = unsafe_field(vm, ARG(0).l, ARG(1).j);
   union dvm_value current = { 0 };
   if (!f || !dvm__get_field_from(vm, ARG(0).l, f->cls, f->name, f->type, &current)) RETI(0);
   if (current.j != ARG(2).j) RETI(0);
   union dvm_value update = { .j = ARG(3).j };
   (void)dvm__set_field_from(vm, ARG(0).l, f->cls, f->name, f->type, update);
   RETI(1);
}

static bool unsafe_cas_object(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)self; (void)nargs;
   dvm_ref object = ARG(0).l;
   struct dvm_object *o = dvm__obj(vm, object);
   if (o && o->kind == DVM_OBJ_ARRAY) {
      int64_t index = unsafe_array_index(o, ARG(1).j);
      dvm_ref *items = dvm_array_data(vm, object);
      if (index < 0 || (uint64_t)index >= dvm_array_length(vm, object)) RETI(0);
      if (items[index] != ARG(2).l) RETI(0);
      items[index] = ARG(3).l;
      dvm_pin(vm, ARG(3).l);
      RETI(1);
   }
   struct dvm_field *f = unsafe_field(vm, object, ARG(1).j);
   union dvm_value current = { 0 };
   if (!f || !dvm__get_field_from(vm, object, f->cls, f->name, f->type, &current)) RETI(0);
   if (current.l != ARG(2).l) RETI(0);
   union dvm_value update = { .l = ARG(3).l };
   (void)dvm__set_field_from(vm, object, f->cls, f->name, f->type, update);
   RETI(1);
}

static bool unsafe_get_object(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, ARG(0).l);
   if (o && o->kind == DVM_OBJ_ARRAY) {
      int64_t index = unsafe_array_index(o, ARG(1).j);
      dvm_ref *items = dvm_array_data(vm, ARG(0).l);
      RETL(items && index >= 0 && (uint64_t)index < dvm_array_length(vm, ARG(0).l)
              ? items[index] : 0);
   }
   struct dvm_field *f = unsafe_field(vm, ARG(0).l, ARG(1).j);
   union dvm_value value = { 0 };
   if (f) (void)dvm__get_field_from(vm, ARG(0).l, f->cls, f->name, f->type, &value);
   RETL(value.l);
}

static bool unsafe_put_object(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)self; (void)nargs; (void)out;
   struct dvm_object *o = dvm__obj(vm, ARG(0).l);
   if (o && o->kind == DVM_OBJ_ARRAY) {
      int64_t index = unsafe_array_index(o, ARG(1).j);
      dvm_ref *items = dvm_array_data(vm, ARG(0).l);
      if (items && index >= 0 && (uint64_t)index < dvm_array_length(vm, ARG(0).l))
         items[index] = ARG(2).l;
      RETV();
   }
   struct dvm_field *f = unsafe_field(vm, ARG(0).l, ARG(1).j);
   if (f) {
      union dvm_value value = { .l = ARG(2).l };
      (void)dvm__set_field_from(vm, ARG(0).l, f->cls, f->name, f->type, value);
   }
   RETV();
}

#define UNSAFE_M(jname, name, tychar)                                         \
   M("get" jname, "(Ljava/lang/Object;J)" tychar, unsafe_get_##name),         \
   M("get" jname "Volatile", "(Ljava/lang/Object;J)" tychar,                  \
     unsafe_get_##name),                                                      \
   M("put" jname, "(Ljava/lang/Object;J" tychar ")V", unsafe_put_##name),     \
   M("put" jname "Volatile", "(Ljava/lang/Object;J" tychar ")V",              \
     unsafe_put_##name),                                                      \
   M("putOrdered" jname, "(Ljava/lang/Object;J" tychar ")V", unsafe_put_##name)

static const struct rt_field rt_unsafe_sfields[] = {
   { "theUnsafe", "Lsun/misc/Unsafe;" }, F_END
};

static const struct rt_method rt_unsafe[] = {
   M("objectFieldOffset", "(Ljava/lang/reflect/Field;)J",
     unsafe_objectFieldOffset),
   SM("getUnsafe", "()Lsun/misc/Unsafe;", unsafe_getUnsafe),
   M("allocateInstance", "(Ljava/lang/Class;)Ljava/lang/Object;",
     unsafe_allocateInstance),
   UNSAFE_M("Boolean", boolean, "Z"),
   UNSAFE_M("Byte",    byte,    "B"),
   UNSAFE_M("Short",   short,   "S"),
   UNSAFE_M("Char",    char,    "C"),
   UNSAFE_M("Int",     int,     "I"),
   UNSAFE_M("Long",    long,    "J"),
   UNSAFE_M("Float",   float,   "F"),
   UNSAFE_M("Double",  double,  "D"),
   M("arrayBaseOffset", "(Ljava/lang/Class;)I", unsafe_arrayBaseOffset),
   M("arrayIndexScale", "(Ljava/lang/Class;)I", unsafe_arrayIndexScale),
   M("compareAndSwapInt", "(Ljava/lang/Object;JII)Z", unsafe_cas_int),
   M("compareAndSwapLong", "(Ljava/lang/Object;JJJ)Z", unsafe_cas_long),
   M("compareAndSwapObject",
     "(Ljava/lang/Object;JLjava/lang/Object;Ljava/lang/Object;)Z",
     unsafe_cas_object),
   M("getObjectVolatile", "(Ljava/lang/Object;J)Ljava/lang/Object;",
     unsafe_get_object),
   M("getObject", "(Ljava/lang/Object;J)Ljava/lang/Object;", unsafe_get_object),
   M("putObjectVolatile", "(Ljava/lang/Object;JLjava/lang/Object;)V",
     unsafe_put_object),
   M("putObject", "(Ljava/lang/Object;JLjava/lang/Object;)V", unsafe_put_object),
   M("putOrderedObject", "(Ljava/lang/Object;JLjava/lang/Object;)V",
     unsafe_put_object),
   M_END,
};

/* The descriptor a Class[] of parameter types describes: "(Ljava/lang/String;I)V".
 *
 * getConstructor(Class...) and getMethod(String, Class...) are told exactly
 * which overload the caller means, and throwing that away leaves the reflection
 * layer picking whichever one happens to come first.  For a class with a single
 * constructor that is invisible; for java.io.FileOutputStream, which has four,
 * it means newInstance() runs a different constructor from the one the caller
 * asked for and fails.  AppsFlyer's obfuscator builds its string-decoding
 * tables in a <clinit> that does exactly this, and the resulting
 * InvocationTargetException left every AFa1hSDK user with
 * NoClassDefFoundError — which the SDK answers by retrying, four thousand
 * times a second, for the whole loading screen. */
static bool reflect_param_sig(struct dvm *vm, dvm_ref types, char *buf,
                              size_t cap)
{
   size_t at = 0;
   if (cap < 4) return false;
   buf[at++] = '(';
   const uint32_t n = types ? dvm_array_length(vm, types) : 0u;
   const dvm_ref *items = types ? dvm_array_data(vm, types) : NULL;
   for (uint32_t i = 0; items && i < n; ++i) {
      /* Via class_desc_of(), not the Class's own `desc`: int.class is a
       * pseudo-class named "int", whose descriptor spells out as `Lint;`.
       * Building the signature from that produced `([BLint;Lint;)V` for
       * `getConstructor(byte[].class, int.class, int.class)`, no method
       * matched it, and the caller fell back to "any <init>" — the first one
       * in the table.  `new ByteArrayInputStream(buf, 20, 7288)` through
       * reflection therefore became `ByteArrayInputStream(buf)`: a stream
       * over the whole file instead of the slice, which is what walked
       * AppsFlyer's block-decrypting reader off the end of its asset.
       * getMethod(name, int.class) had the same hole. */
      char d[128];
      if (!items[i] || !class_desc_of(vm, items[i], d, sizeof d))
         return false;                       /* unknown type: no signature */
      const size_t len = strlen(d);
      if (at + len + 3 > cap) return false;
      memcpy(buf + at, d, len);
      at += len;
   }
   buf[at++] = ')';
   buf[at++] = 'V';
   buf[at] = '\0';
   return true;
}

/* dalvik.system.DexFile.
 *
 * Loading a dex at runtime is how an obfuscated SDK materialises code it
 * shipped as data: AppsFlyer decrypts a small dex out of its assets, writes
 * it, and calls DexFile.loadDex() + loadClass() to get the class back.  With
 * these unresolved, loadClass() answered null and the very next
 * `.getDeclaredConstructor(...)` was an NPE that took AFa1hSDK.<clinit> — and
 * every class that uses it — down for the rest of the process.
 *
 * The VM already loads dex files; this is that loader reached from bytecode.
 * A DexFile keeps the path it opened so the object is not a bare token. */
static const struct rt_field rt_dexfile_fields[] = {
   { "path", "Ljava/lang/String;" },
   { "classes", "Ljava/util/List;" }, F_END
};

static const char *file_path(struct dvm *vm, dvm_ref self);

static int dexfile_descriptor_compare(const void *a, const void *b)
{
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool dexfile_open(struct dvm *vm, dvm_ref self, const char *guest_path)
{
   if (!guest_path || !*guest_path) {
      dvm__throw(vm, "java/io/IOException", "no dex path");
      return false;
   }
   char host[DVM_PATH_MAX];
   const char *hp = arm_exec_map_guest_path(guest_path, host, sizeof host);
   /* loadDex() takes a jar/apk as readily as a bare .dex, and that is the form
    * this SDK writes: the file starts "PK\x03\x04" with classes.dex inside.
    * Handing the archive to the raw-dex loader is a guaranteed refusal. */
   bool loaded = false;
   int first_dex = vm->ndexes;
   if (hp) {
      unsigned char m[4] = { 0 };
      FILE *f = luna_file_fopen(hp, "rb");
      if (f) { (void)!fread(m, 1, sizeof m, f); fclose(f); }
      if (m[0] == 'P' && m[1] == 'K' && m[2] == 3 && m[3] == 4) {
         for (unsigned index = 1; ; ++index) {
            char entry[48];
            if (index == 1) snprintf(entry, sizeof entry, "classes.dex");
            else snprintf(entry, sizeof entry, "classes%u.dex", index);
            size_t len = 0;
            unsigned char *d = arm_exec_zip_read(hp, entry, &len);
            if (!d) break;
            loaded = dvm_add_dex_memory(vm, d, len, guest_path);
            free(d);
            if (!loaded) break;
         }
      } else {
         loaded = dvm_add_dex(vm, hp);
      }
   }
   if (!loaded) {
      struct stat st;
      const long long sz = (hp && !luna_file_stat_follow(hp, &st)) ? (long long)st.st_size : -1;
      char magic[32] = "";
      if (hp) {
         FILE *f = luna_file_fopen(hp, "rb");
         if (f) {
            unsigned char m[8] = { 0 };
            size_t got = fread(m, 1, sizeof m, f);
            fclose(f);
            for (size_t i = 0; i < got; ++i)
               snprintf(magic + i * 3, sizeof magic - i * 3, "%02x ", m[i]);
         }
      }
      fprintf(stderr, "[dex] loadDex failed: guest=%s host=%s size=%lld "
              "magic=[%s]\n", guest_path, hp ? hp : "(null)", sz, magic);
      /* Where did the bytes go?  List the directory the load was pointed at,
       * so a file written under a name or a root the loader does not see
       * shows up instead of being invisible. */
      if (hp) {
         char dir[DVM_PATH_MAX];
         snprintf(dir, sizeof dir, "%s", hp);
         char *slash = strrchr(dir, '/');
         if (slash) *slash = '\0';
         luna_directory *d = luna_directory_open(dir);
         if (d) {
            luna_directory_entry *e;
            int shown = 0;
            while ((e = luna_directory_read(d)) && shown < 12) {
               char full[DVM_PATH_MAX + NAME_MAX + 2];
               struct stat es;
               if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name)
                   >= (int)sizeof full)
                  continue;
               if (luna_file_stat_follow(full, &es) || !S_ISREG(es.st_mode)) continue;
               fprintf(stderr, "[dex]   %s/%s = %lld bytes\n", dir, e->d_name,
                       (long long)es.st_size);
               ++shown;
            }
            luna_directory_close(d);
         }
      }
      dvm__throw(vm, "java/io/IOException", "%s", guest_path);
      return false;
   }
   fprintf(stderr, "[dex] loaded %s\n", guest_path);
   union dvm_value v = { .l = dvm_new_string(vm, guest_path) };
   (void)dvm_set_field(vm, self, "path", "Ljava/lang/String;", v);
   dvm_ref classes = coll_new_list(vm, "Ljava/util/ArrayList;");
   if (!classes) return false;
   size_t capacity = 0, count = 0;
   for (int index = first_dex; index < vm->ndexes; ++index)
      capacity += vm->dexes[index]->file.class_defs_size;
   const char **descriptors = calloc(capacity ? capacity : 1, sizeof *descriptors);
   if (!descriptors) { dvm__throw(vm, "java/lang/OutOfMemoryError", "dex classes"); return false; }
   for (int index = first_dex; index < vm->ndexes; ++index) {
      struct dex_file *dex = &vm->dexes[index]->file;
      for (uint32_t i = 0; i < dex->class_defs_size; ++i) {
         struct dex_class_def def;
         if (!dex_class_def(dex, i, &def)) continue;
         const char *desc = dex_type(dex, def.class_idx);
         if (desc) descriptors[count++] = desc;
      }
   }
   qsort(descriptors, count, sizeof *descriptors, dexfile_descriptor_compare);
   for (size_t i = 0; i < count; ++i) {
      const char *desc = descriptors[i];
      if (i && !strcmp(desc, descriptors[i - 1])) continue;
      size_t length = strlen(desc);
      if (length < 2 || desc[0] != 'L' || desc[length - 1] != ';') continue;
      char *name = luna_strndup(desc + 1, length - 2);
      if (!name) {
         free(descriptors);
         dvm__throw(vm, "java/lang/OutOfMemoryError", "dex class name"); return false;
      }
      for (char *p = name; *p; ++p) if (*p == '/') *p = '.';
      union dvm_value item = { .l = dvm_new_string(vm, name) }, ignored;
      free(name);
      if (!item.l || !list_add(vm, classes, &item, 1, &ignored)) {
         free(descriptors); return false;
      }
   }
   free(descriptors);
   v.l = classes;
   (void)dvm_set_field(vm, self, "classes", "Ljava/util/List;", v);
   return true;
}

static bool dexfile_entries(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value classes = { 0 };
   (void)dvm_get_field(vm, self, "classes", "Ljava/util/List;", &classes);
   dvm_ref enumeration = dvm_new_object(vm, dvm_find_class(vm, "lunaria/util/ResourceKeys"));
   if (!enumeration) return false;
   (void)dvm_set_field(vm, enumeration, "target", "Ljava/lang/Object;", classes);
   RETL(enumeration);
}

static bool dexfile_init(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)nargs;
   const char *p = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!p && ARG(0).l) p = file_path(vm, ARG(0).l);   /* DexFile(File) */
   if (!dexfile_open(vm, self, p)) return false;
   RETV();
}

/* static DexFile.loadDex(source, output, flags) — the same open, answering the
 * DexFile rather than being one. */
static bool dexfile_loadDex(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_class *c = dvm__class_by_desc(vm, "Ldalvik/system/DexFile;");
   dvm_ref o = c ? dvm_new_object(vm, c) : 0;
   if (!o) { dvm__throw(vm, "java/io/IOException", "DexFile"); return false; }
   const char *p = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!dexfile_open(vm, o, p)) return false;
   RETL(o);
}

static bool dexfile_loadClass(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)nargs;
   const char *n = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   /* loadClass() answers null for a class this dex does not define — that is
    * its documented behaviour, and the caller checks for it. */
   union dvm_value classes = { 0 };
   (void)dvm_get_field(vm, self, "classes", "Ljava/util/List;", &classes);
   struct rt_list *list = list_of(vm, classes.l);
   bool defined = false;
   for (uint32_t i = 0; n && list && i < list->size; ++i) {
      const char *entry = dvm_string_utf8(vm, list->items[i]);
      if (entry && !strcmp(n, entry)) { defined = true; break; }
   }
   if (!defined) RETL(0);
   char desc[512];
   snprintf(desc, sizeof desc, "L%s;", n);
   for (char *q = desc; *q; ++q) if (*q == '.') *q = '/';
   struct dvm_class *c = dvm__class_by_desc(vm, desc);
   RETL(class_object_for(vm, c));
}

static bool dexfile_getName(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "path", "Ljava/lang/String;", &v);
   RETL(v.l);
}

static const struct rt_method rt_dexfile[] = {
   M("<init>", "(Ljava/lang/String;)V", dexfile_init),
   M("<init>", "(Ljava/io/File;)V", dexfile_init),
   SM("loadDex", "(Ljava/lang/String;Ljava/lang/String;I)Ldalvik/system/DexFile;",
      dexfile_loadDex),
   M("loadClass", "(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/Class;",
     dexfile_loadClass),
   M("getName", "()Ljava/lang/String;", dexfile_getName),
   M("entries", "()Ljava/util/Enumeration;", dexfile_entries),
   /* The dex was parsed into the VM when it was loaded; there is no handle
    * left to give back, so this really is empty rather than unimplemented. */
   M("close", "()V", empty_void),
   M_END,
};

/* java.lang.reflect.Constructor.  androidx's lifecycle observers are built by
 * reflection — getDeclaredConstructor() followed by newInstance() — so with
 * these missing every observer registration NPE'd on the null constructor. */
static bool cl_getConstructor(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   struct dvm_object *co = dvm__obj(vm, self);
   if (!co || !co->klass) RETL(0);
   char sig[512];
   dvm_ref types = nargs > 0 ? ARG(0).l : 0;
   bool have_sig = reflect_param_sig(vm, types, sig, sizeof sig);

   /* Constructors are declared, never inherited.  A failed lookup must throw
    * NoSuchMethodException: Gson deliberately probes for a no-argument
    * constructor and falls back to constructor-free allocation when it is
    * absent.  Returning a fabricated Constructor made it invoke an arbitrary
    * multi-argument overload with nulls instead.  Host-backed classes remain
    * permissive because their methods live behind the external hook. */
   struct dvm_method *declared = NULL;
   for (int i = 0; have_sig && i < co->klass->nmethods; ++i) {
      struct dvm_method *m = &co->klass->methods[i];
      if (m->name && m->sig && !strcmp(m->name, "<init>") &&
          !strcmp(m->sig, sig)) {
         declared = m;
         break;
      }
   }
   if (!co->klass->external && (!have_sig || !declared)) {
      dvm__throw(vm, "java/lang/NoSuchMethodException", "%s.<init>%s",
                 co->klass->name ? co->klass->name : "?",
                 have_sig ? sig : "(?)");
      return false;
   }

   struct dvm_class *cc =
      dvm__class_by_desc(vm, "Ljava/lang/reflect/Constructor;");
   dvm_ref k = cc ? dvm_new_object(vm, cc) : 0;
   if (!k) RETL(0);
   union dvm_value v = { .l = self };
   (void)dvm_set_field(vm, k, "owner", "Ljava/lang/Class;", v);
   v.l = dvm_new_string(vm, "<init>");
   (void)dvm_set_field(vm, k, "name", "Ljava/lang/String;", v);
   /* Which overload.  Kept as the descriptor rather than the Class[] so
    * newInstance() can hand it straight to the method lookup. */
   if (have_sig) {
      v.l = dvm_new_string(vm, sig);
      (void)dvm_set_field(vm, k, "sig", "Ljava/lang/String;", v);
   }
   v.i = declared ? (int32_t)declared->access : 0;
   (void)dvm_set_field(vm, k, "access", "I", v);
   RETL(k);
}

static dvm_ref reflect_constructor(struct dvm *vm, dvm_ref owner,
                                   const struct dvm_method *method)
{
   struct dvm_class *cc =
      dvm__class_by_desc(vm, "Ljava/lang/reflect/Constructor;");
   dvm_ref ctor = cc ? dvm_new_object(vm, cc) : 0;
   if (!ctor) return 0;
   union dvm_value v = { .l = owner };
   (void)dvm_set_field(vm, ctor, "owner", "Ljava/lang/Class;", v);
   v.l = dvm_new_string(vm, "<init>");
   (void)dvm_set_field(vm, ctor, "name", "Ljava/lang/String;", v);
   if (method && method->sig) {
      v.l = dvm_new_string(vm, method->sig);
      (void)dvm_set_field(vm, ctor, "sig", "Ljava/lang/String;", v);
   }
   v.i = method ? (int32_t)method->access : 0;
   (void)dvm_set_field(vm, ctor, "access", "I", v);
   return ctor;
}

/* Constructors are not inherited.  getConstructors() enumerates this class's
 * public constructors; getDeclaredConstructors() enumerates all of them.  The
 * runtime table predates access flags and records zero for its public platform
 * methods, so builtin/external classes treat that legacy zero as public. */
static bool cl_getConstructors_common(struct dvm *vm, dvm_ref self,
                                      bool public_only, union dvm_value *out)
{
   struct dvm_object *co = dvm__obj(vm, self);
   struct dvm_class *cls = co ? co->klass : NULL;
   int count = 0;
   for (int i = 0; cls && i < cls->nmethods; ++i) {
      const struct dvm_method *m = &cls->methods[i];
      if (!m->name || strcmp(m->name, "<init>")) continue;
      if (public_only && !(m->access & DEX_ACC_PUBLIC) && !m->builtin &&
          !cls->external)
         continue;
      ++count;
   }
   dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/lang/reflect/Constructor;",
                               (uint32_t)count);
   dvm_ref *items = arr ? dvm_array_data(vm, arr) : NULL;
   int at = 0;
   for (int i = 0; items && cls && i < cls->nmethods; ++i) {
      const struct dvm_method *m = &cls->methods[i];
      if (!m->name || strcmp(m->name, "<init>")) continue;
      if (public_only && !(m->access & DEX_ACC_PUBLIC) && !m->builtin &&
          !cls->external)
         continue;
      items[at++] = reflect_constructor(vm, self, m);
   }
   RETL(arr);
}

static bool cl_getConstructors(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)args; (void)nargs;
   return cl_getConstructors_common(vm, self, true, out);
}

static bool cl_getDeclaredConstructors(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)args; (void)nargs;
   return cl_getConstructors_common(vm, self, false, out);
}

static bool rc_newInstance(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   union dvm_value ownerv = { 0 };
   (void)dvm_get_field(vm, self, "owner", "Ljava/lang/Class;", &ownerv);
   struct dvm_object *co = ownerv.l ? dvm__obj(vm, ownerv.l) : NULL;
   struct dvm_class *cls = co ? co->klass : NULL;
   if (!cls) {
      dvm__throw(vm, "java/lang/InstantiationException", "no class");
      return false;
   }
   dvm_ref o = dvm_new_object(vm, cls);
   if (!o) {
      dvm__throw(vm, "java/lang/InstantiationException", "%s", cls->name);
      return false;
   }

   /* The trailing Object[] carries the constructor arguments. */
   int n = 0;
   dvm_ref arr = nargs > 0 ? ARG(0).l : 0;
   uint32_t len = arr ? dvm_array_length(vm, arr) : 0;
   const dvm_ref *items = arr ? dvm_array_data(vm, arr) : NULL;
   dvm_ref slots[16];
   memset(slots, 0, sizeof slots);
   for (; items && (uint32_t)n < len && n < 16; ++n) slots[n] = items[n];

   /* The overload getConstructor() was asked for, when it could be named.
    * Falling back to "any <init>" keeps Class.newInstance() and a host class
    * whose overload this VM does not model working as before. */
   struct dvm_method *ctor = NULL;
   union dvm_value sigv = { 0 };
   if (dvm_get_field(vm, self, "sig", "Ljava/lang/String;", &sigv) && sigv.l) {
      const char *sig = dvm_string_utf8(vm, sigv.l);
      if (sig) ctor = dvm_find_method(vm, cls, "<init>", sig);
   }
   if (!ctor) ctor = dvm_find_method(vm, cls, "<init>", NULL);

   /* A primitive parameter arrives boxed — newInstance() takes Object[], the
    * same as Method.invoke(), which has unboxed for a while.  Handing the box
    * reference over as if it were the number gives the constructor an
    * arbitrary integer: `new ByteArrayInputStream(bytes, off, len)` through
    * reflection got a handle for off and len. */
   union dvm_value call_args[16];
   memset(call_args, 0, sizeof call_args);
   for (int i = 0; i < n; ++i) {
      char pd[64];
      if (ctor && ctor->sig && dvm__sig_param(ctor->sig, i, pd, sizeof pd) &&
          pd[0] && pd[0] != 'L' && pd[0] != '[' &&
          reflect_unbox(vm, slots[i], pd[0], &call_args[i]))
         continue;
      call_args[i].l = slots[i];
   }
   /* Same line the reflective method calls log: which overload was picked and
    * with what.  Constructing through reflection is where an emulator quietly
    * builds the wrong object, and the wrong object is not visible later. */
   if (rt_reflect_trace())
      rt_reflect_log(vm, cls, "<init>",
                     ctor && ctor->sig ? ctor->sig : "(?)", slots, n, 0);
   union dvm_value ret = { 0 };
   if (ctor && !dvm_call(vm, ctor, o, call_args, n, &ret)) {
      char what[256];
      snprintf(what, sizeof what, "%s.<init>", cls->name);
      return reflect_target_failed(vm, what);
   }
   RETL(o);
}

/* Class.newInstance() is the no-argument constructor. */
static bool cl_newInstance(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value ignored = { 0 };
   dvm_ref k = 0;
   if (cl_getConstructor(vm, self, NULL, 0, &ignored)) k = ignored.l;
   if (!k) {
      dvm__throw(vm, "java/lang/InstantiationException", "no constructor");
      return false;
   }
   return rc_newInstance(vm, k, NULL, 0, out);
}

static bool cl_isInstance(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *co = dvm__obj(vm, self);
   struct dvm_class *want = co ? co->klass : NULL;
   struct dvm_class *have = ARG(0).l ? dvm_object_class(vm, ARG(0).l) : NULL;
   RETI(have && want && dvm__class_assignable(vm,have,want));
}

/* Class.cast(Object) — the checked identity function.  It was missing, so the
 * call fell out to the "no such external method" path and answered zero, which
 * is indistinguishable from a legitimate null: protobuf's Unsafe discovery
 * found the singleton, cast it, and got null back. */
static bool cl_cast(struct dvm *vm, dvm_ref self,
                    const union dvm_value *args, int nargs,
                    union dvm_value *out)
{
   (void)nargs;
   dvm_ref o = ARG(0).l;
   if (!o) RETL(0);   /* null casts to anything */
   struct dvm_object *co = dvm__obj(vm, self);
   struct dvm_class *want = co ? co->klass : NULL;
   struct dvm_class *have = dvm_object_class(vm, o);
   for (struct dvm_class *c = have; c && want; c = c->super)
      if (c == want) RETL(o);
   /* Interfaces and everything else class_assignable() knows about. */
   if (want && have && dvm__class_assignable(vm, have, want)) RETL(o);
   dvm__throw(vm, "java/lang/ClassCastException", "%s cannot be cast to %s",
              have && have->name ? have->name : "?",
              want && want->name ? want->name : "?");
   return false;
}

static bool cl_isArray(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *co = dvm__obj(vm, self);
   const char *d = co && co->klass ? co->klass->desc : NULL;
   RETI(d && d[0] == '[');
}

static bool cl_isPrimitive(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *co = dvm__obj(vm, self);
   const char *d = co && co->klass ? co->klass->desc : NULL;
   RETI(d && d[0] && !d[1] && strchr("ZBCSIJFDV", d[0]) != NULL);
}

static bool cl_getComponentType(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *co = dvm__obj(vm, self);
   const char *desc = co && co->klass ? co->klass->desc : NULL;
   RETL(desc && desc[0] == '['
      ? class_object_for(vm, dvm__class_by_desc(vm, desc + 1)) : 0);
}

static bool cl_isInterface(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *co = dvm__obj(vm, self);
   RETI(co && co->klass && co->klass->is_interface);
}
static bool cl_has_flag(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   struct dvm_object *object=dvm__obj(vm,self);
   uint32_t flag=!strcmp(builtin_name(),"isAnnotation") ? DEX_ACC_ANNOTATION : DEX_ACC_SYNTHETIC;
   RETI(object && object->klass && (object->klass->access&flag)!=0);
}

/* Materialised DEX annotations use their annotation interface as their
 * object class; return that interface, not a generated proxy class. */
static bool java_annotation_type(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_class *type = dvm_object_class(vm, self);
   if (!type || !(type->access & DEX_ACC_ANNOTATION)) {
      dvm__throw(vm, "java/lang/IllegalStateException", "not a materialised annotation");
      return false;
   }
   RETL(class_object_for(vm, type));
}

static const struct rt_method rt_java_annotation[] = {
   M("annotationType", "()Ljava/lang/Class;", java_annotation_type), M_END
};

/* Java Class.isEnum excludes anonymous constant-specific subclasses. */
static bool class_is_enum(struct dvm_class *cls)
{
   return cls && (cls->access & 0x4000u) && cls->super &&
          !strcmp(cls->super->desc, "Ljava/lang/Enum;");
}

static bool cl_isEnum(struct dvm *vm, dvm_ref self,
                      const union dvm_value *args, int nargs,
                      union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *object = dvm__obj(vm, self);
   RETI(class_is_enum(object ? object->klass : NULL));
}

/* Class.isRecord / getRecordComponents (API 34): a record is a direct subclass
 * of java.lang.Record and its components are its instance fields in
 * declaration order.  The IMA ads SDK serialises its messages through them. */
static bool cl_isRecord(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *object = dvm__obj(vm, self);
   const struct dvm_class *k = object ? object->klass : NULL;
   const char *super = k && k->super ? dvm_class_name(k->super) : NULL;
   RETI(super && !strcmp(super, "java/lang/Record"));
}

static bool cl_getRecordComponents(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *co = dvm__obj(vm, self);
   struct dvm_class *cls = co ? co->klass : NULL;
   const char *super = cls && cls->super ? dvm_class_name(cls->super) : NULL;
   if (!super || strcmp(super, "java/lang/Record")) RETL(0);
   struct dvm_class *rc = dvm__class_by_desc(vm, "Ljava/lang/reflect/RecordComponent;");
   dvm_ref arr = rc ? dvm_new_array(vm, 'L', "Ljava/lang/reflect/RecordComponent;",
                                    (uint32_t)cls->nifields) : 0;
   dvm_ref *items = arr ? (dvm_ref *)dvm_array_data(vm, arr) : NULL;
   for (int i = 0; items && i < cls->nifields; ++i) {
      const struct dvm_field *fd = &cls->ifields[i];
      dvm_ref c = dvm_new_object(vm, rc);
      if (!c) return false;
      union dvm_value v = { .l = self };
      (void)dvm_set_field(vm, c, "owner", "Ljava/lang/Class;", v);
      v.l = dvm_new_string(vm, fd->name ? fd->name : "");
      (void)dvm_set_field(vm, c, "name", "Ljava/lang/String;", v);
      v.l = dvm_new_string(vm, fd->type ? fd->type : "Ljava/lang/Object;");
      (void)dvm_set_field(vm, c, "sig", "Ljava/lang/String;", v);
      items[i] = c;
   }
   RETL(arr);
}

static bool cl_getEnumConstants(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *object = dvm__obj(vm, self);
   struct dvm_class *cls = object ? object->klass : NULL;
   if (!class_is_enum(cls)) RETL(0);
   if (!dvm_init_class(vm, cls)) return false;
   if (cls->dex) {
      size_t len = strlen(cls->desc) + 4;
      char *sig = malloc(len);
      if (!sig) { dvm__throw(vm, "java/lang/OutOfMemoryError", NULL); return false; }
      snprintf(sig, len, "()[%s", cls->desc);
      struct dvm_method *method = dvm_find_method(vm, cls, "values", sig);
      free(sig);
      union dvm_value value = {0};
      if (!method || !dvm_call(vm, method, 0, NULL, 0, &value)) return false;
      uint32_t count = dvm_array_length(vm, value.l);
      dvm_ref array = dvm_new_array(vm, 'L', cls->desc, count);
      if (array && count) memcpy(dvm_array_data(vm, array),
                                 dvm_array_data(vm, value.l), count * sizeof(dvm_ref));
      RETL(array);
   }
   /* Builtin constants are created in declaration order during definition. */
   uint32_t count = 0;
   for (int i = 0; i < cls->nsfields; ++i)
      if (!strcmp(cls->sfields[i].type, cls->desc)) ++count;
   dvm_ref array = dvm_new_array(vm, 'L', cls->desc, count);
   dvm_ref *data = dvm_array_data(vm, array);
   if (!data && count) return false;
   uint32_t index = 0;
   for (int i = 0; i < cls->nsfields; ++i)
      if (!strcmp(cls->sfields[i].type, cls->desc))
         data[index++] = cls->sslots[cls->sfields[i].slot].l;
   RETL(array);
}

static bool cl_getSuperclass(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct dvm_class *c = o ? o->klass : NULL;
   /* Object, interfaces and primitives have no superclass; everything else
    * does, and reflective walkers stop on the null. */
   if (!c || c->is_interface || c->is_primitive) RETL(0);
   RETL(class_object_for(vm, c->super));
}

static uint32_t class_java_modifiers(struct dvm_class *c)
{
   if (!c) return 0;
   if (c->is_array) {
      uint32_t component = class_java_modifiers(c->elem);
      if (component & DEX_ACC_INTERFACE) component &= ~(DEX_ACC_INTERFACE | DEX_ACC_STATIC);
      return component | DEX_ACC_ABSTRACT | DEX_ACC_FINAL;
   }
   if (c->is_primitive) return DEX_ACC_PUBLIC | DEX_ACC_ABSTRACT | DEX_ACC_FINAL;
   uint32_t flags = c->access;
   if (c->dex) (void)dex_class_inner_flags(c->dex, c->class_def_idx, &flags);
   return flags & 0xffffu;
}

static bool cl_getModifiers(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   RETI(class_java_modifiers(o ? o->klass : NULL));
}

static dvm_ref class_annotation(struct dvm *vm, dvm_ref class_ref,
                                dvm_ref annotation_class_ref,
                                bool inherited)
{
   struct dvm_object *class_obj = dvm__obj(vm, class_ref);
   struct dvm_object *annotation_class_obj =
      dvm__obj(vm, annotation_class_ref);
   struct dvm_class *target = class_obj ? class_obj->klass : NULL;
   struct dvm_class *annotation = annotation_class_obj
      ? annotation_class_obj->klass : NULL;
   if (!target || !annotation || !annotation->desc) return 0;

   bool may_inherit = inherited && annotation->dex &&
      dex_class_annotation(annotation->dex, annotation->class_def_idx,
                           "Ljava/lang/annotation/Inherited;", NULL);
   for (struct dvm_class *c = target; c; c = may_inherit ? c->super : NULL) {
      uint32_t encoded_off;
      if (!c->dex || !dex_class_annotation(c->dex, c->class_def_idx,
                                           annotation->desc, &encoded_off)) {
         if (!may_inherit) break;
         continue;
      }
      dvm_ref ref = dvm_new_object(vm, annotation);
      struct dvm_object *obj = dvm__obj(vm, ref);
      if (!obj) return 0;
      obj->annotation_dex = c->dex;
      obj->annotation_off = encoded_off;
      return ref;
   }
   return 0;
}

static bool cl_getAnnotation(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "annotationClass");
      return false;
   }
   RETL(class_annotation(vm, self, ARG(0).l, true));
}

static bool cl_getDeclaredAnnotation(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "annotationClass");
      return false;
   }
   RETL(class_annotation(vm, self, ARG(0).l, false));
}

static bool cl_isAnnotationPresent(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "annotationClass");
      return false;
   }
   RETI(class_annotation(vm, self, ARG(0).l, true) != 0);
}

/* Build the Annotation[] Class.getDeclaredAnnotations() answers.  Each
 * element is an instance of the annotation type with annotation_dex/off set
 * so element accessors can read the encoded form. */
static dvm_ref class_declared_annotations(struct dvm *vm, dvm_ref class_ref)
{
   struct dvm_object *class_obj = dvm__obj(vm, class_ref);
   struct dvm_class *target = class_obj ? class_obj->klass : NULL;
   if (!target || !target->dex) {
      return dvm_new_array(vm, 'L', "Ljava/lang/annotation/Annotation;", 0);
   }
   uint32_t offs[64];
   const char *types[64];
   int total = dex_class_annotations(target->dex, target->class_def_idx,
                                     offs, types, 64);
   int n = total < 64 ? total : 64;
   dvm_ref arr =
      dvm_new_array(vm, 'L', "Ljava/lang/annotation/Annotation;", (uint32_t)n);
   dvm_ref *items = arr ? (dvm_ref *)dvm_array_data(vm, arr) : NULL;
   for (int i = 0; items && i < n; ++i) {
      struct dvm_class *ac = dvm__class_by_desc(vm, types[i]);
      if (!ac) continue;
      dvm_ref ref = dvm_new_object(vm, ac);
      struct dvm_object *obj = dvm__obj(vm, ref);
      if (!obj) continue;
      obj->annotation_dex = target->dex;
      obj->annotation_off = offs[i];
      items[i] = ref;
   }
   return arr;
}

/* getAnnotations(): declared on this class, plus @Inherited ones from supers
 * that are not already present (same rule Class.getAnnotation walks). */
static dvm_ref class_all_annotations(struct dvm *vm, dvm_ref class_ref)
{
   struct dvm_object *class_obj = dvm__obj(vm, class_ref);
   struct dvm_class *target = class_obj ? class_obj->klass : NULL;
   if (!target) {
      return dvm_new_array(vm, 'L', "Ljava/lang/annotation/Annotation;", 0);
   }
   uint32_t offs[128];
   const char *types[128];
   struct dex_file *dexes[128];
   int n = 0;
   for (struct dvm_class *c = target; c && n < 128; c = c->super) {
      if (!c->dex) continue;
      uint32_t coffs[64];
      const char *ctypes[64];
      int total = dex_class_annotations(c->dex, c->class_def_idx,
                                        coffs, ctypes, 64);
      int cn = total < 64 ? total : 64;
      for (int i = 0; i < cn && n < 128; ++i) {
         if (!ctypes[i]) continue;
         if (c != target) {
            struct dvm_class *ac = dvm__class_by_desc(vm, ctypes[i]);
            if (!ac || !ac->dex ||
                !dex_class_annotation(ac->dex, ac->class_def_idx,
                                      "Ljava/lang/annotation/Inherited;",
                                      NULL))
               continue;
         }
         int dup = 0;
         for (int s = 0; s < n; ++s)
            if (types[s] && !strcmp(types[s], ctypes[i])) { dup = 1; break; }
         if (dup) continue;
         types[n] = ctypes[i];
         offs[n] = coffs[i];
         dexes[n] = c->dex;
         ++n;
      }
   }
   dvm_ref arr =
      dvm_new_array(vm, 'L', "Ljava/lang/annotation/Annotation;", (uint32_t)n);
   dvm_ref *items = arr ? (dvm_ref *)dvm_array_data(vm, arr) : NULL;
   for (int i = 0; items && i < n; ++i) {
      struct dvm_class *ac = dvm__class_by_desc(vm, types[i]);
      if (!ac) continue;
      dvm_ref ref = dvm_new_object(vm, ac);
      struct dvm_object *obj = dvm__obj(vm, ref);
      if (!obj) continue;
      obj->annotation_dex = dexes[i];
      obj->annotation_off = offs[i];
      items[i] = ref;
   }
   return arr;
}

static bool cl_getDeclaredAnnotations(struct dvm *vm, dvm_ref self,
                                      const union dvm_value *args, int nargs,
                                      union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(class_declared_annotations(vm, self));
}

static bool cl_getAnnotations(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(class_all_annotations(vm, self));
}

static bool cl_is_member(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct dvm_class *c = o ? o->klass : NULL;
   RETI(c && c->dex && dex_class_is_member(c->dex, c->class_def_idx));
}

static bool cl_getEnclosingClass(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct dvm_class *c = o ? o->klass : NULL;
   const char *desc = c && c->dex
      ? dex_class_enclosing_type(c->dex, c->class_def_idx) : NULL;
   RETL(desc ? class_object_for(vm, dvm__class_by_desc(vm, desc)) : 0);
}

static bool cl_getEnclosingExecutable(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *object = dvm__obj(vm, self);
   struct dvm_class *cls = object ? object->klass : NULL;
   uint32_t index;
   struct dex_method_id id;
   if (!cls || !cls->dex || cls->is_array || cls->is_primitive ||
       !dex_class_enclosing_method(cls->dex, cls->class_def_idx, &index) ||
       !dex_method_id(cls->dex, index, &id)) RETL(0);
   const char *name = dex_string(cls->dex, id.name_idx);
   bool constructor = !strcmp(builtin_name(), "getEnclosingConstructor");
   /* Class initializers have no public reflection object. Each API returns
    * null when the enclosing executable has the other kind. */
   if (!name || !strcmp(name, "<clinit>") ||
       constructor != !strcmp(name, "<init>")) RETL(0);
   const char *desc = dex_type(cls->dex, id.class_idx);
   struct dvm_class *owner = desc ? dvm__class_by_desc(vm, desc) : NULL;
   if (!owner || owner->synthesized) {
      dvm__throw(vm, "java/lang/NoClassDefFoundError", "%s", desc ? desc : "?");
      return false;
   }
   char *sig = dex_proto_signature_alloc(cls->dex, id.proto_idx);
   if (!sig) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "enclosing method signature");
      return false;
   }
   struct dvm_method *method = NULL;
   for (int i = 0; i < owner->nmethods; ++i) {
      struct dvm_method *candidate = &owner->methods[i];
      if (candidate->name && candidate->sig && !strcmp(candidate->name, name) &&
          !strcmp(candidate->sig, sig)) { method = candidate; break; }
   }
   free(sig);
   if (!method) {
      dvm__throw(vm, "java/lang/NoSuchMethodError", "%s.%s", owner->name, name);
      return false;
   }
   dvm_ref owner_ref = class_object_for(vm, owner);
   if (constructor) RETL(reflect_constructor(vm, owner_ref, method));
   struct dvm_class *mc = dvm__class_by_desc(vm, "Ljava/lang/reflect/Method;");
   dvm_ref result = mc ? dvm_new_object(vm, mc) : 0;
   if (!result) RETL(0);
   dvm_pin(vm, result);
   union dvm_value value = { .l = owner_ref };
   (void)dvm_set_field(vm, result, "owner", "Ljava/lang/Class;", value);
   value.l = dvm_new_string(vm, method->name);
   (void)dvm_set_field(vm, result, "name", "Ljava/lang/String;", value);
   value.l = dvm_new_string(vm, method->sig);
   (void)dvm_set_field(vm, result, "sig", "Ljava/lang/String;", value);
   value.i = (int32_t)method->access;
   (void)dvm_set_field(vm, result, "access", "I", value);
   dvm_unpin(vm, result);
   RETL(result);
}

static bool cl_getDeclaredClasses(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct dvm_class *c = o ? o->klass : NULL;
   int count = c && c->dex && !c->is_array
      ? dex_class_member_types(c->dex, c->class_def_idx, NULL, 0) : 0;
   if (count < 0) {
      dvm__throw(vm, "java/lang/annotation/AnnotationFormatError", "invalid MemberClasses metadata");
      return false;
   }
   const char **types = count ? calloc((size_t)count, sizeof *types) : NULL;
   if (count && !types) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "member classes");
      return false;
   }
   if (count) (void)dex_class_member_types(c->dex, c->class_def_idx, types, count);
   dvm_ref array = dvm_new_array(vm, 'L', "Ljava/lang/Class;", (uint32_t)count);
   if (!array) { free(types); return false; }
   dvm_pin(vm, array);
   for (int i = 0; i < count; ++i) {
      struct dvm_class *member = dvm__class_by_desc(vm, types[i]);
      if (member && member->synthesized) {
         dvm__throw(vm, "java/lang/NoClassDefFoundError", "%s", types[i]);
         dvm_unpin(vm, array); free(types); return false;
      }
      dvm_ref ref = member ? class_object_for(vm, member) : 0;
      if (!ref) { dvm_unpin(vm, array); free(types); return false; }
      ((dvm_ref *)dvm_array_data(vm, array))[i] = ref;
   }
   dvm_unpin(vm, array);
   free(types);
   RETL(array);
}

static bool cl_getDeclaringClass(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   struct dvm_object *o = dvm__obj(vm, self);
   struct dvm_class *c = o ? o->klass : NULL;
   if (!c || !c->dex || !dex_class_is_member(c->dex, c->class_def_idx)) RETL(0);
   return cl_getEnclosingClass(vm, self, args, nargs, out);
}

#define MODIFIER_TEST(fn, bit) \
   static bool fn(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
                  int nargs, union dvm_value *out) \
   { (void)vm; (void)self; (void)nargs; RETI((ARG(0).u & (bit)) != 0); }
MODIFIER_TEST(mod_is_public, DEX_ACC_PUBLIC)
MODIFIER_TEST(mod_is_private, DEX_ACC_PRIVATE)
MODIFIER_TEST(mod_is_protected, DEX_ACC_PROTECTED)
MODIFIER_TEST(mod_is_static, DEX_ACC_STATIC)
MODIFIER_TEST(mod_is_final, DEX_ACC_FINAL)
MODIFIER_TEST(mod_is_synchronized, DEX_ACC_SYNCHRONIZED)
MODIFIER_TEST(mod_is_volatile, DEX_ACC_VOLATILE)
MODIFIER_TEST(mod_is_transient, DEX_ACC_TRANSIENT)
MODIFIER_TEST(mod_is_native, DEX_ACC_NATIVE)
MODIFIER_TEST(mod_is_interface, DEX_ACC_INTERFACE)
MODIFIER_TEST(mod_is_abstract, DEX_ACC_ABSTRACT)
MODIFIER_TEST(mod_is_strict, DEX_ACC_STRICT)
#undef MODIFIER_TEST

static const struct rt_method rt_modifier[] = {
   SM("isPublic", "(I)Z", mod_is_public),
   SM("isPrivate", "(I)Z", mod_is_private),
   SM("isProtected", "(I)Z", mod_is_protected),
   SM("isStatic", "(I)Z", mod_is_static),
   SM("isFinal", "(I)Z", mod_is_final),
   SM("isSynchronized", "(I)Z", mod_is_synchronized),
   SM("isVolatile", "(I)Z", mod_is_volatile),
   SM("isTransient", "(I)Z", mod_is_transient),
   SM("isNative", "(I)Z", mod_is_native),
   SM("isInterface", "(I)Z", mod_is_interface),
   SM("isAbstract", "(I)Z", mod_is_abstract),
   SM("isStrict", "(I)Z", mod_is_strict),
   M_END,
};

/* Generic type information is not in the ordinary dex superclass index: it
 * lives in dalvik.annotation.Signature.  Model the java.lang.reflect Type
 * objects rather than erasing them to Class, because Gson (and many DI/JSON
 * libraries) deliberately distinguishes List<Foo> from raw List. */
static const struct rt_field rt_parameterized_type_fields[] = {
   { "rawType", "Ljava/lang/reflect/Type;" },
   { "actualTypeArguments", "[Ljava/lang/reflect/Type;" },
   { "ownerType", "Ljava/lang/reflect/Type;" }, F_END
};

static const struct rt_field rt_wildcard_type_fields[] = {
   { "upperBounds", "[Ljava/lang/reflect/Type;" },
   { "lowerBounds", "[Ljava/lang/reflect/Type;" }, F_END
};

static const struct rt_field rt_generic_array_type_fields[] = {
   { "genericComponentType", "Ljava/lang/reflect/Type;" }, F_END
};

static const struct rt_field rt_type_variable_fields[] = {
   { "name", "Ljava/lang/String;" },
   { "genericDeclaration", "Ljava/lang/reflect/GenericDeclaration;" },
   { "bounds", "[Ljava/lang/reflect/Type;" }, F_END
};

static bool reflect_get_raw_type(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   if (!dvm_get_field(vm, self, "rawType", "Ljava/lang/reflect/Type;", out))
      out->l = 0;
   return true;
}

static bool reflect_get_type_arguments(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)args; (void)nargs;
   if (!dvm_get_field(vm, self, "actualTypeArguments",
                      "[Ljava/lang/reflect/Type;", out))
      out->l = 0;
   return true;
}

static bool reflect_get_owner_type(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)args; (void)nargs;
   if (!dvm_get_field(vm, self, "ownerType", "Ljava/lang/reflect/Type;", out))
      out->l = 0;
   return true;
}

static bool reflect_get_upper_bounds(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)args; (void)nargs;
   if (!dvm_get_field(vm, self, "upperBounds",
                      "[Ljava/lang/reflect/Type;", out))
      out->l = 0;
   return true;
}

static bool reflect_get_lower_bounds(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)args; (void)nargs;
   if (!dvm_get_field(vm, self, "lowerBounds",
                      "[Ljava/lang/reflect/Type;", out))
      out->l = 0;
   return true;
}

static bool reflect_get_generic_component(struct dvm *vm, dvm_ref self,
                                          const union dvm_value *args,
                                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   if (!dvm_get_field(vm, self, "genericComponentType",
                      "Ljava/lang/reflect/Type;", out))
      out->l = 0;
   return true;
}

static bool type_variable_get_name(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)args; (void)nargs;
   if (!dvm_get_field(vm, self, "name", "Ljava/lang/String;", out)) out->l = 0;
   return true;
}

static bool type_variable_get_declaration(struct dvm *vm, dvm_ref self,
                                          const union dvm_value *args,
                                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   if (!dvm_get_field(vm, self, "genericDeclaration",
                      "Ljava/lang/reflect/GenericDeclaration;", out))
      out->l = 0;
   return true;
}

static bool type_variable_get_bounds(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)args; (void)nargs;
   if (!dvm_get_field(vm, self, "bounds", "[Ljava/lang/reflect/Type;", out))
      out->l = 0;
   return true;
}

static const struct rt_method rt_parameterized_type[] = {
   M("getRawType", "()Ljava/lang/reflect/Type;", reflect_get_raw_type),
   M("getActualTypeArguments", "()[Ljava/lang/reflect/Type;",
     reflect_get_type_arguments),
   M("getOwnerType", "()Ljava/lang/reflect/Type;", reflect_get_owner_type),
   M_END,
};

static const struct rt_method rt_wildcard_type[] = {
   M("getUpperBounds", "()[Ljava/lang/reflect/Type;", reflect_get_upper_bounds),
   M("getLowerBounds", "()[Ljava/lang/reflect/Type;", reflect_get_lower_bounds),
   M_END,
};

static const struct rt_method rt_generic_array_type[] = {
   M("getGenericComponentType", "()Ljava/lang/reflect/Type;",
     reflect_get_generic_component),
   M_END,
};

static const struct rt_method rt_type_variable[] = {
   M("getName", "()Ljava/lang/String;", type_variable_get_name),
   M("getGenericDeclaration", "()Ljava/lang/reflect/GenericDeclaration;",
     type_variable_get_declaration),
   M("getBounds", "()[Ljava/lang/reflect/Type;", type_variable_get_bounds),
   M_END,
};

struct generic_sig_parser {
   struct dvm *vm;
   const char *p;
   int depth;
   dvm_ref declaration;
   struct dvm_class *declaring_class;
   char var_names[16][64];
   dvm_ref variables[16];
   int nvariables;
};

static dvm_ref generic_sig_type(struct generic_sig_parser *sp);

static dvm_ref generic_type_array(struct dvm *vm, const dvm_ref *items, int n)
{
   dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/lang/reflect/Type;",
                               (uint32_t)n);
   dvm_ref *dst = arr ? dvm_array_data(vm, arr) : NULL;
   if (dst && n) memcpy(dst, items, (size_t)n * sizeof *items);
   return arr;
}

static dvm_ref generic_variable_find(struct generic_sig_parser *sp,
                                     const char *name, size_t len)
{
   for (int i = 0; i < sp->nvariables; ++i)
      if (strlen(sp->var_names[i]) == len &&
          !memcmp(sp->var_names[i], name, len))
         return sp->variables[i];
   return 0;
}

static dvm_ref generic_variable_new(struct generic_sig_parser *sp,
                                    const char *name, size_t len)
{
   dvm_ref old = generic_variable_find(sp, name, len);
   if (old) return old;
   if (sp->nvariables >= (int)(sizeof sp->variables / sizeof sp->variables[0]))
      return 0;
   struct dvm_class *vc = dvm__class_by_desc(sp->vm,
      "Llunaria/reflect/TypeVariableImpl;");
   dvm_ref ref = vc ? dvm_new_object(sp->vm, vc) : 0;
   if (!ref) return 0;
   int at = sp->nvariables++;
   if (len >= sizeof sp->var_names[at]) len = sizeof sp->var_names[at] - 1;
   memcpy(sp->var_names[at], name, len);
   sp->var_names[at][len] = '\0';
   sp->variables[at] = ref;
   union dvm_value v = { .l = dvm_new_string_n(sp->vm, name, len) };
   (void)dvm_set_field(sp->vm, ref, "name", "Ljava/lang/String;", v);
   v.l = sp->declaration;
   (void)dvm_set_field(sp->vm, ref, "genericDeclaration",
                       "Ljava/lang/reflect/GenericDeclaration;", v);
   return ref;
}

static void generic_sig_cached_formals(struct generic_sig_parser *sp)
{
   if (!sp->declaring_class || !sp->declaring_class->type_parameters) return;
   dvm_ref *cached = dvm_array_data(sp->vm,
                                   sp->declaring_class->type_parameters);
   int n = (int)dvm_array_length(sp->vm,
                                sp->declaring_class->type_parameters);
   for (int i = 0; cached && i < n && sp->nvariables < 16; ++i) {
      union dvm_value name = { 0 };
      if (!dvm_get_field(sp->vm, cached[i], "name", "Ljava/lang/String;",
                         &name))
         continue;
      const char *s = dvm_string_utf8(sp->vm, name.l);
      if (!s) continue;
      snprintf(sp->var_names[sp->nvariables],
               sizeof sp->var_names[sp->nvariables], "%s", s);
      sp->variables[sp->nvariables++] = cached[i];
   }
}

/* Parse `<E:Ljava/lang/Object;T::Ljava/io/Serializable;>` and install stable
 * TypeVariable identities on the declaring Class object. */
static void generic_sig_formals(struct generic_sig_parser *sp)
{
   if (*sp->p != '<') return;

   /* Reuse variables created by an earlier generic reflection query. */
   generic_sig_cached_formals(sp);

   ++sp->p;
   while (*sp->p && *sp->p != '>') {
      const char *name = sp->p;
      while (*sp->p && *sp->p != ':' && *sp->p != '>') ++sp->p;
      if (*sp->p != ':') break;
      size_t name_len = (size_t)(sp->p - name);
      dvm_ref variable = generic_variable_new(sp, name, name_len);
      dvm_ref bounds[16];
      int nbounds = 0;
      while (*sp->p == ':') {
         ++sp->p;
         if (*sp->p == ':') continue; /* absent class bound */
         dvm_ref bound = generic_sig_type(sp);
         if (bound && nbounds < 16) bounds[nbounds++] = bound;
      }
      if (!nbounds)
         bounds[nbounds++] = class_object_for(sp->vm,
            dvm__class_by_desc(sp->vm, "Ljava/lang/Object;"));
      union dvm_value v = { .l = generic_type_array(sp->vm, bounds, nbounds) };
      (void)dvm_set_field(sp->vm, variable, "bounds",
                          "[Ljava/lang/reflect/Type;", v);
   }
   if (*sp->p == '>') ++sp->p;

   if (sp->declaring_class && !sp->declaring_class->type_parameters) {
      dvm_ref arr = dvm_new_array(sp->vm, 'L',
                                  "Ljava/lang/reflect/TypeVariable;",
                                  (uint32_t)sp->nvariables);
      dvm_ref *dst = arr ? dvm_array_data(sp->vm, arr) : NULL;
      if (dst && sp->nvariables)
         memcpy(dst, sp->variables,
                (size_t)sp->nvariables * sizeof *sp->variables);
      sp->declaring_class->type_parameters = arr;
      dvm_pin(sp->vm, arr);
   }
}

static dvm_ref generic_wildcard(struct generic_sig_parser *sp, char variance)
{
   struct dvm *vm = sp->vm;
   dvm_ref object_type = class_object_for(
      vm, dvm__class_by_desc(vm, "Ljava/lang/Object;"));
   dvm_ref bound = variance == '*' ? object_type : generic_sig_type(sp);
   if (!bound) bound = object_type;
   dvm_ref upper_items[1] = { variance == '-' ? object_type : bound };
   dvm_ref lower_items[1] = { bound };
   dvm_ref upper = generic_type_array(vm, upper_items, 1);
   dvm_ref lower = generic_type_array(vm, lower_items, variance == '-' ? 1 : 0);
   struct dvm_class *wc = dvm__class_by_desc(vm,
      "Llunaria/reflect/WildcardTypeImpl;");
   dvm_ref ref = wc ? dvm_new_object(vm, wc) : 0;
   union dvm_value v = { .l = upper };
   (void)dvm_set_field(vm, ref, "upperBounds", "[Ljava/lang/reflect/Type;", v);
   v.l = lower;
   (void)dvm_set_field(vm, ref, "lowerBounds", "[Ljava/lang/reflect/Type;", v);
   return ref;
}

static dvm_ref generic_class_type(struct generic_sig_parser *sp)
{
   char raw[1024];
   size_t used = 0;
   raw[used++] = *sp->p++; /* L */
   while (*sp->p && *sp->p != '<' && *sp->p != ';' &&
          used + 2 < sizeof raw)
      raw[used++] = *sp->p++;
   raw[used++] = ';';
   raw[used] = '\0';
   dvm_ref raw_type = class_object_for(sp->vm,
      dvm__class_by_desc(sp->vm, raw));
   if (*sp->p != '<') {
      if (*sp->p == ';') ++sp->p;
      return raw_type;
   }

   ++sp->p; /* '<' */
   dvm_ref args[64];
   int nargs = 0;
   while (*sp->p && *sp->p != '>' && nargs < (int)(sizeof args / sizeof args[0])) {
      dvm_ref arg = generic_sig_type(sp);
      if (!arg) break;
      args[nargs++] = arg;
   }
   if (*sp->p == '>') ++sp->p;
   /* A nested-class suffix is part of the same class_type_signature.  The
    * common Android form uses '$' in one segment; consume uncommon dotted
    * suffixes safely even though their owner parameterization is not needed
    * to describe the direct superclass. */
   while (*sp->p && *sp->p != ';') ++sp->p;
   if (*sp->p == ';') ++sp->p;

   dvm_ref argv = generic_type_array(sp->vm, args, nargs);
   struct dvm_class *pc = dvm__class_by_desc(sp->vm,
      "Llunaria/reflect/ParameterizedTypeImpl;");
   dvm_ref ref = pc ? dvm_new_object(sp->vm, pc) : 0;
   union dvm_value v = { .l = raw_type };
   (void)dvm_set_field(sp->vm, ref, "rawType", "Ljava/lang/reflect/Type;", v);
   v.l = argv;
   (void)dvm_set_field(sp->vm, ref, "actualTypeArguments",
                       "[Ljava/lang/reflect/Type;", v);
   v.l = 0;
   (void)dvm_set_field(sp->vm, ref, "ownerType", "Ljava/lang/reflect/Type;", v);
   return ref;
}

static dvm_ref generic_sig_type(struct generic_sig_parser *sp)
{
   if (!sp || !sp->p || !*sp->p || sp->depth++ > 64) return 0;
   dvm_ref result = 0;
   switch (*sp->p) {
      case '+': ++sp->p; result = generic_wildcard(sp, '+'); break;
      case '-': ++sp->p; result = generic_wildcard(sp, '-'); break;
      case '*': ++sp->p; result = generic_wildcard(sp, '*'); break;
      case 'L': result = generic_class_type(sp); break;
      case '[': {
         ++sp->p;
         dvm_ref component = generic_sig_type(sp);
         struct dvm_class *gc = dvm__class_by_desc(sp->vm,
            "Llunaria/reflect/GenericArrayTypeImpl;");
         result = gc ? dvm_new_object(sp->vm, gc) : 0;
         union dvm_value v = { .l = component };
         (void)dvm_set_field(sp->vm, result, "genericComponentType",
                             "Ljava/lang/reflect/Type;", v);
         break;
      }
      case 'T':
      {
         ++sp->p;
         const char *name = sp->p;
         while (*sp->p && *sp->p != ';') ++sp->p;
         size_t len = (size_t)(sp->p - name);
         result = generic_variable_find(sp, name, len);
         if (!result) result = generic_variable_new(sp, name, len);
         if (*sp->p == ';') ++sp->p;
         break;
      }
      default: {
         char primitive[2] = { *sp->p++, '\0' };
         result = class_object_for(sp->vm,
            dvm__class_by_desc(sp->vm, primitive));
         break;
      }
   }
   --sp->depth;
   return result;
}

/* Unlike Field.getType(), getGenericType() must retain parameter arguments.
 * Gson asks it for AssetsConfig.colors and needs List<ColorConfig>, not just
 * List; erasing it makes ObjectTypeAdapter produce LinkedTreeMap elements
 * which then fail the app's ColorConfig cast. */
static bool rf_getGenericType(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value owner = { 0 }, namev = { 0 };
   (void)dvm_get_field(vm, self, "owner", "Ljava/lang/Class;", &owner);
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &namev);
   struct dvm_object *co = dvm__obj(vm, owner.l);
   struct dvm_class *target = co ? co->klass : NULL;
   const char *name = dvm_string_utf8(vm, namev.l);

   for (struct dvm_class *c = target; c && name; c = c->super) {
      if (!class_declares_field(c, name) || !c->dex) continue;
      char signature[4096];
      if (!dex_field_signature(c->dex, c->class_def_idx, name,
                               signature, sizeof signature))
         break;

      dvm_ref declaration = class_object_for(vm, c);
      if (!c->type_parameters) {
         char class_signature[4096];
         if (dex_class_signature(c->dex, c->class_def_idx, class_signature,
                                 sizeof class_signature)) {
            struct generic_sig_parser class_parser = {
               .vm = vm, .p = class_signature, .declaration = declaration,
               .declaring_class = c
            };
            generic_sig_formals(&class_parser);
         }
      }
      struct generic_sig_parser parser = {
         .vm = vm, .p = signature, .declaration = declaration,
         .declaring_class = c
      };
      generic_sig_cached_formals(&parser);
      dvm_ref type = generic_sig_type(&parser);
      if (type) RETL(type);
      break;
   }
   return rf_getType(vm, self, args, nargs, out);
}


/* ---- Method / Constructor: annotations and generic types -----------------
 *
 * A Method records the class it was asked of (`owner`), its name and either
 * its dex signature (`sig`) or, when getMethod() built it, its parameter
 * Class[].  The dex keeps a method's annotations, its parameters'
 * annotations and its generic Signature on the class that *declares* it, so
 * the first step is always to find that class.  Retrofit reads all of these
 * to build a request from an interface method; answering null for them
 * failed every Retrofit service — the HoYoverse login platform among them —
 * with an NPE on the missing parameter-annotation array. */
struct refl_decl {
   struct dvm_class *cls;
   char name[128];
   char sig[1024];     /* full, or "(...)" only when the return is unknown */
   bool full;
};

static bool refl_decl_of(struct dvm *vm, dvm_ref self, struct refl_decl *d)
{
   union dvm_value owner = { 0 }, namev = { 0 }, sigv = { 0 }, pv = { 0 };
   (void)dvm_get_field(vm, self, "owner", "Ljava/lang/Class;", &owner);
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &namev);
   (void)dvm_get_field(vm, self, "sig", "Ljava/lang/String;", &sigv);
   struct dvm_object *co = dvm__obj(vm, owner.l);
   const char *name = namev.l ? dvm_string_utf8(vm, namev.l) : NULL;
   if (!co || !co->klass) return false;
   snprintf(d->name, sizeof d->name, "%s", name && *name ? name : "<init>");
   const char *sig = sigv.l ? dvm_string_utf8(vm, sigv.l) : NULL;
   if (sig && *sig) {
      snprintf(d->sig, sizeof d->sig, "%s", sig);
      d->full = true;
   } else {
      /* getMethod(name, Class...) stored the parameter types instead. */
      size_t used = 0;
      d->sig[used++] = '(';
      if (dvm_get_field(vm, self, "paramTypes", "[Ljava/lang/Class;", &pv) &&
          pv.l) {
         dvm_ref *items = dvm_array_data(vm, pv.l);
         uint32_t n = dvm_array_length(vm, pv.l);
         for (uint32_t i = 0; items && i < n; ++i) {
            char one[256];
            if (!class_desc_of(vm, items[i], one, sizeof one)) return false;
            size_t l = strlen(one);
            if (used + l + 2 >= sizeof d->sig) return false;
            memcpy(d->sig + used, one, l);
            used += l;
         }
      }
      d->sig[used++] = ')';
      d->sig[used] = '\0';
      d->full = false;
   }
   for (struct dvm_class *c = co->klass; c; c = c->super) {
      for (int i = 0; i < c->nmethods; ++i) {
         const struct dvm_method *m = &c->methods[i];
         if (m->cls != c || !m->name || strcmp(m->name, d->name) || !m->sig)
            continue;
         if (d->full ? strcmp(m->sig, d->sig)
                     : strncmp(m->sig, d->sig, strlen(d->sig)))
            continue;
         /* The dex lookups want the full signature. */
         snprintf(d->sig, sizeof d->sig, "%s", m->sig);
         d->full = true;
         d->cls = c;
         return true;
      }
   }
   return false;
}

static dvm_ref refl_annotation_object(struct dvm *vm, struct dex_file *dex,
                                      uint32_t off, const char *type)
{
   struct dvm_class *ac = type ? dvm__class_by_desc(vm, type) : NULL;
   dvm_ref ref = ac ? dvm_new_object(vm, ac) : 0;
   struct dvm_object *obj = ref ? dvm__obj(vm, ref) : NULL;
   if (!obj) return 0;
   obj->annotation_dex = dex;
   obj->annotation_off = off;
   return ref;
}

static dvm_ref refl_annotation_array(struct dvm *vm, struct dex_file *dex,
                                     const uint32_t *offs, const char **types,
                                     int n)
{
   dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/lang/annotation/Annotation;",
                               (uint32_t)(n > 0 ? n : 0));
   if (!arr || n <= 0) return arr;
   dvm_pin(vm, arr);
   int k = 0;
   for (int i = 0; i < n; ++i) {
      dvm_ref a = refl_annotation_object(vm, dex, offs[i], types[i]);
      dvm_ref *items = dvm_array_data(vm, arr);
      if (a && items) items[k++] = a;
   }
   dvm_unpin(vm, arr);
   return arr;
}

/* A Field's annotations belong to its actual declaring class; they are
 * never inherited from an overridden/hidden field with the same name. */
static bool rf_getAnnotations(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value owner={0},field_name={0};
   dvm_get_field(vm,self,"owner","Ljava/lang/Class;",&owner);
   dvm_get_field(vm,self,"name","Ljava/lang/String;",&field_name);
   struct dvm_object *object=dvm__obj(vm,owner.l);
   struct dvm_class *c=object ? object->klass : NULL;
   const char *name=dvm_string_utf8(vm,field_name.l);
   while(c && name && !class_declares_field(c,name)) c=c->super;
   if (!c || !c->dex || !name)
      RETL(dvm_new_array(vm,'L',"Ljava/lang/annotation/Annotation;",0));
   int count=dex_field_annotations(c->dex,c->class_def_idx,name,NULL,NULL,0);
   if (!count) RETL(dvm_new_array(vm,'L',"Ljava/lang/annotation/Annotation;",0));
   uint32_t *offsets=calloc((size_t)count,sizeof *offsets);
   const char **types=calloc((size_t)count,sizeof *types);
   if (!offsets || !types) {
      free(offsets);free(types);
      dvm__throw(vm,"java/lang/OutOfMemoryError","field annotations");return false;
   }
   dex_field_annotations(c->dex,c->class_def_idx,name,offsets,types,count);
   dvm_ref array=refl_annotation_array(vm,c->dex,offsets,types,count);
   free(offsets);free(types);
   RETL(array);
}

/* getAnnotations() / getDeclaredAnnotations(): a method's annotations are
 * never inherited, so both are the declared set. */
static bool rm_getAnnotations(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   struct refl_decl d;
   if (!refl_decl_of(vm, self, &d) || !d.cls->dex)
      RETL(dvm_new_array(vm, 'L', "Ljava/lang/annotation/Annotation;", 0));
   uint32_t offs[64];
   const char *types[64];
   int n = dex_method_annotations(d.cls->dex, d.cls->class_def_idx, d.name,
                                  d.sig, offs, types, 64);
   RETL(refl_annotation_array(vm, d.cls->dex, offs, types, n < 64 ? n : 64));
}

static bool rm_getAnnotation(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "annotationClass");
      return false;
   }
   struct dvm_object *ao = dvm__obj(vm, ARG(0).l);
   struct dvm_class *ann = ao ? ao->klass : NULL;
   struct refl_decl d;
   uint32_t off;
   if (!ann || !ann->desc || !refl_decl_of(vm, self, &d) || !d.cls->dex ||
       !dex_method_annotation(d.cls->dex, d.cls->class_def_idx, d.name, d.sig,
                              ann->desc, &off))
      RETL(0);
   RETL(refl_annotation_object(vm, d.cls->dex, off, ann->desc));
}

/* getParameterAnnotations(): one Annotation[] per parameter.  The dex list
 * is authoritative when the method has one (its length is what ART
 * returns); otherwise every parameter has none. */
static bool rm_getParameterAnnotations(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   struct refl_decl d;
   const bool have = refl_decl_of(vm, self, &d) && d.cls->dex;
   int lists = have ? dex_parameter_annotation_lists(d.cls->dex,
                                                     d.cls->class_def_idx,
                                                     d.name, d.sig) : -1;
   if (lists < 0) {
      union dvm_value pt = { 0 };
      if (!rm_getParameterTypes(vm, self, args, nargs, &pt)) return false;
      lists = pt.l ? (int)dvm_array_length(vm, pt.l) : 0;
   }
   dvm_ref outer = dvm_new_array(vm, 'L', "[Ljava/lang/annotation/Annotation;",
                                 (uint32_t)lists);
   if (!outer) RETL(0);
   dvm_pin(vm, outer);
   for (int i = 0; i < lists; ++i) {
      uint32_t offs[32];
      const char *types[32];
      int n = have ? dex_parameter_annotations(d.cls->dex, d.cls->class_def_idx,
                                               d.name, d.sig, i, offs, types, 32)
                   : 0;
      dvm_ref inner = refl_annotation_array(vm, have ? d.cls->dex : NULL, offs,
                                            types, n < 32 ? n : 32);
      dvm_ref *items = dvm_array_data(vm, outer);
      if (items) items[i] = inner;
   }
   dvm_unpin(vm, outer);
   RETL(outer);
}

/* isDefault(): a public, non-abstract, non-static instance method of an
 * interface. */
static bool rm_isDefault(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs;
   struct refl_decl d;
   if (!refl_decl_of(vm, self, &d) || !d.cls->is_interface) RETI(0);
   const int32_t acc = rt_get_int(vm, self, "access");
   RETI((acc & (DEX_ACC_ABSTRACT | DEX_ACC_PUBLIC | DEX_ACC_STATIC)) ==
        DEX_ACC_PUBLIC);
}

/* getGenericParameterTypes() / getGenericReturnType(): from the method's
 * Signature, whose type variables are the method's own and its class's.
 * Without one the erased types are the answer, as they are on a device. */
static bool rm_generic(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   const bool want_return = !strcmp(builtin_name(), "getGenericReturnType");
   struct refl_decl d;
   char signature[4096];
   if (refl_decl_of(vm, self, &d) && d.cls->dex &&
       dex_method_signature(d.cls->dex, d.cls->class_def_idx, d.name, d.sig,
                            signature, sizeof signature)) {
      struct dvm_class *c = d.cls;
      dvm_ref class_decl = class_object_for(vm, c);
      if (!c->type_parameters) {
         char class_signature[4096];
         if (dex_class_signature(c->dex, c->class_def_idx, class_signature,
                                 sizeof class_signature)) {
            struct generic_sig_parser cp = {
               .vm = vm, .p = class_signature, .declaration = class_decl,
               .declaring_class = c
            };
            generic_sig_formals(&cp);
         }
      }
      struct generic_sig_parser sp = {
         .vm = vm, .p = signature, .declaration = class_decl,
         .declaring_class = c
      };
      generic_sig_cached_formals(&sp);
      /* The method's own type parameters belong to the method. */
      sp.declaring_class = NULL;
      sp.declaration = self;
      generic_sig_formals(&sp);
      if (*sp.p == '(') {
         ++sp.p;
         dvm_ref params[64];
         int n = 0;
         while (*sp.p && *sp.p != ')' && n < 64) {
            dvm_ref t = generic_sig_type(&sp);
            if (!t) break;
            params[n++] = t;
         }
         if (*sp.p == ')') {
            ++sp.p;
            if (want_return) {
               dvm_ref r = generic_sig_type(&sp);
               if (r) RETL(r);
            } else {
               RETL(generic_type_array(vm, params, n));
            }
         }
      }
   }
   if (want_return) return rm_getReturnType(vm, self, args, nargs, out);
   union dvm_value pt = { 0 };
   if (!rm_getParameterTypes(vm, self, args, nargs, &pt)) return false;
   uint32_t n = pt.l ? dvm_array_length(vm, pt.l) : 0;
   dvm_ref *src = pt.l ? dvm_array_data(vm, pt.l) : NULL;
   RETL(generic_type_array(vm, src, (int)n));
}

static bool cl_getGenericSuperclass(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct dvm_class *c = o ? o->klass : NULL;
   if (!c || c->is_interface || c->is_primitive) RETL(0);
   char signature[4096];
   bool have = c->dex
      ? dex_class_signature(c->dex, c->class_def_idx,
                            signature, sizeof signature)
      : c->runtime_signature &&
        snprintf(signature, sizeof signature, "%s", c->runtime_signature) > 0;
   if (have) {
      struct generic_sig_parser sp = {
         .vm = vm, .p = signature, .declaration = self, .declaring_class = c
      };
      generic_sig_formals(&sp);
      dvm_ref type = generic_sig_type(&sp);
      if (type) {
         if (vm->trace)
            fprintf(stderr, "[dvm] Class.getGenericSuperclass %s -> @%x "
                    "(Signature)\n", c->name ? c->name : "?", type);
         RETL(type);
      }
   }
   dvm_ref type = class_object_for(vm, c->super);
   if (vm->trace)
      fprintf(stderr, "[dvm] Class.getGenericSuperclass %s -> %s @%x\n",
              c->name ? c->name : "?",
              c->super && c->super->name ? c->super->name : "null", type);
   RETL(type);
}

static bool cl_getGenericInterfaces(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct dvm_class *c = o ? o->klass : NULL;
   char signature[4096];
   bool have = c && (c->dex
      ? dex_class_signature(c->dex, c->class_def_idx,
                            signature, sizeof signature)
      : c->runtime_signature &&
        snprintf(signature, sizeof signature, "%s", c->runtime_signature) > 0);
   if (have) {
      struct generic_sig_parser sp = {
         .vm = vm, .p = signature, .declaration = self, .declaring_class = c
      };
      generic_sig_formals(&sp);
      (void)generic_sig_type(&sp); /* superclass signature */
      dvm_ref interfaces[64];
      int n = 0;
      while (*sp.p && n < (int)(sizeof interfaces / sizeof interfaces[0])) {
         dvm_ref type = generic_sig_type(&sp);
         if (!type) break;
         interfaces[n++] = type;
      }
      RETL(generic_type_array(vm, interfaces, n));
   }

   int n = c ? c->nifaces : 0;
   dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/lang/reflect/Type;",
                               (uint32_t)n);
   dvm_ref *dst = arr ? dvm_array_data(vm, arr) : NULL;
   for (int i = 0; dst && i < n; ++i)
      dst[i] = class_object_for(vm, c->ifaces[i]);
   RETL(arr);
}

static bool cl_getTypeParameters(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct dvm_class *c = o ? o->klass : NULL;
   if (c && c->type_parameters) RETL(c->type_parameters);
   char signature[4096];
   bool have = c && (c->dex
      ? dex_class_signature(c->dex, c->class_def_idx,
                            signature, sizeof signature)
      : c->runtime_signature &&
        snprintf(signature, sizeof signature, "%s", c->runtime_signature) > 0);
   if (have) {
      struct generic_sig_parser sp = {
         .vm = vm, .p = signature, .declaration = self, .declaring_class = c
      };
      generic_sig_formals(&sp);
      if (c->type_parameters) RETL(c->type_parameters);
   }
   RETL(dvm_new_array(vm, 'L', "Ljava/lang/reflect/TypeVariable;", 0));
}

static bool cl_getInterfaces(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct dvm_class *c = o ? o->klass : NULL;
   int n = c ? c->nifaces : 0;
   dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/lang/Class;", (uint32_t)n);
   dvm_ref *items = arr ? (dvm_ref *)dvm_array_data(vm, arr) : NULL;
   for (int i = 0; items && i < n; ++i)
      items[i] = class_object_for(vm, c->ifaces[i]);
   RETL(arr);
}

/* Class.asSubclass(other) returns this class when it really is a subtype of
 * other and throws ClassCastException otherwise — callers use the throw as
 * the check, so answering "yes" unconditionally would let a wrong class
 * through. */
static bool cl_asSubclass(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *me = dvm__obj(vm, self);
   struct dvm_object *sup = dvm__obj(vm, ARG(0).l);
   struct dvm_class *mc = me ? me->klass : NULL;
   struct dvm_class *sc = sup ? sup->klass : NULL;
   if (mc && sc && dvm__class_assignable(vm, mc, sc)) RETL(self);
   dvm__throw(vm, "java/lang/ClassCastException", "%s is not a subclass of %s",
              mc ? mc->name : "null", sc ? sc->name : "null");
   return false;
}

static bool rc_getName(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value owner={0};
   (void)dvm_get_field(vm,self,"owner","Ljava/lang/Class;",&owner);
   return cl_getName(vm,owner.l,args,nargs,out);
}
static const struct rt_method rt_reflect_ctor[] = {
   M("getName", "()Ljava/lang/String;", rc_getName),
   M("newInstance", "([Ljava/lang/Object;)Ljava/lang/Object;", rc_newInstance),
   M("newInstance", "()Ljava/lang/Object;", rc_newInstance),
   M("setAccessible", "(Z)V", ao_setAccessible),
   M("isAccessible", "()Z", ao_isAccessible),
   /* A Constructor carries the same owner/sig/access a Method does — it is
    * built from rt_method_fields — so the accessors that read them work here
    * unchanged.  Leaving them off made getParameterTypes() unresolved, and a
    * dependency-injection scan that picks a constructor by its parameter list
    * cannot pick one at all when every candidate reports none. */
   M("getParameterTypes", "()[Ljava/lang/Class;", rm_getParameterTypes),
   M("getDeclaringClass", "()Ljava/lang/Class;", rm_getDeclaringClass),
   M("getModifiers", "()I", rm_getModifiers),
   M("getAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",
     rm_getAnnotation),
   M("isAnnotationPresent", "(Ljava/lang/Class;)Z", rm_isAnnotationPresent),
   M("getAnnotations", "()[Ljava/lang/annotation/Annotation;", rm_getAnnotations),
   M("getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;",
     rm_getAnnotations),
   M("getParameterAnnotations", "()[[Ljava/lang/annotation/Annotation;",
     rm_getParameterAnnotations),
   M("getGenericParameterTypes", "()[Ljava/lang/reflect/Type;", rm_generic),
   M_END,
};

static bool cl_getPackage(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out);
static bool cl_getDeclaredMethods(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out);
static bool cl_getMethods(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out);

/* Allocate exactly the dimensions requested. If the supplied component is
 * already an array, those extra dimensions remain null at the leaves. */
static dvm_ref reflect_array_allocate(struct dvm *vm,const char *descriptor,
                                      const int32_t *lengths,unsigned count)
{
   dvm_ref result=dvm_new_array(vm,dvm__kind_of(descriptor+1),descriptor+1,(uint32_t)lengths[0]);
   if (!result) return 0;
   dvm_pin(vm,result);
   if (count>1) {
      for (int32_t i=0;i<lengths[0];++i) {
         dvm_ref child=reflect_array_allocate(vm,descriptor+1,lengths+1,count-1);
         if (!child) {dvm_unpin(vm,result);return 0;}
         ((dvm_ref *)dvm_array_data(vm,result))[i]=child;
      }
   }
   dvm_unpin(vm,result);return result;
}
static dvm_ref reflect_array_create(struct dvm *vm,dvm_ref component_ref,
                                    const int32_t *lengths,unsigned count)
{
   if (!count || count>255) {dvm__throw(vm,"java/lang/IllegalArgumentException","invalid number of array dimensions");return 0;}
   struct dvm_object *co=dvm__obj(vm,component_ref);
   if (!co || !co->klass) {dvm__throw(vm,"java/lang/NullPointerException","componentType");return 0;}
   const char *component=co->klass->desc;
   unsigned existing=0;while(component && component[existing]=='[')++existing;
   if (!component || !strcmp(component,"V") || existing+count>255) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","invalid array component or rank");return 0;
   }
   /* Validate every length before allocating, even after a zero-length axis. */
   for (unsigned i=0;i<count;++i) if(lengths[i]<0) {
      dvm__throw(vm,"java/lang/NegativeArraySizeException","%d",lengths[i]);return 0;
   }
   size_t size=strlen(component)+count+1;
   char *descriptor=malloc(size);
   if (!descriptor) {dvm__throw(vm,"java/lang/OutOfMemoryError","array descriptor");return 0;}
   memset(descriptor,'[',count);memcpy(descriptor+count,component,strlen(component)+1);
   dvm_ref result=reflect_array_allocate(vm,descriptor,lengths,count);
   free(descriptor);
   if (!result && !dvm_exception(vm)) dvm__throw(vm,"java/lang/OutOfMemoryError","array allocation");
   return result;
}
static bool reflect_array_newInstance(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                      int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   int32_t length=ARG(1).i;
   dvm_ref result=reflect_array_create(vm,ARG(0).l,&length,1);
   if (!result) return false;
   RETL(result);
}
static bool reflect_array_newMultiInstance(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                           int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   struct dvm_object *dimensions=dvm__obj(vm,ARG(1).l);
   if (!dimensions) {dvm__throw(vm,"java/lang/NullPointerException","dimensions");return false;}
   if (dimensions->kind!=DVM_OBJ_ARRAY || dimensions->elem_kind!='I') {
      dvm__throw(vm,"java/lang/IllegalArgumentException","dimensions must be int[]");return false;
   }
   if (!dimensions->length || dimensions->length>255) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","invalid number of array dimensions");return false;
   }
   int32_t lengths[255];memcpy(lengths,dimensions->data,dimensions->length*sizeof *lengths);
   dvm_ref result=reflect_array_create(vm,ARG(0).l,lengths,dimensions->length);
   if (!result) return false;
   RETL(result);
}

static bool reflect_array_length(struct dvm *vm, dvm_ref self,
   const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)self;
   dvm_ref array = nargs ? ARG(0).l : 0;
   if (!array) {
      dvm__throw(vm, "java/lang/NullPointerException", "array");
      return false;
   }
   struct dvm_object *object = dvm__obj(vm, array);
   if (!object || object->kind != DVM_OBJ_ARRAY) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "argument is not an array");
      return false;
   }
   RETI((int32_t)object->length);
}

static struct dvm_object *reflect_array_at(struct dvm *vm, dvm_ref ref, int32_t index)
{
   if (!ref) { dvm__throw(vm, "java/lang/NullPointerException", "array"); return NULL; }
   struct dvm_object *a = dvm__obj(vm, ref);
   if (!a || a->kind != DVM_OBJ_ARRAY) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "not an array"); return NULL;
   }
   if (index < 0 || (uint32_t)index >= a->length) {
      dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "%d", index); return NULL;
   }
   return a;
}
static bool array_primitive_widens(char from, char to)
{
   if (from == to) return true;
   const char *targets = from == 'B' ? "SIJFD" : from == 'S' || from == 'C' ? "IJFD" :
      from == 'I' ? "JFD" : from == 'J' ? "FD" : from == 'F' ? "D" : "";
   return strchr(targets, to) != NULL;
}
static union dvm_value array_primitive_convert(union dvm_value v, char from, char to)
{
   if (from == to) return v;
   union dvm_value result = {0};
   if (to == 'D') result.d = from == 'F' ? (double)v.f : from == 'J' ? (double)v.j : (double)v.i;
   else if (to == 'F') result.f = from == 'J' ? (float)v.j : (float)v.i;
   else if (to == 'J') result.j = v.i;
   else result.i = v.i;
   return result;
}
static bool reflect_array_get(struct dvm *vm, dvm_ref self,
   const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_object *a = reflect_array_at(vm, ARG(0).l, ARG(1).i);
   if (!a) return false;
   uint32_t index = (uint32_t)ARG(1).i;
   char kind = a->elem_kind, wanted = rf_accessor_kind();
   union dvm_value value = {0};
   switch (kind) {
   case 'B': value.i = ((int8_t *)a->data)[index]; break;
   case 'Z': value.i = ((uint8_t *)a->data)[index] != 0; break;
   case 'C': value.i = ((uint16_t *)a->data)[index]; break;
   case 'S': value.i = ((int16_t *)a->data)[index]; break;
   case 'I': value.i = ((int32_t *)a->data)[index]; break;
   case 'J': value.j = ((int64_t *)a->data)[index]; break;
   case 'F': value.f = ((float *)a->data)[index]; break;
   case 'D': value.d = ((double *)a->data)[index]; break;
   default: value.l = ((dvm_ref *)a->data)[index]; kind = 'L'; break;
   }
   if (wanted == 'L') RETL(rf_box(vm, kind, value));
   if (!array_primitive_widens(kind, wanted)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "array element cannot widen to %c", wanted); return false;
   }
   *out = array_primitive_convert(value, kind, wanted); return true;
}
static bool reflect_array_set(struct dvm *vm, dvm_ref self,
   const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)self; (void)nargs; (void)out;
   struct dvm_object *a = reflect_array_at(vm, ARG(0).l, ARG(1).i);
   if (!a) return false;
   uint32_t index = (uint32_t)ARG(1).i;
   char kind = a->elem_kind, given = rf_accessor_kind();
   union dvm_value value = ARG(2);
   if (kind == 'L' || kind == '[') {
      struct dvm_class *element = a->cls ? a->cls->elem : NULL;
      if (given != 'L' || (value.l && element &&
          !dvm__class_assignable(vm, dvm_object_class(vm, value.l), element))) {
         dvm__throw(vm, "java/lang/IllegalArgumentException", "incompatible array element"); return false;
      }
      ((dvm_ref *)a->data)[index] = value.l; RETV();
   }
   if (given == 'L') {
      struct dvm_class *box = value.l ? dvm_object_class(vm, value.l) : NULL;
      const char *name = box ? box->name : "";
      static const struct { const char *name; char kind; } boxes[] = {
         {"java/lang/Boolean", 'Z'}, {"java/lang/Byte", 'B'}, {"java/lang/Character", 'C'},
         {"java/lang/Short", 'S'}, {"java/lang/Integer", 'I'}, {"java/lang/Long", 'J'},
         {"java/lang/Float", 'F'}, {"java/lang/Double", 'D'}
      };
      given = 0;
      for (unsigned i = 0; i < sizeof boxes / sizeof boxes[0]; ++i)
         if (!strcmp(name, boxes[i].name)) { given = boxes[i].kind; break; }
      if (given) value = box_get(vm, value.l);
   }
   if (!given || !array_primitive_widens(given, kind)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "incompatible primitive array element"); return false;
   }
   value = array_primitive_convert(value, given, kind);
   switch (kind) {
   case 'B': ((int8_t *)a->data)[index] = (int8_t)value.i; break;
   case 'Z': ((uint8_t *)a->data)[index] = value.i != 0; break;
   case 'C': ((uint16_t *)a->data)[index] = (uint16_t)value.i; break;
   case 'S': ((int16_t *)a->data)[index] = (int16_t)value.i; break;
   case 'I': ((int32_t *)a->data)[index] = value.i; break;
   case 'J': ((int64_t *)a->data)[index] = value.j; break;
   case 'F': ((float *)a->data)[index] = value.f; break;
   case 'D': ((double *)a->data)[index] = value.d; break;
   }
   RETV();
}

static const struct rt_method rt_reflect_array[] = {
   SM("get", "(Ljava/lang/Object;I)Ljava/lang/Object;", reflect_array_get),
   SM("set", "(Ljava/lang/Object;ILjava/lang/Object;)V", reflect_array_set),
   SM("getBoolean", "(Ljava/lang/Object;I)Z", reflect_array_get),
   SM("setBoolean", "(Ljava/lang/Object;IZ)V", reflect_array_set),
   SM("getByte", "(Ljava/lang/Object;I)B", reflect_array_get),
   SM("setByte", "(Ljava/lang/Object;IB)V", reflect_array_set),
   SM("getChar", "(Ljava/lang/Object;I)C", reflect_array_get),
   SM("setChar", "(Ljava/lang/Object;IC)V", reflect_array_set),
   SM("getShort", "(Ljava/lang/Object;I)S", reflect_array_get),
   SM("setShort", "(Ljava/lang/Object;IS)V", reflect_array_set),
   SM("getInt", "(Ljava/lang/Object;I)I", reflect_array_get),
   SM("setInt", "(Ljava/lang/Object;II)V", reflect_array_set),
   SM("getLong", "(Ljava/lang/Object;I)J", reflect_array_get),
   SM("setLong", "(Ljava/lang/Object;IJ)V", reflect_array_set),
   SM("getFloat", "(Ljava/lang/Object;I)F", reflect_array_get),
   SM("setFloat", "(Ljava/lang/Object;IF)V", reflect_array_set),
   SM("getDouble", "(Ljava/lang/Object;I)D", reflect_array_get),
   SM("setDouble", "(Ljava/lang/Object;ID)V", reflect_array_set),
   SM("getLength", "(Ljava/lang/Object;)I", reflect_array_length),
   SM("newInstance", "(Ljava/lang/Class;I)Ljava/lang/Object;",
      reflect_array_newInstance),
   SM("newInstance", "(Ljava/lang/Class;[I)Ljava/lang/Object;",
      reflect_array_newMultiInstance),
   M_END,
};

static const struct rt_method rt_class[] = {
   M("getName", "()Ljava/lang/String;", cl_getName),
   M("getCanonicalName", "()Ljava/lang/String;", cl_getName),
   M("getSimpleName", "()Ljava/lang/String;", cl_getSimpleName),
   SM("forName", "(Ljava/lang/String;)Ljava/lang/Class;", cl_forName),
   SM("forName",
      "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",
      cl_forName),
   M("getClassLoader", "()Ljava/lang/ClassLoader;", cl_getClassLoader),
   M("getResource", "(Ljava/lang/String;)Ljava/net/URL;", cl_getResource),
   M("getResourceAsStream", "(Ljava/lang/String;)Ljava/io/InputStream;",
     cl_class_getResourceAsStream),
   M("toString", "()Ljava/lang/String;", cl_getName),
   M("getMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",
     cl_getMethod),
   M("getDeclaredMethod",
     "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",
     cl_getMethod),
   M("getField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;", cl_getField),
   M("getDeclaredField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;",
     cl_getField),
   M("getConstructor", "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;",
     cl_getConstructor),
   M("getDeclaredConstructor",
     "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;", cl_getConstructor),
   M("getConstructors", "()[Ljava/lang/reflect/Constructor;",
     cl_getConstructors),
   M("getDeclaredConstructors", "()[Ljava/lang/reflect/Constructor;",
     cl_getDeclaredConstructors),
   M("newInstance", "()Ljava/lang/Object;", cl_newInstance),
   M("isInstance", "(Ljava/lang/Object;)Z", cl_isInstance),
   M("cast", "(Ljava/lang/Object;)Ljava/lang/Object;", cl_cast),
   M("isAssignableFrom", "(Ljava/lang/Class;)Z", cl_isAssignableFrom),
   M("isArray", "()Z", cl_isArray),
   M("getComponentType", "()Ljava/lang/Class;", cl_getComponentType),
   M("isPrimitive", "()Z", cl_isPrimitive),
   M("isInterface", "()Z", cl_isInterface),
   M("isAnnotation", "()Z", cl_has_flag),
   M("isSynthetic", "()Z", cl_has_flag),
   M("isEnum", "()Z", cl_isEnum),
   M("isRecord", "()Z", cl_isRecord),
   M("getRecordComponents", "()[Ljava/lang/reflect/RecordComponent;", cl_getRecordComponents),
   M("getEnumConstants", "()[Ljava/lang/Object;", cl_getEnumConstants),
   M("getAnnotation", "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",
     cl_getAnnotation),
   M("getDeclaredAnnotation",
     "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",
     cl_getDeclaredAnnotation),
   M("getAnnotations", "()[Ljava/lang/annotation/Annotation;",
     cl_getAnnotations),
   M("getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;",
     cl_getDeclaredAnnotations),
   M("isAnnotationPresent", "(Ljava/lang/Class;)Z", cl_isAnnotationPresent),
   M("isMemberClass", "()Z", cl_is_member),
   M("isAnonymousClass", "()Z", ret_false),
   M("isLocalClass", "()Z", ret_false),
   M("getPackage", "()Ljava/lang/Package;", cl_getPackage),
   M("getDeclaredMethods", "()[Ljava/lang/reflect/Method;", cl_getDeclaredMethods),
   M("getMethods", "()[Ljava/lang/reflect/Method;", cl_getMethods),
   M("getDeclaredFields", "()[Ljava/lang/reflect/Field;", cl_getDeclaredFields),
   M("getFields", "()[Ljava/lang/reflect/Field;", cl_getFields),
   M("getSuperclass", "()Ljava/lang/Class;", cl_getSuperclass),
   M("getModifiers", "()I", cl_getModifiers),
   M("getDeclaredClasses", "()[Ljava/lang/Class;", cl_getDeclaredClasses),
   M("getDeclaringClass", "()Ljava/lang/Class;", cl_getDeclaringClass),
   M("getEnclosingClass", "()Ljava/lang/Class;", cl_getEnclosingClass),
   M("getEnclosingMethod", "()Ljava/lang/reflect/Method;", cl_getEnclosingExecutable),
   M("getEnclosingConstructor", "()Ljava/lang/reflect/Constructor;", cl_getEnclosingExecutable),
   M("getGenericSuperclass", "()Ljava/lang/reflect/Type;",
     cl_getGenericSuperclass),
   M("getGenericInterfaces", "()[Ljava/lang/reflect/Type;",
     cl_getGenericInterfaces),
   M("getTypeParameters", "()[Ljava/lang/reflect/TypeVariable;",
     cl_getTypeParameters),
   M("getInterfaces", "()[Ljava/lang/Class;", cl_getInterfaces),
   M("asSubclass", "(Ljava/lang/Class;)Ljava/lang/Class;", cl_asSubclass),
   M_END,
};

/* ClassLoader(ClassLoader parent) — the constructor every custom loader in an
 * SDK chains to with super(parent).  Without it the super() call is
 * unresolved, the parent is dropped, and getParent() answers null: delegation
 * stops at the subclass and a class the parent could have loaded is reported
 * missing instead. */
static bool clsl_init_parent(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)out;
   union dvm_value parent = { .l = nargs > 0 ? ARG(0).l : 0 };
   (void)dvm_set_field(vm, self, "parent", "Ljava/lang/ClassLoader;", parent);
   RETV();
}

static bool clsl_get_parent(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "parent", "Ljava/lang/ClassLoader;", &v);
   RETL(v.l);
}

/* findLoadedClass(name): the Class if this VM has it, else null — never a
 * throw.  AppsFlyer asks it before loading; the class is not initialised, as
 * a lookup of an already loaded class would not initialise it either. */
static bool clsl_findLoadedClass(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *n = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!n || !dvm_class_exists(vm, n)) RETL(0);
   char desc[512];
   snprintf(desc, sizeof desc, "L%s;", n);
   for (char *p = desc; *p; ++p) if (*p == '.') *p = '/';
   RETL(class_object_for(vm, dvm__class_by_desc(vm, desc)));
}

static const struct rt_field rt_class_loader_fields[] = {
   { "parent", "Ljava/lang/ClassLoader;" }, F_END
};

static const struct rt_method rt_class_loader[] = {
   M("<init>", "()V", nop_void),
   M("<init>", "(Ljava/lang/ClassLoader;)V", clsl_init_parent),
   M("getParent", "()Ljava/lang/ClassLoader;", clsl_get_parent),
   M("loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", cl_forName),
   M("findLoadedClass", "(Ljava/lang/String;)Ljava/lang/Class;",
     clsl_findLoadedClass),
   SM("getSystemClassLoader", "()Ljava/lang/ClassLoader;", cl_getClassLoader),
   M("getResource", "(Ljava/lang/String;)Ljava/net/URL;", cl_getResource),
   SM("getSystemResource", "(Ljava/lang/String;)Ljava/net/URL;", cl_getResource),
   M("getResourceAsStream", "(Ljava/lang/String;)Ljava/io/InputStream;",
     cl_getResourceAsStream),
   SM("getSystemResourceAsStream",
      "(Ljava/lang/String;)Ljava/io/InputStream;", cl_getResourceAsStream),
   M_END,
};

/* dalvik.system.BaseDexClassLoader / PathClassLoader / DexClassLoader.
 * Combo's ModulesManagerImpl builds a PathClassLoader over the app's own
 * dex path; without these classes the factory's Class.forName path never
 * materialises and every combo_invoke reports "cannot found extern module".
 * The VM already has the app dexes loaded, so loadClass delegates to the
 * same Class.forName path ClassLoader uses. */
static bool dex_path_list_build(struct dvm *, dvm_ref, int, int, const char *);
static bool dexcl_library_path(struct dvm *, dvm_ref, const union dvm_value *, int, union dvm_value *);
static bool dexcl_find_library(struct dvm *, dvm_ref, const union dvm_value *, int, union dvm_value *);
static const struct rt_field rt_dex_class_loader_fields[] = {
   { "sharedLibraryLoaders", "[Ljava/lang/ClassLoader;" },
   { "pathList", "Ldalvik/system/DexPathList;" },
   { "dexPath", "Ljava/lang/String;" },
   F_END
};

static bool dexcl_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   /* BaseDexClassLoader(String dexPath, File optimizedDirectory,
    *                    String librarySearchPath, ClassLoader parent)
    * PathClassLoader(String dexPath, ClassLoader parent)
    * PathClassLoader(String dexPath, String librarySearchPath, ClassLoader parent)
    * DexClassLoader(String dexPath, String optimizedDirectory,
    *                String librarySearchPath, ClassLoader parent) */
   int first_dex = vm->ndexes;
   union dvm_value path = { 0 }, parent = { 0 };
   (void)out;
   if (nargs >= 1) path.l = ARG(0).l;
   if (nargs == 2) parent.l = ARG(1).l;
   else if (nargs == 3) parent.l = ARG(2).l;
   else if (nargs >= 4) parent.l = ARG(3).l;
   (void)dvm_set_field(vm, self, "dexPath", "Ljava/lang/String;", path);
   (void)dvm_set_field(vm, self, "parent", "Ljava/lang/ClassLoader;", parent);
   /* If the path names a guest .dex/.apk that is not yet loaded, open it the
    * same way DexFile does so subsequent loadClass can see its types. */
   if (path.l) {
      const char *gp = dvm_string_utf8(vm, path.l);
      if (gp && *gp) {
         char host[DVM_PATH_MAX];
         const char *hp = arm_exec_map_guest_path(gp, host, sizeof host);
         if (hp) {
            /* Multi-path ClassLoader entries are ':'-separated. */
            char *copy = strdup(hp);
            char *save = NULL;
            for (char *tok = copy ? strtok_r(copy, ":", &save) : NULL; tok;
                 tok = strtok_r(NULL, ":", &save)) {
               unsigned char m[4] = { 0 };
               FILE *f = luna_file_fopen(tok, "rb");
               if (f) { (void)!fread(m, 1, sizeof m, f); fclose(f); }
               if (m[0] == 'P' && m[1] == 'K') {
                  size_t len = 0;
                  unsigned char *d = arm_exec_zip_read(tok, "classes.dex", &len);
                  if (d) {
                     (void)dvm_add_dex_memory(vm, d, len, tok);
                     free(d);
                  }
               } else {
                  (void)dvm_add_dex(vm, tok);
               }
            }
            free(copy);
         }
      }
   }
   const char *library_path = nargs >= 3 && ARG(nargs - 2).l
                               ? dvm_string_utf8(vm, ARG(nargs - 2).l) : "";
   if (!dex_path_list_build(vm, self, first_dex, vm->ndexes, library_path)) return false;
   RETV();
}

static const struct rt_method rt_dex_class_loader[] = {
   M("<init>", "(Ljava/lang/String;Ljava/io/File;Ljava/lang/String;"
               "Ljava/lang/ClassLoader;)V", dexcl_init),
   M("<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
               "Ljava/lang/ClassLoader;)V", dexcl_init),
   M("<init>", "(Ljava/lang/String;Ljava/lang/ClassLoader;)V", dexcl_init),
   M("<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)V",
     dexcl_init),
   M("loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", cl_forName),
   M("getParent", "()Ljava/lang/ClassLoader;", clsl_get_parent),
   M("findLibrary", "(Ljava/lang/String;)Ljava/lang/String;", dexcl_find_library),
   M("getLdLibraryPath", "()Ljava/lang/String;", dexcl_library_path),
   M_END,
};

/* MPL-2.0. Application class-path objects backed by the VM's parsed DEXes.
 * Included after dvm_reflect.h in dvm_runtime.c. */
static dvm_ref loader_file(struct dvm *vm, const char *path)
{
   dvm_ref file = dvm_new_object(vm, dvm_find_class(vm, "java/io/File"));
   union dvm_value value = {.l = dvm_new_string(vm, path)};
   if (!file || !value.l || !dvm_set_field(vm, file, "path", "Ljava/lang/String;", value)) return 0;
   return file;
}

static bool loader_add_directory(struct dvm *vm, dvm_ref list, const char *path)
{
   union dvm_value value = {.l = loader_file(vm, path)}, ignored = {0};
   return value.l && list_add(vm, list, &value, 1, &ignored);
}

static bool dex_path_list_build(struct dvm *vm, dvm_ref loader,
                                int first, int end, const char *library_path)
{
   dvm_ref path_list = dvm_new_object(vm, dvm_find_class(vm, "dalvik/system/DexPathList"));
   if (!path_list) return false;
   (void)dvm_set_field(vm, loader, "pathList", "Ldalvik/system/DexPathList;",
                      (union dvm_value){.l = path_list});
   (void)dvm_set_field(vm, path_list, "definingContext", "Ljava/lang/ClassLoader;",
                      (union dvm_value){.l = loader});
   char **origins = calloc((size_t)(end - first + 1), sizeof *origins);
   if (!origins) return false;
   int count = 0;
   for (int i = first; i < end; ++i) {
      const char *name = vm->dexes[i]->file.path;
      if (!name) continue;
      const char *bang = strchr(name, '!');
      size_t length = bang ? (size_t)(bang - name) : strlen(name);
      bool found = false;
      for (int j = 0; j < count; ++j)
         if (strlen(origins[j]) == length && !memcmp(origins[j], name, length)) {found = true;break;}
      if (found) continue;
      origins[count] = luna_strndup(name, length);
      if (!origins[count]) goto failed;
      ++count;
   }
   union dvm_value elements = {.l = dvm_new_array(vm, 'L', "Ldalvik/system/DexPathList$Element;", count)};
   if (!elements.l) goto failed;
   (void)dvm_set_field(vm, path_list, "dexElements", "[Ldalvik/system/DexPathList$Element;", elements);
   for (int i = 0; i < count; ++i) {
      dvm_ref element = dvm_new_object(vm, dvm_find_class(vm, "dalvik/system/DexPathList$Element"));
      dvm_ref dexfile = dvm_new_object(vm, dvm_find_class(vm, "dalvik/system/DexFile"));
      dvm_ref classes = coll_new_list(vm, "Ljava/util/ArrayList;");
      dvm_ref file = loader_file(vm, origins[i]);
      if (!element || !dexfile || !classes || !file) goto failed;
      ((dvm_ref *)dvm_array_data(vm, elements.l))[i] = element;
      (void)dvm_set_field(vm, element, "path", "Ljava/io/File;", (union dvm_value){.l = file});
      (void)dvm_set_field(vm, element, "dexFile", "Ldalvik/system/DexFile;", (union dvm_value){.l = dexfile});
      (void)dvm_set_field(vm, dexfile, "path", "Ljava/lang/String;", (union dvm_value){.l = dvm_new_string(vm, origins[i])});
      (void)dvm_set_field(vm, dexfile, "classes", "Ljava/util/List;", (union dvm_value){.l = classes});
      size_t length = strlen(origins[i]);
      for (int j = first; j < end; ++j) {
         struct dex_file *dex = &vm->dexes[j]->file;
         if (!dex->path || strncmp(dex->path, origins[i], length) ||
             (dex->path[length] && dex->path[length] != '!')) continue;
         for (uint32_t k = 0; k < dex->class_defs_size; ++k) {
            struct dex_class_def def;
            if (!dex_class_def(dex, k, &def)) goto failed;
            const char *desc = dex_type(dex, def.class_idx);
            size_t n = desc ? strlen(desc) : 0;
            if (n < 2 || desc[0] != 'L' || desc[n-1] != ';') continue;
            char *name = luna_strndup(desc + 1, n - 2);
            if (!name) goto failed;
            for (char *p = name; *p; ++p) if (*p == '/') *p = '.';
            union dvm_value value = {.l = dvm_new_string(vm, name)}, ignored = {0};
            free(name);
            if (!value.l || !list_add(vm, classes, &value, 1, &ignored)) goto failed;
         }
      }
   }
   dvm_ref native = coll_new_list(vm, "Ljava/util/ArrayList;");
   dvm_ref system = coll_new_list(vm, "Ljava/util/ArrayList;");
   if (!native || !system) goto failed;
   (void)dvm_set_field(vm, path_list, "nativeLibraryDirectories", "Ljava/util/List;", (union dvm_value){.l = native});
   (void)dvm_set_field(vm, path_list, "systemNativeLibraryDirectories", "Ljava/util/List;", (union dvm_value){.l = system});
   char *paths = strdup(library_path ? library_path : ""), *save = NULL;
   if (!paths) goto failed;
   for (char *p = strtok_r(paths, ":", &save); p; p = strtok_r(NULL, ":", &save)) {
      if (!loader_add_directory(vm, native, p)) {free(paths);goto failed;}
   }
   free(paths);
   bool arm64 = strstr(lunaria_android_native_lib_path(), "arm64") != NULL;
   if (!loader_add_directory(vm, system, arm64 ? "/system/lib64" : "/system/lib") ||
       !loader_add_directory(vm, system, arm64 ? "/system_ext/lib64" : "/system_ext/lib")) goto failed;
   struct rt_list *app_dirs = list_of(vm, native), *system_dirs = list_of(vm, system);
   uint32_t n = app_dirs->size + system_dirs->size;
   union dvm_value native_elements = {.l = dvm_new_array(vm, 'L', "Ldalvik/system/DexPathList$NativeLibraryElement;", n)};
   if (!native_elements.l) goto failed;
   (void)dvm_set_field(vm, path_list, "nativeLibraryPathElements", "[Ldalvik/system/DexPathList$NativeLibraryElement;", native_elements);
   for (uint32_t i = 0; i < n; ++i) {
      dvm_ref element = dvm_new_object(vm, dvm_find_class(vm, "dalvik/system/DexPathList$NativeLibraryElement"));
      dvm_ref file = i < app_dirs->size ? app_dirs->items[i] : system_dirs->items[i - app_dirs->size];
      if (!element) goto failed;
      ((dvm_ref *)dvm_array_data(vm, native_elements.l))[i] = element;
      (void)dvm_set_field(vm, element, "path", "Ljava/io/File;", (union dvm_value){.l = file});
   }
   for (int i = 0; i < count; ++i) free(origins[i]);
   free(origins);
   return true;
failed:
   for (int i = 0; i < count; ++i) free(origins[i]);
   free(origins);
   if (!dvm_exception(vm)) dvm__throw(vm, "java/lang/OutOfMemoryError", "class path");
   return false;
}

static dvm_ref application_class_loader(struct dvm *vm)
{
   if (vm->application_class_loader) return vm->application_class_loader;
   dvm_ref loader = dvm_new_object(vm, dvm_find_class(vm, "dalvik/system/PathClassLoader"));
   if (!loader) return 0;
   dvm_pin(vm, loader);
   (void)dvm_set_field(vm, loader, "parent", "Ljava/lang/ClassLoader;",
                      (union dvm_value){.l = bootstrap_class_loader(vm)});
   (void)dvm_set_field(vm, loader, "dexPath", "Ljava/lang/String;",
                      (union dvm_value){.l = dvm_new_string(vm, lunaria_android_apk_path())});
   if (!dex_path_list_build(vm, loader, 0, vm->ndexes, lunaria_android_native_lib_path())) {
      dvm_unpin(vm, loader); return 0;
   }
   if(!legacy_http_attach(vm,loader)){dvm_unpin(vm,loader);return 0;}
   vm->application_class_loader = loader;
   return loader;
}

static bool dexcl_library_path(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)args;(void)nargs;
   union dvm_value path_list = {0}, directories = {0}, path = {0};
   (void)dvm_get_field(vm, self, "pathList", "Ldalvik/system/DexPathList;", &path_list);
   (void)dvm_get_field(vm, path_list.l, "nativeLibraryDirectories", "Ljava/util/List;", &directories);
   struct rt_list *list = list_of(vm, directories.l);
   size_t size = 1;
   for (uint32_t i = 0; list && i < list->size; ++i) {
      (void)dvm_get_field(vm, list->items[i], "path", "Ljava/lang/String;", &path);
      const char *s = dvm_string_utf8(vm, path.l);if(s)size += strlen(s) + 1;
   }
   char *text = calloc(size, 1);if(!text)return false;
   for (uint32_t i = 0; list && i < list->size; ++i) {
      (void)dvm_get_field(vm, list->items[i], "path", "Ljava/lang/String;", &path);
      const char *s = dvm_string_utf8(vm, path.l);if(!s)continue;
      if (*text) strcat(text, ":");strcat(text, s);
   }
   dvm_ref result = dvm_new_string(vm, text);free(text);RETL(result);
}

static bool dexcl_find_library(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = dvm_string_utf8(vm, ARG(0).l);
   if(!name) {dvm__throw(vm,"java/lang/NullPointerException","library name");return false;}
   if(strchr(name,'/'))RETL(0);
   union dvm_value path_list={0},elements={0},file={0},path={0};
   (void)dvm_get_field(vm,self,"pathList","Ldalvik/system/DexPathList;",&path_list);
   (void)dvm_get_field(vm,path_list.l,"nativeLibraryPathElements","[Ldalvik/system/DexPathList$NativeLibraryElement;",&elements);
   uint32_t count=dvm_array_length(vm,elements.l);
   for(uint32_t i=0;i<count;++i) {
      dvm_ref element=((dvm_ref *)dvm_array_data(vm,elements.l))[i];
      (void)dvm_get_field(vm,element,"path","Ljava/io/File;",&file);
      (void)dvm_get_field(vm,file.l,"path","Ljava/lang/String;",&path);
      const char *directory=dvm_string_utf8(vm,path.l);if(!directory)continue;
      char guest[DVM_PATH_MAX],host[DVM_PATH_MAX];
      int n=snprintf(guest,sizeof guest,"%s/lib%s.so",directory,name);
      if(n<0 || n>=(int)sizeof guest)continue;
      const char *mapped=arm_exec_map_guest_path(guest,host,sizeof host);
      struct stat st;
      if(mapped && !luna_file_stat_follow(mapped,&st) && S_ISREG(st.st_mode))RETL(dvm_new_string(vm,guest));
   }
   RETL(0);
}

static const struct rt_field rt_dex_path_list_fields[] = {
   {"definingContext","Ljava/lang/ClassLoader;"},
   {"dexElements","[Ldalvik/system/DexPathList$Element;"},
   {"nativeLibraryDirectories","Ljava/util/List;"},
   {"systemNativeLibraryDirectories","Ljava/util/List;"},
   {"nativeLibraryPathElements","[Ldalvik/system/DexPathList$NativeLibraryElement;"},F_END
};
static const struct rt_field rt_dex_element_fields[] = {
   {"path","Ljava/io/File;"},{"dexFile","Ldalvik/system/DexFile;"},F_END
};
static const struct rt_field rt_native_element_fields[] = {
   {"path","Ljava/io/File;"},{"zipDir","Ljava/lang/String;"},F_END
};
static const struct rt_method rt_dex_path_list[] = {M_END};

static bool map_keySet(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out);
static dvm_ref legacy_enumeration_new(struct dvm *vm,dvm_ref target,int kind);
/* MPL-2.0. Android 12 System.java and java.util.Properties contracts.
 * One VM owns one System.props object. Protected platform properties live in
 * its defaults, so getProperty sees them while ordinary Map views do not. */
static dvm_ref props_defaults(struct dvm *vm,dvm_ref self)
{
   union dvm_value value={0};dvm_get_field(vm,self,"defaults","Ljava/util/Properties;",&value);return value.l;
}
static bool props_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(!map_init(vm,self,NULL,0,out))return false;
   dvm_set_field(vm,self,"defaults","Ljava/util/Properties;",(union dvm_value){.l=nargs ? ARG(0).l : 0});RETV();
}
static bool props_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","key");return false;}
   union dvm_value value={0};if(!map_get(vm,self,args,1,&value))return false;
   struct dvm_object *object=dvm__obj(vm,value.l);
   if(object && object->kind==DVM_OBJ_STRING)RETL(value.l);
   dvm_ref defaults=props_defaults(vm,self);
   if(defaults){
      if(!am_vcall(vm,defaults,"getProperty","(Ljava/lang/String;)Ljava/lang/String;",args,1,&value))return false;
      if(value.l)RETL(value.l);
   }
   RETL(nargs>1 ? ARG(1).l : 0);
}
static bool props_put_virtual(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;return am_vcall(vm,self,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",args,2,out);
}
static bool props_putAll(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","map");return false;}
   union dvm_value set={0},iterator={0},entry={0},more={0},pair[2]={{0}},ignored={0};
   if(!am_vcall(vm,ARG(0).l,"entrySet","()Ljava/util/Set;",NULL,0,&set) ||
      !am_vcall(vm,set.l,"iterator","()Ljava/util/Iterator;",NULL,0,&iterator))return false;
   for(;;){
      if(!am_vcall(vm,iterator.l,"hasNext","()Z",NULL,0,&more))return false;
      if(!more.i)break;
      if(!am_vcall(vm,iterator.l,"next","()Ljava/lang/Object;",NULL,0,&entry) ||
         !am_vcall(vm,entry.l,"getKey","()Ljava/lang/Object;",NULL,0,&pair[0]) ||
         !am_vcall(vm,entry.l,"getValue","()Ljava/lang/Object;",NULL,0,&pair[1]) ||
         !props_put_virtual(vm,self,pair,2,&ignored))return false;
   }
   RETV();
}
static bool props_clone(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(!o_clone(vm,self,args,nargs,out))return false;dvm_ref copy=out->l;if(!copy)return false;
   struct dvm_object *source=dvm__obj(vm,self),*target=dvm__obj(vm,copy);
   struct rt_map *original=source ? source->data : NULL;
   if(original){
      size_t bytes=sizeof *original+(size_t)original->cap*sizeof original->e[0];
      struct rt_map *cloned=malloc(bytes);
      if(!cloned){dvm__throw(vm,"java/lang/OutOfMemoryError","Properties clone");return false;}
      memcpy(cloned,original,bytes);memset(cloned->views,0,sizeof cloned->views);cloned->mod_count=0;
      for(uint32_t i=0;i<cloned->size;++i){cloned->e[i].node=0;dvm_pin(vm,cloned->e[i].k);dvm_pin(vm,cloned->e[i].v);}
      target->data=cloned;target->data_destroy=map_destroy;
   }
   RETL(copy);
}
/* Snapshot defaults first, then own entries. Non-string own values do not
 * conceal a string in defaults when stringPropertyNames is requested. */
static dvm_ref props_name_snapshot(struct dvm *vm,dvm_ref self,bool strings_only)
{
   dvm_ref *chain=NULL;size_t count=0,capacity=0;
   for(dvm_ref p=self;p;p=props_defaults(vm,p)){
      for(size_t i=0;i<count;++i)if(chain[i]==p){free(chain);dvm__throw(vm,"java/lang/StackOverflowError","Cyclic Properties defaults");return 0;}
      if(count==capacity){
         size_t next=capacity ? capacity*2 : 8;dvm_ref *grown=realloc(chain,next*sizeof *chain);
         if(!grown){free(chain);dvm__throw(vm,"java/lang/OutOfMemoryError","Properties defaults");return 0;}
         chain=grown;capacity=next;
      }
      chain[count++]=p;
   }
   dvm_ref snapshot=dvm_new_object(vm,dvm_find_class(vm,"java/util/Hashtable"));if(!snapshot){free(chain);return 0;}
   dvm_pin(vm,snapshot);
   bool good=true;
   while(count && good){
      dvm_ref p=chain[--count];struct dvm_object *o=dvm__obj(vm,p);struct rt_map *map=o ? o->data : NULL;
      for(uint32_t i=0;map && i<map->size;++i){
         dvm_ref key=map->e[i].k,value=map->e[i].v;
         struct dvm_object *k=dvm__obj(vm,key),*v=dvm__obj(vm,value);
         if(!k || k->kind!=DVM_OBJ_STRING){
            if(strings_only)continue;
            dvm__throw(vm,"java/lang/ClassCastException","Property key is not a String");good=false;break;
         }
         if(strings_only && (!v || v->kind!=DVM_OBJ_STRING))continue;
         union dvm_value pair[2]={{.l=key},{.l=value}},ignored={0};
         if(!map_put(vm,snapshot,pair,2,&ignored)){good=false;break;}
         o=dvm__obj(vm,p);map=o ? o->data : NULL;
      }
   }
   free(chain);dvm_unpin(vm,snapshot);return good ? snapshot : 0;
}
static bool props_names(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;bool strings=!strcmp(builtin_name(),"stringPropertyNames");
   dvm_ref snapshot=props_name_snapshot(vm,self,strings);if(!snapshot)return false;
   if(strings)return map_keySet(vm,snapshot,NULL,0,out);
   RETL(legacy_enumeration_new(vm,snapshot,0));
}
static bool sys_property_key_present(struct dvm *vm,dvm_ref key)
{
   size_t length=0;return key && sdata(vm,key,&length) && length>0;
}
static bool sys_check_key(struct dvm *vm,dvm_ref key)
{
   if(!key){dvm__throw(vm,"java/lang/NullPointerException","key");return false;}
   if(!sys_property_key_present(vm,key)){dvm__throw(vm,"java/lang/IllegalArgumentException","key is empty");return false;}
   return true;
}
static bool sys_seed_pair(struct dvm *vm,dvm_ref properties,const char *key,const char *value)
{
   union dvm_value pair[2]={{.l=dvm_new_string(vm,key)},{.l=dvm_new_string(vm,value)}},ignored={0};
   return pair[0].l && pair[1].l && map_put(vm,properties,pair,2,&ignored);
}
static dvm_ref sys_readonly_properties(struct dvm *vm)
{
   struct dvm_class *system=dvm_find_class(vm,"java/lang/System");union dvm_value value={0};
   if(!system || !dvm_get_static(vm,system,"unchangeableProps","Ljava/util/Properties;",&value))return 0;
   if(value.l)return value.l;
   dvm_ref defaults=dvm_new_object(vm,dvm_find_class(vm,"java/util/Properties"));if(!defaults)return 0;
   dvm_pin(vm,defaults);bool good=true;
   for(size_t i=0;good && i<sizeof sys_props/sizeof *sys_props;++i){
      if(!strcmp(sys_props[i].k,"user.home"))continue;
      good=sys_seed_pair(vm,defaults,sys_props[i].k,sys_props[i].v);
   }
   const char *locale_keys[]={"user.language","user.region"};
   for(unsigned i=0;good && i<2;++i){char text[512];const char *v=sys_dynamic_prop(locale_keys[i],text,sizeof text);if(v)good=sys_seed_pair(vm,defaults,locale_keys[i],v);}
   if(good)good=dvm_set_static(vm,system,"unchangeableProps","Ljava/util/Properties;",(union dvm_value){.l=defaults});
   dvm_unpin(vm,defaults);return good ? defaults : 0;
}
static dvm_ref sys_new_properties(struct dvm *vm,bool initial,bool seed_changeable)
{
   dvm_ref defaults=sys_readonly_properties(vm);if(!defaults)return 0;
   dvm_ref properties=dvm_new_object(vm,dvm_find_class(vm,"java/lang/System$PropertiesWithNonOverrideableDefaults"));if(!properties)return 0;
   union dvm_value arg={.l=defaults},ignored={0};if(!props_init(vm,properties,&arg,1,&ignored))return 0;
   if(seed_changeable){
      char text[DVM_PATH_MAX];const char *tmp=initial ? sys_dynamic_prop("java.io.tmpdir",text,sizeof text) : "/tmp";
      if(!sys_seed_pair(vm,properties,"java.io.tmpdir",tmp ? tmp : "/tmp") || !sys_seed_pair(vm,properties,"user.home",""))return 0;
      if(initial){char ua[512];const char *agent=sys_dynamic_prop("http.agent",ua,sizeof ua);if(agent && !sys_seed_pair(vm,properties,"http.agent",agent))return 0;}
   }
   return properties;
}
static dvm_ref sys_properties(struct dvm *vm)
{
   struct dvm_class *system=dvm_find_class(vm,"java/lang/System");union dvm_value value={0};
   if(!system || !dvm_get_static(vm,system,"props","Ljava/util/Properties;",&value))return 0;
   if(!value.l){value.l=sys_new_properties(vm,true,true);if(!value.l || !dvm_set_static(vm,system,"props","Ljava/util/Properties;",value))return 0;}
   return value.l;
}
static bool sys_getProperties(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)args;(void)nargs;dvm_ref properties=sys_properties(vm);if(!properties)return false;RETL(properties);
}
static bool sys_getProperty(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;if(!sys_check_key(vm,ARG(0).l))return false;
   dvm_ref properties=sys_properties(vm);return properties && props_get(vm,properties,args,nargs,out);
}
static bool sys_setProperty(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;if(!sys_check_key(vm,ARG(0).l))return false;
   if(!ARG(1).l){dvm__throw(vm,"java/lang/NullPointerException","value");return false;}
   dvm_ref properties=sys_properties(vm);if(!properties || !props_put_virtual(vm,properties,args,nargs,out))return false;
   if(out->l){struct dvm_object *previous=dvm__obj(vm,out->l);if(previous && previous->kind!=DVM_OBJ_STRING){dvm__throw(vm,"java/lang/ClassCastException","Previous value is not a String");return false;}}
   return true;
}
static bool sys_clearProperty(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;if(!sys_check_key(vm,ARG(0).l))return false;
   dvm_ref properties=sys_properties(vm),protected=0;if(!properties || !system_properties_protected(vm,properties,ARG(0).l,&protected))return false;
   if(protected)RETL(0);
   if(!map_remove(vm,properties,args,1,out))return false;
   if(out->l){struct dvm_object *previous=dvm__obj(vm,out->l);if(previous && previous->kind!=DVM_OBJ_STRING){dvm__throw(vm,"java/lang/ClassCastException","Previous value is not a String");return false;}}
   return true;
}
static bool sys_setProperties(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;dvm_ref properties=sys_new_properties(vm,false,!ARG(0).l);if(!properties)return false;
   dvm_pin(vm,properties);union dvm_value ignored={0};
   bool good=!ARG(0).l || props_putAll(vm,properties,args,1,&ignored);
   if(good)good=dvm_set_static(vm,dvm_find_class(vm,"java/lang/System"),"props","Ljava/util/Properties;",(union dvm_value){.l=properties});
   dvm_unpin(vm,properties);if(!good)return false;RETV();
}
static bool sys_lineSeparator(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)args;(void)nargs;RETL(dvm_new_string(vm,"\n"));
}
static const struct rt_field rt_properties_fields[]={{"defaults","Ljava/util/Properties;"},F_END};
static const struct rt_method rt_properties[]={
   M("<init>","()V",props_init),M("<init>","(Ljava/util/Properties;)V",props_init),
   M("getProperty","(Ljava/lang/String;)Ljava/lang/String;",props_get),
   M("getProperty","(Ljava/lang/String;)Ljava/lang/Object;",props_get),
   M("getProperty","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",props_get),
   M("setProperty","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/Object;",props_put_virtual),
   M("putAll","(Ljava/util/Map;)V",props_putAll),M("clone","()Ljava/lang/Object;",props_clone),
   M("propertyNames","()Ljava/util/Enumeration;",props_names),M("stringPropertyNames","()Ljava/util/Set;",props_names),M_END
};
