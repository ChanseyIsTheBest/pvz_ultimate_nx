/* nx_exception_dump.c -- user exception handler.
 *
 * Without this, a fault inside the game image is a black screen and nothing
 * else. libnx enables user exception handling when a translation unit provides
 * __nx_exception_stack, __nx_exception_stack_size and __libnx_exception_handler,
 * so simply linking this file in turns every crash into a symbolized report.
 *
 * Add it early. Almost everything else in the port is debugged through the
 * output of this file plus debug.log, and a fault with no PC is the difference
 * between an afternoon and a week.
 *
 * A note on what you can and cannot do from here: this is a crash reporter,
 * not a fault-recovery mechanism. It is NOT a route to implementing NativeAOT's
 * implicit null checks -- those need the fault converted into a managed
 * NullReferenceException and execution resumed at a handler, and there is no
 * supported way to resume from here. A managed null dereference stays a hard
 * crash. This just tells you where it happened.
 *
 * Every read below is guarded with svcQueryMemory, because the whole point is
 * to survive long enough to print something and a second fault inside the
 * handler tells you nothing at all.
 */

#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "mem_arena.h"
#include "nx_exception_dump.h"
#include "so_util.h"
#include "util.h"

/* The crash dump must survive DEBUG_LOG=0.
 *
 * Everything below writes through debug_log, and in a release build that
 * expands to nothing -- which would leave a fault producing a silent reboot and
 * no evidence at all. Rebinding the name here keeps the dumper's code
 * unchanged and always-on, which is the one thing in this file that must never
 * be optional. */
#undef debug_log
#define debug_log log_write

/* __attribute__((aligned)) rather than alignas: the latter needs <stdalign.h>
 * before C23 and devkitA64's default standard varies by toolchain version. */
u8 __nx_exception_stack[0x8000] __attribute__((aligned(16)));
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

/* ------------------------------------------------------------------------ */

static int readable(uintptr_t addr, size_t len) {
  if (!addr || addr < 0x1000) return 0;
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi;
    u32 pageinfo;
    if (R_FAILED(svcQueryMemory(&mi, &pageinfo, a))) return 0;
    if (mi.type == MemType_Unmapped) return 0;
    if ((mi.perm & Perm_R) == 0) return 0;
    uintptr_t block_end = (uintptr_t)mi.addr + mi.size;
    if (block_end <= a) return 0;
    a = block_end;
  }
  return 1;
}

/* Where the host module itself is loaded.
 *
 * This is the single most expensive thing this file did not print. A crash in
 * mesa, newlib, zlib or our own code arrived as a bare hex address, and the
 * only way to symbolize it was to guess a base and check whether the
 * instruction at the resulting offset looked plausible. That heuristic
 * produced a confident, wrong answer -- an entire round was spent reading a
 * crash as "zlib scan_tree" when the true PC, 0x28b000 further down, was
 * st_update_renderbuffer_surface. The instruction at the guessed offset was
 * `mov w6, w3`, which cannot fault at all; had the base simply been printed,
 * that round would not have happened.
 *
 * _start is at vaddr 0 in the linked image, so its runtime address IS the
 * load base. The span is found by walking mapped regions forward from it. */
extern char _start[];

static uintptr_t g_host_base;
static size_t    g_host_span;

static void host_range_init(void) {
  if (g_host_base) return;
  g_host_base = (uintptr_t)_start;

  uintptr_t a = g_host_base;
  for (int i = 0; i < 16; i++) {
    MemoryInfo mi;
    u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    if (mi.type == MemType_Unmapped) break;
    if (mi.addr + mi.size <= a) break;              /* no forward progress */
    a = (uintptr_t)(mi.addr + mi.size);
    if (a - g_host_base > 0x4000000) break;         /* 64 MB sanity cap */
  }
  g_host_span = a - g_host_base;
  if (!g_host_span) g_host_span = 0x1000000;
}

uintptr_t nx_host_base(void) { host_range_init(); return g_host_base; }
size_t    nx_host_span(void) { host_range_init(); return g_host_span; }

static int in_host(uintptr_t p) {
  host_range_init();
  return p >= g_host_base && p < g_host_base + g_host_span;
}

/* "libLawn_Android.so+0x1a2b3c" if the address is inside a module we loaded,
 * raw hex otherwise. Module-relative is what you want: it is directly
 * comparable against a disassembler with the same file open. */
static void annotate(uintptr_t addr, char *out, size_t outsz) {
  so_module *m = so_find_module_by_addr((const void *)addr);
  if (m) {
    snprintf(out, outsz, "%s+0x%lx", m->name,
             (unsigned long)(addr - (uintptr_t)m->load_virtbase));
    return;
  }
  /* "host+0x171350" pastes straight into addr2line against pvzultimate_nx.elf.
     Raw hex does not, and that difference cost a round. */
  if (in_host(addr)) {
    snprintf(out, outsz, "host+0x%lx", (unsigned long)(addr - nx_host_base()));
    return;
  }
  snprintf(out, outsz, "0x%016lx", (unsigned long)addr);
}

