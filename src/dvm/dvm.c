/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Dalvik interpreter proper: heap, class loading, resolution and the
 * instruction loop.  See dvm.h for why this exists.
 */

#include "dvm/dvm_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------------ *
 * Descriptors
 * ------------------------------------------------------------------------ */

char dvm__kind_of(const char *desc)
{
   if (!desc || !*desc) return 'V';
   switch (*desc) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
      case 'J': case 'F': case 'D': case 'V':
         return *desc;
      default:
         return 'L';   /* object or array */
   }
}

int dvm__slots_of(char kind)
{
   return (kind == 'J' || kind == 'D') ? 2 : (kind == 'V' ? 0 : 1);
}

/* Walks one type descriptor, returning the position just past it. */
static const char *desc_skip(const char *p)
{
   while (*p == '[') ++p;
   if (*p == 'L') {
      while (*p && *p != ';') ++p;
      if (*p == ';') ++p;
   } else if (*p) {
      ++p;
   }
   return p;
}

int dvm_sig_arg_slots(const char *sig)
{
   if (!sig || *sig != '(') return -1;
   int n = 0;
   for (const char *p = sig + 1; *p && *p != ')'; ) {
      const char *q = desc_skip(p);
      if (q == p) return -1;
      n += dvm__slots_of(dvm__kind_of(p));
      p = q;
   }
   return n;
}

int dvm_sig_arg_count(const char *sig)
{
   if (!sig || *sig != '(') return -1;
   int n = 0;
   for (const char *p = sig + 1; *p && *p != ')'; ) {
      const char *q = desc_skip(p);
      if (q == p) return -1;
      ++n;
      p = q;
   }
   return n;
}

char dvm_sig_return_kind(const char *sig)
{
   const char *p = sig ? strchr(sig, ')') : NULL;
   return p ? dvm__kind_of(p + 1) : 'V';
}

/* Parameter descriptor `idx` of a signature, into `buf`. */
bool dvm__sig_param(const char *sig, int idx, char *buf, size_t sz)
{
   if (!sig || *sig != '(') return false;
   const char *p = sig + 1;
   for (int i = 0; *p && *p != ')'; ++i) {
      const char *q = desc_skip(p);
      if (i == idx) {
         size_t l = (size_t)(q - p);
         if (l + 1 > sz) return false;
         memcpy(buf, p, l);
         buf[l] = '\0';
         return true;
      }
      p = q;
   }
   return false;
}

/* JNI form ("com/foo/Bar") into a descriptor ("Lcom/foo/Bar;"). */
static void name_to_desc(const char *name, char *buf, size_t sz)
{
   if (!name) { if (sz) buf[0] = '\0'; return; }
   if (name[0] == 'L' && name[strlen(name) - 1] == ';') {
      snprintf(buf, sz, "%s", name);
   } else if (name[0] == '[') {
      snprintf(buf, sz, "%s", name);
   } else if (name[1] == '\0' && strchr("ZBCSIJFDV", name[0])) {
      snprintf(buf, sz, "%s", name);
   } else {
      snprintf(buf, sz, "L%s;", name);
   }
   /* Java's dotted form shows up in Class.forName() and in strings the app
    * hands us; the dex always uses slashes. */
   for (char *p = buf; *p; ++p)
      if (*p == '.') *p = '/';
}

/* ------------------------------------------------------------------------ *
 * Heap
 * ------------------------------------------------------------------------ */

struct dvm_object *dvm__obj(struct dvm *vm, dvm_ref ref)
{
   if (!ref || ref > vm->heap_size) return NULL;
   struct dvm_object *o = &vm->heap[ref - 1];
   return o->live ? o : NULL;
}

static dvm_ref heap_alloc(struct dvm *vm)
{
   if (vm->free_head) {
      dvm_ref r = vm->free_head;
      vm->free_head = vm->heap[r - 1].next_free;
      memset(&vm->heap[r - 1], 0, sizeof vm->heap[r - 1]);
      vm->heap[r - 1].live = true;
      return r;
   }
   if (vm->heap_size == vm->heap_cap) {
      uint32_t cap = vm->heap_cap ? vm->heap_cap * 2u : 4096u;
      struct dvm_object *n = realloc(vm->heap, (size_t)cap * sizeof *n);
      if (!n) return 0;
      memset(n + vm->heap_cap, 0, (size_t)(cap - vm->heap_cap) * sizeof *n);
      vm->heap = n;
      vm->heap_cap = cap;
   }
   dvm_ref r = ++vm->heap_size;
   vm->heap[r - 1].live = true;
   return r;
}

void dvm_pin(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   if (o) ++o->pins;
}

void dvm_unpin(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   if (o && o->pins) --o->pins;
}

/* No collector.  The VM runs short bursts of app glue code (an activity
 * callback, a lifecycle hook) between returns to native code, and the heap is
 * freed with the VM.  A collector would have to trace the guest's JNI handle
 * table as well, which the stub layer does not model; getting that wrong
 * would free objects the game still holds.  Objects are reclaimed only where
 * the VM can prove it: string temporaries created and dropped inside one
 * builtin. */
static void heap_free(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   if (!o || o->pins) return;
   free(o->utf8);
   free(o->data);
   free(o->slots);
   memset(o, 0, sizeof *o);
   o->next_free = vm->free_head;
   vm->free_head = ref;
}

dvm_ref dvm_new_object(struct dvm *vm, struct dvm_class *cls)
{
   if (!cls) return 0;
   dvm_ref r = heap_alloc(vm);
   if (!r) return 0;
   struct dvm_object *o = &vm->heap[r - 1];
   o->cls = cls;
   o->kind = DVM_OBJ_PLAIN;
   if (cls->islots > 0) {
      o->slots = calloc((size_t)cls->islots, sizeof *o->slots);
      if (!o->slots) { heap_free(vm, r); return 0; }
   }
   return r;
}

dvm_ref dvm_new_string_n(struct dvm *vm, const char *utf8, size_t len)
{
   struct dvm_class *cls = dvm__class_by_desc(vm, "Ljava/lang/String;");
   dvm_ref r = heap_alloc(vm);
   if (!r) return 0;
   struct dvm_object *o = &vm->heap[r - 1];
   o->cls = cls;
   o->kind = DVM_OBJ_STRING;
   o->utf8 = malloc(len + 1);
   if (!o->utf8) { heap_free(vm, r); return 0; }
   if (len) memcpy(o->utf8, utf8, len);
   o->utf8[len] = '\0';
   o->utf8_len = (uint32_t)len;
   {
      const char *want = getenv("LUNARIA_DVM_STRWATCH");
      if (want && strstr(o->utf8, want))
         fprintf(stderr, "[dvm] new-string \"%.200s\" in %s.%s\n", o->utf8,
                 vm->cur_method && vm->cur_method->cls
                    ? vm->cur_method->cls->name : "?",
                 vm->cur_method ? vm->cur_method->name : "?");
   }
   return r;
}

dvm_ref dvm_new_string(struct dvm *vm, const char *utf8)
{
   return dvm_new_string_n(vm, utf8 ? utf8 : "", utf8 ? strlen(utf8) : 0);
}

static int elem_width(char kind)
{
   switch (kind) {
      case 'Z': case 'B': return 1;
      case 'C': case 'S': return 2;
      case 'J': case 'D': return 8;
      default: return 4;
   }
}

dvm_ref dvm_new_array(struct dvm *vm, char elem, const char *elem_desc, uint32_t length)
{
   char desc[256];
   snprintf(desc, sizeof desc, "[%s", elem_desc ? elem_desc : "I");
   struct dvm_class *cls = dvm__class_by_desc(vm, desc);

   dvm_ref r = heap_alloc(vm);
   if (!r) return 0;
   struct dvm_object *o = &vm->heap[r - 1];
   o->cls = cls;
   o->kind = DVM_OBJ_ARRAY;
   o->length = length;
   o->elem_kind = elem;
   size_t bytes = (size_t)length * (size_t)elem_width(elem);
   o->data = calloc(bytes ? bytes : 1, 1);
   if (!o->data) { heap_free(vm, r); return 0; }
   return r;
}

struct dvm_class *dvm_object_class(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   return o ? o->cls : NULL;
}

const char *dvm_string_utf8(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   return (o && o->kind == DVM_OBJ_STRING) ? o->utf8 : NULL;
}

uint32_t dvm_array_length(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   return (o && o->kind == DVM_OBJ_ARRAY) ? o->length : 0;
}

void *dvm_array_data(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   return (o && o->kind == DVM_OBJ_ARRAY) ? o->data : NULL;
}

dvm_ref dvm_wrap_external(struct dvm *vm, const char *class_name, uint32_t host_handle)
{
   char desc[512];
   name_to_desc(class_name, desc, sizeof desc);
   struct dvm_class *cls = dvm__class_by_desc(vm, desc);
   dvm_ref r = heap_alloc(vm);
   if (!r) return 0;
   struct dvm_object *o = &vm->heap[r - 1];
   o->cls = cls;
   o->kind = DVM_OBJ_EXTERNAL;
   o->host_handle = host_handle;
   if (cls && cls->islots > 0)
      o->slots = calloc((size_t)cls->islots, sizeof *o->slots);
   return r;
}

uint32_t dvm_external_handle(struct dvm *vm, dvm_ref ref)
{
   struct dvm_object *o = dvm__obj(vm, ref);
   return o ? o->host_handle : 0;
}

/* --- interning ---------------------------------------------------------- */

struct dvm_intern {
   char *key;
   dvm_ref ref;
};

dvm_ref dvm__intern(struct dvm *vm, const char *utf8)
{
   for (int i = 0; i < vm->ninterns; ++i)
      if (!strcmp(vm->interns[i].key, utf8))
         return vm->interns[i].ref;

   dvm_ref r = dvm_new_string(vm, utf8);
   if (!r) return 0;
   dvm_pin(vm, r);

   if (vm->ninterns == vm->interns_cap) {
      int cap = vm->interns_cap ? vm->interns_cap * 2 : 64;
      struct dvm_intern *n = realloc(vm->interns, (size_t)cap * sizeof *n);
      if (!n) return r;
      vm->interns = n;
      vm->interns_cap = cap;
   }
   vm->interns[vm->ninterns].key = strdup(utf8);
   vm->interns[vm->ninterns].ref = r;
   ++vm->ninterns;
   return r;
}

/* ------------------------------------------------------------------------ *
 * Class registry
 * ------------------------------------------------------------------------ */

static uint32_t str_hash(const char *s)
{
   uint32_t h = 2166136261u;
   for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
   return h;
}

static struct dvm_class *class_lookup(struct dvm *vm, const char *desc)
{
   uint32_t h = str_hash(desc) % DVM_CLASS_HASH;
   for (struct dvm_class *c = vm->class_hash[h]; c; c = c->hash_next)
      if (!strcmp(c->desc, desc)) return c;
   return NULL;
}

static struct dvm_class *class_register(struct dvm *vm, const char *desc)
{
   struct dvm_class *c = calloc(1, sizeof *c);
   if (!c) return NULL;
   c->desc = strdup(desc);
   if (!c->desc) { free(c); return NULL; }

   size_t n = strlen(desc);
   if (desc[0] == 'L' && n >= 2 && desc[n - 1] == ';') {
      c->name = malloc(n - 1);
      if (c->name) { memcpy(c->name, desc + 1, n - 2); c->name[n - 2] = '\0'; }
   } else {
      c->name = strdup(desc);
   }
   if (!c->name) { free(c->desc); free(c); return NULL; }

   uint32_t h = str_hash(desc) % DVM_CLASS_HASH;
   c->hash_next = vm->class_hash[h];
   vm->class_hash[h] = c;

   if (vm->nclasses == vm->classes_cap) {
      int cap = vm->classes_cap ? vm->classes_cap * 2 : 256;
      struct dvm_class **nn = realloc(vm->classes, (size_t)cap * sizeof *nn);
      if (nn) { vm->classes = nn; vm->classes_cap = cap; }
   }
   if (vm->nclasses < vm->classes_cap)
      vm->classes[vm->nclasses++] = c;
   return c;
}

/* dvm_runtime.c builds its classes through this so the registry (hash chain,
 * teardown list) stays owned by one file. */
struct dvm_class *dvm__register_builtin(struct dvm *vm, const char *desc)
{
   return class_register(vm, desc);
}

struct dvm_class *dvm__define_primitive(struct dvm *vm, const char *desc)
{
   struct dvm_class *c = class_lookup(vm, desc);
   if (c) return c;
   c = class_register(vm, desc);
   if (c) { c->is_primitive = true; c->init_state = 2; }
   return c;
}

static bool class_load_from_dex(struct dvm *vm, struct dvm_class *c,
                                struct dvm_dex *dd, uint32_t class_def_idx);

/* Finds or creates the class for a descriptor.  Never returns NULL for a
 * well-formed descriptor: an unknown class becomes an `external` placeholder
 * whose methods route to the host stub layer. */
struct dvm_class *dvm__class_by_desc(struct dvm *vm, const char *desc)
{
   if (!desc || !*desc) return NULL;

   struct dvm_class *c = class_lookup(vm, desc);
   if (c) return c;

