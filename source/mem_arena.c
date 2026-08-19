/* mem_arena.c -- mmap/mprotect over a private arena.
 *
 * Why this cannot just forward to malloc.
 *
 * The .NET GC does not allocate its heap. It RESERVES a large address range up
 * front (mmap with PROT_NONE and MAP_NORESERVE -- typically the whole
 * GCHeapHardLimit), then COMMITS subranges on demand (mprotect to READ|WRITE)
 * and DECOMMITS them again when a generation shrinks (mprotect back to
 * PROT_NONE, or madvise MADV_DONTNEED). A malloc-backed mmap makes the reserve
 * either fail outright or consume the whole budget immediately, and either way
 * the runtime is dead before it allocates a single managed object.
 *
 * So the arena tracks two things per page, independently:
 *   reserved  -- address space is spoken for; nobody else may hand it out
 *   committed -- the page is in use and its contents must be preserved
 *
 * Horizon has no demand paging, so committed pages are not physically distinct
 * from reserved ones -- the whole arena is real memory from the moment
 * __libnx_initheap runs. The distinction is bookkeeping. It still matters,
 * because it is what lets a 768 MB reservation coexist with a 64 MB working set
 * inside a 1 GB arena instead of exhausting it.
 *
 * Executable memory is a separate problem with a separate pool -- see the
 * PROT_EXEC discussion at code_arena below.
 */

#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#include "mem_arena.h"
#include "util.h"

/* mmap prot bits (bionic values -- the game passes these, not ours). */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* mmap flags */
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_NORESERVE  0x4000

#define MAP_FAILED ((void *)-1)

/* madvise */
#define MADV_DONTNEED 4
#define MADV_FREE     8

#define PAGE_SHIFT 12
#define PAGE_SIZE  (1u << PAGE_SHIFT)

/* Per-page flags. */
#define PG_RESERVED  0x01
#define PG_COMMITTED 0x02

/* Set by __libnx_initheap before anything else runs. */
size_t g_heap_total;
void  *g_donate_base;
size_t g_donate_size;
static size_t g_donate_used;
size_t g_gfx_returned;
int    g_gfx_base_moved;
u64    g_alias_size;
u64    g_system_res;
void  *g_arena_base;
size_t g_arena_size;
void  *g_newlib_base;
size_t g_newlib_size;

static uint8_t *g_pageflags;
static size_t   g_npages;
static Mutex    g_lock;

static size_t g_stat_reserved_pages;
static size_t g_stat_committed_pages;
static size_t g_stat_peak_committed;

/* ------------------------------------------------------------------------ */
/* Heap split                                                                */
/* ------------------------------------------------------------------------ */

/* newlib's heap has to cover more than bookkeeping. The big consumers, in
 * order:
 *
 *   ~53 MB  the game image's staging buffer -- so_util allocates the whole
 *           mapped span here and donates it to svcMapProcessCodeMemory
 *    16 MB  the executable pool below
 *   ~1 MB   the arena's own page bitmap (one byte per 4 KB page)
 *           plus thread stacks, fake JNI objects, string conversions
 *
 * 48 MB was not enough for the first of those alone, and the failure looked
 * like "cannot load the .so" rather than "out of memory". 256 MB leaves ample
 * headroom and still gives the arena ~2.9 GB, which is far more than
 * DOTNET_GCHeapHardLimit asks for. */
/* Raised from 96 MB after a 16 KB calloc failed here.
 *
 * Our own bookkeeping is small -- the page bitmap, reference tables, thread
 * stacks and fake JNI objects come to a few MB. What is not small is the
 * runtime's: NativeAOT mallocs for its type loader, reflection metadata and
 * image hydration, and none of that goes through the arena. The arena has
 * ~2.4 GB left after this, far more than DOTNET_GCRegionRange asks for, so
 * the memory is better spent here than reserved for a GC that will not use
 * it. */
/* Raised from 256 MB, on evidence rather than to be generous.
 *
 * The GPU driver allocates through this heap. mesa/nouveau on Switch backs its
 * buffer objects with process memory, so when newlib is full an OpenGL upload
 * fails -- and the game does not check glGetError, so it fails silently. The
 * chain that produced was: heap at 252 of 256 MB, GL_OUT_OF_MEMORY on the next
 * texture upload, the RSB's DecompressionTask reporting gpuDecompressed=0, and
 * then nouveau dereferencing a null buffer object in pushbuf_kref.
 *
 * That looked like four separate faults across several rounds and was one:
 * the heap ran out, and everything downstream reported it in its own vocabulary.
 *
 * The room comes from the arena, which reserves 1029 MB and has never
 * committed more than 134 MB. Reserved address space it does not use is worth
 * less than heap the renderer does. */
