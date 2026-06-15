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
#include "hardware/gpio.h"
#include "hardware/structs/sysinfo.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

extern "C" {
#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
}

// PIZERO-11: Pico-PIO-USB host on the PIO-USB port (D+=28, D-=29). Wiki
// confirms host is supported (device_info demo enumerates a 2.4G wireless
// receiver as ID 05ac:0256). Open issue tracked by RP2350 errata E9 — pins
// configured as pull-down input read HIGH due to leak current; Pico-PIO-USB
// has a workaround gated on chip_version <= 2. We print the chip revision so
// we can tell whether the workaround applies on this part.
#include "pio_usb.h"
#include "Adafruit_TinyUSB.h"
#define HOST_PIN_DP  28
static Adafruit_USBH_Host USBHost;
static volatile uint32_t g_usb_devices = 0;
extern "C" uint32_t pio_usb_host_get_frame_number(void);

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

// PIZERO-02b spike: drop sys clock from 252 MHz to 240 MHz so Pico-PIO-USB
// (which asserts CPU == 120 or 240 MHz) can coexist with DVI. Same 640x480
// H/V layout as libdvi's 60 Hz preset, but bit_clk_khz = 240000 -> 24 MHz
// pixel clock -> ~57.14 Hz refresh. Off-spec vs CEA 640x480p60 (25.175 MHz)
// but well within typical HDMI monitor EDID tolerance.
static const struct dvi_timing dvi_timing_640x480p_57hz_240mhz = {
    .h_sync_polarity = false,
    // PIZERO-30: h_fp=14, h_bp=130 -> h_total = 880, refresh = 24e6/(880*525) =
    // 51.9481 Hz == exactly 32000/616 -> 616 audio samples/frame = 154 packets.
#ifdef HDMI_STD_TIMING
    .h_front_porch   = 58,   // STD-TIMING TEST: h_total 880->924 so 25.2MHz pixel keeps 51.95Hz
#else
    .h_front_porch   = 14,
#endif
    .h_sync_width    = 96,
    .h_back_porch    = 130,
    .h_active_pixels = 640,

    .v_sync_polarity = false,
    .v_front_porch   = 10,
    .v_sync_width    = 2,
    .v_back_porch    = 33,
    .v_active_lines  = 480,

#ifdef HDMI_STD_TIMING
    .bit_clk_khz     = 252000,   // STD-TIMING TEST: 25.2 MHz pixel (~standard 25.175), 252 MHz sysclk
#else
    .bit_clk_khz     = 240000,   // 24 MHz pixel clock, ~57.14 Hz refresh
#endif
};
#define DVI_TIMING   dvi_timing_640x480p_57hz_240mhz

// PIZERO-32: HDMI Audio Clock Regeneration CTS. The value that nulls audio pitch
// is MONITOR-DEPENDENT -- the sink derives the audio rate fs = pixel_clk*N/(128*CTS)
// from ITS assumed pixel clock, which may differ from our off-spec 24 MHz. Higher
// CTS -> lower pitch. 25176 suits the dev monitor (assumes the 25.175 MHz CEA
// clock); sinks that run sharp need more (~26674 drops ~1 semitone). Override with
// -DHDMI_ACR_CTS=<n>. N stays 6144.
#ifndef HDMI_ACR_CTS
#define HDMI_ACR_CTS 25176
#endif

// PIZERO-14: double-buffered. Core 0 renders into the back buffer, then swaps
// g_front at a frame boundary; core 1 samples g_front once per frame -> no tearing.
#ifdef HDMI_DATA_ISLAND
// PIZERO-30 (M1): HDMI-audio builds SINGLE-buffer the framebuffer. The ~150 KB
// freed is needed for the 43 per-vblank-line audio-island buffers (M2). Cost is
// tearing (core 1 may scan g_fb mid-blit); acceptable per docs/audio-decision.md.
static uint16_t g_fb[FRAME_WIDTH * FRAME_HEIGHT] __attribute__((aligned(4)));
static const uint16_t * volatile g_front = g_fb;
#else
// Product baseline: double-buffered, tear-free. Core 0 renders into the back
// buffer, then publishes g_front at a frame boundary; core 1 samples once/frame.
static uint16_t g_fb[2][FRAME_WIDTH * FRAME_HEIGHT] __attribute__((aligned(4)));
static const uint16_t * volatile g_front = g_fb[0];
static int g_back = 1;
#endif
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
// Track what autotype/serial last pressed so we can release just that key
// rather than nuking the whole matrix (which would also drop USB-held keys).
static uint8_t g_pump_last_dscan = K_INVALID;
static bool    g_pump_last_shift = false;

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
            // Release only what this pump pressed; leave USB-held keys alone.
            if (g_pump_last_dscan != K_INVALID) {
                coco_machine_release_key(g_pump_last_dscan);
            }
            if (g_pump_last_shift) coco_machine_release_key(K_SHIFT);
            g_pump_last_dscan = K_INVALID;
            g_pump_last_shift = false;
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
    g_pump_last_dscan = dscan;
    g_pump_last_shift = shift;
    g_kb_hold = g_autotype ? 5 : 3;
}

// ---- HID → DSCAN translation (PIZERO-12) ---------------------------------
// HID usage codes (boot keyboard) -> CoCo DSCAN. Sentinel 0xFF = no mapping.
// We can't use 0 for "unmapped" because K_0 = 0x00.
static uint8_t g_hid_to_dscan[256];

static void hid_table_init(void) {
    memset(g_hid_to_dscan, 0xFF, sizeof(g_hid_to_dscan));
    // 0x04..0x1D = a..z (boot keyboard usage). CoCo K_A..K_Z are contiguous.
    for (int i = 0; i < 26; i++) g_hid_to_dscan[0x04 + i] = (uint8_t)(K_A + i);
    // 0x1E..0x26 = 1..9, 0x27 = 0. CoCo K_0..K_9 contiguous but ordered 0..9.
    g_hid_to_dscan[0x1E] = K_1;  g_hid_to_dscan[0x1F] = K_2;
    g_hid_to_dscan[0x20] = K_3;  g_hid_to_dscan[0x21] = K_4;
    g_hid_to_dscan[0x22] = K_5;  g_hid_to_dscan[0x23] = K_6;
    g_hid_to_dscan[0x24] = K_7;  g_hid_to_dscan[0x25] = K_8;
    g_hid_to_dscan[0x26] = K_9;  g_hid_to_dscan[0x27] = K_0;
    // Misc.
    g_hid_to_dscan[0x28] = K_ENTER;   // Enter
    g_hid_to_dscan[0x29] = K_BREAK;   // Esc -> Break
    g_hid_to_dscan[0x2A] = K_LEFT;    // Backspace -> Left (CoCo delete key)
    g_hid_to_dscan[0x2C] = K_SPACE;   // Space
    g_hid_to_dscan[0x2D] = K_MINUS;   // -
    g_hid_to_dscan[0x33] = K_SEMI;    // ;
    g_hid_to_dscan[0x36] = K_COMMA;   // ,
    g_hid_to_dscan[0x37] = K_DOT;     // .
    g_hid_to_dscan[0x38] = K_SLASH;   // /
    g_hid_to_dscan[0x4F] = K_RIGHT;   // Right arrow
    g_hid_to_dscan[0x50] = K_LEFT;    // Left arrow
    g_hid_to_dscan[0x51] = K_DOWN;    // Down arrow
    g_hid_to_dscan[0x52] = K_UP;      // Up arrow
}