   if (desc[0] == '[') {
      c = class_register(vm, desc);
      if (!c) return NULL;
      c->is_array = true;
      c->init_state = 2;
      c->elem_kind = dvm__kind_of(desc + 1);
      c->elem = dvm__class_by_desc(vm, desc + 1);
      c->super = dvm__class_by_desc(vm, "Ljava/lang/Object;");
      return c;
   }
   if (desc[1] == '\0' && strchr("ZBCSIJFDV", desc[0]))
      return dvm__define_primitive(vm, desc);

   /* A dex definition wins; then a built-in; then a host-backed stub. */
   for (int i = 0; i < vm->ndexes; ++i) {
      int idx = dex_find_class(&vm->dexes[i]->file, desc);
      if (idx < 0) continue;
      c = class_register(vm, desc);
      if (!c) return NULL;
      if (!class_load_from_dex(vm, c, vm->dexes[i], (uint32_t)idx)) {
         c->external = true;
         c->init_state = 2;
      }
      return c;
   }

   c = dvm_runtime_define(vm, desc);
   if (c) return c;

   c = class_register(vm, desc);
   if (!c) return NULL;
   c->external = true;
   c->init_state = 2;
   c->super = strcmp(desc, "Ljava/lang/Object;")
                ? dvm__class_by_desc(vm, "Ljava/lang/Object;") : NULL;
   return c;
}

struct dvm_class *dvm_find_class(struct dvm *vm, const char *name)
{
   char desc[512];
   name_to_desc(name, desc, sizeof desc);
   struct dvm_class *c = dvm__class_by_desc(vm, desc);
   return (c && !c->external) ? c : NULL;
}

bool dvm_class_is_known(struct dvm *vm, const char *name)
{
   char desc[512];
   name_to_desc(name, desc, sizeof desc);
   for (int i = 0; i < vm->ndexes; ++i)
      if (dex_find_class(&vm->dexes[i]->file, desc) >= 0) return true;
   struct dvm_class *c = class_lookup(vm, desc);
   return c && !c->external;
}

/* Does this class exist on the device we present?
 *
 * The app's own classes are in its dexes and the platform's are in the
 * framework; a name in neither is one the device does not have either.  That
 * distinction is not cosmetic: app code *probes* for optional classes and
 * expects the absent ones to say so.  AppsFlyer asks
 * Class.forName("com.miui.referrer.api.GetAppsReferrerClient") to find out
 * whether it is running on a Xiaomi device, and every name resolving to a
 * usable class told it yes — so it went on to use a vendor SDK that is not
 * there, and GameActivity.afStart died before AppsFlyer was ever started.
 *
 * Framework names get the benefit of the doubt whether or not the emulator
 * models them: on a device they are present, and the stub layer answers for
 * them. */
bool dvm_class_exists(struct dvm *vm, const char *name)
{
   static const char *const framework[] = {
      "java/", "javax/", "sun/", "jdk/", "libcore/", "dalvik/", "kotlin/",
      "android/", "com/android/", "org/json/", "org/w3c/", "org/xml/",
      "org/apache/http/", "org/xmlpull/",
   };
   if (!name || !*name) return false;
   char desc[512];
   name_to_desc(name, desc, sizeof desc);
   if (desc[0] == '[') return true;   /* array of anything */
   if (dvm_class_is_known(vm, desc)) return true;
   const char *n = desc[0] == 'L' ? desc + 1 : desc;
   for (size_t i = 0; i < sizeof framework / sizeof *framework; ++i)
      if (!strncmp(n, framework[i], strlen(framework[i]))) return true;
   return false;
}

const char *dvm_class_name(const struct dvm_class *c) { return c ? c->name : "?"; }
const char *dvm_class_super_name(const struct dvm_class *c)
{
   return (c && c->super) ? c->super->name : NULL;
}
const char *dvm_method_name(const struct dvm_method *m) { return m ? m->name : "?"; }
const char *dvm_method_sig(const struct dvm_method *m) { return m ? m->sig : "?"; }
bool dvm_method_is_static(const struct dvm_method *m)
{
   return m && (m->access & DEX_ACC_STATIC);
}

/* --- loading ------------------------------------------------------------ */

static void method_finish(struct dvm_method *m)
{
   m->arg_slots = dvm_sig_arg_slots(m->sig);
   if (m->arg_slots < 0) m->arg_slots = 0;
   m->arg_count = dvm_sig_arg_count(m->sig);
   if (m->arg_count < 0) m->arg_count = 0;
   m->ret_kind = dvm_sig_return_kind(m->sig);
}

static bool class_load_from_dex(struct dvm *vm, struct dvm_class *c,
                                struct dvm_dex *dd, uint32_t class_def_idx)
{
   struct dex_file *d = &dd->file;
   struct dex_class_def cd;
   if (!dex_class_def(d, class_def_idx, &cd)) return false;

   c->dex = d;
   c->class_def_idx = class_def_idx;
   c->access = cd.access_flags;
   c->is_interface = (cd.access_flags & DEX_ACC_INTERFACE) != 0;
   c->static_values_off = cd.static_values_off;

   if (cd.superclass_idx != DEX_NO_INDEX) {
      const char *sd = dex_type(d, cd.superclass_idx);
      if (sd && strcmp(sd, c->desc))
         c->super = dvm__class_by_desc(vm, sd);
   }

   uint16_t ifaces[64];
   int ni = dex_interfaces(d, cd.interfaces_off, ifaces, 64);
   if (ni > 64) ni = 64;
   if (ni > 0) {
      c->ifaces = calloc((size_t)ni, sizeof *c->ifaces);
      if (c->ifaces) {
         for (int i = 0; i < ni; ++i) {
            const char *t = dex_type(d, ifaces[i]);
            if (t) c->ifaces[c->nifaces++] = dvm__class_by_desc(vm, t);
         }
      }
   }

   struct dex_class_data data;
   if (!dex_class_data(d, cd.class_data_off, &data)) {
      /* A class_def with no class_data is legal (a marker interface).  It
       * still has a name, a super and interfaces, so it is fully usable. */
      c->islots = c->super ? c->super->islots : 0;
      return true;
   }

   /* Instance fields sit after the superclass's, so a subclass reference can
    * be read through a superclass field index. */
   int base = c->super ? c->super->islots : 0;
   c->nifields = (int)data.instance_fields_size;
   c->ifields = c->nifields ? calloc((size_t)c->nifields, sizeof *c->ifields) : NULL;
   int slot = base;
   for (int i = 0; i < c->nifields; ++i) {
      struct dex_field_id fid;
      if (!dex_field_id(d, data.instance_fields[i].field_idx, &fid)) continue;
      struct dvm_field *f = &c->ifields[i];
      f->cls = c;
      f->name = dex_string(d, fid.name_idx);
      f->type = dex_type(d, fid.type_idx);
      f->access = data.instance_fields[i].access_flags;
      f->kind = dvm__kind_of(f->type);
      f->width = 1;   /* one union slot holds a wide value */
      f->slot = (uint16_t)slot++;
   }
   c->islots = slot;

   c->nsfields = (int)data.static_fields_size;
   c->sfields = c->nsfields ? calloc((size_t)c->nsfields, sizeof *c->sfields) : NULL;
   for (int i = 0; i < c->nsfields; ++i) {
      struct dex_field_id fid;
      if (!dex_field_id(d, data.static_fields[i].field_idx, &fid)) continue;
      struct dvm_field *f = &c->sfields[i];
      f->cls = c;
      f->name = dex_string(d, fid.name_idx);
      f->type = dex_type(d, fid.type_idx);
      f->access = data.static_fields[i].access_flags;
      f->kind = dvm__kind_of(f->type);
      f->width = 1;
      f->slot = (uint16_t)i;
   }
   c->nsslots = c->nsfields;
   c->sslots = c->nsslots ? calloc((size_t)c->nsslots, sizeof *c->sslots) : NULL;

   int nm = (int)(data.direct_methods_size + data.virtual_methods_size);
   c->methods = nm ? calloc((size_t)nm, sizeof *c->methods) : NULL;
   c->nmethods = 0;
   for (int pass = 0; pass < 2; ++pass) {
      struct dex_encoded_method *list = pass ? data.virtual_methods : data.direct_methods;
      uint32_t n = pass ? data.virtual_methods_size : data.direct_methods_size;
      for (uint32_t i = 0; i < n && c->methods; ++i) {
         struct dex_method_id mid;
         if (!dex_method_id(d, list[i].method_idx, &mid)) continue;
         struct dvm_method *m = &c->methods[c->nmethods];
         m->cls = c;
         m->name = dex_string(d, mid.name_idx);
         m->shorty = dex_proto_shorty(d, mid.proto_idx);
         m->access = list[i].access_flags;
         m->dex = d;
         char sig[1024];
         if (!dex_proto_signature(d, mid.proto_idx, sig, sizeof sig)) continue;
         m->sig = strdup(sig);
         if (!m->sig) continue;
         if (list[i].code_off && dex_code(d, list[i].code_off, &m->code))
            m->has_code = true;
         method_finish(m);
         ++c->nmethods;
      }
   }

   dex_class_data_release(&data);
   return true;
}

/* ------------------------------------------------------------------------ *
 * Resolution
 * ------------------------------------------------------------------------ */

static struct dvm_method *class_own_method(struct dvm_class *c, const char *name,
                                           const char *sig)
{
   for (int i = 0; i < c->nmethods; ++i) {
      if (!c->methods[i].name || strcmp(c->methods[i].name, name)) continue;
      if (sig && c->methods[i].sig && strcmp(c->methods[i].sig, sig)) continue;
      return &c->methods[i];
   }
   return NULL;
}

struct dvm_method *dvm_find_method(struct dvm *vm, struct dvm_class *cls,
                                   const char *name, const char *sig)
{
   for (struct dvm_class *c = cls; c; c = c->super) {
      struct dvm_method *m = class_own_method(c, name, sig);
      if (m) return m;
   }
   /* Default methods live on the interface. */
   for (struct dvm_class *c = cls; c; c = c->super) {
      for (int i = 0; i < c->nifaces; ++i) {
         if (!c->ifaces[i]) continue;
         struct dvm_method *m = dvm_find_method(vm, c->ifaces[i], name, sig);
         if (m && m->has_code) return m;
      }
   }
   return NULL;
}

struct dvm_method *dvm_lookup(struct dvm *vm, const char *class_name,
                              const char *method, const char *sig)
{
   char desc[512];
   name_to_desc(class_name, desc, sizeof desc);
   struct dvm_class *c = dvm__class_by_desc(vm, desc);
   if (!c || c->external) return NULL;
   return dvm_find_method(vm, c, method, sig);
}

static struct dvm_field *class_find_field(struct dvm_class *cls, const char *name,
                                          const char *type, bool statics)
{
   for (struct dvm_class *c = cls; c; c = c->super) {
      struct dvm_field *list = statics ? c->sfields : c->ifields;
      int n = statics ? c->nsfields : c->nifields;
      for (int i = 0; i < n; ++i) {
         if (!list[i].name || strcmp(list[i].name, name)) continue;
         if (type && list[i].type && strcmp(list[i].type, type)) continue;
         return &list[i];
      }
      if (statics) {
         /* Interface constants. */
         for (int i = 0; i < c->nifaces; ++i) {
            if (!c->ifaces[i]) continue;
            struct dvm_field *f = class_find_field(c->ifaces[i], name, type, true);
            if (f) return f;
         }
      }
   }
   return NULL;
}

/* ------------------------------------------------------------------------ *
 * Exceptions
 * ------------------------------------------------------------------------ */

void dvm__throw(struct dvm *vm, const char *class_name, const char *fmt, ...)
{
   char msg[512];
   if (fmt) {
      va_list ap;
      va_start(ap, fmt);
      vsnprintf(msg, sizeof msg, fmt, ap);
      va_end(ap);
   } else {
      msg[0] = '\0';
   }

   char desc[256];
   name_to_desc(class_name, desc, sizeof desc);
   struct dvm_class *cls = dvm__class_by_desc(vm, desc);
   dvm_ref e = dvm_new_object(vm, cls);
   if (e) {
      union dvm_value v = { .l = msg[0] ? dvm_new_string(vm, msg) : 0 };
      (void)dvm_set_field(vm, e, "detailMessage", "Ljava/lang/String;", v);
      struct dvm_object *o = dvm__obj(vm, e);
      if (o && !o->utf8 && msg[0]) o->utf8 = strdup(msg);   /* always readable */
   }
   vm->exception = e;
   vm->exc_ref = e;
   vm->nexc_trace = 0;   /* a fresh throw starts a fresh unwind path */
   if (vm->cur_method) {
      vm->exc_trace[vm->nexc_trace++] = vm->cur_method;
      vm->exc_pc = vm->cur_pc;
   }
   if (vm->trace)
      fprintf(stderr, "[dvm] throw %s: %s\n", class_name, msg);
}

dvm_ref dvm_exception(struct dvm *vm) { return vm->exception; }
void dvm_clear_exception(struct dvm *vm) { vm->exception = 0; vm->parked = false; }

void dvm_describe_exception(struct dvm *vm, dvm_ref exc, char *buf, size_t sz)
{
   struct dvm_object *o = dvm__obj(vm, exc);
   if (!o) { snprintf(buf, sz, "(no exception)"); return; }
   const char *cn = o->cls ? o->cls->name : "java/lang/Throwable";
   union dvm_value msg = { 0 };
   const char *text = NULL;
   if (dvm_get_field(vm, exc, "detailMessage", "Ljava/lang/String;", &msg) && msg.l)
      text = dvm_string_utf8(vm, msg.l);
   if (!text) text = o->utf8;
   snprintf(buf, sz, "%s%s%s", cn, text ? ": " : "", text ? text : "");
}

