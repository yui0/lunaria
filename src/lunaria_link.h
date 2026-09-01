/* ELF debugger structures used by the Android loader on non-ELF hosts. */
#ifndef LUNARIA_LINK_H
#define LUNARIA_LINK_H

#include <elf.h>
#include <stdint.h>

#if defined(__APPLE__)
# if UINTPTR_MAX > 0xffffffffu
#  define ElfW(type) Elf64_##type
# else
#  define ElfW(type) Elf32_##type
# endif
struct link_map {
   ElfW(Addr) l_addr;
   char *l_name;
   ElfW(Dyn) *l_ld;
   struct link_map *l_next, *l_prev;
};
struct dl_phdr_info {
   ElfW(Addr) dlpi_addr;
   const char *dlpi_name;
   const ElfW(Phdr) *dlpi_phdr;
   ElfW(Half) dlpi_phnum;
};
enum { RT_CONSISTENT, RT_ADD, RT_DELETE };
struct r_debug {
   int r_version;
   struct link_map *r_map;
   ElfW(Addr) r_brk;
   int r_state;
   ElfW(Addr) r_ldbase;
};
#else
# include <link.h>
#endif
#endif