// PIZERO-11b: hot-replug watchdog attempt — DISABLED. The premise (read
// D+/D- via gpio_get + look for SE1) doesn't work because Pico-PIO-USB's
// TX state machine holds the pins in J state between transmissions, so
// the post-INOVER reading never changes on unplug. Bypassing the SM (pad
// OD trick) doesn't disambiguate either because RP2350 E9 leak current
// makes "no device" still look like weak-high. See PIZERO-11b for the
// referenced workarounds (external 8.2k pull-downs, A3/A4 silicon, or
// transaction-failure tracking inside the lib).

// HID boot-keyboard report state. tuh_hid_report_received_cb fires from
// USBHost.task() (called once per loop()), so this runs on core 0 alongside
// the CoCo machine — no cross-core sync needed.
static uint8_t g_hid_prev_codes[6] = {0};
static bool    g_hid_shift_prev = false;

static void hid_keyboard_apply(const uint8_t *report) {
    uint8_t mods = report[0];
    const uint8_t *codes = &report[2];
    bool shift = (mods & 0x22u) != 0;  // bit 1 = L-Shift, bit 5 = R-Shift

    // Modifier (shift only — CoCo has no Ctrl/Alt/Meta).
    if (shift && !g_hid_shift_prev) coco_machine_press_key(K_SHIFT);
    if (!shift && g_hid_shift_prev) coco_machine_release_key(K_SHIFT);
    g_hid_shift_prev = shift;

    // Releases: codes in prev but not in current.
    for (int i = 0; i < 6; i++) {
        uint8_t prev = g_hid_prev_codes[i];
        if (prev == 0) continue;
        bool still = false;
        for (int j = 0; j < 6; j++) if (codes[j] == prev) { still = true; break; }
        if (!still) {
            uint8_t ds = g_hid_to_dscan[prev];
            if (ds != 0xFF) coco_machine_release_key(ds);
        }
    }
    // Presses: codes in current but not in prev.
    for (int i = 0; i < 6; i++) {
        uint8_t curr = codes[i];
        if (curr == 0) continue;
        bool was = false;
        for (int j = 0; j < 6; j++) if (g_hid_prev_codes[j] == curr) { was = true; break; }
        if (!was) {
            uint8_t ds = g_hid_to_dscan[curr];
            if (ds != 0xFF) coco_machine_press_key(ds);
        }
    }
    memcpy(g_hid_prev_codes, codes, 6);
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
        Serial.printf("SD mount attempt %d: %s (%d)\r\n", attempt + 1, FRESULT_str(fr), fr);
        delay(200);
    }
    return false;
}

#ifdef HDMI_DATA_ISLAND
// PIZERO-30: pace emulation to the TRUE HDMI refresh. Pixel clock 24 MHz, frame
// is h_total(880) x v_total(525) = 462000 px, so refresh = 24e6/462000 =
// 51.9481 Hz, period = 19250 us. Matching the loop keeps the audio producer, the
// per-frame encoder, and the sink's consumption on one cadence. CYCLES_PER_FRAME
// (17226 / 894886.25 Hz = 19250 us) makes the emulator produce EXACTLY
// 32000/51.9481 = 616 audio samples/frame == the 154 packets we emit (no drift).
#define CYCLES_PER_FRAME 17226
#define FRAME_PERIOD_US  19250
#else
#define CYCLES_PER_FRAME 15000
#define FRAME_PERIOD_US  16762
#endif

#ifdef AUDIO_WAV_DUMP
// ---- PIZERO-18 audio validation: WAV-over-USB-CDC --------------------------
// Streams the emulator's PCM out the serial port as a base64-encoded WAV, framed
// by ---WAV-BEGIN---/---WAV-END--- markers, for a fixed window after boot. No
// hardware wiring: capture the serial log on the host, snip between the markers,
// `base64 -D > coco.wav`. Base64 (not raw) so a cooked tty/monitor can't mangle
// the bytes. All other debug prints are suppressed while dumping.
static const uint32_t AUDIO_DUMP_SECONDS = 9;
static bool     g_audio_dumping = false;
static uint32_t g_audio_dump_start_ms = 0;
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static uint8_t  g_b64_rem[3];
static int      g_b64_remn = 0;
static int      g_b64_col  = 0;

static void b64_emit(uint8_t a, uint8_t b, uint8_t c, int n) {
    char o[4];
    o[0] = B64[a >> 2];
    o[1] = B64[((a & 0x3) << 4) | (b >> 4)];
    o[2] = (n > 1) ? B64[((b & 0xF) << 2) | (c >> 6)] : '=';
    o[3] = (n > 2) ? B64[c & 0x3F] : '=';
    Serial.write((const uint8_t *)o, 4);
    if ((g_b64_col += 4) >= 76) { Serial.write((const uint8_t *)"\r\n", 2); g_b64_col = 0; }
}
static void b64_push(const uint8_t *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_b64_rem[g_b64_remn++] = p[i];
        if (g_b64_remn == 3) { b64_emit(g_b64_rem[0], g_b64_rem[1], g_b64_rem[2], 3); g_b64_remn = 0; }
    }
}
static void b64_flush(void) {
    if (g_b64_remn == 1)      b64_emit(g_b64_rem[0], 0, 0, 1);
    else if (g_b64_remn == 2) b64_emit(g_b64_rem[0], g_b64_rem[1], 0, 2);
    g_b64_remn = 0;
    if (g_b64_col) { Serial.write((const uint8_t *)"\r\n", 2); g_b64_col = 0; }
}
static void wav44(uint8_t h[44], uint32_t rate, uint32_t data_len) {
    uint32_t riff = 36 + data_len, br = rate * 2;       // mono, 16-bit
    memcpy(h, "RIFF", 4);
    h[4]=riff; h[5]=riff>>8; h[6]=riff>>16; h[7]=riff>>24;
    memcpy(h+8, "WAVEfmt ", 8);
    h[16]=16; h[17]=0; h[18]=0; h[19]=0;                // fmt chunk size
    h[20]=1;  h[21]=0;                                  // PCM
    h[22]=1;  h[23]=0;                                  // channels = 1
    h[24]=rate; h[25]=rate>>8; h[26]=rate>>16; h[27]=rate>>24;
    h[28]=br;  h[29]=br>>8;  h[30]=br>>16;  h[31]=br>>24;
    h[32]=2;  h[33]=0;                                  // block align
    h[34]=16; h[35]=0;                                  // bits/sample
    memcpy(h+36, "data", 4);
    h[40]=data_len; h[41]=data_len>>8; h[42]=data_len>>16; h[43]=data_len>>24;
}
static void audio_dump_begin(void) {
    uint32_t rate = coco_machine_audio_rate();
    uint32_t data_len = rate * 2 * AUDIO_DUMP_SECONDS;
    uint8_t h[44];
    wav44(h, rate, data_len);
    Serial.write((const uint8_t *)"\r\n---WAV-BEGIN---\r\n", 19);
    g_b64_remn = 0; g_b64_col = 0;
    b64_push(h, 44);
    g_audio_dumping = true;
    g_audio_dump_start_ms = millis();
}
static void audio_dump_pump(void) {
    int16_t buf[256];
    size_t n;
    while ((n = coco_machine_audio_read(buf, 256)) > 0) {
        b64_push((const uint8_t *)buf, n * 2);
        if (n < 256) break;
    }
    if (millis() - g_audio_dump_start_ms >= AUDIO_DUMP_SECONDS * 1000) {
        b64_flush();
        Serial.write((const uint8_t *)"\r\n---WAV-END---\r\n", 17);
        g_audio_dumping = false;
    }
}
#endif // AUDIO_WAV_DUMP