/* ------------------------------------------------------------------------ *
 * Fields
 * ------------------------------------------------------------------------ */

bool dvm_get_field(struct dvm *vm, dvm_ref obj, const char *name,
                   const char *type, union dvm_value *out)
{
   struct dvm_object *o = dvm__obj(vm, obj);
   if (!o || !o->cls) return false;
   struct dvm_field *f = class_find_field(o->cls, name, type, false);
   if (!f || !o->slots || f->slot >= o->cls->islots) return false;
   *out = o->slots[f->slot];
   return true;
}

bool dvm_set_field(struct dvm *vm, dvm_ref obj, const char *name,
                   const char *type, union dvm_value val)
{
   struct dvm_object *o = dvm__obj(vm, obj);
   if (!o || !o->cls) return false;
   struct dvm_field *f = class_find_field(o->cls, name, type, false);
   if (!f || !o->slots || f->slot >= o->cls->islots) return false;
   o->slots[f->slot] = val;
   return true;
}

bool dvm_get_static(struct dvm *vm, struct dvm_class *cls, const char *name,
                    const char *type, union dvm_value *out)
{
   struct dvm_field *f = class_find_field(cls, name, type, true);
   if (!f || !f->cls->sslots || f->slot >= f->cls->nsslots) return false;
   dvm_init_class(vm, f->cls);
   *out = f->cls->sslots[f->slot];
   return true;
}

bool dvm_set_static(struct dvm *vm, struct dvm_class *cls, const char *name,
                    const char *type, union dvm_value val)
{
   struct dvm_field *f = class_find_field(cls, name, type, true);
   if (!f || !f->cls->sslots || f->slot >= f->cls->nsslots) return false;
   f->cls->sslots[f->slot] = val;
   return true;
}

/* ------------------------------------------------------------------------ *
 * Class initialisation
 * ------------------------------------------------------------------------ */

static bool invoke(struct dvm *vm, struct dvm_method *m, dvm_ref self,
                   const uint32_t *slots, int nslots, union dvm_value *out);

bool dvm_init_class(struct dvm *vm, struct dvm_class *cls)
{
   if (!cls) return true;
   /* A class whose <clinit> threw stays erroneous: every later use has to fail
    * too, because its static fields were never assigned.  Reporting success
    * once the first attempt is over hands out nulls that look like data. */
   if (cls->init_state == 3) {
      dvm__throw(vm, "java/lang/NoClassDefFoundError",
                 "%s failed to initialise", cls->name);
      return false;
   }
   if (cls->init_state) return true;
   cls->init_state = 1;

   if (cls->super) dvm_init_class(vm, cls->super);

   /* Constant static fields come from the encoded array, in declaration
    * order, before <clinit> runs. */
   if (cls->static_values_off && cls->dex && cls->nsslots) {
      struct dex_value *vals = calloc((size_t)cls->nsslots, sizeof *vals);
      if (vals) {
         int n = dex_static_values(cls->dex, cls->static_values_off, vals, cls->nsslots);
         if (n > cls->nsslots) n = cls->nsslots;
         for (int i = 0; i < n; ++i) {
            union dvm_value v = { 0 };
            switch (vals[i].type) {
               case DEX_VALUE_STRING: {
                  const char *s = dex_string(cls->dex, (uint32_t)vals[i].bits);
                  v.l = s ? dvm__intern(vm, s) : 0;
                  break;
               }
               case DEX_VALUE_TYPE: {
                  const char *t = dex_type(cls->dex, (uint32_t)vals[i].bits);
                  struct dvm_class *tc = t ? dvm__class_by_desc(vm, t) : NULL;
                  v.l = tc ? dvm_new_object(vm, dvm__class_by_desc(vm, "Ljava/lang/Class;")) : 0;
                  if (v.l) {
                     struct dvm_object *co = dvm__obj(vm, v.l);
                     co->kind = DVM_OBJ_CLASS;
                     co->klass = tc;
                  }
                  break;
               }
               case DEX_VALUE_NULL:
                  break;
               default:
                  v.ju = vals[i].bits;
                  break;
            }
            cls->sslots[i] = v;
         }
         free(vals);
      }
   }

   struct dvm_method *clinit = class_own_method(cls, "<clinit>", "()V");
   if (clinit && (clinit->has_code || clinit->builtin)) {
      if (vm->trace) fprintf(stderr, "[dvm] <clinit> %s\n", cls->name);
      union dvm_value ret;
      if (!invoke(vm, clinit, 0, NULL, 0, &ret)) {
         char why[512] = "";
         if (vm->exception)
            dvm_describe_exception(vm, vm->exception, why, sizeof why);
         fprintf(stderr, "[dvm] <clinit> %s failed: %s\n", cls->name,
                 why[0] ? why : "no exception");
         cls->init_state = 3;   /* erroneous: never retried, never usable */
         return false;
      }
   }
   cls->init_state = 2;
   return true;
}

/* ------------------------------------------------------------------------ *
 * Argument marshalling
 * ------------------------------------------------------------------------ */

/* Register slots (Dalvik layout) into one union per declared parameter. */
static int slots_to_values(const char *sig, const uint32_t *slots, int nslots,
                           union dvm_value *out, int max)
{
   int n = 0, s = 0;
   if (!sig || *sig != '(') return 0;
   for (const char *p = sig + 1; *p && *p != ')'; ) {
      const char *q = desc_skip(p);
      char k = dvm__kind_of(p);
      if (n >= max) break;
      if (k == 'J' || k == 'D') {
         uint64_t lo = (s < nslots) ? slots[s] : 0;
         uint64_t hi = (s + 1 < nslots) ? slots[s + 1] : 0;
         out[n].ju = lo | (hi << 32);
         s += 2;
      } else {
         out[n].u = (s < nslots) ? slots[s] : 0;
         ++s;
      }
      ++n;
      p = q;
   }
   return n;
}

static int values_to_slots(const char *sig, const union dvm_value *vals, int nvals,
                           uint32_t *out, int max)
{
   int s = 0, n = 0;
   if (!sig || *sig != '(') return 0;
   for (const char *p = sig + 1; *p && *p != ')'; ) {
      const char *q = desc_skip(p);
      char k = dvm__kind_of(p);
      union dvm_value v = (n < nvals) ? vals[n] : (union dvm_value){ 0 };
      if (k == 'J' || k == 'D') {
         if (s >= max - 1) break;
         out[s++] = (uint32_t)v.ju;
         out[s++] = (uint32_t)(v.ju >> 32);
      } else {
         if (s >= max) break;
         out[s++] = v.u;
      }
      ++n;
      p = q;
   }
   return s;
}

/* ------------------------------------------------------------------------ *
 * The interpreter
 * ------------------------------------------------------------------------ */

#define A_HI(u) ((uint8_t)((u) >> 12))
#define OPCODE(u) ((uint8_t)((u) & 0xff))
#define AA(u) ((uint8_t)((u) >> 8))
#define A4(u) ((uint8_t)(((u) >> 8) & 0x0f))
#define B4(u) ((uint8_t)(((u) >> 12) & 0x0f))

struct frame {
   struct dvm_method *m;
   uint32_t *regs;
   uint32_t nregs;
   uint32_t pc;
   uint64_t result;   /* the pseudo-register move-result reads */
};

static inline uint64_t rw(const uint32_t *r, int i)
{
   return (uint64_t)r[i] | ((uint64_t)r[i + 1] << 32);
}

static inline void sw(uint32_t *r, int i, uint64_t v)
{
   r[i] = (uint32_t)v;
   r[i + 1] = (uint32_t)(v >> 32);
}

static inline float bits_to_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f_to_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline double bits_to_d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t d_to_bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static bool class_assignable(struct dvm *vm, struct dvm_class *from,
                             struct dvm_class *to);

static bool iface_assignable(struct dvm *vm, struct dvm_class *from,
                             struct dvm_class *to)
{
   for (struct dvm_class *c = from; c; c = c->super)
      for (int i = 0; i < c->nifaces; ++i)
         if (c->ifaces[i] && class_assignable(vm, c->ifaces[i], to))
            return true;
   return false;
}

bool dvm__class_assignable(struct dvm *vm, struct dvm_class *from,
                           struct dvm_class *to)
{
   return class_assignable(vm, from, to);
}

static bool class_assignable(struct dvm *vm, struct dvm_class *from,
                             struct dvm_class *to)
{
   if (!from || !to) return true;   /* unknown: do not block the app */
   if (!strcmp(to->desc, "Ljava/lang/Object;")) return true;
   for (struct dvm_class *c = from; c; c = c->super)
      if (c == to || !strcmp(c->desc, to->desc)) return true;
   if (iface_assignable(vm, from, to)) return true;
   if (from->is_array && to->is_array)
      return class_assignable(vm, from->elem, to->elem);
   /* Unknown hierarchy is not evidence of assignability.  Returning true for
    * every external class makes `x instanceof InternalSentinel` succeed for
    * arbitrary objects; lock-free queues then mistake real tasks for their
    * private marker objects and spin forever.  Exact classes and every known
    * superclass/interface were handled above, so the only sound answer left
    * is false. */
   return false;
}

/* Resolves the target of an invoke-virtual / -interface at the receiver's
 * actual class. */
static struct dvm_method *virtual_target(struct dvm *vm, dvm_ref self,
                                         struct dvm_method *decl)
{
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o || !o->cls || o->cls == decl->cls) return decl;
   struct dvm_method *m = dvm_find_method(vm, o->cls, decl->name, decl->sig);
   return m ? m : decl;
}

/* --- external / native dispatch ----------------------------------------- */

static void note_missing(struct dvm *vm, const char *what)
{
   for (int i = 0; i < vm->nmissing; ++i)
      if (!strcmp(vm->missing[i], what)) return;
   if (vm->nmissing == vm->missing_cap) {
      int cap = vm->missing_cap ? vm->missing_cap * 2 : 64;
      char **n = realloc(vm->missing, (size_t)cap * sizeof *n);
      if (!n) return;
      vm->missing = n;
      vm->missing_cap = cap;
   }
   vm->missing[vm->nmissing++] = strdup(what);
   fprintf(stderr, "[dvm] unresolved: %s\n", what);
}

bool dvm__call_out(struct dvm *vm, struct dvm_class *cls, const char *name,
                   const char *sig, bool is_static, bool is_native,
                   dvm_ref self, const uint32_t *slots, int nslots,
                   union dvm_value *out)
{
   union dvm_value args[64];
   int nargs = slots_to_values(sig, slots, nslots, args, 64);
   memset(out, 0, sizeof *out);

   if (getenv("LUNARIA_DVM_OUTTRACE"))
      fprintf(stderr, "[out] %s.%s%s%s\n", cls->name, name, sig,
              is_native ? " (native)" : "");

   if (is_native && vm->hooks.call_native &&
       vm->hooks.call_native(vm->hooks.user, vm, cls->name, name, sig,
                             is_static, self, args, nargs, out))
      return true;

   if (vm->hooks.call_external &&
       vm->hooks.call_external(vm->hooks.user, vm, cls->name, name, sig,
                               self, args, nargs, out))
      return true;

   char what[512];
   snprintf(what, sizeof what, "%s.%s%s", cls->name, name, sig);
   note_missing(vm, what);
   /* Returning zero is what the stub layer did before this module existed.
    * Throwing here would abort app code that only wanted a no-op logger. */
   return true;
}

/* --- the loop ----------------------------------------------------------- */

/* Finds the catch handler for `pc`, or -1.  Sets *out_pc to the handler
 * address on success. */
static int find_handler(struct dvm *vm, struct dvm_method *m, uint32_t pc,
                        struct dvm_class *exc_cls, uint32_t *out_pc)
{
   const struct dex_code *code = &m->code;
   if (!code->tries_size || !code->tries || !m->dex) return -1;

   for (uint32_t i = 0; i < code->tries_size; ++i) {
      const uint8_t *t = code->tries + (size_t)i * 8u;
      uint32_t start, count16;
      memcpy(&start, t, 4);
      uint16_t cnt, hoff;
      memcpy(&cnt, t + 4, 2);
      memcpy(&hoff, t + 6, 2);
      count16 = cnt;
      if (pc < start || pc >= start + count16) continue;

      size_t p = (size_t)(code->handlers - m->dex->p) + hoff;
      int32_t size;
      p = dex_sleb(m->dex, p, &size);
      int npairs = size < 0 ? -size : size;
      for (int k = 0; k < npairs; ++k) {
         uint32_t type_idx, addr;
         p = dex_uleb(m->dex, p, &type_idx);
         p = dex_uleb(m->dex, p, &addr);
         const char *td = dex_type(m->dex, type_idx);
         struct dvm_class *tc = td ? dvm__class_by_desc(vm, td) : NULL;
         if (tc && class_assignable(vm, exc_cls, tc)) {
            *out_pc = addr;
            return 0;
         }
      }
      if (size <= 0) {
         uint32_t all;
         p = dex_uleb(m->dex, p, &all);
         *out_pc = all;
         return 0;
      }
   }
   return -1;
}

/* A cooperative park is an interpreter unwind token, not a Java
 * InterruptedException.  It must bypass typed catch clauses, but it still has
 * to execute encoded catch-all handlers: javac/d8 use those for finally, and
 * skipping them leaves executor locks/active-task slots permanently held. */
