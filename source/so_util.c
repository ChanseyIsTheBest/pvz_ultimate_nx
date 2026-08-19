#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "mem_arena.h"
#include "so_util.h"
#include "util.h"

so_module *so_module_list = NULL;

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uintptr_t)(a) - 1))
#define PAGE_SIZE 0x1000

/* ------------------------------------------------------------------------ */
/* Trap for unresolved imports. A NULL PLT slot jumps to address 0 and you get
 * a fault with no context; this gives you the symbol name instead. */

static void unresolved_trap(void) {
  /* so_relocate() logged every unresolved name at load time with an
   * "[so] UNRESOLVED <name>" line. Whichever of those the game reached first
   * is the one to implement. */
  fatal_error("an unresolved import was called.\n"
              "Search debug.log for '[so] UNRESOLVED' and add those symbols "
              "to the table in imports.c.");
}

/* ------------------------------------------------------------------------ */

/* On failure, list what IS there. A filename mismatch is otherwise invisible:
 * the file is plainly present on the card and the loader still says no. */
static void list_dir_of(const char *path) {
  char dir[512];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  if (!slash) return;
  *slash = 0;

  DIR *d = opendir(dir);
  if (!d) { debug_log("[so] (cannot list %s either)\n", dir); return; }
  debug_log("[so] contents of %s:\n", dir);
  struct dirent *e;
  while ((e = readdir(d)) != NULL)
    if (strstr(e->d_name, ".so")) debug_log("[so]     %s\n", e->d_name);
  closedir(d);
}