#if defined(HDMI_DATA_ISLAND) && defined(HDMI_AUDIO_SWAPTEST)
// PIZERO-30 (M0): swap-only IRQ viability check. Two IDENTICAL pre-encoded
// vblank-island banks; the IRQ callback merely ping-pongs the data-island
// read_addrs per vblank line -- NO encoding (the thing that broke video in
// Option A). Since both banks are identical, audio/video should be unchanged
// from the proven static build; the ONLY new thing exercised is the per-line
// read_addr store in IRQ context. If video stays solid, Model P (pointer-select
// in the IRQ) is safe and M2 can build the real per-line encoder on top.
static uint32_t g_isl2[2][3][320];          // [bank][lane][words] = 7.7 KB
static volatile uint32_t g_swap_ctr = 0;
static void __not_in_flash_func(swaptest_vblank_cb)(void) {
    uint32_t pp = g_swap_ctr++ & 1u;
    dvi0.dma_list_vblank_nosync.l0[3].read_addr = g_isl2[pp][0];
    dvi0.dma_list_vblank_nosync.l1[1].read_addr = g_isl2[pp][1];
    dvi0.dma_list_vblank_nosync.l2[1].read_addr = g_isl2[pp][2];
}
#endif // HDMI_DATA_ISLAND && HDMI_AUDIO_SWAPTEST

#if defined(HDMI_DATA_ISLAND) && !defined(HDMI_AUDIO_SWAPTEST) && !defined(HDMI_AUDIO_STATIC)
// PIZERO-30 (M2, Option B final): clean LIVE HDMI audio, distributed across the
// ACTIVE lines so the sink's audio FIFO never starves. Each of N_ALINES active
// scanlines (spread evenly over the 480) carries an audio data island in its
// (widened) back porch; ~154 packets/frame = ~616 samples @ ~51.8 Hz ~= 32 kHz.
// Double-buffered: core 0 fills the OFF bank and flips one word; the DMA IRQ
// latches the bank once per frame and only READS it -> no tearing. The IRQ does
// pointer stores only -- NO encoding (encoding in the IRQ broke video, Option A).
//
// Vblank carries just AVI + Audio InfoFrame + ACR (g_info, one line) + control.
// 32 kHz, LOOSE packing test: 616 samples/frame = 154 packets = 77 lines x 2.
// Spreading 154 packets over 77 lines (2/line) leaves ~44px control margin per
// island in the bp=130 back porch (vs 12px when 3/line) -- same refresh/rate,
// just less crammed. The split is what makes 77 per-line buffers affordable.
// Shared (both delivery schemes): the bp-island buffer width, the no-island
// active framing (used on lines with no audio packet), the vblank info/ACR + a
// control-only filler, and the IEC frame counter.
#define ALINE_BP_W   72     // bp-island buffer width; >= h_bp/2 (65 at h_bp=130)
static uint32_t g_bp0[ALINE_BP_W], g_bk1[ALINE_BP_W], g_bk2[ALINE_BP_W];  // no-island active framing
static uint32_t g_info[3][320];                   // vblank: AVI + Audio InfoFrame + ACR
static uint32_t g_ctrl[3][320];                   // vblank: control-only filler
static uint32_t g_aud_framectr = 0;               // IEC 60958 frame counter

#ifdef HDMI_STREAM_AUDIO
// PIZERO-38: STREAMING per-active-line delivery (Takano model). Instead of 77
// pre-encoded per-line banks, a tiny ROTATING POOL of bp-island buffers is
// encoded ONE LINE AHEAD in the active-line IRQ -- which already fires during
// the PREVIOUS line's scanout (dvi.cpp:239), giving the full ~33 us line budget
// for the ~4.8 us encode (PIZERO-36; it does NOT fit the 5.4 us back-porch
// window, so same-line encoding is exactly the old Option A blackout). A 16.16
// sample meter emits 0..4 samples on (nearly) every active line -> near-constant
// feed, killing the frame-rate warble of the bursty 77-line scheme. Pool RAM:
// 3 * APOOL * 72 * 4 ~= 5 KB (vs ~130 KB of banks). Encoding runs on core 1.
#define APOOL 6                                    // rotating slots (>=2 in flight + margin)
static uint32_t g_pool0[APOOL][ALINE_BP_W];
static uint32_t g_pool1[APOOL][ALINE_BP_W];
static uint32_t g_pool2[APOOL][ALINE_BP_W];
static int      g_pool_w     = 0;                  // next pool slot (IRQ-only)
static int32_t  g_meter_acc  = 0;                  // 16.16 sample accumulator (IRQ-only)
static int32_t  g_meter_step = 0;                  // 16.16 samples per active line (set at init)
#define AUDIO_SAMPLES_PER_FRAME 924                // 48000 Hz / ~51.95 Hz refresh
static int16_t  g_stream_last = 0;                 // hold-last on ring underrun (IRQ-only)
static volatile uint32_t g_stream_under = 0;       // ring-underrun (hold-last) count (diagnostic)
#ifdef HDMI_AUDIO_SYNTH
#define SYNTH_TBL 512
static int16_t  g_sine[SYNTH_TBL];                 // one 440 Hz period, filled on core 0
static uint32_t g_synth_ph   = 0;                  // 32-bit phase accumulator (IRQ-only)
static uint32_t g_synth_inc  = 0;                  // per-sample phase step (set at init)
#endif
#else
#define N_ALINES   77
static inline int aline_pkts(int line) { (void)line; return 3; }   // 77*3 = 231 pkts = 924 samp = 48 kHz
// Per-line audio-island banks. After the DMA split these are BP-ONLY (h_bp/2
// words) on every lane; double-buffered, core 0 fills the OFF bank per frame.
static uint32_t g_la0[2][N_ALINES][ALINE_BP_W];   // lane 0 back porch
static uint32_t g_la1[2][N_ALINES][ALINE_BP_W];   // lane 1 back porch
static uint32_t g_la2[2][N_ALINES][ALINE_BP_W];   // lane 2 back porch
static int16_t  g_aslot[480];                     // active scanline -> audio line idx, or -1
#ifdef HDMI_EVEN_AUDIO
// PIZERO-34: even delivery. Spread the same N_ALINES audio lines across ALL 523
// no-sync+active lines (active 0-479, fp 480-489, bp 490-522) instead of only the
// 480 active ones, so audio is delivered ~every 6.8 lines INCLUDING vblank -> no
// 1.6ms per-frame gap. g_vaslot maps a vblank line (fp 0-9, bp 10-42) to its audio
// line; g_vbp_dflt* are the control-only back-porch buffers used on non-audio
// vblank lines (saved from the DMA list after dvi_init).
static int16_t      g_vaslot[43];
static const void  *g_vbp_dflt0, *g_vbp_dflt1, *g_vbp_dflt2;
#endif
static volatile uint32_t g_active_bank = 0;       // core0 writes, IRQ reads
static uint32_t g_irq_bank = 0;                   // latched once per frame (IRQ only)
#endif // HDMI_STREAM_AUDIO

#ifdef HDMI_STREAM_AUDIO
// One mono sample for the streaming meter. Synth = a 440 Hz wavetable (no sinf in
// the IRQ); live = the CoCo ring (hold-last on underrun). Both run on core 1.
static inline int16_t __not_in_flash_func(stream_next_sample)(void) {
#ifdef HDMI_AUDIO_SYNTH
    int16_t s = g_sine[g_synth_ph >> 23];          // top 9 bits index the 512-entry table
    g_synth_ph += g_synth_inc;
    return s;
#else
    int16_t s;
    if (coco_machine_audio_read(&s, 1) < 1) { s = g_stream_last; g_stream_under++; }   // PIZERO-39: ring from IRQ
    g_stream_last = s;
    return s;
#endif
}

