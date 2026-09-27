// disk_overlay.h — the F12 disk drives overlay (PIZERO-81, PIZERO-114).
//
// F12 opens a 6847-style list of the .dsk images on the card over the
// running machine, which pauses. Keys 0-3 put the highlighted disk in that
// drive, or take it out if it is already there (hot swap, no reset). F12 or
// ESC close it and the machine carries on. Key rules: overlay_keys.h.

#ifndef DISK_OVERLAY_H
#define DISK_OVERLAY_H

#include <stdint.h>

// How the overlay puts the finished 32x16 card on screen; main.cpp owns the
// framebuffer(s), so it supplies this.
typedef void (*disk_overlay_present_fn)(void);

void disk_overlay_init(disk_overlay_present_fn present);

// Feed the six keycodes of a HID report. Returns true when the report
// belongs to the overlay and must not reach the CoCo. *closed is set when
// this report closed it, so the caller can resync its own key state.
bool disk_overlay_key(const uint8_t codes[6], uint32_t frame, bool *closed);

// Once a frame while open: key repeat and redraw-if-changed.
void disk_overlay_frame(uint32_t frame);

bool disk_overlay_is_open(void);

// Open from something other than F12 (the BOOT button, PIZERO-128).
void disk_overlay_open(void);

#endif  // DISK_OVERLAY_H
