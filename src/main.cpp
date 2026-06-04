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
    .h_front_porch   = 16,
    .h_sync_width    = 96,
    // PIZERO-30: widened from 48 -> 66 px so an audio data island (44px) + a
    // 12px control gap + the 8px video preamble + 2px guard all fit in an ACTIVE
    // line's back porch -> lets us distribute HDMI audio evenly across active
    // lines (vblank-only burst overran the sink FIFO). h_total 800->818 ->
    // refresh ~55.9 Hz (was ~57.14). bp0 framing buffer must be >= 66/2 = 33 words.
    .h_back_porch    = 66,
    .h_active_pixels = 640,

    .v_sync_polarity = false,
    .v_front_porch   = 10,
    .v_sync_width    = 2,
    .v_back_porch    = 33,
    .v_active_lines  = 480,

    .bit_clk_khz     = 240000,   // 24 MHz pixel clock, ~57.14 Hz refresh
};
#define DVI_TIMING   dvi_timing_640x480p_57hz_240mhz

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
// is h_total(818) x v_total(525) = 429450 px, so refresh = 24e6/429450 =
// 55.886 Hz, period = 17893 us. Matching the loop to it keeps the audio
// producer, the per-frame encoder, and the sink's consumption on one cadence.
// CYCLES_PER_FRAME scales to keep emulation real-time (16013 / 894886.25 Hz =
// 17893 us) and makes the emulator produce ~32000/55.886 = ~573 audio samples
// per frame.
#define CYCLES_PER_FRAME 16013
#define FRAME_PERIOD_US  17893
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
// PIZERO-30 (M2, Option B): clean LIVE HDMI audio, DOUBLE-BUFFERED.
//
// Layout per HDMI frame (vblank = 10 front-porch + 33 back-porch nosync lines):
//   - audio is packed densely into N_AUDIO_LINES back-porch lines, each carrying
//     PKTS_PER_LINE audio sample packets; N_AUDIO_LINES*PKTS_PER_LINE*4 = exactly
//     AUDIO_SAMPLES_PER_FRAME (=560), which at 57.142857 vblanks/s == the declared
//     32 kHz, so the sink's audio FIFO neither under- nor over-runs.
//   - one back-porch line carries AVI + Audio InfoFrame + ACR (g_info), once/frame.
//   - every other vblank line emits a static control-only buffer (g_ctrl).
//
// The audio lines are PING-PONGED (two banks). Core 0 fills the OFF bank, issues a
// barrier, then flips a single word (g_active_bank). The DMA IRQ latches the bank
// ONCE at the start of each vblank and only ever READS it -- so core 0 never writes
// a buffer the IRQ is reading. This eliminates the single-bank tear that mangled
// the audio (the "motorboat"). The IRQ still does pointer stores only -- NO
// encoding (encoding in the IRQ broke video in Option A; M0 proved selection safe).
#define N_AUDIO_LINES   14
#define PKTS_PER_LINE   10
#define AUDIO_SAMPLES_PER_FRAME (N_AUDIO_LINES * PKTS_PER_LINE * 4)   // 560
#define AUDIO_INFO_VCTR 0                          // back-porch line carrying InfoFrames/ACR
#define AUDIO_FIRST_VCTR 1                          // first back-porch audio line (v_ctr)
static uint32_t g_aud[2][N_AUDIO_LINES][3][320];   // ping-pong audio lines (~105 KB)
static uint32_t g_info[3][320];                    // static: AVI + Audio InfoFrame + ACR
static uint32_t g_ctrl[3][320];                    // static: control-only filler line
static volatile uint32_t g_active_bank = 0;        // core0 writes, IRQ reads
static uint32_t g_aud_framectr = 0;                // IEC 60958 frame counter (core 0 only)

// DMA IRQ (vblank line): point this line's data-island DMA at the right buffer.
// Back-porch v_ctr 0 -> InfoFrame line; v_ctr 1..N_AUDIO_LINES -> that bank's audio
// line; everything else -> control-only. The bank is latched once per vblank (at
// the first front-porch line) so the whole frame uses one consistent bank.
static void __not_in_flash_func(audio_select_vblank_cb)(void) {
    static uint32_t latched_bank = 0;
    const bool back = (dvi0.timing_state.v_state == DVI_STATE_BACK_PORCH);
    const uint v_ctr = dvi0.timing_state.v_ctr;
    if (!back && v_ctr == 0) latched_bank = g_active_bank;   // start of vblank: latch
    const uint32_t (*sel)[320] = g_ctrl;
    if (back) {
        if (v_ctr == AUDIO_INFO_VCTR) sel = g_info;
        else if (v_ctr >= AUDIO_FIRST_VCTR && v_ctr < AUDIO_FIRST_VCTR + N_AUDIO_LINES)
            sel = g_aud[latched_bank][v_ctr - AUDIO_FIRST_VCTR];
    }
    dvi0.dma_list_vblank_nosync.l0[3].read_addr = sel[0];
    dvi0.dma_list_vblank_nosync.l1[1].read_addr = sel[1];
    dvi0.dma_list_vblank_nosync.l2[1].read_addr = sel[2];
}