// DMA IRQ, ACTIVE scanline (fires during the PREVIOUS line's scanout): meter this
// line's sample count, encode one audio island into the next rotating-pool slot,
// and point the back-porch DMA blocks at it -- or at the no-island framing when
// the meter yields 0 samples. The full-line budget (~33 us) covers the ~4.8 us
// encode (PIZERO-36); same-line back-porch encoding would not (Option A).
static void __not_in_flash_func(stream_audio_cb)(void) {
    // Meter free-runs (acc stays in [0,1) sample): resetting at v==0 discarded the
    // fractional carry -> 923 vs the producer's 924 samp/frame -> slow ring drift.
    g_meter_acc += g_meter_step;
    int n = g_meter_acc >> 16;
    if (n > 4) n = 4;
    if (n <= 0) {                                  // no island this line: plain framing
        dvi0.dma_list_active.l0[2].read_addr = g_bp0;
        dvi0.dma_list_active.l1[1].read_addr = g_bk1;
        dvi0.dma_list_active.l2[1].read_addr = g_bk2;
        return;
    }
    g_meter_acc -= (int32_t)n << 16;
    int16_t lr[8];
    for (int f = 0; f < n; ++f) { int16_t s = stream_next_sample(); lr[2*f] = lr[2*f+1] = s; }
    dvi_data_packet_t pkt;
    dvi_di_set_audio_samples(&pkt, lr, n, g_aud_framectr);
    g_aud_framectr = (g_aud_framectr + (uint32_t)n) % 192u;
    dvi_di_compute_parity(&pkt);
    const int w = g_pool_w;
    g_pool_w = (w + 1) % APOOL;
    const int hv = ((!DVI_TIMING.v_sync_polarity) ? 2 : 0) | ((!DVI_TIMING.h_sync_polarity) ? 1 : 0);
    dvi_di_encode_header(&g_pool0[w][5], &pkt, hv, true);          // island at offset 5 (after preamble+guard)
    dvi_di_encode_subpacket(&g_pool1[w][5], &g_pool2[w][5], &pkt);
    dvi0.dma_list_active.l0[2].read_addr = g_pool0[w];
    dvi0.dma_list_active.l1[1].read_addr = g_pool1[w];
    dvi0.dma_list_active.l2[1].read_addr = g_pool2[w];
}
#else
// DMA IRQ, ACTIVE scanline: repoint the back-porch/blanking blocks at this line's
// audio buffer (or static framing). Latch the bank at the first active line.
static void __not_in_flash_func(active_audio_cb)(void) {
    const uint v = dvi0.timing_state.v_ctr;        // 0 .. v_active_lines-1
    if (v == 0) g_irq_bank = g_active_bank;
    const int j = (v < 480) ? g_aslot[v] : -1;
    const uint32_t *a0, *a1, *a2;
    if (j >= 0) { a0 = g_la0[g_irq_bank][j]; a1 = g_la1[g_irq_bank][j]; a2 = g_la2[g_irq_bank][j]; }
    else        { a0 = g_bp0;               a1 = g_bk1;               a2 = g_bk2; }
    dvi0.dma_list_active.l0[2].read_addr = a0;   // lane 0 back porch
    dvi0.dma_list_active.l1[1].read_addr = a1;   // lane 1 bp (split block [1])
    dvi0.dma_list_active.l2[1].read_addr = a2;   // lane 2 bp
}
#endif // HDMI_STREAM_AUDIO

// DMA IRQ, vblank (no-sync) line: AVI/AudioIF/ACR on the first back-porch line,
// control elsewhere. (Audio no longer lives in vblank.)
static void __not_in_flash_func(audio_vblank_info_cb)(void) {
    // PIZERO-30: send AVI+AudioIF+ACR on EVERY vblank line (was just v_ctr==0 ->
    // 1/frame). ~43 ACR/frame gives the sink's audio PLL frequent clock updates;
    // once/frame let it wobble at the frame rate. (Matches the proven static build.)
#ifdef HDMI_EVEN_AUDIO
    // PIZERO-34: SINGLE island per vblank line (dual islands break sink sync).
    // Audio-scheduled vblank lines carry audio in the back porch + CONTROL in the
    // active block; all other vblank lines carry ACR/AVI/AudioIF in the active
    // block + control bp. (g_irq_bank latched at active v==0, before vblank.)
    const uint vs = dvi0.timing_state.v_state, vc = dvi0.timing_state.v_ctr;
    const int vbi = (vs == DVI_STATE_FRONT_PORCH) ? (int)vc : (10 + (int)vc);  // fp 0-9, bp 10-42
    const int j = (vbi >= 0 && vbi < 43) ? g_vaslot[vbi] : -1;
    if (j >= 0) {
        // audio line: bp = audio island, active block = control (no info island)
        dvi0.dma_list_vblank_nosync.l0[2].read_addr = g_la0[g_irq_bank][j];
        dvi0.dma_list_vblank_nosync.l1[1].read_addr = g_la1[g_irq_bank][j];
        dvi0.dma_list_vblank_nosync.l2[1].read_addr = g_la2[g_irq_bank][j];
        dvi0.dma_list_vblank_nosync.l0[3].read_addr = g_ctrl[0];
        dvi0.dma_list_vblank_nosync.l1[2].read_addr = g_ctrl[1];
        dvi0.dma_list_vblank_nosync.l2[2].read_addr = g_ctrl[2];
    } else {
        // info line: ACR/AVI/AudioIF in active block, control bp
        dvi0.dma_list_vblank_nosync.l0[2].read_addr = g_vbp_dflt0;
        dvi0.dma_list_vblank_nosync.l1[1].read_addr = g_vbp_dflt1;
        dvi0.dma_list_vblank_nosync.l2[1].read_addr = g_vbp_dflt2;
        dvi0.dma_list_vblank_nosync.l0[3].read_addr = g_info[0];
        dvi0.dma_list_vblank_nosync.l1[2].read_addr = g_info[1];
        dvi0.dma_list_vblank_nosync.l2[2].read_addr = g_info[2];
    }
#else
    dvi0.dma_list_vblank_nosync.l0[3].read_addr = g_info[0];
    dvi0.dma_list_vblank_nosync.l1[2].read_addr = g_info[1];   // lane 1 active block is [2] after split
    dvi0.dma_list_vblank_nosync.l2[2].read_addr = g_info[2];
#endif
}

// Core 0 (after blit): re-encode the OFF bank's audio islands from the ring, flip.
// (Streaming delivery (PIZERO-38) encodes per-line in the IRQ instead -- see
// stream_audio_cb; the bank refill + its synth_fill helper are bank-path only.)
#if defined(HDMI_AUDIO_SYNTH) && !defined(HDMI_STREAM_AUDIO)
// CONTROL EXPERIMENT (PIZERO-30): bypass the emulator entirely and feed a
// mathematically clean 440 Hz sine straight into the HDMI audio islands. Same
// encoder + DMA + transport as live audio. A pure sine has ~no harmonics, so any
// roughness heard is the TRANSPORT, not the CoCo waveform / resampling.
#include <math.h>
static float g_synth_phase = 0.0f;
static void synth_fill(int16_t *out, int n) {
    const float inc = 2.0f * 3.14159265f * 440.0f / 48000.0f;   // 440 Hz @ 48 kHz
    for (int i = 0; i < n; ++i) {
        out[i] = (int16_t)(6000.0f * sinf(g_synth_phase));       // ~18% FS clean sine
        g_synth_phase += inc;
        if (g_synth_phase > 6.2831853f) g_synth_phase -= 6.2831853f;
    }
}
#endif

