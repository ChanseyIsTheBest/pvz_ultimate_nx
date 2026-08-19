#ifndef PVZU_MEM_ARENA_H
#define PVZU_MEM_ARENA_H

#include <stddef.h>

typedef enum {
  HEAP_NONE = 0,
  HEAP_OVERRIDE,   /* hbloader handed us a heap through the homebrew ABI */
  HEAP_SETSIZE,    /* we claimed one ourselves with svcSetHeapSize       */
  HEAP_FALLBACK    /* neither worked; static buffer, nothing will run    */
} HeapMode;

extern HeapMode g_heap_mode;
extern size_t   g_heap_total;
extern size_t   g_gfx_returned;
extern int      g_gfx_base_moved;

/* Populated by __libnx_initheap (defined in mem_arena.c) before main runs. */
extern void  *g_arena_base;
extern size_t g_arena_size;
extern void  *g_newlib_base;
extern size_t g_newlib_size;

/* Build the page table and map the executable pool. Call after log_init so the
 * diagnostics are visible. Returns -1 if no arena was claimed, which in
 * practice means applet mode. */
int  arena_init(void);

/* Page-aligned memory for anything that will be handed to
 * svcMapProcessCodeMemory. Never allocate that from malloc: the syscall
 * donates the pages and they fault at the source address afterwards, while
 * the allocator still thinks it owns them. Never freed. */
void *donation_alloc(size_t size);

/* Make the thunk block containing addr executable, if it is one and it is not
 * already. Blocks are kept writable while the runtime generates stubs into
 * them, so this must run before the first call through such a pointer. */
int code_ensure_executable(const void *addr);
extern void  *g_donate_base;
extern size_t g_donate_size;

/* Reserved / committed / peak, plus code pool usage. Worth calling on the same
 * cadence as gc_watch_report -- the two together tell you whether the GC is
 * behaving or thrashing. */
void arena_report(void);

/* newlib heap consumption. Called by arena_report, and worth calling
 * directly around anything that allocates heavily. */
void heap_report(void);

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset);
int   munmap_fake(void *addr, size_t length);
int   mprotect_fake(void *addr, size_t len, int prot);
int   madvise_fake(void *addr, size_t len, int advice);
int   mlock_fake(void *addr, size_t len);
int   munlock_fake(void *addr, size_t len);

#endif