static int find_catchall_handler(struct dvm_method *m, uint32_t pc,
                                 uint32_t *out_pc)
{
   const struct dex_code *code = &m->code;
   if (!code->tries_size || !code->tries || !m->dex) return -1;
   for (uint32_t i = 0; i < code->tries_size; ++i) {
      const uint8_t *t = code->tries + (size_t)i * 8u;
      uint32_t start;
      uint16_t count, hoff;
      memcpy(&start, t, 4);
      memcpy(&count, t + 4, 2);
      memcpy(&hoff, t + 6, 2);
      if (pc < start || pc >= start + count) continue;
      size_t p = (size_t)(code->handlers - m->dex->p) + hoff;
      int32_t size;
      p = dex_sleb(m->dex, p, &size);
      int npairs = size < 0 ? -size : size;
      for (int k = 0; k < npairs; ++k) {
         uint32_t ignored;
         p = dex_uleb(m->dex, p, &ignored);
         p = dex_uleb(m->dex, p, &ignored);
      }
      if (size <= 0) {
         p = dex_uleb(m->dex, p, out_pc);
         (void)p;
         return 0;
      }
   }
   return -1;
}

/* Reads the 35c/3rc argument registers into `slots`. */
static int gather_args(const uint16_t *insns, uint32_t nins, uint32_t pc, bool range,
                       uint32_t *regs, uint32_t nregs, uint32_t *slots)
{
   uint16_t u0 = insns[pc];
   uint16_t cccc = (pc + 2 < nins) ? insns[pc + 2] : 0;
   if (range) {
      int n = AA(u0);
      uint16_t first = cccc;
      for (int i = 0; i < n; ++i)
         slots[i] = (uint32_t)(first + i) < nregs ? regs[first + i] : 0;
      return n;
   }
   int n = B4(u0);
   uint16_t g = A4(u0);
   uint16_t fedc = cccc;
   uint16_t idx[5] = {
      (uint16_t)(fedc & 0xf), (uint16_t)((fedc >> 4) & 0xf),
      (uint16_t)((fedc >> 8) & 0xf), (uint16_t)((fedc >> 12) & 0xf), g
   };
   if (n > 5) n = 5;
   for (int i = 0; i < n; ++i)
      slots[i] = idx[i] < nregs ? regs[idx[i]] : 0;
   return n;
}

static struct dvm_class *resolve_type(struct dvm *vm, struct dvm_dex *dd, uint32_t idx)
{
   if (dd->type_cache && idx < dd->file.type_ids_size && dd->type_cache[idx])
      return dd->type_cache[idx];
   const char *t = dex_type(&dd->file, idx);
   if (!t) return NULL;
   struct dvm_class *c = dvm__class_by_desc(vm, t);
   if (dd->type_cache && idx < dd->file.type_ids_size)
      dd->type_cache[idx] = c;
   return c;
}

static struct dvm_dex *dex_of(struct dvm *vm, struct dex_file *f)
{
   for (int i = 0; i < vm->ndexes; ++i)
      if (&vm->dexes[i]->file == f) return vm->dexes[i];
   return NULL;
}

static struct dvm_method *resolve_method(struct dvm *vm, struct dvm_dex *dd,
                                         uint32_t idx, struct dvm_class **out_cls,
                                         const char **out_name, char *sigbuf,
                                         size_t sigsz)
{
   struct dex_method_id mid;
   if (!dex_method_id(&dd->file, idx, &mid)) return NULL;
   const char *cd = dex_type(&dd->file, mid.class_idx);
   const char *name = dex_string(&dd->file, mid.name_idx);
   if (!cd || !name) return NULL;
   if (!dex_proto_signature(&dd->file, mid.proto_idx, sigbuf, sigsz)) return NULL;

   struct dvm_class *cls = dvm__class_by_desc(vm, cd);
   *out_cls = cls;
   *out_name = name;
   if (!cls) return NULL;

   if (dd->method_cache && idx < dd->file.method_ids_size && dd->method_cache[idx])
      return dd->method_cache[idx];

   struct dvm_method *m = dvm_find_method(vm, cls, name, sigbuf);
   if (m && dd->method_cache && idx < dd->file.method_ids_size)
      dd->method_cache[idx] = m;
   return m;
}

static struct dvm_field *resolve_field(struct dvm *vm, struct dvm_dex *dd,
                                       uint32_t idx, bool statics,
                                       struct dvm_class **out_cls,
                                       const char **out_name, const char **out_type)
{
   if (dd->field_cache && idx < dd->file.field_ids_size && dd->field_cache[idx]) {
      struct dvm_field *f = dd->field_cache[idx];
      *out_cls = f->cls;
      *out_name = f->name;
      *out_type = f->type;
      return f;
   }
   struct dex_field_id fid;
   if (!dex_field_id(&dd->file, idx, &fid)) return NULL;
   const char *cd = dex_type(&dd->file, fid.class_idx);
   *out_name = dex_string(&dd->file, fid.name_idx);
   *out_type = dex_type(&dd->file, fid.type_idx);
   if (!cd || !*out_name) return NULL;
   struct dvm_class *cls = dvm__class_by_desc(vm, cd);
   *out_cls = cls;
   if (!cls) return NULL;
   struct dvm_field *f = class_find_field(cls, *out_name, *out_type, statics);
   if (f && dd->field_cache && idx < dd->file.field_ids_size)
      dd->field_cache[idx] = f;
   return f;
}

static int dex_index(struct dvm *vm, struct dvm_dex *dd)
{
   for (int i = 0; i < vm->ndexes; ++i)
      if (vm->dexes[i] == dd) return i;
   return -1;
}

static dvm_ref const_string(struct dvm *vm, struct dvm_dex *dd, uint32_t idx)
{
   if (dd->string_cache && idx < dd->file.string_ids_size && dd->string_cache[idx])
      return dd->string_cache[idx];
   const char *s = dex_string(&dd->file, idx);
   {
      const char *want = getenv("LUNARIA_DVM_STRWATCH");
      if (want && s && strstr(s, want))
         fprintf(stderr, "[dvm] const-string[%u] = \"%s\" in %s.%s (dex %d)\n",
                 idx, s,
                 vm->cur_method && vm->cur_method->cls
                    ? vm->cur_method->cls->name : "?",
                 vm->cur_method ? vm->cur_method->name : "?",
                 dex_index(vm, dd));
   }
   dvm_ref r = dvm__intern(vm, s ? s : "");
   if (dd->string_cache && idx < dd->file.string_ids_size)
      dd->string_cache[idx] = r;
   return r;
}

static dvm_ref class_object(struct dvm *vm, struct dvm_class *cls)
{
   if (!cls) return 0;
   if (cls->class_object) return cls->class_object;
   dvm_ref r = dvm_new_object(vm, dvm__class_by_desc(vm, "Ljava/lang/Class;"));
   struct dvm_object *o = dvm__obj(vm, r);
   if (o) {
      o->kind = DVM_OBJ_CLASS;
      o->klass = cls;
      ++o->pins;
   }
   cls->class_object = r;
   return r;
}

/* Array element access shared by aget/aput. */
static bool array_check(struct dvm *vm, struct dvm_object *a, uint32_t idx)
{
   if (!a || a->kind != DVM_OBJ_ARRAY) {
      dvm__throw(vm, "java/lang/NullPointerException", "array is null");
      return false;
   }
   if (idx >= a->length) {
      dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException",
                 "length=%u index=%u", a->length, idx);
      return false;
   }
   return true;
}

static bool execute(struct dvm *vm, struct frame *fr, union dvm_value *out);

static bool invoke(struct dvm *vm, struct dvm_method *m, dvm_ref self,
                   const uint32_t *slots, int nslots, union dvm_value *out)
{
   memset(out, 0, sizeof *out);
   if (!m) return true;

   if (vm->depth >= DVM_MAX_FRAMES) {
      dvm__throw(vm, "java/lang/StackOverflowError", "%d frames", vm->depth);
      return false;
   }

   bool is_static = (m->access & DEX_ACC_STATIC) != 0;
   if (!is_static && !self && !(m->access & DEX_ACC_NATIVE)) {
      /* "self" marks the callee-side check, so a null receiver can be told
       * apart from the call-site one below. */
      dvm__throw(vm, "java/lang/NullPointerException", "self %s.%s%s",
                 m->cls->name, m->name, m->sig ? m->sig : "");
      return false;
   }

   if (is_static && m->cls) dvm_init_class(vm, m->cls);

   const char *trace_class = getenv("LUNARIA_DVM_TRACE_CLASS");
   bool trace_this = !trace_class ||
      (m->cls && m->cls->name && strstr(m->cls->name, trace_class));
   if (vm->trace && trace_this)
      fprintf(stderr, "[dvm] %*scall %s.%s%s\n", vm->depth * 2, "",
              m->cls ? m->cls->name : "?", m->name, m->sig ? m->sig : "");

   if (m->builtin) {
      union dvm_value args[64];
      int nargs = slots_to_values(m->sig, slots, nslots, args, 64);
      ++vm->depth;
      bool ok = m->builtin(vm, self, args, nargs, out);
      --vm->depth;
      return ok && !vm->exception;
   }

   if (!m->has_code) {
      bool native = (m->access & DEX_ACC_NATIVE) != 0;
      ++vm->depth;
      bool ok = dvm__call_out(vm, m->cls, m->name, m->sig, is_static, native,
                         self, slots, nslots, out);
      --vm->depth;
      return ok && !vm->exception;
   }

   uint16_t nregs = m->code.registers_size;
   uint16_t nins = m->code.ins_size;
   if (nregs < nins) nregs = nins;

   uint32_t stackbuf[64];
   uint32_t *regs = nregs <= 64 ? stackbuf : calloc(nregs ? nregs : 1, sizeof *regs);
   if (!regs) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "%u registers", nregs);
      return false;
   }
   memset(regs, 0, (size_t)nregs * sizeof *regs);

   /* Parameters occupy the last ins_size registers, `this` first. */
   int base = (int)nregs - (int)nins;
   int w = base;
   if (!is_static && w < nregs) regs[w++] = self;
   for (int i = 0; i < nslots && w < nregs; ++i)
      regs[w++] = slots[i];

   struct frame fr = { .m = m, .regs = regs, .nregs = nregs, .pc = 0 };
   ++vm->depth;
   bool ok = execute(vm, &fr, out);
   --vm->depth;

   /* Record the frames the exception passes on its way out, innermost first. */
   if (!ok && vm->exception && m != vm->exc_trace[0] &&
       vm->nexc_trace < (int)(sizeof vm->exc_trace / sizeof vm->exc_trace[0]))
      vm->exc_trace[vm->nexc_trace++] = m;

   if (regs != stackbuf) free(regs);
   return ok;
}