int so_file_load(so_module *mod, const char *path) {
  FILE *f = locked_fopen(path, "rb");

  /* .NET for Android derives the library name from the assembly name, so the
   * assembly Lawn.Android produces libLawn.Android.so -- a DOT, not an
   * underscore. Tooling that copies the file out of an APK often normalises
   * that to an underscore, and the two are easy to mistake for each other at a
   * glance. Try both rather than making the user notice. */
  char alt[512];
  if (!f) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t dirlen = (size_t)(base - path);

    static const char *candidates[] = {
      "libLawn.Android.so",
      "libLawn_Android.so",
      "libLawn.so",
    };
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]) && !f; i++) {
      if (!strcmp(candidates[i], base)) continue;   /* already tried */
      snprintf(alt, sizeof(alt), "%.*s%s", (int)dirlen, path, candidates[i]);
      f = locked_fopen(alt, "rb");
      if (f) { debug_log("[so] %s not found; using %s instead\n", base, candidates[i]); path = alt; }
    }
  }

  if (!f) {
    debug_log("[so] cannot open %s\n", path);
    list_dir_of(path);
    return -1;
  }

  /* Read the header and program table only.
   *
   * The previous version slurped the entire file into a malloc buffer and then
   * copied segments out of it, which meant holding the 41 MB file AND the
   * ~53 MB mapped image at the same time. Streaming each PT_LOAD straight from
   * disk into its final position halves the peak requirement and removes the
   * larger of the two allocations entirely. */
  locked_fseek(f, 0, SEEK_END);
  long fsize = locked_ftell(f);
  locked_fseek(f, 0, SEEK_SET);

  if (fsize <= (long)sizeof(Elf64_Ehdr)) {
    debug_log("[so] %s is only %ld bytes -- truncated or not a library\n", path, fsize);
    locked_fclose(f);
    return -1;
  }

  Elf64_Ehdr ehdr;
  if (locked_fread(&ehdr, 1, sizeof(ehdr), f) != sizeof(ehdr)) {
    debug_log("[so] could not read ELF header from %s\n", path);
    locked_fclose(f);
    return -1;
  }

  if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
      ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
      ehdr.e_machine != EM_AARCH64) {
    debug_log("[so] %s is not an ARM64 ELF64 (class %u machine %u) -- "
              "is this the arm64-v8a build?\n",
              path, ehdr.e_ident[EI_CLASS], ehdr.e_machine);
    locked_fclose(f);
    return -1;
  }

  size_t phsize = (size_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
  Elf64_Phdr *phdrs = malloc(phsize);
  if (!phdrs) {
    debug_log("[so] out of memory for %zu phdr bytes\n", phsize);
    locked_fclose(f);
    return -1;
  }
  locked_fseek(f, (long)ehdr.e_phoff, SEEK_SET);
  if (locked_fread(phdrs, 1, phsize, f) != phsize) {
    debug_log("[so] could not read %u program headers\n", ehdr.e_phnum);
    free(phdrs); locked_fclose(f);
    return -1;
  }

  /* Span of all PT_LOADs. p_vaddr is 0-based in this .so, but do not assume. */
  uintptr_t lo = UINTPTR_MAX, hi = 0;
  for (size_t i = 0; i < ehdr.e_phnum; i++) {
    if (phdrs[i].p_type != PT_LOAD) continue;
    if (phdrs[i].p_vaddr < lo) lo = phdrs[i].p_vaddr;
    uintptr_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
    if (end > hi) hi = end;
  }
  if (lo == UINTPTR_MAX) {
    debug_log("[so] %s has no PT_LOAD segments\n", path);
    free(phdrs); locked_fclose(f);
    return -1;
  }

  mod->load_size = ALIGN_UP(hi - lo, PAGE_SIZE);
  debug_log("[so] %s: %ld MB on disk, needs %zu MB mapped\n",
            path, fsize >> 20, mod->load_size >> 20);

  /* Staging pages. svcMapProcessCodeMemory donates these to the code mapping,
   * so they stay allocated for the module's lifetime. This comes out of
   * newlib's heap, so NEWLIB_HEAP_SIZE in mem_arena.c has to be comfortably
   * larger than the mapped image plus the code pool. */
  /* From the donation zone, not malloc: so_flush_caches hands these very pages
   * to svcMapProcessCodeMemory, after which they fault at this address. */
  mod->load_base = donation_alloc(mod->load_size);
  if (!mod->load_base) {
    debug_log("[so] no donation-zone space for %zu MB of staging\n",
              mod->load_size >> 20);
    free(phdrs); locked_fclose(f);
    return -1;
  }

  /* Stream each PT_LOAD into place. memsz > filesz is BSS and is already
   * zeroed above -- that is what covers .hydrated, which needs no special
   * handling despite its size. */
  for (size_t i = 0; i < ehdr.e_phnum; i++) {
    if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_filesz == 0) continue;
    void *dst = (void *)((uintptr_t)mod->load_base + (phdrs[i].p_vaddr - lo));
    locked_fseek(f, (long)phdrs[i].p_offset, SEEK_SET);
    size_t got = locked_fread(dst, 1, phdrs[i].p_filesz, f);
    if (got != phdrs[i].p_filesz) {
      debug_log("[so] short read on segment %zu: wanted %llu, got %zu\n",
                i, (unsigned long long)phdrs[i].p_filesz, got);
      free(mod->load_base); free(phdrs); locked_fclose(f);
      mod->load_base = NULL;
      return -1;
    }
  }
  locked_fclose(f);

  /* Reserve the destination address space now, but DO NOT map yet.
   *
   * Relocations are applied to load_base while it is still ordinary writable
   * heap. Mapping first would force a blanket RW permission call to make the
   * region writable, and that call transitions the memory state out of
   * AliasCode -- after which RX can never be set. Reserving early just means
   * relocation knows the final address to write into the slots. */
  virtmemLock();
  mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, PAGE_SIZE);
  mod->load_memrv    = virtmemAddReservation(mod->load_virtbase, mod->load_size);
  virtmemUnlock();
  if (!mod->load_virtbase) {
    debug_log("[so] no code-memory address space for %zu MB\n", mod->load_size >> 20);
    free(phdrs);
    return -1;
  }

  mod->vaddr_lo  = lo;
  mod->load_bias = (uintptr_t)mod->load_virtbase - lo;

  /* NO blanket svcSetProcessMemoryPermission here, deliberately.
   *
   * svcMapProcessCodeMemory already leaves the destination user-read-write, so
   * relocation works without one. Worse, setting a code region to RW
   * TRANSITIONS its memory state from AliasCode to AliasCodeData, and
   * SetProcessMemoryPermission requires the FlagCode state -- so a blanket RW
   * call here consumes the one transition available and makes every later
   * permission change fail with InvalidCurrentMemory (0xd401). The symptom is
   * an instruction abort inside init_array, several steps removed from the
   * cause. */

  /* Record phdrs for dl_iterate_phdr. Keep our own copy in normal memory --
   * the mapped image goes read-only shortly and we do not want the runtime
   * following a pointer into a segment whose permissions we later change. */
  mod->phnum = ehdr.e_phnum;
  Elf64_Phdr *phcopy = calloc(mod->phnum, sizeof(Elf64_Phdr));
  memcpy(phcopy, phdrs, mod->phnum * sizeof(Elf64_Phdr));
  mod->phdr = phcopy;

  /* Walk PT_DYNAMIC out of the STAGING copy. The image is not mapped yet, so
   * these pointers are into load_base; so_finalize rebases them once the real
   * mapping exists. */
  uintptr_t base = (uintptr_t)mod->load_base - lo;
  Elf64_Dyn *dyn = NULL;
  for (size_t i = 0; i < ehdr.e_phnum; i++)
    if (phdrs[i].p_type == PT_DYNAMIC)
      dyn = (Elf64_Dyn *)(base + phdrs[i].p_vaddr - lo);

  if (!dyn) { debug_log("[so] no PT_DYNAMIC\n"); free(phdrs); return -1; }

  size_t relasz = 0, relaent = sizeof(Elf64_Rela);
  size_t pltrelsz = 0, syment = sizeof(Elf64_Sym);
  for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
    switch (d->d_tag) {
      case DT_SYMTAB:     mod->dynsym    = (Elf64_Sym  *)(base + d->d_un.d_ptr); break;
      case DT_STRTAB:     mod->dynstrtab = (const char *)(base + d->d_un.d_ptr); break;
      case DT_RELA:       mod->reladyn   = (Elf64_Rela *)(base + d->d_un.d_ptr); break;
      case DT_JMPREL:     mod->relaplt   = (Elf64_Rela *)(base + d->d_un.d_ptr); break;
      case DT_INIT_ARRAY: mod->init_array = (void (**)(void))(base + d->d_un.d_ptr); break;
      case DT_RELASZ:     relasz   = d->d_un.d_val; break;
      case DT_RELAENT:    relaent  = d->d_un.d_val; break;
      case DT_PLTRELSZ:   pltrelsz = d->d_un.d_val; break;
      case DT_SYMENT:     syment   = d->d_un.d_val; break;
      case DT_INIT_ARRAYSZ: mod->num_init_array = d->d_un.d_val / sizeof(void *); break;
      default: break;
    }
  }

  mod->num_reladyn = relaent   ? relasz   / relaent : 0;
  mod->num_relaplt = pltrelsz  ? pltrelsz / sizeof(Elf64_Rela) : 0;

  /* dynsym has no count in the dynamic section; bound it by the start of
   * dynstrtab, which immediately follows it in this layout. */
  if (mod->dynsym && mod->dynstrtab)
    mod->num_dynsym = ((uintptr_t)mod->dynstrtab - (uintptr_t)mod->dynsym) / syment;

  snprintf(mod->name, sizeof(mod->name), "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);

  mod->next = so_module_list;
  so_module_list = mod;

  debug_log("[so] %s staged for %p size 0x%zx (%zu phdrs, %zu dynsym, "
            "%zu rela, %zu jmprel, %zu init)\n",
            mod->name, mod->load_virtbase, mod->load_size, mod->phnum,
            mod->num_dynsym, mod->num_reladyn, mod->num_relaplt,
            mod->num_init_array);

  free(phdrs);
  return 0;
}

