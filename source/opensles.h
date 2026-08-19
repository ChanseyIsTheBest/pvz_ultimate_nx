#ifndef PVZU_OPENSLES_H
#define PVZU_OPENSLES_H

#include <stdint.h>

/* Interface identifiers. Callers compare these by pointer, never by value. */
extern const void *SL_IID_ENGINE;
extern const void *SL_IID_PLAY;
extern const void *SL_IID_BUFFERQUEUE;
extern const void *SL_IID_VOLUME;
extern const void *SL_IID_PLAYBACKRATE;
extern const void *SL_IID_ANDROIDCONFIGURATION;
extern const void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE;

/* The only symbol a caller resolves by name; everything else is reached
 * through the returned object. */
int32_t slCreateEngine(void **engine, uint32_t numOptions, const void *options,
                       uint32_t numInterfaces, const void *ids,
                       const uint32_t *req);

int  opensles_init(void);
void opensles_shutdown(void);

#endif