static bool execute(struct dvm *vm, struct frame *fr, union dvm_value *out)
{
   struct dvm_method *m = fr->m;
   const char *trace_class = getenv("LUNARIA_DVM_TRACE_CLASS");
   bool trace_this = !trace_class ||
      (m->cls && m->cls->name && strstr(m->cls->name, trace_class));
   const uint16_t *insns = m->code.insns;
   uint32_t nins = m->code.insns_size;
   uint32_t nregs = fr->nregs;
   uint32_t *r = fr->regs;
   struct dvm_dex *dd = dex_of(vm, m->dex);
   if (m->dex && !dd) {
      /* Every constant-pool index in this method is meaningless without its
       * own dex; resolving against another one silently returns the wrong
       * class and method.  Fail loudly instead. */
      static bool warned;
      if (!warned) {
         warned = true;
         fprintf(stderr, "[dvm] %s.%s: dex not registered — constant pool "
                 "cannot be resolved\n", m->cls ? m->cls->name : "?", m->name);
      }
   }
   char sigbuf[1024];

   memset(out, 0, sizeof *out);

/* Operand fetch.  A truncated method would otherwise read past the end of the
 * instruction array; the dex is third-party input, so read zero instead. */
#define IU(k) ((pc + (uint32_t)(k) < nins) ? insns[pc + (uint32_t)(k)] : (uint16_t)0)
#define REQ(reg) do { if ((uint32_t)(reg) >= nregs) { \
      dvm__throw(vm, "java/lang/VerifyError", "%s.%s register %u of %u", \
                 m->cls->name, m->name, (unsigned)(reg), nregs); \
      goto exception; } } while (0)

   for (;;) {
      if (fr->pc >= nins) {
         dvm__throw(vm, "java/lang/VerifyError", "%s.%s ran past the end",
                    m->cls->name, m->name);
         goto exception;
      }
      ++vm->steps;
      if (vm->step_limit && ++vm->call_steps > vm->step_limit) {
         fprintf(stderr, "[dvm] step limit reached in %s.%s — aborting the call\n",
                 m->cls->name, m->name);
         dvm__throw(vm, "java/lang/Error", "dvm step limit");
         goto exception;
      }

      uint32_t pc = fr->pc;
      uint16_t u0 = insns[pc];
      uint8_t op = OPCODE(u0);
      vm->cur_method = m;
      vm->cur_pc = pc;

      if (vm->trace >= 2 && trace_this)
         fprintf(stderr, "[dvm]   %s.%s @%04x op=%02x words=%04x,%04x,%04x\n",
                 m->cls->name, m->name, pc, op, u0, IU(1), IU(2));

      switch (op) {
      case 0x00: /* nop, and the payload pseudo-ops when reached by accident */
         fr->pc += 1;
         break;

      /* --- moves --- */
      case 0x01: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = r[B4(u0)]; fr->pc += 1; break;
      case 0x07: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = r[B4(u0)]; fr->pc += 1; break;
      case 0x02: case 0x08:
         REQ(AA(u0)); REQ(IU(1));
         r[AA(u0)] = r[IU(1)];
         fr->pc += 2;
         break;
      case 0x03: case 0x09:
         REQ(IU(1)); REQ(IU(2));
         r[IU(1)] = r[IU(2)];
         fr->pc += 3;
         break;
      case 0x04:
         REQ(A4(u0) + 1); REQ(B4(u0) + 1);
         sw(r, A4(u0), rw(r, B4(u0)));
         fr->pc += 1;
         break;
      case 0x05:
         REQ(AA(u0) + 1); REQ(IU(1) + 1);
         sw(r, AA(u0), rw(r, IU(1)));
         fr->pc += 2;
         break;
      case 0x06:
         REQ(IU(1) + 1); REQ(IU(2) + 1);
         sw(r, IU(1), rw(r, IU(2)));
         fr->pc += 3;
         break;

      case 0x0a: /* move-result */
      case 0x0c: /* move-result-object */
         REQ(AA(u0));
         r[AA(u0)] = (uint32_t)fr->result;
         fr->pc += 1;
         break;
      case 0x0b: /* move-result-wide */
         REQ(AA(u0) + 1);
         sw(r, AA(u0), fr->result);
         fr->pc += 1;
         break;
      case 0x0d: /* move-exception */
         REQ(AA(u0));
         /* The handler entry already cleared the pending exception; the
          * object itself is kept in exc_ref for exactly this instruction. */
         r[AA(u0)] = vm->exception ? vm->exception : vm->exc_ref;
         vm->exception = 0;
         fr->pc += 1;
         break;

      /* --- returns --- */
      case 0x0e:
         return true;
      case 0x0f: case 0x11:
         REQ(AA(u0));
         out->u = r[AA(u0)];
         return true;
      case 0x10:
         REQ(AA(u0) + 1);
         out->ju = rw(r, AA(u0));
         return true;

      /* --- constants --- */
      case 0x12: { /* const/4 */
         REQ(A4(u0));
         int32_t v = (int32_t)(int8_t)(uint8_t)(B4(u0) << 4) >> 4;
         r[A4(u0)] = (uint32_t)v;
         fr->pc += 1;
         break;
      }
      case 0x13: /* const/16 */
         REQ(AA(u0));
         r[AA(u0)] = (uint32_t)(int32_t)(int16_t)IU(1);
         fr->pc += 2;
         break;
      case 0x14: /* const */
         REQ(AA(u0));
         r[AA(u0)] = (uint32_t)IU(1) | ((uint32_t)IU(2) << 16);
         fr->pc += 3;
         break;
      case 0x15: /* const/high16 */
         REQ(AA(u0));
         r[AA(u0)] = (uint32_t)IU(1) << 16;
         fr->pc += 2;
         break;
      case 0x16: /* const-wide/16 */
         REQ(AA(u0) + 1);
         sw(r, AA(u0), (uint64_t)(int64_t)(int16_t)IU(1));
         fr->pc += 2;
         break;
      case 0x17: /* const-wide/32 */
         REQ(AA(u0) + 1);
         sw(r, AA(u0), (uint64_t)(int64_t)(int32_t)
            ((uint32_t)IU(1) | ((uint32_t)IU(2) << 16)));
         fr->pc += 3;
         break;
      case 0x18: { /* const-wide */
         REQ(AA(u0) + 1);
         uint64_t v = 0;
         for (int i = 0; i < 4; ++i) v |= (uint64_t)IU(1 + i) << (16 * i);
         sw(r, AA(u0), v);
         fr->pc += 5;
         break;
      }
      case 0x19: /* const-wide/high16 */
         REQ(AA(u0) + 1);
         sw(r, AA(u0), (uint64_t)IU(1) << 48);
         fr->pc += 2;
         break;
      case 0x1a: /* const-string */
         REQ(AA(u0));
         r[AA(u0)] = dd ? const_string(vm, dd, IU(1)) : 0;
         fr->pc += 2;
         break;
      case 0x1b: /* const-string/jumbo */
         REQ(AA(u0));
         r[AA(u0)] = dd ? const_string(vm, dd,
                        (uint32_t)IU(1) | ((uint32_t)IU(2) << 16)) : 0;
         fr->pc += 3;
         break;
      case 0x1c: /* const-class */
         REQ(AA(u0));
         r[AA(u0)] = class_object(vm, dd ? resolve_type(vm, dd, IU(1)) : NULL);
         fr->pc += 2;
         break;

      /* --- monitors: the VM is single-threaded per call, so these are no-ops.
       * Guest threads never enter bytecode concurrently — a JNI call runs to
       * completion on the calling thread. */
      case 0x1d: case 0x1e:
         fr->pc += 1;
         break;

      case 0x1f: { /* check-cast */
         REQ(AA(u0));
         struct dvm_class *t = dd ? resolve_type(vm, dd, IU(1)) : NULL;
         struct dvm_object *o = dvm__obj(vm, r[AA(u0)]);
         if (o && t && !class_assignable(vm, o->cls, t)) {
            dvm__throw(vm, "java/lang/ClassCastException", "%s to %s",
                       o->cls ? o->cls->name : "?", t->name);
            goto exception;
         }
         fr->pc += 2;
         break;
      }
      case 0x20: { /* instance-of */
         REQ(A4(u0)); REQ(B4(u0));
         struct dvm_class *t = dd ? resolve_type(vm, dd, IU(1)) : NULL;
         struct dvm_object *o = dvm__obj(vm, r[B4(u0)]);
         r[A4(u0)] = (o && t && class_assignable(vm, o->cls, t)) ? 1u : 0u;
         fr->pc += 2;
         break;
      }
      case 0x21: { /* array-length */
         REQ(A4(u0)); REQ(B4(u0));
         struct dvm_object *o = dvm__obj(vm, r[B4(u0)]);
         if (!o || o->kind != DVM_OBJ_ARRAY) {
            dvm__throw(vm, "java/lang/NullPointerException", "array-length");
            goto exception;
         }
         r[A4(u0)] = o->length;
         fr->pc += 1;
         break;
      }
      case 0x22: { /* new-instance */
         REQ(AA(u0));
         struct dvm_class *t = dd ? resolve_type(vm, dd, IU(1)) : NULL;
         if (!t) { dvm__throw(vm, "java/lang/NoClassDefFoundError", NULL); goto exception; }
         dvm_init_class(vm, t);
         if (t->external && vm->hooks.new_external) {
            uint32_t h = vm->hooks.new_external(vm->hooks.user, vm, t->name);
            r[AA(u0)] = h ? h : dvm_new_object(vm, t);
         } else {
            r[AA(u0)] = dvm_new_object(vm, t);
         }
         fr->pc += 2;
         break;
      }
      case 0x23: { /* new-array */
         REQ(A4(u0)); REQ(B4(u0));
         struct dvm_class *t = dd ? resolve_type(vm, dd, IU(1)) : NULL;
         int32_t len = (int32_t)r[B4(u0)];
         if (len < 0) {
            dvm__throw(vm, "java/lang/NegativeArraySizeException", "%d", len);
            goto exception;
         }
         const char *ed = (t && t->desc[0] == '[') ? t->desc + 1 : "I";
         r[A4(u0)] = dvm_new_array(vm, dvm__kind_of(ed), ed, (uint32_t)len);
         fr->pc += 2;
         break;
      }
      case 0x24: case 0x25: { /* filled-new-array[/range] */
         bool range = (op == 0x25);
         struct dvm_class *t = dd ? resolve_type(vm, dd, IU(1)) : NULL;
         uint32_t slots[256];
         int n = gather_args(insns, nins, pc, range, r, nregs, slots);
         const char *ed = (t && t->desc[0] == '[') ? t->desc + 1 : "I";
         char kind = dvm__kind_of(ed);
         dvm_ref arr = dvm_new_array(vm, kind, ed, (uint32_t)n);
         struct dvm_object *ao = dvm__obj(vm, arr);
         if (ao) {
            for (int i = 0; i < n; ++i) {
               switch (elem_width(kind)) {
                  case 1: ((uint8_t *)ao->data)[i] = (uint8_t)slots[i]; break;
                  case 2: ((uint16_t *)ao->data)[i] = (uint16_t)slots[i]; break;
                  default: ((uint32_t *)ao->data)[i] = slots[i]; break;
               }
            }
         }
         fr->result = arr;
         fr->pc += 3;
         break;
      }
      case 0x26: { /* fill-array-data */
         REQ(AA(u0));
         int32_t off = (int32_t)((uint32_t)IU(1) | ((uint32_t)IU(2) << 16));
         uint32_t poff = pc + (uint32_t)off;
         struct dvm_object *ao = dvm__obj(vm, r[AA(u0)]);
         if (!ao || ao->kind != DVM_OBJ_ARRAY) {
            dvm__throw(vm, "java/lang/NullPointerException", "fill-array-data");
            goto exception;
         }
         if (poff + 4 <= nins && insns[poff] == 0x0300) {
            uint16_t width = insns[poff + 1];
            uint32_t count = (uint32_t)insns[poff + 2] | ((uint32_t)insns[poff + 3] << 16);
            uint32_t n = count < ao->length ? count : ao->length;
            /* The payload is `count * width` bytes; a truncated one would
             * read past the code array. */
            uint64_t units = ((uint64_t)count * width + 1u) / 2u;
            if (poff + 4u + units <= nins && width == (uint16_t)elem_width(ao->elem_kind))
               memcpy(ao->data, (const uint8_t *)&insns[poff + 4], (size_t)n * width);
         }
         fr->pc += 3;
         break;
      }
      case 0x27: /* throw */
         REQ(AA(u0));
         if (!r[AA(u0)]) {
            dvm__throw(vm, "java/lang/NullPointerException", "throw null");
         } else {
            /* An explicit throw skips dvm__throw, so stamp the origin here or
             * the backtrace would attribute it to whichever frame unwinds
             * first.  A rethrow of the same object — the `move-exception;
             * monitor-exit; throw` shape a synchronized block compiles to —
             * must keep the origin it already has, or the frame that really
             * failed is lost. */
            vm->exception = r[AA(u0)];
            if (vm->exc_ref != r[AA(u0)]) {
               vm->exc_ref = r[AA(u0)];
               vm->nexc_trace = 0;
               vm->exc_trace[vm->nexc_trace++] = m;
               vm->exc_pc = pc;
            } else if (vm->nexc_trace <
                       (int)(sizeof vm->exc_trace / sizeof vm->exc_trace[0]) &&
                       (vm->nexc_trace == 0 ||
                        vm->exc_trace[vm->nexc_trace - 1] != m)) {
               vm->exc_trace[vm->nexc_trace++] = m;
            }
         }
         goto exception;

      /* --- branches --- */
      case 0x28: { /* goto */
         int32_t off = (int8_t)AA(u0);
         if (!off) { dvm__throw(vm, "java/lang/VerifyError", "goto 0"); goto exception; }
         fr->pc = (uint32_t)((int32_t)pc + off);
         break;
      }
      case 0x29:
         fr->pc = (uint32_t)((int32_t)pc + (int16_t)IU(1));
         break;
      case 0x2a:
         fr->pc = (uint32_t)((int32_t)pc + (int32_t)
                  ((uint32_t)IU(1) | ((uint32_t)IU(2) << 16)));
         break;

      case 0x2b: case 0x2c: { /* packed-switch / sparse-switch */
         REQ(AA(u0));
         int32_t off = (int32_t)((uint32_t)IU(1) | ((uint32_t)IU(2) << 16));
         uint32_t poff = pc + (uint32_t)off;
         int32_t val = (int32_t)r[AA(u0)];
         uint32_t target = pc + 3;
         if (poff + 2 <= nins) {
            uint16_t ident = insns[poff];
            uint16_t size = insns[poff + 1];
            if (ident == 0x0100 && op == 0x2b &&
                (uint64_t)poff + 4u + (uint64_t)size * 2u <= nins) {
               int32_t first = (int32_t)((uint32_t)insns[poff + 2] |
                                         ((uint32_t)insns[poff + 3] << 16));
               int64_t k = (int64_t)val - first;
               if (k >= 0 && k < size) {
                  uint32_t e = poff + 4 + (uint32_t)k * 2u;
                  target = (uint32_t)((int32_t)pc + (int32_t)
                     ((uint32_t)insns[e] | ((uint32_t)insns[e + 1] << 16)));
               }
            } else if (ident == 0x0200 && op == 0x2c &&
                       (uint64_t)poff + 2u + (uint64_t)size * 4u <= nins) {
               for (uint16_t k = 0; k < size; ++k) {
                  uint32_t ke = poff + 2 + (uint32_t)k * 2u;
                  int32_t key = (int32_t)((uint32_t)insns[ke] |
                                          ((uint32_t)insns[ke + 1] << 16));
                  if (key != val) continue;
                  uint32_t te = poff + 2 + (uint32_t)size * 2u + (uint32_t)k * 2u;
                  target = (uint32_t)((int32_t)pc + (int32_t)
                     ((uint32_t)insns[te] | ((uint32_t)insns[te + 1] << 16)));
                  break;
               }
            }
         }
         fr->pc = target;
         break;
      }

      /* --- comparisons --- */
      case 0x2d: case 0x2e: { /* cmpl-float / cmpg-float */
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff); REQ(bc >> 8);
         float a = bits_to_f(r[bc & 0xff]), b = bits_to_f(r[bc >> 8]);
         int32_t v;
         if (isunordered(a, b)) v = (op == 0x2d) ? -1 : 1;   /* NaN */
         else if (isgreater(a, b)) v = 1;
         else if (isless(a, b)) v = -1;
         else v = 0;
         r[AA(u0)] = (uint32_t)v;
         fr->pc += 2;
         break;
      }
      case 0x2f: case 0x30: { /* cmpl-double / cmpg-double */
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ((bc & 0xff) + 1); REQ((bc >> 8) + 1);
         double a = bits_to_d(rw(r, bc & 0xff)), b = bits_to_d(rw(r, bc >> 8));
         int32_t v;
         if (isunordered(a, b)) v = (op == 0x2f) ? -1 : 1;   /* NaN */
         else if (isgreater(a, b)) v = 1;
         else if (isless(a, b)) v = -1;
         else v = 0;
         r[AA(u0)] = (uint32_t)v;
         fr->pc += 2;
         break;
      }
      case 0x31: { /* cmp-long */
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ((bc & 0xff) + 1); REQ((bc >> 8) + 1);
         int64_t a = (int64_t)rw(r, bc & 0xff), b = (int64_t)rw(r, bc >> 8);
         r[AA(u0)] = (uint32_t)(a > b ? 1 : (a == b ? 0 : -1));
         fr->pc += 2;
         break;
      }

      case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: {
         REQ(A4(u0)); REQ(B4(u0));
         int32_t a = (int32_t)r[A4(u0)], b = (int32_t)r[B4(u0)];
         bool t;
         switch (op) {
            case 0x32: t = (a == b); break;
            case 0x33: t = (a != b); break;
            case 0x34: t = (a < b); break;
            case 0x35: t = (a >= b); break;
            case 0x36: t = (a > b); break;
            default:   t = (a <= b); break;
         }
         fr->pc = t ? (uint32_t)((int32_t)pc + (int16_t)IU(1)) : pc + 2;
         break;
      }
      case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: {
         REQ(AA(u0));
         int32_t a = (int32_t)r[AA(u0)];
         bool t;
         switch (op) {
            case 0x38: t = (a == 0); break;
            case 0x39: t = (a != 0); break;
            case 0x3a: t = (a < 0); break;
            case 0x3b: t = (a >= 0); break;
            case 0x3c: t = (a > 0); break;
            default:   t = (a <= 0); break;
         }
         fr->pc = t ? (uint32_t)((int32_t)pc + (int16_t)IU(1)) : pc + 2;
         break;
      }

      /* --- arrays --- */
      case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a: {
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff); REQ(bc >> 8);
         struct dvm_object *a = dvm__obj(vm, r[bc & 0xff]);
         uint32_t idx = r[bc >> 8];
         if (!array_check(vm, a, idx)) goto exception;
         switch (op) {
            case 0x44: r[AA(u0)] = ((uint32_t *)a->data)[idx]; break;
            case 0x45: REQ(AA(u0) + 1); sw(r, AA(u0), ((uint64_t *)a->data)[idx]); break;
            case 0x46: r[AA(u0)] = ((uint32_t *)a->data)[idx]; break;
            case 0x47: r[AA(u0)] = ((uint8_t *)a->data)[idx] ? 1u : 0u; break;
            case 0x48: r[AA(u0)] = (uint32_t)(int32_t)(int8_t)((uint8_t *)a->data)[idx]; break;
            case 0x49: r[AA(u0)] = ((uint16_t *)a->data)[idx]; break;
            default:   r[AA(u0)] = (uint32_t)(int32_t)(int16_t)((uint16_t *)a->data)[idx]; break;
         }
         fr->pc += 2;
         break;
      }
      case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: case 0x50: case 0x51: {
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff); REQ(bc >> 8);
         struct dvm_object *a = dvm__obj(vm, r[bc & 0xff]);
         uint32_t idx = r[bc >> 8];
         if (!array_check(vm, a, idx)) goto exception;
         switch (op) {
            case 0x4b: ((uint32_t *)a->data)[idx] = r[AA(u0)]; break;
            case 0x4c: REQ(AA(u0) + 1); ((uint64_t *)a->data)[idx] = rw(r, AA(u0)); break;
            case 0x4d: ((uint32_t *)a->data)[idx] = r[AA(u0)]; break;
            case 0x4e: ((uint8_t *)a->data)[idx] = r[AA(u0)] ? 1u : 0u; break;
            case 0x4f: ((uint8_t *)a->data)[idx] = (uint8_t)r[AA(u0)]; break;
            case 0x50: ((uint16_t *)a->data)[idx] = (uint16_t)r[AA(u0)]; break;
            default:   ((uint16_t *)a->data)[idx] = (uint16_t)r[AA(u0)]; break;
         }
         fr->pc += 2;
         break;
      }

      /* --- instance fields --- */
      case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: {
         REQ(A4(u0)); REQ(B4(u0));
         struct dvm_class *fc; const char *fn, *ft;
         struct dvm_field *f = dd ? resolve_field(vm, dd, IU(1), false, &fc, &fn, &ft)
                                  : NULL;
         struct dvm_object *o = dvm__obj(vm, r[B4(u0)]);
         if (!o) {
            dvm__throw(vm, "java/lang/NullPointerException", "iget %s", fn ? fn : "?");
            goto exception;
         }
         /* A field the object does not have belongs to a host-backed class;
          * those carry no state on this side, so it reads as zero — the same
          * answer the stub layer gave before this module existed. */
         union dvm_value v = { 0 };
         if (f && o->slots && f->slot < (o->cls ? o->cls->islots : 0))
            v = o->slots[f->slot];
         if (vm->trace && op == 0x54)
            fprintf(stderr, "[dvm] %*siget-object @%x.%s:%s -> @%x\n",
                    vm->depth * 2, "", r[B4(u0)], fn ? fn : "?",
                    ft ? ft : "?", v.l);
         if (op == 0x53) { REQ(A4(u0) + 1); sw(r, A4(u0), v.ju); }
         else r[A4(u0)] = v.u;
         fr->pc += 2;
         break;
      }
      case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
         REQ(A4(u0)); REQ(B4(u0));
         struct dvm_class *fc; const char *fn, *ft;
         struct dvm_field *f = dd ? resolve_field(vm, dd, IU(1), false, &fc, &fn, &ft)
                                  : NULL;
         struct dvm_object *o = dvm__obj(vm, r[B4(u0)]);
         if (!o) {
            dvm__throw(vm, "java/lang/NullPointerException", "iput %s", fn ? fn : "?");
            goto exception;
         }
         union dvm_value v = { 0 };
         if (op == 0x5a) { REQ(A4(u0) + 1); v.ju = rw(r, A4(u0)); }
         else v.u = r[A4(u0)];
         if (f && o->slots && f->slot < (o->cls ? o->cls->islots : 0))
            o->slots[f->slot] = v;
         if (vm->trace && op == 0x5b)
            fprintf(stderr, "[dvm] %*siput-object @%x.%s:%s <- @%x\n",
                    vm->depth * 2, "", r[B4(u0)], fn ? fn : "?",
                    ft ? ft : "?", v.l);
         fr->pc += 2;
         break;
      }

      /* --- static fields --- */
      case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: {
         REQ(AA(u0));
         struct dvm_class *fc = NULL; const char *fn = NULL, *ft = NULL;
         struct dvm_field *f = dd ? resolve_field(vm, dd, IU(1), true, &fc, &fn, &ft)
                                  : NULL;
         union dvm_value v = { 0 };
         if (f) {
            /* A <clinit> that throws leaves the class erroneous and its static
             * fields unset; reading one anyway hands the app a null it can
             * only fail on later, far from the cause. */
            if (!dvm_init_class(vm, f->cls)) goto exception;
            if (f->cls->sslots && f->slot < f->cls->nsslots)
               v = f->cls->sslots[f->slot];
         } else if (fc && fn && vm->hooks.get_external_static) {
            /* Nothing defines this class — not a dex, not a builtin, not the
             * host stubs.  A device raises NoClassDefFoundError here, and app
             * code guards for it: AppsFlyer probes for Xiaomi's install
             * referrer exactly this way.  Answering null instead turns a
             * handled absence into an NPE one call later. */
            if (!vm->hooks.get_external_static(vm->hooks.user, vm, fc->name, fn,
                                               ft, &v)) {
               dvm__throw(vm, "java/lang/NoClassDefFoundError", "%s.%s",
                          fc->name, fn);
               goto exception;
            }
         }
         if (vm->trace && op == 0x62)
            fprintf(stderr, "[dvm] %*ssget-object %s.%s:%s -> @%x\n",
                    vm->depth * 2, "", fc ? fc->name : "?", fn ? fn : "?",
                    ft ? ft : "?", v.l);
         if (op == 0x61) { REQ(AA(u0) + 1); sw(r, AA(u0), v.ju); }
         else r[AA(u0)] = v.u;
         fr->pc += 2;
         break;
      }
      case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: {
         REQ(AA(u0));
         struct dvm_class *fc; const char *fn, *ft;
         struct dvm_field *f = dd ? resolve_field(vm, dd, IU(1), true, &fc, &fn, &ft)
                                  : NULL;
         union dvm_value v = { 0 };
         if (op == 0x68) { REQ(AA(u0) + 1); v.ju = rw(r, AA(u0)); }
         else v.u = r[AA(u0)];
         if (f) {
            if (!dvm_init_class(vm, f->cls)) goto exception;
            if (f->cls->sslots && f->slot < f->cls->nsslots)
               f->cls->sslots[f->slot] = v;
         }
         if (vm->trace && op == 0x69)
            fprintf(stderr, "[dvm] %*ssput-object %s.%s:%s <- @%x\n",
                    vm->depth * 2, "", fc ? fc->name : "?", fn ? fn : "?",
                    ft ? ft : "?", v.l);
         fr->pc += 2;
         break;
      }

      /* --- invokes --- */
      case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:
      case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: {
         bool range = (op >= 0x74);
         uint8_t kind = range ? (uint8_t)(op - 0x74 + 0x6e) : op;
         uint32_t midx = IU(1);

         struct dvm_class *cls = NULL;
         const char *name = NULL;
         struct dvm_method *target =
            dd ? resolve_method(vm, dd, midx, &cls, &name, sigbuf, sizeof sigbuf) : NULL;

         uint32_t slots[256];
         int n = gather_args(insns, nins, pc, range, r, nregs, slots);

         bool is_static = (kind == 0x71);
         dvm_ref self = 0;
         const uint32_t *argslots = slots;
         int nargslots = n;
         if (!is_static) {
            if (n < 1) {
               dvm__throw(vm, "java/lang/VerifyError", "invoke with no receiver");
               goto exception;
            }
            self = slots[0];
            argslots = slots + 1;
            nargslots = n - 1;
            if (!self) {
               /* The method index pins which constant-pool entry resolved to
                * this name, which matters when the resolution itself looks
                * wrong. */
               dvm__throw(vm, "java/lang/NullPointerException",
                          "receiver %s.%s meth@%u (insns@0x%lx w0=%04x w1=%04x)",
                          cls ? cls->name : "?", name ? name : "?", midx,
                          dd ? (unsigned long)((const uint8_t *)insns - dd->file.p)
                             : 0UL,
                          (unsigned)insns[pc], (unsigned)IU(1));
               goto exception;
            }
         }

         /* invoke-virtual / -interface re-dispatch on the receiver. */
         if (target && (kind == 0x6e || kind == 0x72))
            target = virtual_target(vm, self, target);
         else if (!target && self && (kind == 0x6e || kind == 0x72)) {
            /* The declared type carries no such method — an interface whose
             * definition is not in the dex, or one modelled here without it.
             * Dispatch is on the receiver, so look there before giving up and
             * handing the call to the host stubs, which would answer null.
             * Iterable.iterator() reached this path constantly: the sequence
             * types Kotlin's split() builds implement it, but the interface
             * itself declares nothing the VM can see. */
            struct dvm_object *ro = dvm__obj(vm, self);
            if (ro && ro->cls)
               target = dvm_find_method(vm, ro->cls, name, sigbuf);
         }

         union dvm_value ret;
         bool ok;
         if (target) {
            ok = invoke(vm, target, self, argslots, nargslots, &ret);
         } else if (cls && name) {
            ++vm->depth;
            ok = dvm__call_out(vm, cls, name, sigbuf,
                          is_static, false, self, argslots, nargslots, &ret);
            --vm->depth;
            ok = ok && !vm->exception;
         } else {
            memset(&ret, 0, sizeof ret);
            ok = true;
         }
         if (!ok || vm->exception) goto exception;

         fr->result = ret.ju;
         fr->pc += 3;   /* 35c and 3rc are both three code units */
         break;
      }

      /* --- unary --- */
      case 0x7b: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = (uint32_t)(-(int32_t)r[B4(u0)]); fr->pc += 1; break;
      case 0x7c: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = ~r[B4(u0)]; fr->pc += 1; break;
      case 0x7d: REQ(A4(u0) + 1); REQ(B4(u0) + 1); sw(r, A4(u0), (uint64_t)(-(int64_t)rw(r, B4(u0)))); fr->pc += 1; break;
      case 0x7e: REQ(A4(u0) + 1); REQ(B4(u0) + 1); sw(r, A4(u0), ~rw(r, B4(u0))); fr->pc += 1; break;
      case 0x7f: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = f_to_bits(-bits_to_f(r[B4(u0)])); fr->pc += 1; break;
      case 0x80: REQ(A4(u0) + 1); REQ(B4(u0) + 1); sw(r, A4(u0), d_to_bits(-bits_to_d(rw(r, B4(u0))))); fr->pc += 1; break;

      case 0x81: REQ(A4(u0) + 1); REQ(B4(u0)); sw(r, A4(u0), (uint64_t)(int64_t)(int32_t)r[B4(u0)]); fr->pc += 1; break;
      case 0x82: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = f_to_bits((float)(int32_t)r[B4(u0)]); fr->pc += 1; break;
      case 0x83: REQ(A4(u0) + 1); REQ(B4(u0)); sw(r, A4(u0), d_to_bits((double)(int32_t)r[B4(u0)])); fr->pc += 1; break;
      case 0x84: REQ(A4(u0)); REQ(B4(u0) + 1); r[A4(u0)] = (uint32_t)rw(r, B4(u0)); fr->pc += 1; break;
      case 0x85: REQ(A4(u0)); REQ(B4(u0) + 1); r[A4(u0)] = f_to_bits((float)(int64_t)rw(r, B4(u0))); fr->pc += 1; break;
      case 0x86: REQ(A4(u0) + 1); REQ(B4(u0) + 1); sw(r, A4(u0), d_to_bits((double)(int64_t)rw(r, B4(u0)))); fr->pc += 1; break;
      case 0x87: { /* float-to-int, with Java's saturating semantics */
         REQ(A4(u0)); REQ(B4(u0));
         float f = bits_to_f(r[B4(u0)]);
         int32_t v;
         if (isnan(f)) v = 0;
         else if (f >= 2147483647.0f) v = INT32_MAX;
         else if (f <= -2147483648.0f) v = INT32_MIN;
         else v = (int32_t)f;
         r[A4(u0)] = (uint32_t)v;
         fr->pc += 1;
         break;
      }
      case 0x88: {
         REQ(A4(u0) + 1); REQ(B4(u0));
         float f = bits_to_f(r[B4(u0)]);
         int64_t v;
         if (isnan(f)) v = 0;
         else if (f >= 9223372036854775807.0f) v = INT64_MAX;
         else if (f <= -9223372036854775808.0f) v = INT64_MIN;
         else v = (int64_t)f;
         sw(r, A4(u0), (uint64_t)v);
         fr->pc += 1;
         break;
      }
      case 0x89: REQ(A4(u0) + 1); REQ(B4(u0)); sw(r, A4(u0), d_to_bits((double)bits_to_f(r[B4(u0)]))); fr->pc += 1; break;
      case 0x8a: {
         REQ(A4(u0)); REQ(B4(u0) + 1);
         double f = bits_to_d(rw(r, B4(u0)));
         int32_t v;
         if (isnan(f)) v = 0;
         else if (f >= 2147483647.0) v = INT32_MAX;
         else if (f <= -2147483648.0) v = INT32_MIN;
         else v = (int32_t)f;
         r[A4(u0)] = (uint32_t)v;
         fr->pc += 1;
         break;
      }
      case 0x8b: {
         REQ(A4(u0) + 1); REQ(B4(u0) + 1);
         double f = bits_to_d(rw(r, B4(u0)));
         int64_t v;
         if (isnan(f)) v = 0;
         else if (f >= 9223372036854775807.0) v = INT64_MAX;
         else if (f <= -9223372036854775808.0) v = INT64_MIN;
         else v = (int64_t)f;
         sw(r, A4(u0), (uint64_t)v);
         fr->pc += 1;
         break;
      }
      case 0x8c: REQ(A4(u0)); REQ(B4(u0) + 1); r[A4(u0)] = f_to_bits((float)bits_to_d(rw(r, B4(u0)))); fr->pc += 1; break;
      case 0x8d: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = (uint32_t)(int32_t)(int8_t)r[B4(u0)]; fr->pc += 1; break;
      case 0x8e: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = (uint16_t)r[B4(u0)]; fr->pc += 1; break;
      case 0x8f: REQ(A4(u0)); REQ(B4(u0)); r[A4(u0)] = (uint32_t)(int32_t)(int16_t)r[B4(u0)]; fr->pc += 1; break;

      /* --- binary op vAA, vBB, vCC --- */
      case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
      case 0x96: case 0x97: case 0x98: case 0x99: case 0x9a: {
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff); REQ(bc >> 8);
         int32_t a = (int32_t)r[bc & 0xff], b = (int32_t)r[bc >> 8];
         int32_t v = 0;
         switch (op) {
            case 0x90: v = (int32_t)((uint32_t)a + (uint32_t)b); break;
            case 0x91: v = (int32_t)((uint32_t)a - (uint32_t)b); break;
            case 0x92: v = (int32_t)((uint32_t)a * (uint32_t)b); break;
            case 0x93:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? INT32_MIN : a / b;
               break;
            case 0x94:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? 0 : a % b;
               break;
            case 0x95: v = a & b; break;
            case 0x96: v = a | b; break;
            case 0x97: v = a ^ b; break;
            case 0x98: v = (int32_t)((uint32_t)a << (b & 31)); break;
            case 0x99: v = a >> (b & 31); break;
            default:   v = (int32_t)((uint32_t)a >> (b & 31)); break;
         }
         r[AA(u0)] = (uint32_t)v;
         fr->pc += 2;
         break;
      }
      case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f: case 0xa0:
      case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: {
         REQ(AA(u0) + 1);
         uint16_t bc = IU(1);
         bool shift = (op >= 0xa3);
         REQ((bc & 0xff) + 1);
         REQ(shift ? (uint32_t)(bc >> 8) : (uint32_t)((bc >> 8) + 1));
         int64_t a = (int64_t)rw(r, bc & 0xff);
         int64_t b = shift ? (int32_t)r[bc >> 8] : (int64_t)rw(r, bc >> 8);
         int64_t v = 0;
         switch (op) {
            case 0x9b: v = (int64_t)((uint64_t)a + (uint64_t)b); break;
            case 0x9c: v = (int64_t)((uint64_t)a - (uint64_t)b); break;
            case 0x9d: v = (int64_t)((uint64_t)a * (uint64_t)b); break;
            case 0x9e:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT64_MIN && b == -1) ? INT64_MIN : a / b;
               break;
            case 0x9f:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT64_MIN && b == -1) ? 0 : a % b;
               break;
            case 0xa0: v = a & b; break;
            case 0xa1: v = a | b; break;
            case 0xa2: v = a ^ b; break;
            case 0xa3: v = (int64_t)((uint64_t)a << (b & 63)); break;
            case 0xa4: v = a >> (b & 63); break;
            default:   v = (int64_t)((uint64_t)a >> (b & 63)); break;
         }
         sw(r, AA(u0), (uint64_t)v);
         fr->pc += 2;
         break;
      }
      case 0xa6: case 0xa7: case 0xa8: case 0xa9: case 0xaa: {
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff); REQ(bc >> 8);
         float a = bits_to_f(r[bc & 0xff]), b = bits_to_f(r[bc >> 8]), v;
         switch (op) {
            case 0xa6: v = a + b; break;
            case 0xa7: v = a - b; break;
            case 0xa8: v = a * b; break;
            case 0xa9: v = a / b; break;
            default:   v = fmodf(a, b); break;
         }
         r[AA(u0)] = f_to_bits(v);
         fr->pc += 2;
         break;
      }
      case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: {
         REQ(AA(u0) + 1);
         uint16_t bc = IU(1);
         REQ((bc & 0xff) + 1); REQ((bc >> 8) + 1);
         double a = bits_to_d(rw(r, bc & 0xff)), b = bits_to_d(rw(r, bc >> 8)), v;
         switch (op) {
            case 0xab: v = a + b; break;
            case 0xac: v = a - b; break;
            case 0xad: v = a * b; break;
            case 0xae: v = a / b; break;
            default:   v = fmod(a, b); break;
         }
         sw(r, AA(u0), d_to_bits(v));
         fr->pc += 2;
         break;
      }

      /* --- binary op/2addr --- */
      case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5:
      case 0xb6: case 0xb7: case 0xb8: case 0xb9: case 0xba: {
         REQ(A4(u0)); REQ(B4(u0));
         int32_t a = (int32_t)r[A4(u0)], b = (int32_t)r[B4(u0)];
         int32_t v = 0;
         switch (op) {
            case 0xb0: v = (int32_t)((uint32_t)a + (uint32_t)b); break;
            case 0xb1: v = (int32_t)((uint32_t)a - (uint32_t)b); break;
            case 0xb2: v = (int32_t)((uint32_t)a * (uint32_t)b); break;
            case 0xb3:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? INT32_MIN : a / b;
               break;
            case 0xb4:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? 0 : a % b;
               break;
            case 0xb5: v = a & b; break;
            case 0xb6: v = a | b; break;
            case 0xb7: v = a ^ b; break;
            case 0xb8: v = (int32_t)((uint32_t)a << (b & 31)); break;
            case 0xb9: v = a >> (b & 31); break;
            default:   v = (int32_t)((uint32_t)a >> (b & 31)); break;
         }
         r[A4(u0)] = (uint32_t)v;
         fr->pc += 1;
         break;
      }
      case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf: case 0xc0:
      case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5: {
         bool shift = (op >= 0xc3);
         REQ(A4(u0) + 1);
         REQ(shift ? (uint32_t)B4(u0) : (uint32_t)(B4(u0) + 1));
         int64_t a = (int64_t)rw(r, A4(u0));
         int64_t b = shift ? (int32_t)r[B4(u0)] : (int64_t)rw(r, B4(u0));
         int64_t v = 0;
         switch (op) {
            case 0xbb: v = (int64_t)((uint64_t)a + (uint64_t)b); break;
            case 0xbc: v = (int64_t)((uint64_t)a - (uint64_t)b); break;
            case 0xbd: v = (int64_t)((uint64_t)a * (uint64_t)b); break;
            case 0xbe:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT64_MIN && b == -1) ? INT64_MIN : a / b;
               break;
            case 0xbf:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT64_MIN && b == -1) ? 0 : a % b;
               break;
            case 0xc0: v = a & b; break;
            case 0xc1: v = a | b; break;
            case 0xc2: v = a ^ b; break;
            case 0xc3: v = (int64_t)((uint64_t)a << (b & 63)); break;
            case 0xc4: v = a >> (b & 63); break;
            default:   v = (int64_t)((uint64_t)a >> (b & 63)); break;
         }
         sw(r, A4(u0), (uint64_t)v);
         fr->pc += 1;
         break;
      }
      case 0xc6: case 0xc7: case 0xc8: case 0xc9: case 0xca: {
         REQ(A4(u0)); REQ(B4(u0));
         float a = bits_to_f(r[A4(u0)]), b = bits_to_f(r[B4(u0)]), v;
         switch (op) {
            case 0xc6: v = a + b; break;
            case 0xc7: v = a - b; break;
            case 0xc8: v = a * b; break;
            case 0xc9: v = a / b; break;
            default:   v = fmodf(a, b); break;
         }
         r[A4(u0)] = f_to_bits(v);
         fr->pc += 1;
         break;
      }
      case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: {
         REQ(A4(u0) + 1); REQ(B4(u0) + 1);
         double a = bits_to_d(rw(r, A4(u0))), b = bits_to_d(rw(r, B4(u0))), v;
         switch (op) {
            case 0xcb: v = a + b; break;
            case 0xcc: v = a - b; break;
            case 0xcd: v = a * b; break;
            case 0xce: v = a / b; break;
            default:   v = fmod(a, b); break;
         }
         sw(r, A4(u0), d_to_bits(v));
         fr->pc += 1;
         break;
      }

      /* --- literal ops --- */
      case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xd4:
      case 0xd5: case 0xd6: case 0xd7: {
         REQ(A4(u0)); REQ(B4(u0));
         int32_t a = (int32_t)r[B4(u0)];
         int32_t b = (int16_t)IU(1);
         int32_t v = 0;
         switch (op) {
            case 0xd0: v = (int32_t)((uint32_t)a + (uint32_t)b); break;
            case 0xd1: v = (int32_t)((uint32_t)b - (uint32_t)a); break;  /* rsub */
            case 0xd2: v = (int32_t)((uint32_t)a * (uint32_t)b); break;
            case 0xd3:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? INT32_MIN : a / b;
               break;
            case 0xd4:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? 0 : a % b;
               break;
            case 0xd5: v = a & b; break;
            case 0xd6: v = a | b; break;
            default:   v = a ^ b; break;
         }
         r[A4(u0)] = (uint32_t)v;
         fr->pc += 2;
         break;
      }
      case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd:
      case 0xde: case 0xdf: case 0xe0: case 0xe1: case 0xe2: {
         REQ(AA(u0));
         uint16_t bc = IU(1);
         REQ(bc & 0xff);
         int32_t a = (int32_t)r[bc & 0xff];
         int32_t b = (int8_t)(uint8_t)(bc >> 8);
         int32_t v = 0;
         switch (op) {
            case 0xd8: v = (int32_t)((uint32_t)a + (uint32_t)b); break;
            case 0xd9: v = (int32_t)((uint32_t)b - (uint32_t)a); break;
            case 0xda: v = (int32_t)((uint32_t)a * (uint32_t)b); break;
            case 0xdb:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? INT32_MIN : a / b;
               break;
            case 0xdc:
               if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "divide by zero"); goto exception; }
               v = (a == INT32_MIN && b == -1) ? 0 : a % b;
               break;
            case 0xdd: v = a & b; break;
            case 0xde: v = a | b; break;
            case 0xdf: v = a ^ b; break;
            case 0xe0: v = (int32_t)((uint32_t)a << (b & 31)); break;
            case 0xe1: v = a >> (b & 31); break;
            default:   v = (int32_t)((uint32_t)a >> (b & 31)); break;
         }
         r[AA(u0)] = (uint32_t)v;
         fr->pc += 2;
         break;
      }

      /* --- invoke-polymorphic / -custom, const-method-*: these need a
       * MethodHandle runtime.  Java 8 lambdas in an APK are desugared by d8
       * into ordinary classes, so this only shows up in code that reflects on
       * method handles.  Skip the call and yield zero rather than derail the
       * whole method. */
      case 0xfa: case 0xfb: fr->pc += 4; fr->result = 0; break;
      case 0xfc: case 0xfd: fr->pc += 3; fr->result = 0; break;
      case 0xfe: case 0xff: REQ(AA(u0)); r[AA(u0)] = 0; fr->pc += 2; break;

      default:
         dvm__throw(vm, "java/lang/VerifyError", "%s.%s: opcode %02x at %04x",
                    m->cls->name, m->name, op, pc);
         goto exception;
      }
      continue;

