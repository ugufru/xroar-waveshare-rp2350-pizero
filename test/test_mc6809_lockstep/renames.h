// renames.h: give one core's external symbols a prefix (P), so two cores
// link into one test binary. Included before the core's sources.
#define LSR_CAT2(a, b) a##_##b
#define LSR_CAT(a, b) LSR_CAT2(a, b)
#define coco_mem_cycle   LSR_CAT(LS_PREFIX, mem_cycle)
#define MC6809_HALT_SET  LSR_CAT(LS_PREFIX, MC6809_HALT_SET)
#define MC6809_NMI_SET   LSR_CAT(LS_PREFIX, MC6809_NMI_SET)
#define MC6809_FIRQ_SET  LSR_CAT(LS_PREFIX, MC6809_FIRQ_SET)
#define MC6809_IRQ_SET   LSR_CAT(LS_PREFIX, MC6809_IRQ_SET)
#define mc6809_get_pc    LSR_CAT(LS_PREFIX, mc6809_get_pc)
#define mc6809_set_pc    LSR_CAT(LS_PREFIX, mc6809_set_pc)
#define mc6809_is_a      LSR_CAT(LS_PREFIX, mc6809_is_a)
#define mc6809_part      LSR_CAT(LS_PREFIX, mc6809_part)
#define mc6809_get_trace_pc LSR_CAT(LS_PREFIX, mc6809_get_trace_pc)