/* Sized from the arena end, not as a fixed constant.
 *
 * The constant was raised 96 -> 256 -> 768 MB over three rounds, and each
 * raise meant re-checking the arithmetic against the arena by hand. It also
 * created a cliff: a pool one byte under NEWLIB + DONATION + 64 MB fell all
 * the way back to "newlib takes everything and the GC gets nothing".
 *
 * Reserve what the arena must have, cap what newlib may take, and let the
 * split fall out. Evidence for the floor, measured on the run that produced
 * this change:
 *
 *     [arena] reserve 1024 MB          <- DOTNET_GCRegionRange = 0x40000000
 *     [arena] reserve 1 MB, reserve 4 MB   <- every non-GC mmap_fake, total
 *     [arena] reserved 1029 MB, committed 477 MB (peak 477 MB) of 1960 MB
 *
 * So the arena must be able to RESERVE 1029 MB, and never committed half of
 * that. 1152 MB covers the reservation with 123 MB spare. The 768 MB that was
 * left over beyond the reservation was doing nothing, and the renderer needs
 * it: the last run ended at 767 of 768 MB of newlib with GL_OUT_OF_MEMORY on
 * a texture upload.
 *
 * The cap exists because this is not yet known to be a bounded appetite. If a
 * run reaches the cap too, the answer is to reduce what the game caches, not
 * to raise this again -- see the texture accounting in gl_guard.c, which says
 * how much of the heap is textures. */
#define ARENA_FLOOR      (1152ull * 1024 * 1024)
#define NEWLIB_HEAP_MAX  (1536ull * 1024 * 1024)
#define NEWLIB_HEAP_MIN  (256ull  * 1024 * 1024)

/* Memory that will be DONATED to svcMapProcessCodeMemory, kept out of newlib's
 * reach entirely.
 *
 * This is not tidiness. svcMapProcessCodeMemory takes ownership of the source
 * pages and makes them fault at the source address -- but malloc still has
 * them on its books, still has chunk headers adjacent to them, and will walk
 * into them on the next large or aligned request. That is precisely what
 * happened: the 16 MB code pool was memalign'd from newlib, donated, and then
 * the 53 MB allocation for the game image faulted inside the donated range.
 *
 * Anything destined for donation comes from here instead. Nothing is ever
 * freed -- donated memory cannot be reclaimed anyway. Sized for the game image
 * (~53 MB), the code pool (16 MB), and room for a second library later. */
#define DONATION_ZONE_SIZE (192ull * 1024 * 1024)

/* Executable memory is no longer pre-reserved. Each thunk block gets its own
 * mapping when the runtime asks for one, because a block has to be remapped
 * individually to regain the AliasCode state that RX requires -- see
 * code_make_executable. Blocks come out of the donation zone, so that is what
 * bounds the total. */

/* Memory handed BACK to the system before we take the rest.
 *
 * Under title override hbloader has already committed essentially all of the
 * process's memory to the process. switch-mesa allocates its framebuffers and
 * GPU command buffers through nvservices, which draws from the SYSTEM pool --
 * so if we keep everything, EGL initialisation fails at Stage 5 with nothing
 * obviously wrong in our own code.
 *
 * Leaving the tail of the heap merely unused does not help: the memory is
 * already committed to us. It has to be returned with svcSetHeapSize.
 *
 * (Credit for this one goes to the Hitman GO port, which documents it after
 * evidently losing time to it.) */
#define GFX_RESERVE_SIZE (256ull * 1024 * 1024)

/* Never shrink below this much usable heap, whatever the arithmetic says. */
#define HEAP_FLOOR (1024ull * 1024 * 1024)

/* Recorded here rather than logged, because this runs before newlib, before
 * the SD card is mounted, and before anything can open a file. arena_init()
 * reports it once logging exists. */
HeapMode g_heap_mode = HEAP_NONE;

