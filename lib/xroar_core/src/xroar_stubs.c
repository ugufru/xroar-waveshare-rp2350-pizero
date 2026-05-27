/*
 * xroar_stubs.c — definitions for symbols our stub xroar.h / logging.h declare.
 *
 * These are the bare-minimum globals XRoar modules expect to find at link
 * time. None of them are actively read or written by the modules we vendor;
 * they exist purely so unresolved references resolve.
 */
#include <stddef.h>

#include "logging.h"
#include "xroar.h"

/* Global log/debug flags. All zero — every LOG_* macro is a no-op when
 * LOGGING is not defined (see logging.h), but the struct itself is
 * referenced by mc6809.c (logging.trace_cpu) at compile time. */
struct logging logging;

/* The machine-wide event list. coco_machine.c (COCO-25) will own this. */
struct event_list *machine_event_list_global;

/* fs_* stubs. serialise.c references these for save-state I/O; we don't
 * implement save-state on the Pico, so every stub is a no-op that returns
 * a sentinel value. They exist solely to satisfy the linker. */
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>

off_t   fs_file_size(FILE *fd)                                       { (void)fd; return 0; }
int     fs_truncate(FILE *fd, off_t length)                          { (void)fd; (void)length; return -1; }
uint32_t fs_file_crc32(FILE *fd)                                     { (void)fd; return 0; }
size_t  fs_file_crc32_block(FILE *fd, uint32_t *c, size_t l)         { (void)fd; (void)c; (void)l; return 0; }
int     fs_write_uint8(FILE *fd, int v)                              { (void)fd; (void)v; return -1; }
int     fs_write_uint16(FILE *fd, int v)                             { (void)fd; (void)v; return -1; }
int     fs_write_uint16_le(FILE *fd, int v)                          { (void)fd; (void)v; return -1; }
int     fs_write_uint31(FILE *fd, int v)                             { (void)fd; (void)v; return -1; }
int     fs_read_uint8(FILE *fd)                                      { (void)fd; return -1; }
int     fs_read_uint16(FILE *fd)                                     { (void)fd; return -1; }
int     fs_read_uint16_le(FILE *fd)                                  { (void)fd; return -1; }
int     fs_read_uint31(FILE *fd)                                     { (void)fd; return -1; }
int     fs_sizeof_vuint32(uint32_t v)                                { (void)v; return 0; }
int     fs_write_vuint32(FILE *fd, uint32_t v)                       { (void)fd; (void)v; return -1; }
uint32_t fs_read_vuint32(FILE *fd, int *nread)                       { (void)fd; if (nread) *nread = 0; return 0; }
int     fs_sizeof_vint32(int32_t v)                                  { (void)v; return 0; }
int     fs_write_vint32(FILE *fd, int32_t v)                         { (void)fd; (void)v; return -1; }
int32_t fs_read_vint32(FILE *fd, int *nread)                         { (void)fd; if (nread) *nread = 0; return 0; }
