/* atomics_check.h -- see atomics_check.c for why this exists. */
#ifndef ATOMICS_CHECK_H
#define ATOMICS_CHECK_H

/* Verifies that an AArch64 ldaxr/stlxr CAS loop can actually complete on each
 * kind of memory the runtime puts lock state in. Returns non-zero if every
 * region passed. Call once at startup, after the .so is mapped and the arena
 * exists. */
int atomics_check_run(void);

#endif