void __libnx_initheap(void) {
  /* Only needs to be big enough to reach main() and write one diagnosis. An
   * 8 MB static buffer here just bloats the NRO's BSS for a path that should
   * never run. */
  static char fallback[1024 * 1024];
  extern char *fake_heap_start, *fake_heap_end;

  void  *addr = NULL;
  size_t size = 0;

  /* THE IMPORTANT PART.
   *
   * Under hbloader the heap is already allocated: hbloader reserved it, mapped
   * our NRO's code into it, and passes the remainder to us through the
   * homebrew ABI. Calling svcSetHeapSize() in that situation resizes the
   * region our own code is executing from -- which is why the previous version
   * died with a User Break during libnx startup, before a single thread
   * existed. libnx's own __libnx_initheap checks this first, and so must any
   * replacement for it. */
  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
    g_heap_mode = HEAP_OVERRIDE;

    /* Give GFX_RESERVE_SIZE back to the system pool so mesa can allocate.
     *
     * The arithmetic must be relative to the HEAP BASE, not to the override
     * address. hbloader loads the NRO itself out of the low end of the heap,
     * so the override region starts ABOVE the base, and svcSetHeapSize sizes
     * the whole region from the base. Computing the new size from `addr` would
     * shrink past the NRO and unmap the code currently executing. */
    u64 heap_base = 0;
    if (R_SUCCEEDED(svcGetInfo(&heap_base, InfoType_HeapRegionAddress,
                               CUR_PROCESS_HANDLE, 0)) &&
        heap_base && (uintptr_t)addr >= (uintptr_t)heap_base) {

      size_t head      = (uintptr_t)addr - (uintptr_t)heap_base;  /* NRO etc. */
      size_t cur_total = head + size;

      if (cur_total > GFX_RESERVE_SIZE &&
          cur_total - GFX_RESERVE_SIZE > head + HEAP_FLOOR) {

        size_t want_total = (cur_total - GFX_RESERVE_SIZE) & ~0x1FFFFFull;
        void  *new_base   = NULL;

        if (R_SUCCEEDED(svcSetHeapSize(&new_base, want_total))) {
          if ((uintptr_t)new_base == (uintptr_t)heap_base) {
            size = ((uintptr_t)heap_base + want_total) - (uintptr_t)addr;
            g_gfx_returned = GFX_RESERVE_SIZE;
          } else {
            /* The base moved, so `addr` now points into memory we no longer
             * own -- and the heap has ALREADY shrunk, so simply declining the
             * result is not enough. Put it back before anything touches it. */
            void *restore = NULL;
            svcSetHeapSize(&restore, cur_total);
            g_gfx_base_moved = 1;
          }
        }
        /* On failure addr/size stay exactly as hbloader gave them. A smaller
         * heap is adopted only when the shrink succeeded AND the base held. */
      }
    }

    /* Diagnostics for arena_init to log. On-demand commit through
     * svcMapPhysicalMemory would let a reservation cost no physical memory,
     * which is what .NET's GC really wants -- but it needs a system resource
     * pool that a title-override process does not have, so it always fails
     * with InvalidState. Recorded here so the numbers are visible rather than
     * inviting someone to try it again. */
    svcGetInfo(&g_alias_size,  InfoType_AliasRegionSize,        CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&g_system_res,  InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  } else {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used,  InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);

    /* svcSetHeapSize wants a 2 MB multiple; leave 2 MB of headroom. */
    if (total > used + 0x200000)
      size = (total - used - 0x200000) & ~0x1FFFFFull;

    if (size > 0 && R_SUCCEEDED(svcSetHeapSize(&addr, size))) {
      g_heap_mode = HEAP_SETSIZE;
    } else {
      addr = fallback;
      size = sizeof(fallback);
      g_heap_mode = HEAP_FALLBACK;
    }
  }

  g_heap_total = size;

  /* Split newlib's heap from the GC arena. In fallback mode there is nothing
   * to split -- give newlib everything and let arena_init report the problem
   * rather than handing the GC a few hundred KB and failing mysteriously. */
  size_t newlib = 0;
  if (size > DONATION_ZONE_SIZE + ARENA_FLOOR)
    newlib = size - DONATION_ZONE_SIZE - ARENA_FLOOR;
  if (newlib > NEWLIB_HEAP_MAX) newlib = NEWLIB_HEAP_MAX;

  if (g_heap_mode == HEAP_FALLBACK || newlib < NEWLIB_HEAP_MIN) {
    g_newlib_base = addr;
    g_newlib_size = size;
    g_donate_base = NULL;
    g_donate_size = 0;
    g_arena_base  = NULL;
    g_arena_size  = 0;
  } else {
    /* newlib | donation zone | arena, in that order. */
    g_newlib_base = addr;
    g_newlib_size = newlib;
    g_donate_base = (char *)addr + newlib;
    g_donate_size = DONATION_ZONE_SIZE;
    g_arena_base  = (char *)g_donate_base + DONATION_ZONE_SIZE;
    g_arena_size  = size - newlib - DONATION_ZONE_SIZE;
  }

  fake_heap_start = (char *)g_newlib_base;
  fake_heap_end   = (char *)g_newlib_base + g_newlib_size;
}

/* ------------------------------------------------------------------------ */
/* Executable memory                                                         */
/* ------------------------------------------------------------------------ */

/* The problem, stated exactly.
 *
 * svcMapProcessCodeMemory leaves the destination in AliasCode state and NOT
 * writable. To store a thunk we must call svcSetProcessMemoryPermission for
 * RW, which moves the range to AliasCodeData -- and that transition is
 * one-way. The later request for RX then fails with InvalidCurrentMemory,
 * which is what made ThunkBlocks.GetNewThunksBlock throw
 * PlatformNotSupportedException and took the whole Java.Interop bootstrap with
 * it.
 *
 * The way out is to give the range a fresh state rather than a second
 * transition. Unmapping returns the donated source pages to us; remapping
 * produces AliasCode again, at the same virtual address, with RX available.
 * The contents survive because we copy them out first.
 *
 * Each block is therefore its own mapping rather than a slice of one pool --
 * blocks become executable independently, and remapping one must not disturb
 * another. */
