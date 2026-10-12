/* MPL-2.0. Shared Java names for primitive JVM/dex descriptors. */
#ifndef LUNARIA_JAVA_TYPE_H
#define LUNARIA_JAVA_TYPE_H
#include <stddef.h>
static inline const char *luna_java_primitive_name(const char *desc)
{
   if (!desc || !desc[0] || desc[1]) return NULL;
   switch (desc[0]) {
   case 'Z': return "boolean";
   case 'B': return "byte";
   case 'C': return "char";
   case 'S': return "short";
   case 'I': return "int";
   case 'J': return "long";
   case 'F': return "float";
   case 'D': return "double";
   case 'V': return "void";
   default: return NULL;
   }
}
#endif