/* libnx ThreadExceptionDesc values. These are NOT small ordinals -- the first
 * version of this table guessed 0..6 and reported the real code 0x100 as
 * "unknown", which threw away the single most useful fact in the dump. */
/* Which of our regions does this address belong to? A fault address that lands
 * in the donation zone, or just past the end of a mapping, or in unmapped
 * space, each points at a completely different bug -- and the raw hex looks
 * identical in all three cases. */
static const char *describe_address(u64 a, char *buf, size_t n) {
  uintptr_t p = (uintptr_t)a;

  if (p == 0) { snprintf(buf, n, "-> NULL"); return buf; }
  if (p < 0x1000) { snprintf(buf, n, "-> NULL + 0x%lx (offset off a null pointer)",
                             (unsigned long)p); return buf; }

  so_module *m = so_find_module_by_addr((const void *)p);
  if (m) {
    snprintf(buf, n, "-> inside %s (+0x%lx)", m->name,
             (unsigned long)(p - (uintptr_t)m->load_virtbase));
    return buf;
  }
  if (in_host(p)) {
    snprintf(buf, n, "-> inside the host image (host+0x%lx)",
             (unsigned long)(p - nx_host_base()));
    return buf;
  }
  for (m = so_module_list; m; m = m->next) {
    uintptr_t lo = (uintptr_t)m->load_virtbase;
    if (p >= lo + m->load_size && p < lo + m->load_size + 0x100000) {
      snprintf(buf, n, "-> 0x%lx PAST the end of %s -- overrun",
               (unsigned long)(p - lo - m->load_size), m->name);
      return buf;
    }
  }

  if (g_donate_base && p >= (uintptr_t)g_donate_base &&
      p <  (uintptr_t)g_donate_base + g_donate_size) {
    snprintf(buf, n, "-> in the DONATION ZONE (+0x%lx). These pages were given "
             "to svcMapProcessCodeMemory and fault at this address by design.",
             (unsigned long)(p - (uintptr_t)g_donate_base));
    return buf;
  }
  if (g_arena_base && p >= (uintptr_t)g_arena_base &&
      p <  (uintptr_t)g_arena_base + g_arena_size) {
    snprintf(buf, n, "-> in the GC arena (+0x%lx)",
             (unsigned long)(p - (uintptr_t)g_arena_base));
    return buf;
  }
  if (g_newlib_base && p >= (uintptr_t)g_newlib_base &&
      p <  (uintptr_t)g_newlib_base + g_newlib_size) {
    snprintf(buf, n, "-> in the newlib heap (+0x%lx)",
             (unsigned long)(p - (uintptr_t)g_newlib_base));
    return buf;
  }

  MemoryInfo mi;
  u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, p)) && mi.type == MemType_Unmapped) {
    snprintf(buf, n, "-> UNMAPPED");
    return buf;
  }
  snprintf(buf, n, "-> outside every region we manage");
  return buf;
}