// Core 0 (frame slack, after blit): drain 560 samples and re-encode the OFF bank's
// audio lines, then flip. Never touches the bank the IRQ is currently reading.
static void audio_encode_frame(void) {
    const uint32_t off = g_active_bank ^ 1u;
    int16_t mono[PKTS_PER_LINE * 4];
    int16_t lr[8];
    dvi_data_packet_t pkt;
    for (int line = 0; line < N_AUDIO_LINES; ++line) {
        size_t n = coco_machine_audio_read(mono, sizeof(mono) / sizeof(mono[0]));
        // On underrun hold the last sample (DC) instead of zeroing -> no click.
        for (size_t i = n; i < sizeof(mono) / sizeof(mono[0]); ++i) mono[i] = (i ? mono[i - 1] : 0);
        for (int k = 0; k < PKTS_PER_LINE; ++k) {
            for (int f = 0; f < 4; ++f) { int16_t s = mono[k*4 + f]; lr[2*f] = lr[2*f+1] = s; }
            dvi_di_set_audio_samples(&pkt, lr, 4, g_aud_framectr);
            g_aud_framectr = (g_aud_framectr + 4) % 192u;
            dvi_di_compute_parity(&pkt);
            dvi_write_audio_island(&DVI_TIMING, false,
                                   g_aud[off][line][0], g_aud[off][line][1], g_aud[off][line][2],
                                   8 + 27 * k, &pkt);
        }
    }
    __dmb();                 // publish all island writes before the bank flip
    g_active_bank = off;     // single-word flip; the IRQ picks it up next vblank
}
#endif // HDMI_DATA_ISLAND && !SWAPTEST && !STATIC

void setup() {
    Serial.begin(115200);
    // Bump wait + slow ramp so a freshly-reconnected USB-CDC monitor catches
    // boot banners. (loop() also repeats a recap for the first ~5 s.)
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 4000) delay(50);
    delay(200);  // a little extra room after the host attaches
    Serial.print("\r\nXRoar on RP2350-PiZero (PIZERO-09)\r\n");
    Serial.flush();

    // Bring up DVI first so we have a display even if SD/ROM fails.
    // Clear both buffers so the (static) black border is set in each.
    memset(g_fb, 0, sizeof(g_fb));
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    delay(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);        // 240 MHz (PIZERO-02b)

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
#ifdef HDMI_AUDIO_STATIC
        static uint32_t isl0[320], isl1[320], isl2[320];   // vblank island line, h_active(640)/2 words/lane
#endif
        static uint32_t bp0[40], bk1[96], bk2[96];          // active-line back-porch / blanking framing (bp0 >= h_bp/2=33)
        dvi_di_init();
        // Convert to HDMI mode: AVI InfoFrame data island in vblank + video
        // preamble/guard band on active lines. A sink rejects bare data islands
        // unless the active video is also HDMI-framed (the M2 black-screen).
        dvi_data_packet_t pkts[7];   // [0..2] AVI/AudioIF/ACR, [3..6] up to 4 audio slots
        dvi_di_set_avi_infoframe(&pkts[0], 0);
        dvi_di_set_audio_infoframe(&pkts[1], 1 /*2ch*/, DVI_AUDIO_SF_32K, DVI_AUDIO_SS_16);
        dvi_di_set_acr(&pkts[2], 24000, 4096);             // 32 kHz @ 24 MHz pixel clock
        // M4 step 2a: STATIC test tone -- a 12-sample (~2.67 kHz @ 32 kHz) square
        // wave in 3 audio sample packets, identical on every vblank line. The rate
        // is only approximate (static single buffer, ~31 kHz effective) so it may
        // drift over seconds, but it answers "does the monitor PLAY HDMI audio at
        // all" before building the dynamic per-frame engine (step 2b).
        static int16_t tone[12 * 2];
        for (int i = 0; i < 12; ++i) { int16_t v = (i < 6) ? 8000 : -8000; tone[2*i] = tone[2*i+1] = v; }
        dvi_di_set_audio_samples(&pkts[3], &tone[0],  4, 0);
        dvi_di_set_audio_samples(&pkts[4], &tone[8],  4, 4);
        dvi_di_set_audio_samples(&pkts[5], &tone[16], 4, 8);
        for (int i = 0; i < 6; ++i) dvi_di_compute_parity(&pkts[i]);