typedef struct CodeBlock {
  struct CodeBlock *next;
  void   *dst;      /* the address the runtime holds                        */
  void   *src;      /* donated pages, inaccessible while mapped             */
  size_t  size;
  VirtmemReservation *rv;
  bool    executable;
} CodeBlock;

static CodeBlock *g_code_blocks;
static size_t     g_code_total;

static CodeBlock *code_block_of(const void *p) {
  for (CodeBlock *b = g_code_blocks; b; b = b->next)
    if ((uintptr_t)p >= (uintptr_t)b->dst &&
        (uintptr_t)p <  (uintptr_t)b->dst + b->size) return b;
  return NULL;
}

static int in_code_pool(const void *p) { return code_block_of(p) != NULL; }

static void *code_alloc(size_t len) {
  len = (len + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

  CodeBlock *b = calloc(1, sizeof(CodeBlock));
  if (!b) return NULL;

  /* From the donation zone: these pages are handed to the kernel and become
   * inaccessible here, which malloc must never be allowed to hand out again. */
  b->src  = donation_alloc(len);
  b->size = len;
  if (!b->src) { free(b); return NULL; }

  virtmemLock();
  b->dst = virtmemFindCodeMemory(len, PAGE_SIZE);
  b->rv  = virtmemAddReservation(b->dst, len);
  virtmemUnlock();
  if (!b->dst) { free(b); return NULL; }

  Result rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(),
                                      (u64)b->dst, (u64)b->src, len);
  if (R_FAILED(rc)) {
    debug_log("[arena] code map failed: %08x\n", rc);
    free(b);
    return NULL;
  }
  /* Writable now; the state cost of this is undone by the remap below. */
  svcSetProcessMemoryPermission(envGetOwnProcessHandle(), (u64)b->dst, len, Perm_Rw);

  b->next = g_code_blocks;
  g_code_blocks = b;
  g_code_total += len;
  debug_log("[arena] code block %p +%zu KB writable (%zu KB total)\n",
            b->dst, len >> 10, g_code_total >> 10);
  return b->dst;
}

/* Make a block executable by rebuilding its mapping, and split it.
 *
 * A thunk mapping is not uniformly code. NativeAOT lays it out as stubs in the
 * first half and per-thunk context data in the second, and the runtime keeps
 * writing that data long after the stubs are final -- so making the whole
 * block read-execute faults on the next context write, half a block up from
 * where the stubs live. That is precisely the fault we saw at base + 0x8000
 * of a 64 KB block.
 *
 * So the halves get different permissions. That is only possible because the
 * remap gives the whole range a fresh AliasCode state first: each half then
 * has its own single transition available, which is the one thing this
 * platform allows. Doing it without the remap would fail on the second half
 * for the same reason everything else has.
 *
 * The split is at the midpoint, which is what the observed fault offset says.
 * If a future fault lands inside a block at some other boundary, this constant
 * is the thing to revisit. */
static int code_remap(CodeBlock *b, bool executable) {
  void *tmp = malloc(b->size);
  if (!tmp) { debug_log("[arena] no memory to stage a code block\n"); return -1; }
  memcpy(tmp, b->dst, b->size);

  Result rc = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(),
                                        (u64)b->dst, (u64)b->src, b->size);
  if (R_FAILED(rc)) {
    debug_log("[arena] code unmap failed: %08x\n", rc);
    free(tmp);
    return -1;
  }

  memcpy(b->src, tmp, b->size);   /* accessible again once unmapped */
  free(tmp);

  rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(),
                               (u64)b->dst, (u64)b->src, b->size);
  if (R_FAILED(rc)) {
    debug_log("[arena] code remap failed: %08x -- %p is now unmapped\n",
              rc, b->dst);
    return -1;
  }

  if (!executable) {
    rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(),
                                       (u64)b->dst, b->size, Perm_Rw);
    if (R_FAILED(rc)) { debug_log("[arena] RW refused: %08x\n", rc); return -1; }
    b->executable = false;
    debug_log("[arena] code block %p -> writable (remapped)\n", b->dst);
    return 0;
  }

  size_t code_half = (b->size / 2) & ~(size_t)(PAGE_SIZE - 1);
  if (code_half == 0) code_half = b->size;

  rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(),
                                     (u64)b->dst, code_half, Perm_Rx);
  if (R_FAILED(rc)) {
    debug_log("[arena] RX refused on the stub half: %08x\n", rc);
    return -1;
  }

  if (code_half < b->size) {
    /* The context half stays writable. A failure here is not fatal on its own
     * -- the stubs can still run -- but the next context write will fault, so
     * say so rather than letting it look like a fresh mystery. */
    Result rc2 = svcSetProcessMemoryPermission(
        envGetOwnProcessHandle(), (u64)((char *)b->dst + code_half),
        b->size - code_half, Perm_Rw);
    if (R_FAILED(rc2))
      debug_log("[arena] could not keep the data half of %p writable: %08x -- "
                "expect a write fault around +0x%zx\n",
                b->dst, rc2, code_half);
  }

  armDCacheFlush(b->dst, code_half);
  armICacheInvalidate(b->dst, code_half);
  b->executable = true;
  debug_log("[arena] code block %p split: stubs +0..%zuK are RX, "
            "context +%zuK..%zuK stays RW\n",
            b->dst, code_half >> 10, code_half >> 10, b->size >> 10);
  return 0;
}

