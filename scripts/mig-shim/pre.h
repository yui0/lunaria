/* Shim for building Apple migcom on Linux: stand-ins for Darwin-only macros. */
#include <stdint.h>
#include <sys/types.h>
#include <stddef.h>
#define __kernel_ptr_semantics
#define __DARWIN_ALIGN32(x) 
#define __DARWIN_ALIGN(x)
#define __AVAILABILITY_INTERNAL__
#ifndef __APPLE__
#define __APPLE__ 1
#endif
#define __MACH__ 1
typedef unsigned int __darwin_natural_t;
#define __enum_decl(name, type, ...) typedef type name; enum __VA_ARGS__
#define __options_decl(name, type, ...) typedef type name; enum __VA_ARGS__
#define __enum_closed