exception:
      {
         if (!vm->exception)
            dvm__throw(vm, "java/lang/Error", "internal: no exception object");
         struct dvm_object *eo = dvm__obj(vm, vm->exception);
         uint32_t handler;
         /* A parked thread is not a failure the app can handle: it is a
          * thread that would be waiting inside the platform right now.  The
          * unwind therefore passes catch clauses by and ends the thread — a
          * dispatcher's `catch (InterruptedException e) { continue; }` would
          * otherwise put it straight back on the empty queue it just parked
          * on, and spin for the whole time slice. */
         if (vm->parked) {
            if (find_catchall_handler(m, fr->pc, &handler) == 0) {
               vm->exc_ref = vm->exception;
               vm->exception = 0;
               fr->pc = handler;
               continue;
            }
            return false;
         }
         if (find_handler(vm, m, fr->pc, eo ? eo->cls : NULL, &handler) == 0) {
            /* Dalvik clears the pending exception when control reaches the
             * handler; move-exception only *retrieves* it, and dx omits that
             * instruction entirely when the catch variable is unused.  Leaving
             * the exception set meant such a handler ran with the throw still
             * pending, so the first call it made returned "exception" and the
             * caught error escaped anyway — `catch (ClassNotFoundException e)`
             * blocks that merely log were the common shape.  Keep the object
             * reachable for a move-exception that does follow. */
            vm->exc_ref = vm->exception;
            vm->exception = 0;
            fr->pc = handler;
            continue;
         }
         return false;
      }
   }