/* ------------------------------------------------------------------------ */

/* These two are spelled differently across toolchains (glibc's elf.h, devkitA64's,
 * and the NDK's do not agree), so pin the numeric values rather than depending on
 * whichever header wins. */
#ifndef R_AARCH64_TLS_TPREL64
#define R_AARCH64_TLS_TPREL64 1030
#endif
#ifndef R_AARCH64_TLSDESC
#define R_AARCH64_TLSDESC 1031
#endif

static uintptr_t resolve_import(const char *name, so_module *self,
                                DynLibFunction *funcs, size_t num_funcs) {
  for (size_t i = 0; i < num_funcs; i++)
    if (!strcmp(name, funcs[i].symbol)) return funcs[i].func;

  /* Then any module already loaded.
   *
   * Real libraries have real dependencies on each other: libopenmpt imports
   * 109 C++ symbols that libc++_shared exports, and resolving only against the
   * static table left every one of them trapped. A dynamic loader resolves
   * against the modules it has already mapped, and so must this one now that
   * dlopen can map more than the game itself.
   *
   * Search order is deliberate: our table wins. Those entries are shims chosen
   * on purpose -- a libc function we redirect to the VFS, for instance -- and a
   * real implementation from a loaded module must not silently displace one. */
  for (so_module *m = so_module_list; m; m = m->next) {
    /* Never the module being relocated right now.
     *
     * so_file_load puts a module on the list BEFORE so_relocate runs, so the
     * module currently under relocation is on it too -- and its symbol tables
     * still hold unrelocated pointers. Walking them read a wild address and
     * faulted inside strcmp, part-way through libopenmpt's own relocation.
     *
     * A module also has no business resolving its imports against its own
     * exports: that is what the local symbol table is for, and so_relocate
     * handles it already. */
    if (m == self) continue;
    uintptr_t a = so_symbol(m, name);
    if (a) return a;
  }
  return 0;
}

