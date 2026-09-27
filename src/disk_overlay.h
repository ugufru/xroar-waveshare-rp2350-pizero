// disk_overlay.h: the F12 overlay (PIZERO-81, PIZERO-114, PIZERO-136).
//
// F12 opens a 6847-style list over the running machine, which pauses.
// Left/Right switch between disks, programs and cartridges; see
// disk_overlay.cpp for what each list's keys do. F12 or ESC close it and the
// machine carries on. Key rules: overlay_keys.h.

#ifndef DISK_OVERLAY_H
#define DISK_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

// How the overlay puts the finished 32x16 card on screen; main.cpp owns the
// framebuffer(s), so it supplies this.
typedef void (*disk_overlay_present_fn)(void);

// ENTER on an entry: kind is enum cat_kind (dsk_catalog.h), path the FatFs
// path. Return true to accept, and the overlay closes; the caller performs
// the launch from loop(). Return false with a short reason in msg (card
// text, upper case, at most msg_sz - 1 characters) and the overlay stays.
typedef bool (*disk_overlay_launch_fn)(int kind, const char *path,
                                       char *msg, size_t msg_sz);

void disk_overlay_init(disk_overlay_present_fn present, disk_overlay_launch_fn launch);

// Feed the six keycodes of a HID report. Returns true when the report
// belongs to the overlay and must not reach the CoCo. *closed is set when
// this report closed it (ESC, F12 or an accepted launch), so the caller can
// resync its own key state.
bool disk_overlay_key(const uint8_t codes[6], uint32_t frame, bool *closed);

// Once a frame while open: key repeat and redraw-if-changed.
void disk_overlay_frame(uint32_t frame);

bool disk_overlay_is_open(void);

// Open from something other than F12 (the BOOT button, PIZERO-128).
void disk_overlay_open(void);

#endif  // DISK_OVERLAY_H
