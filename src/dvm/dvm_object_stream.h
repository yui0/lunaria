/* Copyright © 2026 Project Lunaria
 * SPDX-License-Identifier: MPL-2.0
 * Private C implementation of Java serialization string records. */
#ifndef DVM_OBJECT_STREAM_H
#define DVM_OBJECT_STREAM_H

static const struct rt_field rt_object_stream_fields[] = {
   { "handles", "Ljava/util/ArrayList;" }, F_END
};

static dvm_ref object_stream_handles(struct dvm *vm, dvm_ref stream)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, stream, "handles", "Ljava/util/ArrayList;", &v);
   if (!v.l) {
      v.l = coll_new_list(vm, "Ljava/util/ArrayList;");
      (void)dvm_set_field(vm, stream, "handles", "Ljava/util/ArrayList;", v);
   }
   return v.l;
}

static bool object_stream_write(struct dvm *vm, dvm_ref stream, dvm_ref value,
                                union dvm_value *out)
{
   if (!value) return data_write_be(vm, stream, 0x70, 1, out);
   struct dvm_object *object = dvm__obj(vm, value);
   if (!object || object->kind != DVM_OBJ_STRING) {
      dvm__throw(vm, "java/io/NotSerializableException", "%s",
                 object && object->cls ? object->cls->name : "unknown object");
      return false;
   }
   dvm_ref handles = object_stream_handles(vm, stream);
   struct rt_list *list = list_of(vm, handles);
   if (!list) return false;
   for (uint32_t i = 0; i < list->size; ++i)
      if (list->items[i] == value) {
         union dvm_value ignored;
         return data_write_be(vm, stream, 0x71, 1, &ignored) &&
                data_write_be(vm, stream, 0x7e0000u + i, 4, out);
      }
   /* Java uses modified UTF-8: NUL has two bytes and supplementary
    * characters are encoded as two three-byte UTF-16 surrogates. */
   struct rt_bytes encoded = { 0 };
   for (size_t i = 0; i < object->utf8_len; ) {
      uint32_t cp = utf8_next(object->utf8, object->utf8_len, &i);
      uint16_t units[2];
      unsigned count = 1;
      if (cp > 0xffff) {
         cp -= 0x10000;
         units[0] = (uint16_t)(0xd800u + (cp >> 10));
         units[1] = (uint16_t)(0xdc00u + (cp & 0x3ff));
         count = 2;
      } else units[0] = (uint16_t)cp;
      for (unsigned j = 0; j < count; ++j) {
         char bytes[3];
         size_t n = units[j] ? utf8_put_unit(bytes, units[j]) : 2;
         if (!units[j]) { bytes[0] = (char)0xc0; bytes[1] = (char)0x80; }
         if (!bytes_append(&encoded, bytes, n)) { free(encoded.p); return false; }
      }
   }
   if (encoded.len > INT32_MAX) { free(encoded.p); return false; }
   union dvm_value ignored, arg = { 0 };
   bool large = encoded.len >= 65536;
   bool ok = data_write_be(vm, stream, large ? 0x7c : 0x74, 1, &ignored) &&
             data_write_be(vm, stream, encoded.len, large ? 8 : 2, &ignored);
   if (ok) {
      arg.l = dvm_new_array(vm, 'B', "B", (uint32_t)encoded.len);
      void *data = arg.l ? dvm_array_data(vm, arg.l) : NULL;
      if (data && encoded.len) memcpy(data, encoded.p, encoded.len);
      dvm_pin(vm, arg.l);
      ok = arg.l && st_write_array(vm, stream, &arg, 1, &ignored);
      dvm_unpin(vm, arg.l);
   }
   free(encoded.p);
   if (!ok) return false;
   arg.l = value;
   return list_add(vm, handles, &arg, 1, &ignored) && (out->l = 0, true);
}

static bool object_stream_read(struct dvm *vm, dvm_ref stream,
                               union dvm_value *out)
{
   uint64_t token, length;
   dvm_ref handles = object_stream_handles(vm, stream);
   struct rt_list *list = list_of(vm, handles);
   if (!list) return false;
   /* Reset records may repeat; consume them without recursive C frames. */
   for (;;) {
      if (!data_read_be(vm, stream, 1, &token)) return false;
      if (token != 0x79) break;
      union dvm_value ignored;
      if (!list_clear(vm, handles, NULL, 0, &ignored)) return false;
   }
   if (token == 0x70) { out->l = 0; return true; }
   if (token == 0x71) {
      if (!data_read_be(vm, stream, 4, &length)) return false;
      if (length < 0x7e0000u || length - 0x7e0000u >= list->size) {
         dvm__throw(vm, "java/io/StreamCorruptedException", "invalid object handle");
         return false;
      }
      out->l = list->items[length - 0x7e0000u];
      return true;
   }
   if (token != 0x74 && token != 0x7c) {
      dvm__throw(vm, "java/io/InvalidClassException", "unsupported object token %02X", (unsigned)token);
      return false;
   }
   if (!data_read_be(vm, stream, token == 0x74 ? 2 : 8, &length)) return false;
   if (length > INT32_MAX) {
      dvm__throw(vm, "java/io/StreamCorruptedException", "invalid string length");
      return false;
   }
   dvm_ref bytes = dvm_new_array(vm, 'B', "B", (uint32_t)length);
   if (!bytes) return false;
   dvm_pin(vm, bytes);
   union dvm_value arg = { .l = bytes }, ignored;
   if (!data_read_fully(vm, stream, &arg, 1, &ignored)) { dvm_unpin(vm, bytes); return false; }
   const uint8_t *data = dvm_array_data(vm, bytes);
   uint16_t *units = malloc(((size_t)length + 1) * sizeof *units);
   char *text = malloc((size_t)length + 1);
   if (!units || !text) { free(units); free(text); dvm_unpin(vm, bytes); return false; }
   size_t used = 0;
   bool valid = true;
   for (size_t i = 0; i < length; ) {
      unsigned c = data[i++];
      if (c < 0x80) units[used++] = (uint16_t)c;
      else {
         unsigned extra = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 0;
         if (!extra || extra > length - i) { valid = false; break; }
         unsigned value = c & (extra == 1 ? 0x1f : 0x0f);
         for (unsigned j = 0; j < extra; ++j) {
            unsigned next = data[i++];
            if ((next & 0xc0) != 0x80) { valid = false; break; }
            value = (value << 6) | (next & 0x3f);
         }
         if (!valid) break;
         units[used++] = (uint16_t)value;
      }
   }
   if (valid) {
      size_t size = jcs_utf16_to_wtf8(units, used, text);
      arg.l = dvm_new_string_n(vm, text, size);
   }
   free(units); free(text); dvm_unpin(vm, bytes);
   if (!valid) {
      dvm__throw(vm, "java/io/UTFDataFormatException", "malformed serialized string");
      return false;
   }
   if (!arg.l || !list_add(vm, handles, &arg, 1, &ignored)) return false;
   out->l = arg.l;
   return true;
}
#endif
