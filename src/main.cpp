// xroar-waveshare-rp2350-pizero — XRoar (Tandy CoCo) on mini-HDMI (PIZERO-09).
//
// Core 0: CoCo emulation (6809 + SAM + PIA + VDG) + serial/autotype keyboard +
//         FDC sector reads from SD. Blits the VDG output into a 320x240 RGB565
//         framebuffer once per emulated frame.
// Core 1: libdvi static-framebuffer worker — continuously TMDS-encodes g_fb and
//         scans it out at 640x480p60 (2x of 320x240). Decoupled from core 0, so
//         emulation speed never starves the display (tearing instead of dropouts).
//
// Display path: libdvi (PIO) on GPIO 32-39; see README.md. USB host deferred.

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"

extern "C" {
#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
}

extern "C" {
#include "ff.h"
#include "f_util.h"
}

#include "coco_boot.h"

extern "C" {
#include "coco_machine.h"
}

extern "C" void coco_machine_loadm_write(uint16_t addr, const uint8_t *src, uint16_t len);
extern "C" void coco_machine_jump(uint16_t entry);

static void loadm_write_cb(uint16_t addr, const uint8_t *data, uint16_t len, void *ctx) {
    (void)ctx;
    coco_machine_loadm_write(addr, data, len);
}

// ---- Display (libdvi) ----------------------------------------------------
#define FRAME_WIDTH  320
#define FRAME_HEIGHT 240
#define DVI_TIMING   dvi_timing_640x480p_60hz

// PIZERO-14: double-buffered. Core 0 renders into the back buffer, then swaps
// g_front at a frame boundary; core 1 samples g_front once per frame -> no tearing.
static uint16_t g_fb[2][FRAME_WIDTH * FRAME_HEIGHT] __attribute__((aligned(4)));
static const uint16_t * volatile g_front = g_fb[0];
static int g_back = 1;
static struct dvi_inst dvi0;

static void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    // Never returns: continuously encodes the current front buffer.
    dvi_static_framebuf_main_16bpp(&dvi0, &g_front);
}

// ---- Keyboard (serial + autotype, same as the AMOLED port) ---------------
static const char *g_autotype        = nullptr;
static int         g_autotype_warmup = 0;

enum {
    K_0 = 0x00, K_1, K_2, K_3, K_4, K_5, K_6, K_7,
    K_8 = 0x08, K_9, K_COLON, K_SEMI, K_COMMA, K_MINUS, K_DOT, K_SLASH,
    K_AT = 0x10, K_A, K_B, K_C, K_D, K_E, K_F, K_G,
    K_H = 0x18, K_I, K_J, K_K, K_L, K_M, K_N, K_O,
    K_P = 0x20, K_Q, K_R, K_S, K_T, K_U, K_V, K_W,
    K_X = 0x28, K_Y, K_Z, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_SPACE,
    K_ENTER = 0x30, K_CLEAR, K_BREAK,
    K_SHIFT = 0x37,
    K_INVALID = 0x3F,
};

static void ascii_to_dscan(char c, uint8_t *dscan_out, bool *shift_out) {
    *shift_out = false;
    if (c >= 'a' && c <= 'z') { *dscan_out = K_A + (c - 'a'); return; }
    if (c >= 'A' && c <= 'Z') { *dscan_out = K_A + (c - 'A'); return; }
    if (c >= '0' && c <= '9') { *dscan_out = K_0 + (c - '0'); return; }
    switch (c) {
        case ' ':  *dscan_out = K_SPACE; return;
        case '\r': case '\n': *dscan_out = K_ENTER; return;
        case ':':  *dscan_out = K_COLON; return;
        case ';':  *dscan_out = K_SEMI;  return;
        case ',':  *dscan_out = K_COMMA; return;
        case '-':  *dscan_out = K_MINUS; return;
        case '.':  *dscan_out = K_DOT;   return;
        case '/':  *dscan_out = K_SLASH; return;
        case '@':  *dscan_out = K_AT;    return;
        case '!':  *dscan_out = K_1; *shift_out = true; return;
        case '"':  *dscan_out = K_2; *shift_out = true; return;
        case '#':  *dscan_out = K_3; *shift_out = true; return;
        case '$':  *dscan_out = K_4; *shift_out = true; return;
        case '%':  *dscan_out = K_5; *shift_out = true; return;
        case '&':  *dscan_out = K_6; *shift_out = true; return;
        case '\'': *dscan_out = K_7; *shift_out = true; return;
        case '(':  *dscan_out = K_8; *shift_out = true; return;
        case ')':  *dscan_out = K_9; *shift_out = true; return;
        case '*':  *dscan_out = K_COLON; *shift_out = true; return;
        case '+':  *dscan_out = K_SEMI;  *shift_out = true; return;
        case '<':  *dscan_out = K_COMMA; *shift_out = true; return;
        case '=':  *dscan_out = K_MINUS; *shift_out = true; return;
        case '>':  *dscan_out = K_DOT;   *shift_out = true; return;
        case '?':  *dscan_out = K_SLASH; *shift_out = true; return;
        case 0x08: case 0x7F: *dscan_out = K_LEFT; return;
        case 0x03: case 0x1B: *dscan_out = K_BREAK; return;
        case 0x0C:           *dscan_out = K_CLEAR; return;
        default:   *dscan_out = K_INVALID; return;
    }
}

static int g_kb_hold = 0;
static int g_kb_gap  = 0;

static int next_keychar() {
    if (g_autotype) {
        if (g_autotype_warmup > 0) { g_autotype_warmup--; return -1; }
        char c = *g_autotype;
        if (c == '\0') { g_autotype = nullptr; return -1; }
        g_autotype++;
        return (unsigned char)c;
    }
    if (Serial.available()) return Serial.read();
    return -1;
}

