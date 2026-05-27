/*
 * machine.h — stub for the RP2350 port.
 *
 * Just enough to let part.c and serialise.c declare external
 * `struct machine_partdb_entry` / `struct cart_partdb_entry` symbols.
 * The desktop XRoar version owns the full machine-config system; we will
 * write our own thin coco_machine.c in COCO-25 instead of porting this.
 */
#ifndef XROAR_PORT_MACHINE_H_
#define XROAR_PORT_MACHINE_H_

#include <stdint.h>
#include "part.h"

struct machine_config;

struct machine_partdb_entry {
    struct partdb_entry partdb_entry;
    void (*config_complete)(struct machine_config *mc);
    _Bool (*is_working_config)(struct machine_config *mc);
    const char *cart_arch;
};

struct cart_config;

struct cart_partdb_entry {
    struct partdb_entry partdb_entry;
    void (*config_complete)(struct cart_config *cc);
    _Bool (*is_working_config)(struct cart_config *cc);
};

#define MACHINE_SIGINT  (2)
#define MACHINE_SIGILL  (4)
#define MACHINE_SIGTRAP (5)
#define MACHINE_SIGFPE  (8)

#endif