static int code_make_executable(CodeBlock *b) {
  return b->executable ? 0 : code_remap(b, true);
}

static int code_make_writable(CodeBlock *b) {
  return b->executable ? code_remap(b, false) : 0;
}

/* ------------------------------------------------------------------------ */

static inline size_t page_of(const void *p) {
  return ((uintptr_t)p - (uintptr_t)g_arena_base) >> PAGE_SHIFT;
}
static inline void *addr_of(size_t page) {
  return (char *)g_arena_base + (page << PAGE_SHIFT);
}
static inline int in_arena(const void *p) {
  return g_arena_base && (uintptr_t)p >= (uintptr_t)g_arena_base &&
         (uintptr_t)p <  (uintptr_t)g_arena_base + g_arena_size;
}

/* Page-aligned bump allocator. No free: donated pages are gone for good. */
void *donation_alloc(size_t size) {
  size = (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

  mutexLock(&g_lock);
  if (!g_donate_base || g_donate_used + size > g_donate_size) {
    mutexUnlock(&g_lock);
    debug_log("[arena] donation zone exhausted: wanted %zu MB, %zu MB left "
              "of %zu MB. Raise DONATION_ZONE_SIZE in mem_arena.c.\n",
              size >> 20,
              g_donate_base ? (g_donate_size - g_donate_used) >> 20 : 0,
              g_donate_size >> 20);
    return NULL;
  }
  void *p = (char *)g_donate_base + g_donate_used;
  g_donate_used += size;
  mutexUnlock(&g_lock);

  memset(p, 0, size);
  debug_log("[arena] donation: %zu MB at %p (%zu of %zu MB used)\n",
            size >> 20, p, g_donate_used >> 20, g_donate_size >> 20);
  return p;
}

int arena_init(void) {
  static const char *mode_name[] = {
    "none", "hbloader heap override", "svcSetHeapSize", "static fallback"
  };
  debug_log("[arena] heap mode: %s, %llu MB total\n",
            mode_name[g_heap_mode], (unsigned long long)(g_heap_total >> 20));

  if (g_gfx_returned)
    debug_log("[arena] returned %llu MB to the system pool for mesa\n",
              (unsigned long long)(g_gfx_returned >> 20));
  else if (g_heap_mode == HEAP_OVERRIDE)
    debug_log("[arena] *** could not return memory to the system pool. "
              "EGL init will probably fail at Stage 5. ***\n");

  if (g_gfx_base_moved)
    debug_log("[arena] heap base moved during the shrink; reverted. "
              "Running with the full heap and no gfx reserve.\n");

  debug_log("[arena] alias region %llu MB, system resource %llu MB "
            "(no system resource => on-demand commit is unavailable)\n",
            (unsigned long long)(g_alias_size >> 20),
            (unsigned long long)(g_system_res >> 20));

  if (!g_arena_base) {
    debug_log("[arena] NO ARENA -- only %llu MB of heap was available.\n",
              (unsigned long long)(g_heap_total >> 20));
    if (g_heap_mode == HEAP_FALLBACK)
      debug_log("[arena] The heap could not be claimed at all. Something is "
                "wrong with the launch environment.\n");
    else
      debug_log("[arena] This is applet mode. Relaunch via a title override: "
                "hold R while starting an installed game.\n");
    return -1;
  }

  g_npages    = g_arena_size >> PAGE_SHIFT;
  g_pageflags = calloc(g_npages, 1);
  if (!g_pageflags) { debug_log("[arena] cannot allocate page table\n"); return -1; }

  debug_log("[arena] executable memory is allocated per block on demand\n");
  debug_log("[arena] donation zone %p .. %p (%llu MB)\n",
            g_donate_base, (char *)g_donate_base + g_donate_size,
            (unsigned long long)(g_donate_size >> 20));
  debug_log("[arena] %p .. %p (%llu MB, %zu pages); newlib heap %llu MB\n",
            g_arena_base, (char *)g_arena_base + g_arena_size,
            (unsigned long long)(g_arena_size >> 20), g_npages,
            (unsigned long long)(g_newlib_size >> 20));
  return 0;
}

/* Find a run of free pages.
 *
 * First-fit from a rover rather than from page zero. The arena is 800k pages;
 * restarting the scan at zero every time makes each allocation walk the whole
 * region, and .NET commits in many small pieces once the heap is warm. That
 * cost lands in the frame loop as hitching, not as an obvious failure.
 *
 * The rover wraps, so the search still covers everything -- it just starts
 * where the last one finished, which is where free pages usually are. */
static size_t g_rover;

static long scan_range(size_t from, size_t to, size_t npages) {
  size_t run = 0;
  for (size_t i = from; i < to; i++) {
    if (g_pageflags[i] == 0) {
      if (++run == npages) return (long)(i + 1 - npages);
    } else {
      run = 0;
    }
  }
  return -1;
}

static long find_free_run(size_t npages) {
  long r = scan_range(g_rover, g_npages, npages);
  if (r < 0 && g_rover > 0)
    r = scan_range(0, g_rover, npages);   /* wrap */
  if (r >= 0) {
    g_rover = (size_t)r + npages;
    if (g_rover >= g_npages) g_rover = 0;
  }
  return r;
}

static void mark(size_t first, size_t n, uint8_t set, uint8_t clear) {
  for (size_t i = first; i < first + n && i < g_npages; i++) {
    uint8_t before = g_pageflags[i];
    g_pageflags[i] = (uint8_t)((before | set) & (uint8_t)~clear);
    uint8_t after = g_pageflags[i];

    if (!(before & PG_RESERVED)  && (after & PG_RESERVED))  g_stat_reserved_pages++;
    if ( (before & PG_RESERVED)  && !(after & PG_RESERVED)) g_stat_reserved_pages--;
    if (!(before & PG_COMMITTED) && (after & PG_COMMITTED)) g_stat_committed_pages++;
    if ( (before & PG_COMMITTED) && !(after & PG_COMMITTED))g_stat_committed_pages--;
  }
  if (g_stat_committed_pages > g_stat_peak_committed)
    g_stat_peak_committed = g_stat_committed_pages;
}

/* ------------------------------------------------------------------------ */

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset) {
  (void)offset;

  if (length == 0) return MAP_FAILED;
  size_t npages = (length + PAGE_SIZE - 1) >> PAGE_SHIFT;

  /* File mappings. The runtime should not need one -- we load the image
   * ourselves -- so treat this as a signal that something unexpected is
   * happening rather than silently half-supporting it. */
  if (fd >= 0) {
    debug_log("[arena] mmap of fd %d (len %zu) -- not supported\n", fd, length);
    return MAP_FAILED;
  }

  /* Executable request. Must come from the code pool: arena pages are ordinary
   * heap and can never be made executable, no matter what we do with
   * mprotect afterwards. */
  if (prot & PROT_EXEC) {
    void *p = code_alloc(length);
    debug_log("[arena] mmap len %zu prot=%s%s%s -> code pool %p\n", length,
              (prot & PROT_READ)  ? "R" : "-",
              (prot & PROT_WRITE) ? "W" : "-",
              (prot & PROT_EXEC)  ? "X" : "-", p);

    /* Asking for write and execute at once is the case Horizon cannot honour:
     * a range may be RW or RX, never both. We hand back writable memory so the
     * thunk can be stored, and the execute will fault unless the runtime
     * mprotects first. If that fault appears, DOTNET_EnableWriteXorExecute=1
     * in runtime_glue.c is the lever -- it makes the runtime map the same
     * memory twice and keep the write and execute pointers apart itself. */
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
      debug_log("[arena] NOTE: RWX requested. Granted RW; execution from this "
                "block will fault unless the runtime calls mprotect first.\n");
    return p ? p : MAP_FAILED;
  }

  mutexLock(&g_lock);

  if (!g_pageflags) { mutexUnlock(&g_lock); return MAP_FAILED; }

  /* addr supplied: commit inside an existing reservation. This is the GC's
   * commit path and it is the common case once the heap is warm. */
  if (addr && in_arena(addr)) {
    size_t first = page_of(addr);
    if (first + npages > g_npages) { mutexUnlock(&g_lock); return MAP_FAILED; }

    if (!(flags & MAP_FIXED)) {
      /* Advisory hint. Honour it only if the range is genuinely free or
       * already ours; otherwise fall through to a fresh allocation. */
      int usable = 1;
      for (size_t i = first; i < first + npages; i++)
        if (g_pageflags[i] & PG_COMMITTED) { usable = 0; break; }
      if (!usable) goto fresh;
    }

    uint8_t set = PG_RESERVED | ((prot == PROT_NONE) ? 0 : PG_COMMITTED);
    uint8_t clr = (prot == PROT_NONE) ? PG_COMMITTED : 0;
    mark(first, npages, set, clr);
    mutexUnlock(&g_lock);
    return addr;
  }

fresh: ;
  long first = find_free_run(npages);
  if (first < 0) {
    mutexUnlock(&g_lock);
    debug_log("[arena] OUT OF SPACE: wanted %zu pages (%zu KB). "
              "reserved=%zu committed=%zu of %zu pages.\n",
              npages, length >> 10,
              g_stat_reserved_pages, g_stat_committed_pages, g_npages);
    debug_log("[arena] If this is the GC's initial reservation, lower "
              "DOTNET_GCHeapHardLimit in runtime_glue.c.\n");
    return MAP_FAILED;
  }

  uint8_t set = PG_RESERVED | ((prot == PROT_NONE) ? 0 : PG_COMMITTED);
  mark((size_t)first, npages, set, 0);
  void *p = addr_of((size_t)first);
  mutexUnlock(&g_lock);

  /* Reservations are large and rare; log them, because their size tells you
   * immediately whether GCHeapHardLimit took effect. */
  if (prot == PROT_NONE && length >= (1u << 20))
    debug_log("[arena] reserve %zu MB at %p\n", length >> 20, p);

  return p;
}