#ifndef HDMI_STREAM_AUDIO
static void audio_encode_frame(void) {
    const uint32_t off = g_active_bank ^ 1u;
    const int hv = ((!DVI_TIMING.v_sync_polarity) ? 2 : 0) | ((!DVI_TIMING.h_sync_polarity) ? 1 : 0);
    int16_t mono[12];
    int16_t lr[8];
    dvi_data_packet_t pkt;
    for (int line = 0; line < N_ALINES; ++line) {
        const int np = aline_pkts(line);
        const int want = np * 4;
#ifdef HDMI_AUDIO_SYNTH
        synth_fill(mono, want);                       // clean sine, no emulation
#else
        size_t n = coco_machine_audio_read(mono, (size_t)want);
        for (size_t i = n; i < (size_t)want; ++i) mono[i] = (i ? mono[i - 1] : 0);   // hold last on underrun
#endif
        for (int k = 0; k < np; ++k) {
            for (int f = 0; f < 4; ++f) { int16_t s = mono[k*4 + f]; lr[2*f] = lr[2*f+1] = s; }
            dvi_di_set_audio_samples(&pkt, lr, 4, g_aud_framectr);
            g_aud_framectr = (g_aud_framectr + 4) % 192u;
            dvi_di_compute_parity(&pkt);
            // Rewrite only the data words (preamble/guards/framing are static).
            // bp-only buffers after the split -> island at offset 0 on all lanes.
            dvi_di_encode_header(&g_la0[off][line][5 + 16 * k], &pkt, hv, k == 0);
            dvi_di_encode_subpacket(&g_la1[off][line][5 + 16 * k],
                                    &g_la2[off][line][5 + 16 * k], &pkt);
        }
    }
    __dmb();
    g_active_bank = off;
}
#endif // !HDMI_STREAM_AUDIO
#endif // HDMI_DATA_ISLAND && !SWAPTEST && !STATIC

// ── PIZERO-33: freeze auto-recovery + root-cause ──────────────────────────
// A hardware watchdog, petted once per loop() on core 0, auto-reboots the board
// if core 0 wedges anywhere (the recurring everyday freeze; suspected USB-host
// path per PIZERO-11b). Phase markers written to watchdog SCRATCH registers —
// which SURVIVE a watchdog reset (but not power-on) — record where core 0 last
// was, so after an auto-reboot we print exactly which operation hung. A magic in
// scratch[0] distinguishes a real freeze record from power-on garbage. PIZERO-33
// persistent log: scratch[3] packs a running freeze COUNT + the LAST stuck phase,
// so the tally survives across auto-reboots and is shown in the [run] telemetry —
// you can connect a monitor any time (while still powered) and see how many times
// and where it froze, without catching the recovery boot live.
//   scratch[0]=WD_MAGIC  scratch[1]=phase  scratch[2]=heartbeat
//   scratch[3]=[31:8]=freeze count  [7:0]=last-freeze phase
// Build with -DWATCHDOG_DISABLE to leave a freeze frozen (live debugging).
#ifndef WATCHDOG_TIMEOUT_MS
#define WATCHDOG_TIMEOUT_MS 3000   // >> the ~19ms frame loop; only a real hang trips it
#endif
#define WD_MAGIC 0x05330534u   // bumped for scratch[3] freeze-log layout (invalidates stale scratch)
enum { WP_NONE = 0, WP_SETUP, WP_LOOP, WP_USB, WP_KBD, WP_EMU, WP_RENDER, WP_BLIT, WP_AUDIO, WP_PACE };
static uint32_t g_freeze_count = 0;        // persistent across reboots (from scratch[3])
static uint32_t g_last_freeze_phase = WP_NONE;
static const char *wd_phase_name(uint32_t p) {
    switch (p) {
        case WP_SETUP: return "SETUP";       case WP_LOOP:   return "loop-top";
        case WP_USB:   return "USBHost.task"; case WP_KBD:   return "keyboard";
        case WP_EMU:   return "emulate";     case WP_RENDER: return "VDG-render";
        case WP_BLIT:  return "blit";        case WP_AUDIO:  return "audio-encode";
        case WP_PACE:  return "frame-pace";  default:        return "?";
    }
}
#ifdef WATCHDOG_DISABLE
static inline void wd_phase(uint32_t p)     { (void)p; }
static inline void wd_heartbeat(uint32_t h) { (void)h; }
#else
static inline void wd_phase(uint32_t p)     { watchdog_hw->scratch[1] = p; }
static inline void wd_heartbeat(uint32_t h) { watchdog_hw->scratch[2] = h; }
#endif

void setup() {
    Serial.begin(115200);
    // Bump wait + slow ramp so a freshly-reconnected USB-CDC monitor catches
    // boot banners. (loop() also repeats a recap for the first ~5 s.)
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 4000) delay(50);
    delay(200);  // a little extra room after the host attaches
    Serial.print("\r\nXRoar on RP2350-PiZero (PIZERO-09)\r\n");
    Serial.flush();

#ifndef WATCHDOG_DISABLE
    // PIZERO-33: carry the freeze tally across reboots (scratch survives a watchdog
    // reset, not power-on; WD_MAGIC guards validity).
    // A real freeze = a genuine watchdog TIMER timeout (loop() stopped petting).
    // A FORCEd reboot (power-on / manual reset / picotool / flash) is NOT TIMER, so
    // it starts a FRESH session (count back to 0) -- the tally means "freezes since
    // I last started it", and this also auto-clears the self-test's count on deploy.
    bool freeze_reboot = (watchdog_hw->reason & WATCHDOG_REASON_TIMER_BITS)
                         && watchdog_hw->scratch[0] == WD_MAGIC;
    if (freeze_reboot) {
        g_freeze_count = watchdog_hw->scratch[3] >> 8;
        if (g_freeze_count > 1000000u) g_freeze_count = 0;      // garbage guard
        g_last_freeze_phase = watchdog_hw->scratch[1];
        g_freeze_count++;
        Serial.printf("[watchdog] *** FREEZE RECOVERED *** core 0 was stuck in '%s' after ~%lu frames (~%lus); freeze #%lu this session -> auto-rebooted\r\n",
                      wd_phase_name(g_last_freeze_phase),
                      (unsigned long)watchdog_hw->scratch[2],
                      (unsigned long)(watchdog_hw->scratch[2] / 52u),
                      (unsigned long)g_freeze_count);
        Serial.flush();
    } else {
        g_freeze_count = 0;
        g_last_freeze_phase = WP_NONE;
    }
    watchdog_hw->scratch[0] = WD_MAGIC;
    watchdog_hw->scratch[3] = (g_freeze_count << 8) | (g_last_freeze_phase & 0xFF);
    wd_phase(WP_SETUP);
    wd_heartbeat(0);
#endif

    // Bring up DVI first so we have a display even if SD/ROM fails.
    // Clear both buffers so the (static) black border is set in each.
    memset(g_fb, 0, sizeof(g_fb));
#ifdef HDMI_STD_TIMING
    vreg_set_voltage(VREG_VOLTAGE_1_25);   // STD-TIMING TEST: extra headroom for 252 MHz
#else
    vreg_set_voltage(VREG_VOLTAGE_1_20);
#endif
    delay(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);        // 240 MHz (PIZERO-02b) / 252 MHz std-timing

    // Read RP2350 chip revision — relevant to errata E9 (pull-down input
    // reads HIGH due to leak current). Pico-PIO-USB applies a workaround
    // only when chip_version <= 2 (A0..A2). Print so we know our part.
    uint32_t chip_id = *((volatile uint32_t *)(SYSINFO_BASE + SYSINFO_CHIP_ID_OFFSET));
    uint32_t chip_rev = (chip_id & SYSINFO_CHIP_ID_REVISION_BITS) >>
                        SYSINFO_CHIP_ID_REVISION_LSB;
    Serial.printf("RP2350 chip_id=%08lx revision=%lu (E9 workaround active: %s)\r\n",
                  (unsigned long)chip_id, (unsigned long)chip_rev,
                  chip_rev <= 2 ? "yes" : "NO — may explain pull-down-reads-HIGH");
    Serial.flush();

    hid_table_init();   // PIZERO-12: build HID-usage -> CoCo-DSCAN lookup.

    // PIZERO-11: PIO-USB host on core 0, PIO 1 (libdvi will own PIO 0).
    // Init BEFORE multicore_launch_core1 — alarm_pool_create() needs
    // cross-core sync that deadlocks if core 1 is already in libdvi's
    // DMA-IRQ loop.
