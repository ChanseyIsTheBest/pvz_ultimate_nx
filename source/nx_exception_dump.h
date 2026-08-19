/* nx_exception_dump.h -- the crash handler's view of where the host is loaded.
 *
 * __libnx_exception_handler itself needs no declaration: libnx defines it weak
 * and picks ours up at link time. What is worth exporting is the host load
 * base, because every address in a crash dump that is not inside a .so we
 * loaded is relative to it, and without it those addresses cannot be
 * symbolized at all.
 */
#ifndef PVZU_NX_EXCEPTION_DUMP_H
#define PVZU_NX_EXCEPTION_DUMP_H

#include <stddef.h>
#include <stdint.h>

/* Runtime load base of pvzultimate_nx.elf. Subtract it from a raw address in
 * a dump to get an offset that addr2line will accept against the linked ELF:
 *
 *     addr2line -e pvzultimate_nx.elf -f -C -i 0x<offset>
 */
uintptr_t nx_host_base(void);

/* Size of the mapped host image, found by walking regions forward from the
 * base. Used to decide whether an address belongs to the host at all. */
size_t nx_host_span(void);

#endif /* PVZU_NX_EXCEPTION_DUMP_H */
