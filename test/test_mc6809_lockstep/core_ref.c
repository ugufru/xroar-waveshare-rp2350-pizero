// core_ref.c: the frozen reference core (test/mc6809_reference), prefix ref_.
#define LS_PREFIX ref
#define LS_NAME "reference"
#define LS_TABLE ls_ref
#include "renames.h"
#include "../mc6809_reference/mc6809.c"
#include "lockstep_core.inc"