#ifndef HDMI_STD_TIMING   // PIO-USB asserts CPU==120||240MHz; skip at 252 MHz std-timing test
    {
        pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
        pio_cfg.pin_dp     = HOST_PIN_DP;
        pio_cfg.pio_tx_num = 1;
        pio_cfg.pio_rx_num = 1;
        USBHost.configure_pio_usb(1, &pio_cfg);
        // Default any HID interface to boot protocol at mount — that's how the
        // common wireless keyboard/mouse dongles want to be talked to (and
        // simplifies the report layout: 8-byte boot keyboard, 3+1+ byte boot
        // mouse). Has to be set BEFORE begin().
        tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
        USBHost.begin(1);
        Serial.printf("USB host up: PIO-USB D+=%d/D-=%d on pio1\r\n",
                      HOST_PIN_DP, HOST_PIN_DP + 1);
        Serial.flush();
    }
#else
    Serial.print("STD-TIMING TEST: USB host SKIPPED (252 MHz)\r\n");
#endif

    pio_set_gpio_base(DVI_DEFAULT_SERIAL_CONFIG.pio, 16);   // TMDS on GPIO 32-39
    dvi0.timing  = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
#ifdef HDMI_DATA_ISLAND
    // M2 (PIZERO-28): inject one benign NULL data island into every vertical-
    // blanking (no-sync) line. Off-screen + identical pixel count, so a
    // malformed island cannot disturb the picture. Acceptance test: video stays
    // rock-solid on the monitor (proves data-island injection doesn't break sync
    // on this board's wiring/timing). Must run before core 1 starts the DVI.
    {
        dvi_di_init();
#ifdef HDMI_ENCODE_BENCH
        // PIZERO-36 (Stage 0): measure the cost of ONE per-active-line audio
        // encode now that the encoder is RAM-resident (PIZERO-37). This is the
        // go/no-go number for encoding inside the core-1 DMA IRQ. The back-porch
        // window at h_bp=130px / 24 MHz pixel is ~5.42 us; the full line ~33 us.
        {
            static uint32_t b0[24], b1[24], b2[24];   // one island = 16 words + slack
            dvi_data_packet_t bpk;
            int16_t blr[8] = {1000,1000,-1000,-1000,2000,2000,-2000,-2000};
            volatile uint32_t sink = 0;
            // Call through volatile fn-pointers so the compiler can't inline the
            // encoder back into this (flash) bench -- forces the standalone
            // RAM-resident copies (PIZERO-37), which is what the IRQ will use.
            void (*volatile f_samp)(dvi_data_packet_t*, const int16_t*, int, uint32_t) = dvi_di_set_audio_samples;
            void (*volatile f_par)(dvi_data_packet_t*) = dvi_di_compute_parity;
            void (*volatile f_hdr)(uint32_t*, const dvi_data_packet_t*, int, bool) = dvi_di_encode_header;
            void (*volatile f_sub)(uint32_t*, uint32_t*, const dvi_data_packet_t*) = dvi_di_encode_subpacket;
            const int ITERS = 20000;
            uint32_t t0 = time_us_32();
            for (int i = 0; i < ITERS; ++i) {
                f_samp(&bpk, blr, 2, (uint32_t)(i & 0xff));
                f_par(&bpk);
                f_hdr(&b0[5], &bpk, 0, true);
                f_sub(&b1[5], &b2[5], &bpk);
                sink += b0[5] ^ b1[6] ^ b2[7];
            }
            uint32_t dt = time_us_32() - t0;
            Serial.printf("[bench] 1-packet encode: %u iters in %u us -> %u ns/encode (sink=%lu)\r\n",
                          (unsigned)ITERS, (unsigned)dt, (unsigned)((uint64_t)dt * 1000u / ITERS), (unsigned long)sink);
            Serial.printf("[bench] budget: back-porch ~5420 ns, full-line ~33333 ns\r\n");
        }
#endif
#if defined(HDMI_AUDIO_SWAPTEST) || defined(HDMI_AUDIO_STATIC)
        static uint32_t bp0[72], bk1[128], bk2[128];        // active-line framing
        dvi_data_packet_t pkts[6];
        dvi_di_set_avi_infoframe(&pkts[0], 0);
        dvi_di_set_audio_infoframe(&pkts[1], 1 /*2ch*/, DVI_AUDIO_SF_48K, DVI_AUDIO_SS_16);
        dvi_di_set_acr(&pkts[2], 25176, 6144);   // TEST: CTS for assumed 25.175 MHz sink clock
        static int16_t tone[12 * 2];
        for (int i = 0; i < 12; ++i) { int16_t v = (i < 6) ? 8000 : -8000; tone[2*i] = tone[2*i+1] = v; }
        dvi_di_set_audio_samples(&pkts[3], &tone[0],  4, 0);
        dvi_di_set_audio_samples(&pkts[4], &tone[8],  4, 4);
        dvi_di_set_audio_samples(&pkts[5], &tone[16], 4, 8);
        for (int i = 0; i < 6; ++i) dvi_di_compute_parity(&pkts[i]);
#endif
#if defined(HDMI_AUDIO_SWAPTEST)
        for (int b = 0; b < 2; ++b)
            dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false,
                                                 &dvi0.dma_list_vblank_nosync, pkts, 6,
                                                 g_isl2[b][0], g_isl2[b][1], g_isl2[b][2]);
        dvi0.vblank_callback = swaptest_vblank_cb;
        dvi_setup_active_hdmi_framing(&DVI_TIMING, dvi0.dma_cfg, &dvi0.dma_list_active, bp0, bk1, bk2);
        Serial.print("[hdmi] M0 swaptest: per-line read_addr ping-pong\r\n");
#elif defined(HDMI_AUDIO_STATIC)
        static uint32_t isl0[320], isl1[320], isl2[320];
        dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false,
                                             &dvi0.dma_list_vblank_nosync, pkts, 6, isl0, isl1, isl2);
        dvi_setup_active_hdmi_framing(&DVI_TIMING, dvi0.dma_cfg, &dvi0.dma_list_active, bp0, bk1, bk2);
        Serial.print("[hdmi] M4 step2a: static test tone in vblank\r\n");
#else
        // M2 final: vblank carries AVI/AudioIF/ACR; live audio is on ACTIVE lines.
        dvi_data_packet_t ipk[3];
        dvi_di_set_avi_infoframe(&ipk[0], 0);
        dvi_di_set_audio_infoframe(&ipk[1], 1 /*2ch*/, DVI_AUDIO_SF_48K, DVI_AUDIO_SS_16);
#ifdef HDMI_STD_TIMING
        dvi_di_set_acr(&ipk[2], 25200, 6144);    // STD-TIMING TEST: CTS for actual ~25.2 MHz pixel
#else
        dvi_di_set_acr(&ipk[2], HDMI_ACR_CTS, 6144);   // PIZERO-32: monitor-tunable (default 25176)