static void apply_relocs(so_module *mod, Elf64_Rela *rela, size_t n,
                         DynLibFunction *funcs, size_t num_funcs,
                         size_t *unresolved) {
  /* Two different bases, and mixing them up is the whole trick here:
   *   write -- into the staging copy, which is still plain writable heap
   *   base  -- the address the image WILL live at, which is the value that
   *            actually gets stored into each slot */
  uintptr_t write = (uintptr_t)mod->load_base - mod->vaddr_lo;
  uintptr_t base  = mod->load_bias;

  for (size_t i = 0; i < n; i++) {
    uintptr_t *slot = (uintptr_t *)(write + rela[i].r_offset);
    uint32_t   type = ELF64_R_TYPE(rela[i].r_info);
    uint32_t   sym  = ELF64_R_SYM(rela[i].r_info);

    switch (type) {
      case R_AARCH64_RELATIVE:
        *slot = base + (uintptr_t)rela[i].r_addend;
        break;

      case R_AARCH64_ABS64:
      case R_AARCH64_GLOB_DAT:
      case R_AARCH64_JUMP_SLOT: {
        if (!sym || !mod->dynsym) { *slot = 0; break; }
        const char *name = mod->dynstrtab + mod->dynsym[sym].st_name;

        if (mod->dynsym[sym].st_shndx != SHN_UNDEF) {
          /* Defined here. */
          *slot = base + mod->dynsym[sym].st_value + (uintptr_t)rela[i].r_addend;
          break;
        }

        uintptr_t f = resolve_import(name, mod, funcs, num_funcs);
        if (f) {
          *slot = f + (uintptr_t)rela[i].r_addend;
        } else {
          debug_log("[so] UNRESOLVED %s\n", name);
          (*unresolved)++;
          *slot = (uintptr_t)unresolved_trap;
        }
        break;
      }

      case R_AARCH64_TLS_TPREL64:
      case R_AARCH64_TLSDESC:
        /* NativeAOT uses pthread_key_* rather than ELF TLS for its managed
         * thread context, so these should not appear. If one does, the fix is
         * a real TLS block, not a zero. */
        debug_log("[so] TLS reloc type %u at 0x%lx -- unhandled\n",
                  type, (unsigned long)rela[i].r_offset);
        break;

      default:
        debug_log("[so] unknown reloc type %u at 0x%lx\n",
                  type, (unsigned long)rela[i].r_offset);
        break;
    }
  }
}

int so_relocate(so_module *mod, DynLibFunction *funcs, size_t num_funcs) {
  size_t unresolved = 0;
  apply_relocs(mod, mod->reladyn, mod->num_reladyn, funcs, num_funcs, &unresolved);
  apply_relocs(mod, mod->relaplt, mod->num_relaplt, funcs, num_funcs, &unresolved);
  debug_log("[so] relocated; %zu unresolved imports trapped\n", unresolved);
  return 0;
}