#undef REQ
#undef THROW_AND_DISPATCH
}

/* ------------------------------------------------------------------------ *
 * Public entry points
 * ------------------------------------------------------------------------ */

/* Runs the threads bytecode started during the call that just finished.  The
 * slice is deliberately small: a dispatcher loop that has drained its queue
 * would otherwise spin against the ordinary step limit and stall the frame. */
uint64_t dvm__now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void dvm__run_pending_threads(struct dvm *vm)
{
   if (vm->draining || !vm->npending) return;
   vm->draining = true;
   uint64_t saved_limit = vm->step_limit;
   bool saved_quiet = vm->quiet_uncaught;

   for (int round = 0; vm->npending && round < 8; ++round) {
      dvm_ref list[32];
      bool is_thread[32];
      /* Entries whose delay has not elapsed stay queued for a later drain. */
      uint64_t now = dvm__now_ms();
      int n = 0, keep = 0;
      for (int i = 0; i < vm->npending; ++i) {
         if (vm->pending_due_ms[i] > now) {
            vm->pending_threads[keep] = vm->pending_threads[i];
            vm->pending_is_thread[keep] = vm->pending_is_thread[i];
            vm->pending_due_ms[keep] = vm->pending_due_ms[i];
            ++keep;
            continue;
         }
         list[n] = vm->pending_threads[i];
         is_thread[n] = vm->pending_is_thread[i];
         ++n;
      }
      vm->npending = keep;
      if (!n) break;
      /* Newly-started work gets the first slice.  This mirrors the local LIFO
       * queues used by Android/JVM work-stealing pools and prevents a Future's
       * producer from sitting behind a full set of already-idle daemon worker
       * loops in this single-host-thread VM.  Every entry is still run once in
       * the round, so older work cannot starve. */
      for (int i = n - 1; i >= 0; --i) {
         struct dvm_class *c = dvm_object_class(vm, list[i]);
         struct dvm_method *run = c ? dvm_find_method(vm, c, "run", "()V") : NULL;
         if (run && (run->has_code || run->builtin)) {
            union dvm_value ret;
            uint64_t before = vm->steps;
            vm->step_limit = DVM_THREAD_SLICE;
            vm->quiet_uncaught = true;
            /* A Runnable handed to Handler.post() runs on the main thread; one
             * started with Thread.start() runs as itself.  App code asserts on
             * the difference, so Thread.currentThread() has to follow it. */
            dvm_ref saved_thread = vm->cur_thread;
            vm->cur_thread = is_thread[i] ? list[i] : 0;
            bool ok = dvm_call(vm, run, list[i], NULL, 0, &ret);
            vm->cur_thread = saved_thread;
            vm->quiet_uncaught = saved_quiet;
            vm->step_limit = saved_limit;
            char how[256];
            how[0] = '\0';
            if (!ok && vm->exception)
               dvm_describe_exception(vm, vm->exception, how, sizeof how);
            dvm_clear_exception(vm);
            static int log_n = 0;
            /* The cap keeps startup readable; a trace run wants every one of
               them, because a runnable that keeps re-queueing is the symptom. */
            const bool report = (log_n++ < 24 || vm->trace);
            if (report)
               fprintf(stderr, "[dvm] thread %s.run() ran %llu steps%s%s\n",
                       run->cls ? run->cls->name : "?",
                       (unsigned long long)(vm->steps - before),
                       how[0] ? " — " : "", how[0] ? how : "");
            /* A Runnable that died of an exception is the interesting case, and
             * the class of the Runnable says nothing about where it died — the
             * throw is usually several frames down inside an SDK.  Print the
             * unwind path for it exactly as dvm_call() does for an uncaught
             * exception on the main thread. */
            if (report && how[0])
               for (int f_i = 0; f_i < vm->nexc_trace; ++f_i) {
                  struct dvm_method *f = vm->exc_trace[f_i];
                  if (!f) continue;
                  if (f_i == 0)
                     fprintf(stderr, "[dvm]   at %s.%s%s +0x%x\n",
                             f->cls ? f->cls->name : "?", f->name,
                             f->sig ? f->sig : "", vm->exc_pc * 2);
                  else
                     fprintf(stderr, "[dvm]   at %s.%s%s\n",
                             f->cls ? f->cls->name : "?", f->name,
                             f->sig ? f->sig : "");
               }
            vm->nexc_trace = 0;
         }
         dvm_unpin(vm, list[i]);
      }
   }

   vm->step_limit = saved_limit;
   vm->quiet_uncaught = saved_quiet;
   vm->draining = false;
}

