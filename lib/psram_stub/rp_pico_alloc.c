/*
 * psram_stub.c — drop-in replacement for psramlib's rp_mem_*().
 *
 * The Waveshare RP2350-Touch-AMOLED-1.8 has no external PSRAM, but several
 * upstream sources (coco_machine, the XRoar regression tests) call
 * rp_mem_malloc() expecting a 64 KB PSRAM block for the CoCo RAM image. We
 * back all those calls with a single shared 64 KB SRAM pool. Tests run
 * sequentially in setup() and each overwrites the previous, which is fine
 * because they don't observe each other's state. coco_machine_init() runs
 * after the tests and takes the same pool for the live CoCo RAM. AMOLED-19.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define POOL_SIZE (64 * 1024)
static uint8_t g_pool[POOL_SIZE] __attribute__((aligned(4)));

void *rp_mem_malloc(size_t size) {
    return (size <= POOL_SIZE) ? g_pool : NULL;
}

void rp_mem_free(void *p) {
    (void)p;
}

void *rp_mem_calloc(size_t num, size_t size) {
    size_t total = num * size;
    void *p = rp_mem_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *rp_mem_realloc(void *p, size_t size) {
    (void)p;
    return rp_mem_malloc(size);
}

size_t rp_mem_avail(void) { return POOL_SIZE; }