#endif
        for (int i = 0; i < 3; ++i) dvi_di_compute_parity(&ipk[i]);
        dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false,
                                             &dvi0.dma_list_vblank_nosync, ipk, 3, g_info[0], g_info[1], g_info[2]);
        dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false,
                                             &dvi0.dma_list_vblank_nosync, ipk, 0, g_ctrl[0], g_ctrl[1], g_ctrl[2]);
        dvi0.vblank_callback = audio_vblank_info_cb;
        // Silence island, used to lay the static framing (preamble/guards/video
        // preamble) into the per-line buffers; the data words get rewritten later.
        dvi_data_packet_t sil[3];
        int16_t z[8] = {0,0,0,0,0,0,0,0};
        for (int i = 0; i < 3; ++i) { dvi_di_set_audio_samples(&sil[i], z, 4, (uint32_t)(i * 4)); dvi_di_compute_parity(&sil[i]); }
#ifdef HDMI_STREAM_AUDIO
        // PIZERO-38: lay framing + a silence island into each rotating-pool slot
        // (the per-line IRQ rewrites only the island words). Pool-init repoints
        // dma_list transiently; framing last leaves the no-island default until
        // stream_audio_cb takes over on the first active line.
        for (int w = 0; w < APOOL; ++w)
            dvi_setup_active_audio_line(&DVI_TIMING, dvi0.dma_cfg, &dvi0.dma_list_active,
                                        g_pool0[w], g_pool1[w], g_pool2[w], sil, 1, true);
        dvi_setup_active_hdmi_framing(&DVI_TIMING, dvi0.dma_cfg, &dvi0.dma_list_active, g_bp0, g_bk1, g_bk2);
        // 16.16 samples per active line so AUDIO_SAMPLES_PER_FRAME spreads over 480.
        g_meter_step = (int32_t)(((int64_t)AUDIO_SAMPLES_PER_FRAME << 16) / 480);
#ifdef HDMI_AUDIO_SYNTH
        for (int i = 0; i < SYNTH_TBL; ++i)
            g_sine[i] = (int16_t)(6000.0f * sinf((float)i * (2.0f * 3.14159265f / SYNTH_TBL)));   // ~18% FS
        g_synth_inc = (uint32_t)((double)440.0 / 48000.0 * 4294967296.0);   // 440 Hz @ 48 kHz, 32-bit phase
        Serial.print("[hdmi] M2 STREAM: 440 Hz synth, per-line IRQ encode (PIZERO-38)\r\n");
#else
        Serial.print("[hdmi] M2 STREAM: live per-line IRQ encode (PIZERO-38)\r\n");
#endif
        dvi0.active_line_callback = stream_audio_cb;
#else
#ifdef HDMI_EVEN_AUDIO
        // Capture the control-only back-porch buffers the base vblank setup left
        // (dvi_init set these; the info-island setup above only touched the active
        // block), so non-audio vblank lines can restore them in the IRQ.
        g_vbp_dflt0 = dvi0.dma_list_vblank_nosync.l0[2].read_addr;
        g_vbp_dflt1 = dvi0.dma_list_vblank_nosync.l1[1].read_addr;
        g_vbp_dflt2 = dvi0.dma_list_vblank_nosync.l2[1].read_addr;
#endif
        for (int b = 0; b < 2; ++b)
            for (int line = 0; line < N_ALINES; ++line) {
#ifdef HDMI_EVEN_AUDIO
                const int gi = (int)((long)line * 523 / N_ALINES);   // global line idx
                const bool vf = (gi < 480);                          // active -> video follows
                struct dvi_scanline_dma_list *L = vf ? &dvi0.dma_list_active : &dvi0.dma_list_vblank_nosync;
#else
                const bool vf = true;
                struct dvi_scanline_dma_list *L = &dvi0.dma_list_active;
#endif
                dvi_setup_active_audio_line(&DVI_TIMING, dvi0.dma_cfg, L,
                                            g_la0[b][line], g_la1[b][line], g_la2[b][line],
                                            sil, aline_pkts(line), vf);
            }
        // Static framing for non-audio active lines (also = dma_list_active default).
        dvi_setup_active_hdmi_framing(&DVI_TIMING, dvi0.dma_cfg, &dvi0.dma_list_active, g_bp0, g_bk1, g_bk2);
#ifdef HDMI_EVEN_AUDIO
        // Spread N_ALINES audio lines across ALL 523 lines (active 0-479, fp/bp).
        for (int i = 0; i < 480; ++i) g_aslot[i]  = -1;
        for (int i = 0; i < 43;  ++i) g_vaslot[i] = -1;
        for (int j = 0; j < N_ALINES; ++j) {
            const int gi = (int)((long)j * 523 / N_ALINES);
            if (gi < 480) g_aslot[gi] = (int16_t)j; else g_vaslot[gi - 480] = (int16_t)j;
        }
        Serial.print("[hdmi] M2: EVEN audio across active+vblank (PIZERO-34)\r\n");
#else
        // Spread N_ALINES audio lines evenly across the 480 active scanlines.
        for (int i = 0; i < 480; ++i) g_aslot[i] = -1;
        for (int j = 0; j < N_ALINES; ++j) g_aslot[(j * 480) / N_ALINES] = (int16_t)j;
        Serial.print("[hdmi] M2: active-line audio, ~924 samp/frame @ 51.95Hz\r\n");
#endif
        dvi0.active_line_callback = active_audio_cb;
#endif // HDMI_STREAM_AUDIO
#endif
    }
#endif
    multicore_launch_core1(core1_main);
    Serial.printf("DVI up: %dx%d -> 640x480 ~57Hz, sys=%lu kHz (PIZERO-02b)\r\n",
                  FRAME_WIDTH, FRAME_HEIGHT, (unsigned long)(clock_get_hz(clk_sys) / 1000));
    Serial.flush();

    if (!mount_sd()) { Serial.print("SD mount FAILED — no ROMs, idle.\r\n"); return; }
    if (!coco_boot_load_rom_from_sd(g_coco_rom)) {
        Serial.print("ROM load FAILED (need /coco/bas12.rom [+extbas11.rom])\r\n"); return;
    }
    if (!coco_machine_init(g_coco_rom, sizeof(g_coco_rom))) {
        Serial.print("coco_machine_init failed\r\n"); return;
    }

    // Boot strategy from /coco/autorun.txt (see AUTORUN.md); default = Disk BASIC.
    static struct coco_autorun autorun = {};
    static char path[80];
#ifdef AUDIO_WAV_DUMP
    // Validation build: ignore autorun and boot a clean BASIC OK prompt so the
    // injected SOUND test program actually runs. (The SD autorun.txt otherwise
    // DIRECT-loads a game — e.g. spacewarp.bin — and our keystrokes go nowhere.)
    (void)autorun; (void)path;
#else
    bool have_autorun = coco_boot_load_autorun(&autorun);

    bool direct = have_autorun && autorun.direct_name[0]
                  && coco_boot_resolve("bin", autorun.direct_name, path, sizeof(path));
    if (direct) {
        for (int i = 0; i < 30; i++) coco_machine_run_cycles(15000);  // let PIA DDRs settle
        uint16_t entry = 0;
        if (coco_boot_parse_loadm(path, loadm_write_cb, nullptr, &entry)) {
            Serial.printf("[autorun] DIRECT %s, jump $%04X\r\n", path, entry);
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
                Serial.printf("[autorun] disk: %s\r\n", path);
            }
            if (have_autorun && autorun.autotype[0]) {
                g_autotype = autorun.autotype;
                g_autotype_warmup = 180;
            }
        }
    }
#endif // AUDIO_WAV_DUMP
    g_machine_running = true;
    Serial.print("[main] coco_machine running\r\n");
