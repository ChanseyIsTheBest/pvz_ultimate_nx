#ifndef PVZU_ICU_PROBE_H
#define PVZU_ICU_PROBE_H

/* Temporary diagnostic -- see icu_probe.c. Every ICU symbol gets its own
 * trampoline so a call can be attributed to a specific function. */
void *icu_probe_symbol(const char *symbol);
int   icu_probe_count(void);
void  icu_probe_report(void);

#endif