int so_flush_caches(so_module *mod) {
  /* Map now, with relocation already done. The destination comes out of
   * svcMapProcessCodeMemory in AliasCode state, and because nothing has asked
   * for RW on it, the per-page permission calls below still work. */
  Result rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(),
                                      (u64)mod->load_virtbase,
                                      (u64)mod->load_base,
                                      mod->load_size);
  if (R_FAILED(rc)) {
    debug_log("[so] svcMapProcessCodeMemory failed: %08x\n", rc);
    return -1;
  }

  uintptr_t base   = (uintptr_t)mod->load_virtbase;
  size_t    npages = mod->load_size >> 12;

  /* Decide per page, then apply in runs. Iterating PT_LOADs directly leaves
   * holes -- this image's segments do not abut, so there are 3-page gaps that
   * would never receive a call. Pages outside any segment default to RW, which
   * is correct: nothing executes there. */
  unsigned char *want_x = calloc(npages, 1);
  if (!want_x) { debug_log("[so] out of memory for the page map\n"); return -1; }

  for (size_t i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type != PT_LOAD) continue;
    if (!(mod->phdr[i].p_flags & PF_X)) continue;
    size_t first = (mod->phdr[i].p_vaddr - mod->vaddr_lo) >> 12;
    size_t last  = (mod->phdr[i].p_vaddr - mod->vaddr_lo
                    + mod->phdr[i].p_memsz + 0xFFF) >> 12;
    for (size_t pg = first; pg < last && pg < npages; pg++) want_x[pg] = 1;
  }

  size_t pg = 0;
  int failures = 0;
  while (pg < npages) {
    unsigned char x = want_x[pg];
    size_t end = pg;
    while (end < npages && want_x[end] == x) end++;

    u64 addr = base + ((u64)pg << 12);
    u64 size = (u64)(end - pg) << 12;
    rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), addr, size,
                                       x ? Perm_Rx : Perm_Rw);
    if (R_FAILED(rc)) {
      debug_log("[so] perm %s on 0x%lx +0x%lx failed: %08x "
                "(module %u, description %u)\n",
                x ? "RX" : "RW", (unsigned long)addr, (unsigned long)size, rc,
                (unsigned)(rc & 0x1FF), (unsigned)((rc >> 9) & 0x1FFF));
      failures++;
    }
    pg = end;
  }
  free(want_x);

  if (failures)
    fatal_error("could not set %d memory permission range(s).\n"
                "Description 106 is InvalidCurrentMemory: the range is not in "
                "a code state. Nothing may request RW on this mapping before "
                "these calls.", failures);

  /* Rebase every retained pointer from the staging copy to the live mapping.
   *
   * svcMapProcessCodeMemory DONATES the source pages: load_base faults on
   * access from here on. Anything still pointing into it -- the symbol table,
   * the string table, init_array -- has to move, or the first lookup after
   * this point faults on memory we no longer own. */
  uintptr_t delta = (uintptr_t)mod->load_virtbase - (uintptr_t)mod->load_base;
  if (mod->dynsym)     mod->dynsym     = (Elf64_Sym *)((uintptr_t)mod->dynsym + delta);
  if (mod->dynstrtab)  mod->dynstrtab  = (const char *)((uintptr_t)mod->dynstrtab + delta);
  if (mod->init_array) mod->init_array = (void (**)(void))((uintptr_t)mod->init_array + delta);
  mod->reladyn = NULL;   /* consumed; pointed into the donated staging pages */
  mod->relaplt = NULL;

  armDCacheFlush(mod->load_virtbase, mod->load_size);
  armICacheInvalidate(mod->load_virtbase, mod->load_size);
  debug_log("[so] mapped at %p, permissions applied across %zu pages\n",
            mod->load_virtbase, npages);
  return 0;
}

void so_initialize(so_module *mod) {
  debug_log("[so] running %zu init_array entries\n", mod->num_init_array);
  for (size_t i = 0; i < mod->num_init_array; i++) {
    if (!mod->init_array[i]) continue;
    debug_log("[so]   init_array[%zu] = %p\n", i, (void *)mod->init_array[i]);
    mod->init_array[i]();
  }
  debug_log("[so] init_array complete\n");
}

uintptr_t so_symbol(so_module *mod, const char *name) {
  if (!mod->dynsym || !mod->dynstrtab) return 0;
  for (size_t i = 0; i < mod->num_dynsym; i++) {
    if (mod->dynsym[i].st_shndx == SHN_UNDEF) continue;
    const char *n = mod->dynstrtab + mod->dynsym[i].st_name;
    if (!strcmp(n, name))
      return mod->load_bias + mod->dynsym[i].st_value;
  }
  return 0;
}

so_module *so_find_module_by_addr(const void *addr) {
  uintptr_t a = (uintptr_t)addr;
  for (so_module *m = so_module_list; m; m = m->next) {
    uintptr_t lo = (uintptr_t)m->load_virtbase;
    if (a >= lo && a < lo + m->load_size) return m;
  }
  return NULL;
}

void so_unload(so_module *mod) {
  if (!mod->load_virtbase) return;
  svcSetProcessMemoryPermission(envGetOwnProcessHandle(),
                                (u64)mod->load_virtbase, mod->load_size, Perm_Rw);
  svcUnmapProcessCodeMemory(envGetOwnProcessHandle(),
                            (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();
  free((void *)mod->phdr);
  mod->load_virtbase = NULL;
}
