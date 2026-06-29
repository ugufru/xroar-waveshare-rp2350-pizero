/*
 * usb_hotplug_diag.c — PIZERO-11b diagnostics shim.
 *
 * Pico-PIO-USB's internal header pio_usb_ll.h is C-only: it uses
 * `static inline __force_inline` which expands to a duplicate `inline` and
 * fails to compile in a C++ translation unit. main.cpp is C++, so we isolate
 * the header here (compiled as C, exactly how the library compiles it) and
 * expose a tiny C ABI that main.cpp calls via extern "C" declarations.
 *
 * What this surfaces for the hot-replug investigation:
 *  - line_state: the library's OWN bus reader (pio_usb_bus_get_line_state),
 *    which applies the RP2350-E9 input-leakage drain only on chip rev<=2.
 *    This board is rev3, so the workaround is compiled out -> if an unplug
 *    still reads J/FS instead of SE0 here, that is the E9-masking signature.
 *  - connected: the stack's debounced view of whether a device is present.
 *    The key bisection — stays 1 after unplug => host never saw the disconnect.
 *  - is_fullspeed / event: supporting detail.
 *
 * Diagnostic-only; remove (or flag-gate) with the rest of the PIZERO-11b
 * instrumentation once the root cause is settled.
 */
#include "pio_usb_ll.h"

uint8_t usbdiag_line_state(void) {
    return (uint8_t)pio_usb_bus_get_line_state(PIO_USB_ROOT_PORT(0));
}

int usbdiag_connected(void) {
    return PIO_USB_ROOT_PORT(0)->connected ? 1 : 0;
}

int usbdiag_fullspeed(void) {
    return PIO_USB_ROOT_PORT(0)->is_fullspeed ? 1 : 0;
}

unsigned usbdiag_event(void) {
    return (unsigned)PIO_USB_ROOT_PORT(0)->event;
}

/* Endpoint error / interrupt-status registers the stack maintains. A device
 * removed mid-operation can't answer IN polls, so these are where a disconnect
 * actually shows up (the line-state/connected flag are blind — the host drives
 * the bus). */
unsigned usbdiag_ep_error(void) {
    return (unsigned)PIO_USB_ROOT_PORT(0)->ep_error;
}

unsigned usbdiag_ints(void) {
    return (unsigned)PIO_USB_ROOT_PORT(0)->ints;
}
