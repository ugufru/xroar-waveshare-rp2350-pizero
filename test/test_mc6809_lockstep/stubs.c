// stubs.c: the two symbols the core needs from the rest of XRoar.
#include <stdlib.h>
#include <stdint.h>
void *part_new(size_t psize) { return calloc(1, psize); }
void delegate_void_default_bool_uint16(void *sptr, _Bool a, uint16_t b) { (void)sptr; (void)a; (void)b; }