int munmap_fake(void *addr, size_t length) {
  if (in_code_pool(addr)) return 0;   /* code pool is never reclaimed */
  if (!in_arena(addr) || !g_pageflags) return 0;

  mutexLock(&g_lock);
  size_t first = page_of(addr);
  mark(first, (length + PAGE_SIZE - 1) >> PAGE_SHIFT, 0,
       PG_RESERVED | PG_COMMITTED);
  /* Pull the rover back to reclaimed space, otherwise it drifts forward past
   * pages that are free again and the wrap does all the work. */
  if (first < g_rover) g_rover = first;
  mutexUnlock(&g_lock);
  return 0;
}

int mprotect_fake(void *addr, size_t len, int prot) {
  size_t npages = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;

  /* Making an existing mapping executable. Only possible if it came from the
   * code pool -- that pool is mapped through svcMapProcessCodeMemory, which is
   * what allows the permission change at all. */
  if (prot & PROT_EXEC) {
    CodeBlock *b = code_block_of(addr);
    if (b) {
      /* Write wins when both are asked for.
       *
       * The runtime requests read-write-execute and then generates stubs into
       * the block -- it mprotects first and writes afterwards. Horizon grants
       * RW or RX, never both, so honouring the execute bit here drops write
       * permission a moment before the writes arrive, which is exactly the
       * fault we kept seeing.
       *
       * Staying writable defers the problem to the first CALL instead, and
       * that is a point we control: every one of these thunks becomes a JNI
       * function pointer that we invoke ourselves, so code_ensure_executable
       * flips the block just before it is used. */
      if (prot & PROT_WRITE) {
        debug_log("[arena] mprotect RWX on %p -- keeping it writable; it will "
                  "be made executable on first call\n", addr);
        return code_make_writable(b) == 0 ? 0 : -1;
      }
      if (code_make_executable(b) != 0) {
        debug_log("[arena] could not make %p executable; the runtime will "
                  "report PlatformNotSupported and abandon thunk allocation\n",
                  addr);
        return -1;
      }
      return 0;
    }
    if (0) {
      /* svcSetProcessMemoryPermission requires a page-aligned base, and the
       * length must cover whole pages from that base. Passing the caller's
       * address unchanged fails outright when it points into the middle of a
       * page, and computing the page count from the length alone loses the
       * final page whenever the range straddles a boundary. Floor the start,
       * ceil the end, and derive the size from those. */
      uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)(PAGE_SIZE - 1);
      uintptr_t hi = ((uintptr_t)addr + len + PAGE_SIZE - 1)
                     & ~(uintptr_t)(PAGE_SIZE - 1);
      size_t span = hi - lo;

      Result rc = svcSetProcessMemoryPermission(
          envGetOwnProcessHandle(), (u64)lo, span, Perm_Rx);
      armDCacheFlush((void *)lo, span);
      armICacheInvalidate((void *)lo, span);

      if (R_SUCCEEDED(rc))
        debug_log("[arena] code block 0x%lx +%zu KB now executable -- "
                  "write-then-execute works here\n",
                  (unsigned long)lo, span >> 10);

      if (R_FAILED(rc)) {
        /* If this fires, the write-then-execute-in-place model does not work
         * here and no amount of tuning this function will make it. Horizon's
         * only sanctioned route to executable memory is the JIT API, and that
         * hands back TWO addresses -- a writable view and an executable view
         * at different addresses -- which a caller that writes and then
         * executes at one pointer cannot use.
         *
         * The way out is not to patch this. It is DOTNET_EnableWriteXorExecute,
         * currently 0 in runtime_glue.c. Set to 1, the runtime does its own
         * double mapping: it maps the same memory twice, keeps the write and
         * execute pointers separate itself, and translates between them. That
         * is exactly Horizon's model, and it needs no changes to the game
         * binary -- only that our mmap can serve two mappings backed by the
         * same pages, via libnx jit_t.
         *
         * Note also that the kernel allows only a handful of JIT regions
         * (~10 before 0xce01), so that must be one large region carved up
         * internally, not one per allocation. */
        debug_log("[arena] *** RX transition on %p failed: %08x ***\n"
                  "[arena] Write-then-execute in place is not possible here.\n"
                  "[arena] Try DOTNET_EnableWriteXorExecute=1 in runtime_glue.c "
                  "so the runtime does its own double mapping, and back it with "
                  "libnx jit_t (ONE large region -- the kernel caps how many "
                  "exist).\n", addr, rc);
        return -1;
      }
      return 0;
    }
    /* Not fatal, but the runtime is about to execute from ordinary memory.
     * The fix is in mmap_fake -- widen the heuristic that routes allocations
     * to the code pool. */
    debug_log("[arena] *** mprotect PROT_EXEC on non-code page %p (len %zu) ***\n"
              "[arena] This will fault when executed. See mmap_fake's PROT_EXEC "
              "path -- the allocation needs routing to the code pool.\n",
              addr, len);
    return -1;
  }

  {
    CodeBlock *b = code_block_of(addr);
    if (b) {
      /* Dropping execute permission on a thunk block means the runtime is
       * about to write to it again. */
      if (code_make_writable(b) != 0)
        debug_log("[arena] could not return %p to writable\n", addr);
      return 0;
    }
  }

  if (!in_arena(addr) || !g_pageflags) return 0;

  mutexLock(&g_lock);
  if (prot == PROT_NONE)
    mark(page_of(addr), npages, PG_RESERVED, PG_COMMITTED);   /* decommit */
  else
    mark(page_of(addr), npages, PG_RESERVED | PG_COMMITTED, 0); /* commit  */
  mutexUnlock(&g_lock);
  return 0;
}

