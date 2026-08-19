#ifndef PVZU_DL_SHIM_H
#define PVZU_DL_SHIM_H

#include <elf.h>
#include <stddef.h>

/* These must match bionic/glibc layout exactly -- the runtime indexes them by
 * offset. Defined here rather than in the .c so the declarations below can
 * name them and the compiler can check the definitions against these. */
struct dl_phdr_info_compat {
  Elf64_Addr         dlpi_addr;      /* relocation bias, not lowest address */
  const char        *dlpi_name;
  const Elf64_Phdr  *dlpi_phdr;
  Elf64_Half         dlpi_phnum;
  unsigned long long dlpi_adds;
  unsigned long long dlpi_subs;
  size_t             dlpi_tls_modid;
  void              *dlpi_tls_data;
};

typedef struct {
  const char *dli_fname;
  void       *dli_fbase;
  const char *dli_sname;
  void       *dli_saddr;
} Dl_info_compat;

typedef int (*phdr_cb)(struct dl_phdr_info_compat *info, size_t size, void *data);

int   dl_iterate_phdr_fake(phdr_cb cb, void *data);
int   dladdr_fake(const void *addr, Dl_info_compat *info);
void *dlopen_fake(const char *name, int flag);
void *dlsym_fake(void *handle, const char *symbol);
int   dlclose_fake(void *handle);
const char *dlerror_fake(void);

/* Implemented in imports.c, where the static versions already live. Exposed
 * so dlsym can hand the same functions to the managed P/Invoke path. */
void *dlsym_android_log_print(void);
void *dlsym_android_log_vprint(void);
void *dlsym_android_log_write(void);

/* Resolve a symbol from the loader's import table (imports.c). */
void *imports_lookup(const char *name);

#endif