static const char *desc_error(u32 code) {
  switch (code) {
    case 0x100: return "instruction abort -- executing a non-executable page";
    case 0x101: return "other / data abort";
    case 0x102: return "misaligned PC";
    case 0x103: return "misaligned SP";
    case 0x104: return "trap (brk / __builtin_trap)";
    case 0x106: return "SError";
    case 0x301: return "bad SVC";
    default:    return "unrecognised";
  }
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  char sym[128];

  debug_log("\n"
            "================================================================\n"
            "  CRASH: %s (error_desc 0x%x)\n"
            "================================================================\n",
            desc_error(ctx->error_desc), ctx->error_desc);

  /* For a data abort this is the answer: FAR holds the address that faulted,
   * and ESR says whether it was a read or a write and why it failed. Printing
   * PC alone tells you where the program was, not what it touched -- which is
   * the difference between a diagnosis and a guess. */
  if (ctx->error_desc == 0x101 || ctx->error_desc == 0x100) {
    u64 far = ctx->far.x;
    u32 esr = ctx->esr;
    u32 ec  = (esr >> 26) & 0x3F;
    u32 wnr = (esr >> 6) & 1;
    u32 dfsc = esr & 0x3F;

    const char *why = "unknown";
    switch (dfsc & 0x3C) {
      case 0x00: why = "address size fault"; break;
      case 0x04: why = "translation fault -- nothing mapped there"; break;
      case 0x08: why = "access flag fault"; break;
      case 0x0C: why = "permission fault -- mapped, but not allowed"; break;
      default: break;
    }

    debug_log("  FAULT ADDRESS  0x%016lx  (%s, %s)\n",
              (unsigned long)far,
              (ec == 0x24 || ec == 0x25) ? (wnr ? "write" : "read") : "execute",
              why);
    debug_log("  %s\n", describe_address(far, sym, sizeof(sym)));
    debug_log("  ESR 0x%08x\n", esr);

  /* Recognise a managed null check.
   *
   * NativeAOT tests a reference by dereferencing it and discarding the result:
   *     ldr wzr, [xN]        (0xb940001f | N<<5)
   * On Linux the resulting SIGSEGV is caught and rethrown as a
   * NullReferenceException with a managed stack trace. We cannot deliver
   * signals, so it arrives here as a bare read of address zero and kills the
   * process instead.
   *
   * Saying so explicitly matters because the two cases look identical in a
   * register dump: our own code dereferencing null is a bug in the shim, while
   * this is managed code checking a reference WE handed it. The fix is
   * upstream -- find the method that returned null -- not at this address.
   *
   * Resuming is possible in principle: the kernel takes an updated elr_el1
   * from the exception TLS structure and svcReturnFromException(0) pivots to
   * it. Redirecting to the runtime's registered handler would turn every one
   * of these into a catchable exception. That is a real piece of work and is
   * not attempted here. */
  {
    u32 insn = 0;
    u64 pc = ctx->pc.x;
    if (far == 0 && readable((uintptr_t)pc, 4)) {
      insn = *(volatile u32 *)(uintptr_t)pc;
      if ((insn & 0xFFFFFC1F) == 0xB940001F) {
        int reg = (int)((insn >> 5) & 0x1F);
        debug_log("\n  This is a MANAGED NULL CHECK: `ldr wzr, [x%d]` with "
                  "x%d = 0.\n"
                  "  On Linux this becomes a NullReferenceException with a "
                  "managed stack trace;\n"
                  "  here it is fatal because signals cannot be delivered.\n"
                  "  The bug is whatever handed managed code a null -- look "
                  "for a\n"
                  "  \"NULL OBJECT from ...\" line above, not at this "
                  "address.\n\n", reg, reg);
      }
    }
  }
  }

  annotate((uintptr_t)ctx->pc.x, sym, sizeof(sym));
  debug_log("  PC  %s\n", sym);
  annotate((uintptr_t)ctx->lr.x, sym, sizeof(sym));
  debug_log("  LR  %s\n", sym);
  debug_log("  SP  0x%016lx\n", (unsigned long)ctx->sp.x);
  debug_log("  FP  0x%016lx\n", (unsigned long)ctx->fp.x);

  debug_log("\n  registers:\n");
  /* 29 registers, x0..x28. Pair them, then print the odd one out -- reading
   * cpu_gprs[29] to make the loop tidy is an out-of-bounds read, and doing it
   * inside a crash handler is how you turn a diagnosable fault into a second
   * fault with no output at all. */
  for (int i = 0; i + 1 < 29; i += 2) {
    debug_log("    x%-2d 0x%016lx    x%-2d 0x%016lx\n",
              i,     (unsigned long)ctx->cpu_gprs[i].x,
              i + 1, (unsigned long)ctx->cpu_gprs[i + 1].x);
  }
  debug_log("    x28 0x%016lx\n", (unsigned long)ctx->cpu_gprs[28].x);

  /* Frame-pointer chain. Best effort: the game image is stripped and AOT code
   * does not guarantee a frame pointer in every method, so a short chain is
   * normal and does not mean the walk is broken. Trust the PC above all. */
  debug_log("\n  backtrace (best effort):\n");
  uintptr_t fp = (uintptr_t)ctx->fp.x;
  for (int depth = 0; depth < 24; depth++) {
    if (!readable(fp, 16)) break;
    uintptr_t next = *(uintptr_t *)fp;
    uintptr_t ret  = *(uintptr_t *)(fp + 8);
    if (!ret) break;
    annotate(ret, sym, sizeof(sym));
    debug_log("    #%-2d %s\n", depth, sym);
    if (next <= fp) break;   /* chain must ascend or it is not a chain */
    fp = next;
  }

  debug_log("\n  loaded modules:\n");
  debug_log("    %-28s %p .. %p   <- symbolize host+N with:\n"
            "    %-28s addr2line -e pvzultimate_nx.elf -f -C -i 0xN\n",
            "pvzultimate_nx.elf (host)", (void *)nx_host_base(),
            (void *)(nx_host_base() + nx_host_span()), "");
  for (so_module *m = so_module_list; m; m = m->next)
    debug_log("    %-28s %p .. %p\n", m->name, m->load_virtbase,
              (char *)m->load_virtbase + m->load_size);

  /* Stack words around SP. Frequently the fastest way to recognise which call
   * you are in when the frame chain gives up. */
  uintptr_t sp = (uintptr_t)ctx->sp.x;
  if (readable(sp, 256)) {
    debug_log("\n  stack at SP:\n");
    for (int i = 0; i < 16; i += 2) {
      uintptr_t a = sp + (uintptr_t)i * 8;
      debug_log("    [sp+0x%02x] 0x%016lx  0x%016lx\n",
                i * 8, (unsigned long)*(uintptr_t *)a,
                (unsigned long)*(uintptr_t *)(a + 8));
    }
  }

  debug_log("================================================================\n");
  log_shutdown();

  /* Break explicitly rather than falling through. This guarantees Atmosphere's
   * creport fires, and creport dumps EVERY thread's registers and stack -- not
   * just the one that faulted. For anything involving the runtime's own
   * threads that second report is usually worth more than this one. */
  svcBreak(BreakReason_Panic, 0, 0);
}