int madvise_fake(void *addr, size_t len, int advice) {
  if (advice != MADV_DONTNEED && advice != MADV_FREE) return 0;
  if (!in_arena(addr) || !g_pageflags) return 0;

  /* DONTNEED means "the contents no longer matter and must read as zero if
   * touched again". With no demand paging we honour that by zeroing. The GC
   * relies on this for decommitted gen0 space. */
  mutexLock(&g_lock);
  size_t npages = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
  memset(addr, 0, npages << PAGE_SHIFT);
  mark(page_of(addr), npages, PG_RESERVED, PG_COMMITTED);
  mutexUnlock(&g_lock);
  return 0;
}

int mlock_fake(void *a, size_t l)   { (void)a; (void)l; return 0; }
int munlock_fake(void *a, size_t l) { (void)a; (void)l; return 0; }

/* How far newlib's heap has actually grown.
 *
 * NOTE, because it has been misread once: sbrk(0) is the BREAK POSITION, i.e.
 * the high-water mark, not live bytes. malloc only extends the break when no
 * free chunk fits, so a break at the limit does mean an allocation is about to
 * fail -- but it does not distinguish "this much is live" from "this much was
 * once live and the free space left behind is too fragmented to reuse". The
 * texture accounting in gl_guard.c is what separates those two. */