static void pump_keyboard() {
    if (g_kb_hold > 0) {
        if (--g_kb_hold == 0) {
            coco_machine_release_all_keys();
            g_kb_gap = g_autotype ? 4 : 2;
        }
        return;
    }
    if (g_kb_gap > 0) { g_kb_gap--; return; }
    int c = next_keychar();
    if (c < 0) return;
    uint8_t dscan; bool shift;
    ascii_to_dscan((char)c, &dscan, &shift);
    if (dscan == K_INVALID) return;
    if (shift) coco_machine_press_key(K_SHIFT);
    coco_machine_press_key(dscan);
    g_kb_hold = g_autotype ? 5 : 3;
}

// ---- ROM/boot (same flow as the AMOLED port) -----------------------------
static uint8_t g_coco_rom[16384];
static uint8_t g_cart_rom[8192];
static bool    g_machine_running = false;

static bool mount_sd() {
    static FATFS fs;
    FRESULT fr = FR_DISK_ERR;
    for (int attempt = 0; attempt < 5; attempt++) {
        fr = f_mount(&fs, "0:", 1);
        if (fr == FR_OK) return true;
        Serial.printf("SD mount attempt %d: %s (%d)\n", attempt + 1, FRESULT_str(fr), fr);
        delay(200);
    }
    return false;
}

#define CYCLES_PER_FRAME 15000
#define FRAME_PERIOD_US  16762

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 1500) delay(10);
    Serial.println("\nXRoar on RP2350-PiZero (PIZERO-09)");

    // Bring up DVI first so we have a display even if SD/ROM fails.
    // Clear both buffers so the (static) black border is set in each.
    memset(g_fb, 0, sizeof(g_fb));
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    delay(10);
    pio_set_gpio_base(DVI_DEFAULT_SERIAL_CONFIG.pio, 16);   // TMDS on GPIO 32-39
    dvi0.timing  = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);        // ~252 MHz
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    multicore_launch_core1(core1_main);
    Serial.printf("DVI up: %dx%d -> 640x480p60, sys=%lu kHz\n",
                  FRAME_WIDTH, FRAME_HEIGHT, (unsigned long)(clock_get_hz(clk_sys) / 1000));

    if (!mount_sd()) { Serial.println("SD mount FAILED — no ROMs, idle."); return; }
    if (!coco_boot_load_rom_from_sd(g_coco_rom)) {
        Serial.println("ROM load FAILED (need /coco/bas12.rom [+extbas11.rom])"); return;
    }
    if (!coco_machine_init(g_coco_rom, sizeof(g_coco_rom))) {
        Serial.println("coco_machine_init failed"); return;
    }

    // Boot strategy from /coco/autorun.txt (see AUTORUN.md); default = Disk BASIC.
    static struct coco_autorun autorun = {};
    static char path[80];
    bool have_autorun = coco_boot_load_autorun(&autorun);

    bool direct = have_autorun && autorun.direct_name[0]
                  && coco_boot_resolve("bin", autorun.direct_name, path, sizeof(path));
    if (direct) {
        for (int i = 0; i < 30; i++) coco_machine_run_cycles(15000);  // let PIA DDRs settle
        uint16_t entry = 0;
        if (coco_boot_parse_loadm(path, loadm_write_cb, nullptr, &entry)) {
            Serial.printf("[autorun] DIRECT %s, jump $%04X\n", path, entry);
            coco_machine_jump(entry);
        }
    } else {
        const char *cart = (have_autorun && autorun.cart_name[0]) ? autorun.cart_name : "disk11.rom";
        if (coco_boot_load_cart_named(cart, g_cart_rom)) {
            coco_machine_install_cart(g_cart_rom);
            bool dsk = have_autorun && autorun.disk_name[0]
                       ? (coco_boot_resolve("dsk", autorun.disk_name, path, sizeof(path))
                          && coco_boot_attach_dsk(path))
                       : (coco_boot_find_default_dsk(path, sizeof(path))
                          && coco_boot_attach_dsk(path));
            if (dsk) {
                coco_machine_install_disk_reader(coco_boot_disk_read_sector);
                Serial.printf("[autorun] disk: %s\n", path);
            }
            if (have_autorun && autorun.autotype[0]) {
                g_autotype = autorun.autotype;
                g_autotype_warmup = 180;
            }
        }
    }
    g_machine_running = true;
    Serial.println("[main] coco_machine running");
}

void loop() {
    if (!g_machine_running) { delay(1000); return; }
    static uint32_t next_us = 0;
    if (next_us == 0) next_us = micros();

    pump_keyboard();
    uint32_t a = micros();
    coco_machine_run_cycles(CYCLES_PER_FRAME);
    uint32_t b = micros();
    coco_machine_render_frame();                  // regenerate VDG buffer (SUPPRESS_RENDER_SCANLINE)
    uint32_t c = micros();
    coco_boot_blit_vdg_pizero(g_fb[g_back]);      // render into the back buffer
    g_front = g_fb[g_back];                        // publish: core 1 picks it up at its next frame
    g_back ^= 1;
    uint32_t d = micros();

    // Pace to real time; resync if we fell behind rather than spiral.
    next_us += FRAME_PERIOD_US;
    int32_t rem = (int32_t)(next_us - micros());
    if (rem > 0) delayMicroseconds((uint32_t)rem);
    else         next_us = micros();

    static uint32_t last = 0, frames = 0;
    frames++;
    uint32_t now = millis();
    if (now - last >= 1000) {
        Serial.printf("[run] fps=%lu cpu=%luus render=%luus blit=%luus\n",
                      (unsigned long)frames, (unsigned long)(b - a),
                      (unsigned long)(c - b), (unsigned long)(d - c));
        frames = 0; last = now;
    }
}