#ifdef AUDIO_WAV_DUMP
    // PIZERO-18 validation build: the capture is armed from loop() once the
    // USB-CDC host attaches (so flashing then opening the monitor doesn't miss
    // the window during the post-upload reset). See docs/audio-decision.md.
#endif
}

void loop() {
#ifndef WATCHDOG_DISABLE
    // PIZERO-33: arm on the first iteration, then pet every loop. If core 0 wedges
    // in any phase below for > WATCHDOG_TIMEOUT_MS, the watchdog reboots the board
    // and the next boot reports the stuck phase (wd_phase markers in scratch).
    static bool wd_armed = false;
    if (!wd_armed) { wd_armed = true; watchdog_enable(WATCHDOG_TIMEOUT_MS, true); }
    watchdog_update();
    static uint32_t hb = 0; wd_heartbeat(++hb);
#ifdef WATCHDOG_SELFTEST
    // PIZERO-33 validation: after ~10s, deliberately wedge core 0 in a known phase.
    // Expect: ~3s later the watchdog reboots and the banner reports stuck in 'emulate'.
    if (hb == 520) { wd_phase(WP_EMU);
        Serial.print("[selftest] forcing a core-0 hang in phase 'emulate' (watchdog should reboot in ~3s)...\r\n");
        Serial.flush();
        for (;;) tight_loop_contents();
    }
#endif
#endif
    wd_phase(WP_USB);
    USBHost.task();   // PIZERO-11: service USB host transfers (prime freeze suspect).
    wd_phase(WP_LOOP);
    if (!g_machine_running) { delay(1000); return; }
    static uint32_t next_us = 0;
    if (next_us == 0) next_us = micros();

    wd_phase(WP_KBD);
    pump_keyboard();
    uint32_t a = micros();
    wd_phase(WP_EMU);
    coco_machine_run_cycles(CYCLES_PER_FRAME);
    uint32_t b = micros();
    wd_phase(WP_RENDER);
    coco_machine_render_frame();                  // regenerate VDG buffer (SUPPRESS_RENDER_SCANLINE)
    uint32_t c = micros();
    wd_phase(WP_BLIT);
#ifdef HDMI_DATA_ISLAND
    coco_boot_blit_vdg_pizero(g_fb);              // single buffer; g_front already points at it
#else
    coco_boot_blit_vdg_pizero(g_fb[g_back]);      // render into the back buffer
    g_front = g_fb[g_back];                        // publish: core 1 picks it up at its next frame
    g_back ^= 1;
#endif
    uint32_t d = micros();
    wd_phase(WP_AUDIO);
#if defined(HDMI_DATA_ISLAND) && !defined(HDMI_AUDIO_SWAPTEST) && !defined(HDMI_AUDIO_STATIC) && !defined(HDMI_STREAM_AUDIO)
    audio_encode_frame();                          // M2 (bank path): refill the OFF bank's audio islands
    // (HDMI_STREAM_AUDIO encodes per-line in the IRQ instead -- nothing to do here.)
#endif
    uint32_t e = micros();
    (void)e;
    wd_phase(WP_PACE);

#ifdef AUDIO_WAV_DUMP
    // Arm on USB-CDC attach (DTR), then autotype a deterministic ascending-tone
    // program (SOUND = core Color BASIC, no ECB) and stream a base64 WAV.
    static bool dump_armed = false;
    if (!dump_armed && Serial) {
        dump_armed = true;
        g_autotype = "\rSOUND159,255\r";   // PIZERO-32 measure: sustained ~440Hz A for freq analysis
        g_autotype_warmup = 120;   // ~2 s for the OK prompt to settle
        audio_dump_begin();
    }
    if (g_audio_dumping) audio_dump_pump();
#endif

    // Pace to real time; resync if we fell behind rather than spiral.
    next_us += FRAME_PERIOD_US;
    int32_t rem = (int32_t)(next_us - micros());
    if (rem > 0) delayMicroseconds((uint32_t)rem);
    else         next_us = micros();

    static uint32_t last = 0, frames = 0;
    frames++;
    uint32_t now = millis();
    if (now - last >= 1000) {
#ifdef AUDIO_WAV_DUMP
        if (g_audio_dumping) { frames = 0; last = now; return; }  // don't corrupt the base64 stream
#endif
        // PIZERO-11 diagnostic: D+/D- (post-INOVER, as PIO-USB sees them),
        // SOF frame counter (proves the 1 ms timer is firing), USB devices
        // mounted. With a FS device the lib expects D+=1 D-=0 (J state);
        // with an LS device D+=0 D-=1 (K state); empty bus = both 0.
        int dp = gpio_get(HOST_PIN_DP);
        int dm = gpio_get(HOST_PIN_DP + 1);
        uint32_t sof = pio_usb_host_get_frame_number();
        Serial.printf("[run] fps=%lu cpu=%luus render=%luus blit=%luus aud=%luus "
                      "| D+=%d D-=%d sof=%lu usb=%lu | freezes=%lu last=%s\r\n",
                      (unsigned long)frames, (unsigned long)(b - a),
                      (unsigned long)(c - b), (unsigned long)(d - c),
                      (unsigned long)(e - d),
                      dp, dm, (unsigned long)sof,
                      (unsigned long)g_usb_devices,
                      (unsigned long)g_freeze_count,
                      g_freeze_count ? wd_phase_name(g_last_freeze_phase) : "none");
#ifdef HDMI_STREAM_AUDIO
        // PIZERO-38/39 audio-ring health: fill should hover near AUDIO_RING/2 with
        // skips (overflow) and under (underrun) staying ~flat once primed.
        { uint32_t fill = 0, skips = 0;
          coco_machine_audio_stats(&fill, &skips);
          Serial.printf("[aud] ring fill=%lu skips=%lu under=%lu\r\n",
                        (unsigned long)fill, (unsigned long)skips, (unsigned long)g_stream_under); }
#endif
        frames = 0; last = now;
    }
}

// ---- USB host HID callbacks (PIZERO-11) ---------------------------------
// TinyUSB looks up these weak symbols by C name; extern "C" prevents mangling.
extern "C" {

void tuh_mount_cb(uint8_t daddr) {
    g_usb_devices++;
    Serial.printf("[usb] device attached, addr=%u\r\n", daddr);
}

void tuh_umount_cb(uint8_t daddr) {
    if (g_usb_devices) g_usb_devices--;
    Serial.printf("[usb] device removed, addr=%u\r\n", daddr);
}

void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx,
                      uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    uint8_t proto = tuh_hid_interface_protocol(daddr, idx);
    const char *kind = (proto == HID_ITF_PROTOCOL_KEYBOARD) ? "keyboard"
                     : (proto == HID_ITF_PROTOCOL_MOUSE)    ? "mouse"
                                                            : "generic";
    Serial.printf("[usb] HID mount addr=%u idx=%u proto=%u (%s) desc_len=%u\r\n",
                  daddr, idx, proto, kind, desc_len);
    if (!tuh_hid_receive_report(daddr, idx)) {
        Serial.printf("[usb] tuh_hid_receive_report FAILED idx=%u\r\n", idx);
    } else {
        Serial.printf("[usb] receive_report armed idx=%u\r\n", idx);
    }
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx) {
    Serial.printf("[usb] HID unmount addr=%u idx=%u\r\n", daddr, idx);
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t idx,
                                uint8_t const *report, uint16_t len) {
    // PIZERO-12: treat any 8-byte report as boot keyboard (our test dongle
    // mislabels its keyboard interface as proto=2/mouse). 3-byte report is
    // a boot mouse (no consumer yet — PIZERO-13). Other lengths: ignore.
    if (len >= 8) {
        hid_keyboard_apply(report);
    }
    tuh_hid_receive_report(daddr, idx);
}

} // extern "C"