void heap_report(void) {
  void *brk = sbrk(0);
  if (!g_newlib_base || brk == (void *)-1) return;

  size_t used = (uintptr_t)brk - (uintptr_t)g_newlib_base;
  debug_log("[heap] newlib %zu of %zu MB used (%zu MB free)\n",
            used >> 20, g_newlib_size >> 20,
            g_newlib_size > used ? (g_newlib_size - used) >> 20 : 0);

  if (used > (g_newlib_size / 10) * 9)
    debug_log("[heap] *** over 90%% consumed -- allocations are about to "
              "start failing. This is not only a malloc problem: the GPU driver "
              "backs its buffer objects with this heap, so a full heap surfaces "
              "as GL_OUT_OF_MEMORY on the next texture upload, and then as a "
              "framebuffer with no attachment. Check the [gl] textures line "
              "before raising NEWLIB_HEAP_MAX: if textures do not account for "
              "most of this, raising it only moves the wall. ***\n");
}

/* Called immediately before invoking a pointer that may live in a thunk
 * block. Cheap when the block is already executable, and a no-op for any
 * address we did not hand out. */
int code_ensure_executable(const void *addr) {
  CodeBlock *b = code_block_of(addr);
  if (!b || b->executable) return 0;
  return code_make_executable(b);
}

void arena_report(void) {
  heap_report();
  if (!g_pageflags) return;
  debug_log("[arena] reserved %zu MB, committed %zu MB (peak %zu MB) of %zu MB; "
            "code pool %zu/%llu KB\n",
            (g_stat_reserved_pages  << PAGE_SHIFT) >> 20,
            (g_stat_committed_pages << PAGE_SHIFT) >> 20,
            (g_stat_peak_committed  << PAGE_SHIFT) >> 20,
            g_arena_size >> 20,
            g_code_total >> 10, 0ull);
}
