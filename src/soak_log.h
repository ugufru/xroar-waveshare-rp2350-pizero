// soak_log.h: the board's own soak record on the SD card (PIZERO-201).
//
// With soak_log = on in settings.txt, each boot opens the next
// /coco/log/soak-NNN.txt, writes a header, and once a minute appends the
// [run] and [aud] telemetry lines, timestamped from the clock, then syncs.
// A soak then needs no laptop: power the board from a supply, pull the card
// a day later and run scripts/soak.py analyse on the file.
//
// The clock is the RP2350's always-on timer (no RTC, no backup cell on this
// board): it runs while powered and through a watchdog reboot, and at
// power-on it is seeded from the newest file time on the card, which the
// Mac wrote with real time. See coco_boot_clock_init().

#ifndef SOAK_LOG_H
#define SOAK_LOG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open this boot's log file and write its header. Returns false when the
// file could not be made (the soak carries on without it).
bool soak_log_begin(const char *fw, const char *env, bool watchdog_reboot,
                    uint32_t freezes, const char *freeze_phase, const char *reset_reason);

// Called once a second with this second's telemetry lines; writes every
// 60th, and the first. Cheap when nothing is due.
void soak_log_tick(const char *run_line, const char *aud_line);

bool soak_log_active(void);

#ifdef __cplusplus
}
#endif
#endif
