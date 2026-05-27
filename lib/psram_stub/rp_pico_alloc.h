/*
 * rp_pico_alloc.h — header for the no-PSRAM stub. See rp_pico_alloc.c.
 * Drop-in API compatible with the upstream psramlib so that callers
 * (coco_machine, test_*.cpp) link unchanged.
 */
#ifndef RP_PICO_ALLOC_H_
#define RP_PICO_ALLOC_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *rp_mem_malloc (size_t size);
void   rp_mem_free   (void *p);
void  *rp_mem_calloc (size_t num, size_t size);
void  *rp_mem_realloc(void *p, size_t size);
size_t rp_mem_avail  (void);

#ifdef __cplusplus
}
#endif

#endif
