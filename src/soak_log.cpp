// soak_log.cpp: see soak_log.h (PIZERO-201).

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "f_util.h"
#include "hardware/watchdog.h"

#include "soak_log.h"
#include "coco_boot.h"

#define LOG_DIR "0:/coco/log"
#define SOAK_INTERVAL_S 60

static FIL *g_file;             // on the heap only while logging: pizero_wavmeas has no static RAM to spare
static bool g_open;
static uint32_t g_lines;        // telemetry seconds seen since begin

static bool append(const char *text) {
    UINT bw = 0;
    FRESULT fr = f_write(g_file, text, (UINT)strlen(text), &bw);
    if (fr != FR_OK || bw != strlen(text)) {
        Serial.printf("[soak] write failed (%d): logging stops\r\n", fr);
        f_close(g_file); free(g_file); g_file = nullptr;
        g_open = false;
        return false;
    }
    return true;
}

static void sync_now(void) {
    if (!g_open) return;
    uint32_t t0 = micros();
    FRESULT fr = f_sync(g_file);
    watchdog_update();
    if (fr != FR_OK) { Serial.printf("[soak] sync failed (%d)\r\n", fr); f_close(g_file); free(g_file); g_file = nullptr; g_open = false; }
    else if (micros() - t0 > 50000) Serial.printf("[soak] slow sync: %lu us\r\n", (unsigned long)(micros() - t0));
}

extern "C" bool soak_log_begin(const char *fw, const char *env, bool watchdog_reboot,
                               uint32_t freezes, const char *freeze_phase) {
    FRESULT fr = f_mkdir(LOG_DIR);
    if (fr != FR_OK && fr != FR_EXIST) { Serial.printf("[soak] cannot create %s (%d)\r\n", LOG_DIR, fr); return false; }
    char path[48];
    FILINFO fi;
    unsigned k;
    for (k = 1; k <= 999; k++) {
        snprintf(path, sizeof path, LOG_DIR "/soak-%03u.txt", k);
        if (f_stat(path, &fi) != FR_OK) break;
    }
    if (k > 999) { Serial.print("[soak] " LOG_DIR " is full\r\n"); return false; }
    if (!g_file) g_file = (FIL *)malloc(sizeof *g_file);
    if (!g_file) { Serial.print("[soak] no memory for the log file\r\n"); return false; }
    fr = f_open(g_file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) { Serial.printf("[soak] cannot open %s (%d)\r\n", path, fr); free(g_file); g_file = nullptr; return false; }
    g_open = true;
    g_lines = 0;
    char clock[40], src[24], head[320];
    coco_boot_clock_text(clock, sizeof clock, src, sizeof src);
    // The first line names the format for scripts/soak.py; the rest reads
    // like the serial banner it already understands.
    snprintf(head, sizeof head,
             "=== CoCo Zero soak log, interval %us, clock %s from %s\r\n"
             "=== DATE %.10s\r\n"
             "XRoar on RP2350-PiZero (soak log) fw=%s env=%s boot=%s freezes=%lu last=%s\r\n",
             SOAK_INTERVAL_S, clock, src, clock, fw, env,
             watchdog_reboot ? "watchdog-reboot" : "power-on",
             (unsigned long)freezes, freeze_phase);
    if (!append(head)) return false;
    sync_now();
    Serial.printf("[soak] logging to %s every %u s, clock %s (%s)\r\n", path, SOAK_INTERVAL_S, clock, src);
    return g_open;
}

extern "C" void soak_log_tick(const char *run_line, const char *aud_line) {
    if (!g_open) return;
    if (g_lines++ % SOAK_INTERVAL_S != 0) return;
    char clock[40], src[24], stamp[64];
    coco_boot_clock_text(clock, sizeof clock, src, sizeof src);
    // "[HH:MM:SS.000]" is the shape scripts/soak.py record writes, so the
    // analyser reads both; the date rides on its own line when it changes.
    static char last_date[11];
    if (strncmp(last_date, clock, 10) != 0) {
        snprintf(last_date, sizeof last_date, "%.10s", clock);
        snprintf(stamp, sizeof stamp, "=== DATE %s\r\n", last_date);
        if (!append(stamp)) return;
    }
    uint32_t up = millis() / 1000u;
    snprintf(stamp, sizeof stamp, "[%.8s.000] up=%lu:%02lu:%02lu ", clock + 11,
             (unsigned long)(up / 3600u), (unsigned long)((up / 60u) % 60u), (unsigned long)(up % 60u));
    if (!append(stamp) || !append(run_line) || !append("\r\n")) return;
    if (aud_line && aud_line[0]) {
        snprintf(stamp, sizeof stamp, "[%.8s.000] ", clock + 11);
        if (!append(stamp) || !append(aud_line) || !append("\r\n")) return;
    }
    sync_now();
}

extern "C" bool soak_log_active(void) { return g_open; }