#ifdef HDMI_AUDIO_SWAPTEST
        // M0 viability check: fill BOTH (identical) banks, install the per-line
        // ping-pong callback. read_addr is repointed every vblank line in IRQ.
        for (int b = 0; b < 2; ++b)
            dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false,
                                                 &dvi0.dma_list_vblank_nosync, pkts, 6,
                                                 g_isl2[b][0], g_isl2[b][1], g_isl2[b][2]);
        dvi0.vblank_callback = swaptest_vblank_cb;
        Serial.print("[hdmi] M0 swaptest: per-line read_addr ping-pong (2 identical banks)\r\n");
#elif defined(HDMI_AUDIO_STATIC)
        dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false,
                                             &dvi0.dma_list_vblank_nosync, pkts, 6,
                                             isl0, isl1, isl2);
        Serial.print("[hdmi] M4 step2a: AVI/AudioIF/ACR + static test tone in vblank\r\n");
#else
        // M2 (Option B): double-buffered live audio. Build the static InfoFrame
        // line (AVI+AudioIF+ACR using pkts[0..2]), a control-only filler line, and
        // both ping-pong banks of audio lines (initially silence; core 0 refills
        // them every frame). Install the per-line index-select callback (pointer
        // stores only -- proven safe by M0; double-buffering removes the tear).
        dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false,
                                             &dvi0.dma_list_vblank_nosync, pkts, 3,
                                             g_info[0], g_info[1], g_info[2]);
        dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false,
                                             &dvi0.dma_list_vblank_nosync, pkts, 0,
                                             g_ctrl[0], g_ctrl[1], g_ctrl[2]);
        dvi_data_packet_t apk[PKTS_PER_LINE];
        int16_t sil[8] = {0,0,0,0,0,0,0,0};
        for (int i = 0; i < PKTS_PER_LINE; ++i) {
            dvi_di_set_audio_samples(&apk[i], sil, 4, (uint32_t)(i * 4));
            dvi_di_compute_parity(&apk[i]);
        }
        for (int b = 0; b < 2; ++b)
            for (int line = 0; line < N_AUDIO_LINES; ++line)
                dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false,
                                                     &dvi0.dma_list_vblank_nosync, apk, PKTS_PER_LINE,
                                                     g_aud[b][line][0], g_aud[b][line][1], g_aud[b][line][2]);
        dvi0.vblank_callback = audio_select_vblank_cb;
        Serial.print("[hdmi] M2: live audio, double-buffered, 560 samp/frame @ 57.14Hz\r\n");
#endif
        dvi_setup_active_hdmi_framing(&DVI_TIMING, dvi0.dma_cfg,
                                      &dvi0.dma_list_active, bp0, bk1, bk2);
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
    USBHost.task();   // PIZERO-11: service USB host transfers.
    if (!g_machine_running) { delay(1000); return; }
    static uint32_t next_us = 0;
    if (next_us == 0) next_us = micros();

    pump_keyboard();
    uint32_t a = micros();
    coco_machine_run_cycles(CYCLES_PER_FRAME);
    uint32_t b = micros();
    coco_machine_render_frame();                  // regenerate VDG buffer (SUPPRESS_RENDER_SCANLINE)
    uint32_t c = micros();
#ifdef HDMI_DATA_ISLAND
    coco_boot_blit_vdg_pizero(g_fb);              // single buffer; g_front already points at it
#else
    coco_boot_blit_vdg_pizero(g_fb[g_back]);      // render into the back buffer
    g_front = g_fb[g_back];                        // publish: core 1 picks it up at its next frame
    g_back ^= 1;
#endif
    uint32_t d = micros();
#if defined(HDMI_DATA_ISLAND) && !defined(HDMI_AUDIO_SWAPTEST) && !defined(HDMI_AUDIO_STATIC)
    audio_encode_frame();                          // M2: refill all 43 vblank lines' audio islands
#endif
    uint32_t e = micros();
    (void)e;

#ifdef AUDIO_WAV_DUMP
    // Arm on USB-CDC attach (DTR), then autotype a deterministic ascending-tone
    // program (SOUND = core Color BASIC, no ECB) and stream a base64 WAV.
    static bool dump_armed = false;
    if (!dump_armed && Serial) {
        dump_armed = true;
        g_autotype = "\rFORI=1TO8:SOUNDI*28,4:NEXTI\r";
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
                      "| D+=%d D-=%d sof=%lu usb=%lu\r\n",
                      (unsigned long)frames, (unsigned long)(b - a),
                      (unsigned long)(c - b), (unsigned long)(d - c),
                      (unsigned long)(e - d),
                      dp, dm, (unsigned long)sof,
                      (unsigned long)g_usb_devices);
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