bool dvm_call(struct dvm *vm, struct dvm_method *m, dvm_ref self,
              const union dvm_value *args, int nargs, union dvm_value *out)
{
   union dvm_value dummy;
   if (!out) out = &dummy;
   memset(out, 0, sizeof *out);
   if (!m) return false;

   uint32_t slots[256];
   int n = values_to_slots(m->sig, args, nargs, slots, 256);
   vm->exception = 0;
   /* Cleared on the way in, not on the way out: the unwind path belongs to the
    * call that just failed, and the caller (dvm__run_pending_threads(), which
    * reports a Runnable that died) needs to read it after invoke() returns. */
   vm->nexc_trace = 0;
   if (!vm->depth) { vm->call_steps = 0; vm->parked = false; }
   bool ok = invoke(vm, m, self, slots, n, out);
   if (!ok && vm->exception && !vm->quiet_uncaught) {
      char buf[512];
      dvm_describe_exception(vm, vm->exception, buf, sizeof buf);
      fprintf(stderr, "[dvm] uncaught %s in %s.%s\n", buf,
              m->cls ? m->cls->name : "?", m->name);
      for (int i = 0; i < vm->nexc_trace; ++i) {
         struct dvm_method *f = vm->exc_trace[i];
         if (i == 0)
            fprintf(stderr, "[dvm]   at %s.%s%s +0x%x\n",
                    f->cls ? f->cls->name : "?", f->name,
                    f->sig ? f->sig : "", vm->exc_pc * 2);
         else
            fprintf(stderr, "[dvm]   at %s.%s%s\n",
                    f->cls ? f->cls->name : "?", f->name, f->sig ? f->sig : "");
      }
   }
   if (!vm->depth) dvm__run_pending_threads(vm);
   return ok;
}

/* ------------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------------ */

struct dvm *dvm_create(const struct dvm_hooks *hooks)
{
   struct dvm *vm = calloc(1, sizeof *vm);
   if (!vm) return NULL;
   if (hooks) vm->hooks = *hooks;
   vm->step_limit = 200u * 1000u * 1000u;

   const char *t = getenv("LUNARIA_DVM_TRACE");
   if (t) vm->trace = atoi(t);

   dvm_runtime_install(vm);
   return vm;
}

void dvm_destroy(struct dvm *vm)
{
   if (!vm) return;

   for (uint32_t i = 0; i < vm->heap_size; ++i) {
      free(vm->heap[i].utf8);
      free(vm->heap[i].data);
      free(vm->heap[i].slots);
   }
   free(vm->heap);

   for (int i = 0; i < vm->nclasses; ++i) {
      struct dvm_class *c = vm->classes[i];
      for (int k = 0; k < c->nmethods; ++k)
         free(c->methods[k].sig);
      free(c->methods);
      free(c->ifields);
      free(c->sfields);
      free(c->sslots);
      free(c->ifaces);
      free(c->desc);
      free(c->name);
      free(c);
   }
   free(vm->classes);

   for (int i = 0; i < vm->ndexes; ++i) {
      free(vm->dexes[i]->type_cache);
      free(vm->dexes[i]->method_cache);
      free(vm->dexes[i]->field_cache);
      free(vm->dexes[i]->string_cache);
      dex_close(&vm->dexes[i]->file);
      free(vm->dexes[i]);
   }
   free(vm->dexes);

   for (int i = 0; i < vm->ninterns; ++i)
      free(vm->interns[i].key);
   free(vm->interns);

   for (int i = 0; i < vm->nmissing; ++i)
      free(vm->missing[i]);
   free(vm->missing);

   free(vm);
}

bool dvm_add_dex(struct dvm *vm, const char *path)
{
   if (vm->ndexes == vm->dexes_cap) {
      int cap = vm->dexes_cap ? vm->dexes_cap * 2 : 8;
      struct dvm_dex **n = realloc(vm->dexes, (size_t)cap * sizeof *n);
      if (!n) return false;
      vm->dexes = n;
      vm->dexes_cap = cap;
   }
   struct dvm_dex *dd = calloc(1, sizeof *dd);
   if (!dd) return false;
   if (!dex_open(&dd->file, path)) {
      free(dd);
      return false;
   }

   dd->type_cache   = calloc(dd->file.type_ids_size + 1u, sizeof *dd->type_cache);
   dd->method_cache = calloc(dd->file.method_ids_size + 1u, sizeof *dd->method_cache);
   dd->field_cache  = calloc(dd->file.field_ids_size + 1u, sizeof *dd->field_cache);
   dd->string_cache = calloc(dd->file.string_ids_size + 1u, sizeof *dd->string_cache);

   vm->dexes[vm->ndexes++] = dd;
   fprintf(stderr, "[dvm] loaded %s: %u classes, %u methods\n",
           path, dd->file.class_defs_size, dd->file.method_ids_size);
   return true;
}

int dvm_add_apk_dir(struct dvm *vm, const char *dir)
{
   static const char *const subdirs[] = { "", "base/" };
   int loaded = 0;
   for (size_t s = 0; s < sizeof subdirs / sizeof subdirs[0]; ++s) {
      for (int i = 0; i < 64; ++i) {
         char path[1024];
         if (i == 0) snprintf(path, sizeof path, "%s/%sclasses.dex", dir, subdirs[s]);
         else snprintf(path, sizeof path, "%s/%sclasses%d.dex", dir, subdirs[s], i + 1);
         if (!dvm_add_dex(vm, path)) {
            if (i == 0) continue;   /* some APKs start at classes2.dex */
            break;
         }
         ++loaded;
      }
   }
   return loaded;
}

void dvm_set_trace(struct dvm *vm, int level) { if (vm) vm->trace = level; }
uint64_t dvm_instructions(const struct dvm *vm) { return vm ? vm->steps : 0; }
void dvm_set_step_limit(struct dvm *vm, uint64_t limit) { if (vm) vm->step_limit = limit; }
