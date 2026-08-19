/* so_util.h -- minimal ARM64 ELF shared-object loader for Horizon.
 *
 * Differs from the loaders in the GameMaker/Unity reference ports in one way
 * that matters: it records the program header table after mapping, because
 * NativeAOT locates its own image through dl_iterate_phdr(). See dl_shim.c.
 */
#ifndef PVZU_SO_UTIL_H
#define PVZU_SO_UTIL_H

#include <elf.h>
#include <stddef.h>
#include <stdint.h>
#include <switch.h>

typedef struct {
  const char *symbol;   /* undefined symbol name in the target .so */
  uintptr_t   func;     /* address of our replacement                */
} DynLibFunction;

typedef struct so_module {
  struct so_module *next;

  char name[64];

  /* Staging buffer -- the pages donated to svcMapProcessCodeMemory. Must not
   * be freed while the module is mapped. */
  void  *load_base;
  /* Address the image is executable at. This is the relocation bias: for this
   * .so the first PT_LOAD has p_vaddr 0, so vaddr + load_virtbase == real. */
  void  *load_virtbase;
  size_t load_size;

  /* Lowest p_vaddr across PT_LOADs, and the bias such that
   * vaddr + load_bias == final address. Kept because relocation happens before
   * mapping: it writes into load_base but stores load_bias-relative values. */
  uintptr_t vaddr_lo;
  uintptr_t load_bias;
  VirtmemReservation *load_memrv;

  /* Program headers, copied out of the mapped image. dl_iterate_phdr() hands
   * these to the runtime verbatim. */
  const Elf64_Phdr *phdr;
  size_t            phnum;

  /* Dynamic section pieces used for symbol lookup and dladdr(). This .so is
   * stripped: dynsym has 3 usable FUNC entries, so dladdr() can resolve the
   * module base reliably and a symbol name only rarely. That is sufficient --
   * NativeAOT wants dli_fbase far more than it wants dli_sname. */
  Elf64_Sym  *dynsym;
  size_t      num_dynsym;
  const char *dynstrtab;

  Elf64_Rela *reladyn;  size_t num_reladyn;
  Elf64_Rela *relaplt;  size_t num_relaplt;

  void (**init_array)(void);
  size_t  num_init_array;
} so_module;

/* Head of the loaded-module list. dl_shim walks this. */
extern so_module *so_module_list;

/* Read <path> into a staging buffer, reserve code memory, map, and record
 * phdrs. Does not relocate. Returns 0 on success. */
int so_file_load(so_module *mod, const char *path);

/* Apply RELA + JMPREL. Undefined symbols are looked up in <funcs>; anything
 * unresolved is pointed at a trap that logs the name and aborts, so a missing
 * shim surfaces as a readable message rather than a jump to zero. */
int so_relocate(so_module *mod, DynLibFunction *funcs, size_t num_funcs);

/* Apply final segment permissions (RX for text, RW for data). Call after
 * so_relocate. */
int so_flush_caches(so_module *mod);

/* Run DT_INIT_ARRAY. */
void so_initialize(so_module *mod);

/* Look up an exported symbol. For this .so the useful ones are JNI_OnLoad,
 * JNI_OnUnload and Java_net_dot_jni_nativeaot_JavaInteropRuntime_init. */
uintptr_t so_symbol(so_module *mod, const char *name);

/* Which module contains <addr>, or NULL. Used by dladdr() and the fault
 * handler to print "libLawn.Android.so+0xOFFSET". */
so_module *so_find_module_by_addr(const void *addr);

void so_unload(so_module *mod);

#endif /* PVZU_SO_UTIL_H */
