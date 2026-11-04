// xroar-waveshare-rp2350-pizero: XRoar (Tandy CoCo) on a mini video port, DVI (PIZERO-09).
//
// Core 0: CoCo emulation (6809 + SAM + PIA + VDG) + serial/autotype keyboard +
//         FDC sector reads from SD. Blits the VDG output into a 320x240 RGB565
//         framebuffer once per emulated frame.
// Core 1: libdvi static-framebuffer worker: continuously TMDS-encodes g_fb and
//         scans it out at 640x480p60 (2x of 320x240). Decoupled from core 0, so
//         emulation speed never starves the display (tearing instead of dropouts).
//
// Display path: libdvi (PIO) on GPIO 32-39; see README.md. USB host deferred.

#include <Arduino.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/sysinfo.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/powman.h"

extern "C" {
#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
}

// PIZERO-11: Pico-PIO-USB host on the PIO-USB port (D+=28, D-=29). Wiki
// confirms host is supported (device_info demo enumerates a 2.4G wireless
// receiver as ID 05ac:0256). Open issue tracked by RP2350 errata E9, pins
// configured as pull-down input read HIGH due to leak current; Pico-PIO-USB
// has a workaround gated on chip_version <= 2. We print the chip revision so
// we can tell whether the workaround applies on this part.
#include "pio_usb.h"
#include "Adafruit_TinyUSB.h"
#include "midi_usb_pack.h"      // PIZERO-287: MIDI bytes <-> USB-MIDI packets, host-tested
#define HOST_PIN_DP  28
static Adafruit_USBH_Host USBHost;
static volatile uint32_t g_usb_devices = 0;
// PIZERO-11b diag: HID report flow, the signal that actually changes on
// unplug (a removed device can't answer IN polls). rpts climbs while a device
// streams reports; rfail counts tuh_hid_receive_report() rejections.
static volatile uint32_t g_hid_reports   = 0;
// PIZERO-13: the bound pad's decoded reports, and its last state, for the
// once-a-second [pad] line (proves input reaches the emulated joysticks).
static volatile uint32_t g_pad_decoded = 0;
static volatile uint16_t g_pad_x = 32767, g_pad_y = 32767;
static volatile uint8_t  g_pad_fire = 0;
static volatile uint32_t g_hid_recv_fail = 0;
extern "C" uint32_t pio_usb_host_get_frame_number(void);

#ifdef USB_HOTPLUG_RECOVER
// PIZERO-51: phantom-flood hot-replug recovery. On this rev3 board an unplugged
// device is INVISIBLE to line_state/connected/ep_error/ints: the PIO SM pins the
// bus at J/FS, so PIO-USB decodes the held-idle line as a valid IN response and
// "completes" a byte-IDENTICAL HID report ~180x/s forever (PIZERO-11b, HW-confirmed
// 2026-06-29). A real boot keyboard reports only on key transitions (it NAKs while
// idle), so a long run of byte-identical reports on one interface == the device is
// gone. We count that run per (daddr,idx); crossing the threshold asks loop() to
// tear down and re-enumerate the root port at a safe point (NOT from inside the tuh
// callback, which is mid-stack). Alternatives were ruled out in PIZERO-11b:
// E9-drain (the line is DRIVEN, not leaking) and transfer-error detection (no error
// ever fires). Flag-gated so the default product build is untouched until HW-proven.
#ifndef USB_PHANTOM_FLOOD_N
#define USB_PHANTOM_FLOOD_N 100        // consecutive identical reports (~0.55s @ ~180/s)
#endif
#define USB_HP_MAX_DADDR 5             // one root port -> low addresses; skip beyond
#define USB_HP_TRACK_LEN 16            // compare the leading bytes (8-byte kbd report + slack)
static uint8_t  g_hp_prev[USB_HP_MAX_DADDR][CFG_TUH_HID][USB_HP_TRACK_LEN];
static uint16_t g_hp_run [USB_HP_MAX_DADDR][CFG_TUH_HID];
static uint8_t  g_hp_last[8] = {0};   // last flooding payload, for the recovery log
static volatile bool g_usb_recover_pending = false;

// Track one interface's report; return true once it has flooded >= N identical.
static bool hotplug_note_report(uint8_t daddr, uint8_t idx,
                                const uint8_t *report, uint16_t len) {
    if (daddr < 1 || daddr > USB_HP_MAX_DADDR || idx >= CFG_TUH_HID) return false;
    uint8_t *prev = g_hp_prev[daddr - 1][idx];
    uint16_t n = len < USB_HP_TRACK_LEN ? len : USB_HP_TRACK_LEN;
    bool same = true;
    for (uint16_t i = 0; i < n; i++) {
        if (report[i] != prev[i]) same = false;
        prev[i] = report[i];
    }
    uint16_t *run = &g_hp_run[daddr - 1][idx];
    if (same) { if (*run < 0xFFFF) (*run)++; } else { *run = 0; }
    if (*run >= USB_PHANTOM_FLOOD_N) {
        for (int i = 0; i < 8; i++) g_hp_last[i] = (i < len) ? report[i] : 0;
        return true;
    }
    return false;
}

static void hotplug_reset_tracking(void) {
    for (int d = 0; d < USB_HP_MAX_DADDR; d++)
        for (int i = 0; i < CFG_TUH_HID; i++) g_hp_run[d][i] = 0;
}
#endif // USB_HOTPLUG_RECOVER
// PIZERO-11b diag: thin C shim over the C-only pio_usb_ll.h internals
// (root_port_t + pio_usb_bus_get_line_state). Defined in src/usb_hotplug_diag.c
// so that header is never pulled into this C++ TU.
extern "C" uint8_t  usbdiag_line_state(void);  // 0=SE0 1=J/FS 2=K/LS 3=SE1
extern "C" int      usbdiag_connected(void);
extern "C" int      usbdiag_fullspeed(void);
extern "C" unsigned usbdiag_event(void);       // 0=NONE 1=CONNECT 2=DISCONNECT 3=HUB
extern "C" unsigned usbdiag_ep_error(void);    // endpoint-error register
extern "C" unsigned usbdiag_ints(void);        // interrupt-status register
// PIZERO-51: force the pio-usb disconnect branch so TinyUSB umounts a device that
// vanished without the (masked) SE0 the library normally needs. See the shim.
extern "C" void usbdiag_force_disconnect(void);

extern "C" {
#include "ff.h"
#include "f_util.h"
}

#include "coco_boot.h"
#include "dsk_catalog.h"   // PIZERO-81a
#include "disk_overlay.h"  // PIZERO-81/114: F12 disk drives overlay
#include "rsdos_dir.h"     // PIZERO-81d: find the program to run on a disk
#include "cart_gmc.h"      // PIZERO-142: bank-switched cartridges
#include "settings.h"      // PIZERO-145: /coco/settings.txt
#include "text_editor.h"   // PIZERO-146: on-screen editor for it
#include "text_edit_keys.h" // PIZERO-146/147: the editor's status words
#include "gamepad.h"        // PIZERO-13: USB gamepad -> CoCo joysticks
#include "key_translate.h"  // PIZERO-163: USB keycaps -> CoCo chords
#include "usb_audio.h"      // PIZERO-255: a USB headphone DAC
#include "hid_consumer.h"   // PIZERO-255: volume keys on keyboards and DACs
#include "soak_log.h"        // PIZERO-201: telemetry to the card
#include "wd_post.h"         // PIZERO-227: where core 0 really was at a freeze
#include "fw_version.h"     // PIZERO-156: generated by scripts/fw_version.py
#include "pico/unique_id.h" // PIZERO-156: the board's serial number
#include "cpu_prof.h"        // PIZERO-209: empty unless CPU_PROF
#include "boot_messages.h"
extern "C" {
#include "events.h"   // PIZERO-33 runaway guard counters
}

extern "C" {
#include "coco_machine.h"
#ifdef VIDEO_640
#include "coco_palette.h"   // PIZERO-134: GIME colours for the scanout palette
#endif

// PIZERO-316: USB serial drops what is printed while no monitor has the port
// open. From reset until settings.txt has said whether to wait for one, the
// output is also kept, and printed as soon as a monitor opens the port.
// PIZERO-319: it is kept in a buffer the caller lends (the ROM buffer, not
// yet in use), never the heap: a 4 KB block taken and freed at the very
// start left a hole at the bottom of the heap, under the timer's alarm pool
// and the video queues, and both interrupts then faulted after F11 saves.
// Otherwise everything passes straight through to the USB serial port.
static decltype(Serial) &g_usb_serial = Serial;
class HeldSerial : public Stream {
public:
    void hold(void *buf, size_t cap) { buf_ = (char *)buf; cap_ = buf_ ? cap : 0; n_ = 0; lost_ = 0; }
    void release(void) { send_held(); buf_ = nullptr; cap_ = n_ = 0; }   // the buffer is the lender's again
    size_t held(void) const { return n_; }
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *b, size_t len) override {
        if (buf_) {
            if (!g_usb_serial) {
                size_t k = len < cap_ - n_ ? len : cap_ - n_;
                memcpy(buf_ + n_, b, k);
                n_ += k;
                lost_ += len - k;
                return len;
            }
            send_held();
        }
        g_usb_serial.write(b, len);
        return len;
    }
    int available(void) override { return g_usb_serial.available(); }
    int read(void) override { return g_usb_serial.read(); }
    int peek(void) override { return g_usb_serial.peek(); }
    int availableForWrite(void) override { return g_usb_serial.availableForWrite(); }
    void flush(void) override { g_usb_serial.flush(); }
    void begin(unsigned long baud) { g_usb_serial.begin(baud); }
    explicit operator bool() { return (bool)g_usb_serial; }
private:
    void send_held(void) {
        if (!buf_ || !n_ || !g_usb_serial) return;
        g_usb_serial.write((const uint8_t *)buf_, n_);
        if (lost_) g_usb_serial.printf("[boot] (%u bytes of the boot's output were lost before a monitor opened)\r\n", (unsigned)lost_);
        n_ = lost_ = 0;
    }
    char  *buf_ = nullptr;
    size_t cap_ = 0, n_ = 0, lost_ = 0;
};
static HeldSerial g_held_serial;
#define Serial g_held_serial
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
// but well within typical monitor EDID tolerance.
static const struct dvi_timing dvi_timing_640x480p_57hz_240mhz = {
    .h_sync_polarity = false,
    // PIZERO-30: h_fp=14, h_bp=130 -> h_total = 880, refresh = 24e6/(880*525) =
    // 51.9481 Hz == exactly 32000/616 -> 616 audio samples/frame = 154 packets.
#if defined(AV_60HZ)
    .h_front_porch   = 8,    // PIZERO-45: 800-total split (8/96/56/640) so the 56px back porch fits ONE audio island
#elif defined(AV_60HZ_TEST)
    .h_front_porch   = 16,   // PIZERO-44 60Hz TEST: standard 800-total blanking
#elif defined(AV_STD_TIMING)
    .h_front_porch   = 58,   // STD-TIMING TEST: h_total 880->924 so 25.2MHz pixel keeps 51.95Hz
#else
    .h_front_porch   = 14,
#endif
#if defined(AV_60HZ) && defined(AV_HDMI_WIDE_BP)
    .h_sync_width    = 92,   // PIZERO-233: 8/92/60/640, 4px of sync moved to the back porch
#else
    .h_sync_width    = 96,
#endif
#if defined(AV_60HZ) && defined(AV_HDMI_WIDE_BP)
    // PIZERO-232/233: at 56 the control period between the audio island's
    // trailing guard and the video guard is 10px (1 word + the 8px video
    // preamble); HDMI asks for at least 12. 60 gives 14px. h_total stays 800,
    // so still 60.0 Hz. Taking the 4px from the front porch (4/96/60) lost the
    // MS2109 capture stick, so they come from the sync pulse instead.
    .h_back_porch    = 60,
#elif defined(AV_60HZ)
    .h_back_porch    = 56,   // PIZERO-45: 28 words -- just fits one streaming island; h_total=800 -> 60.0 Hz
#elif defined(AV_60HZ_TEST)
    .h_back_porch    = 48,   // 60Hz TEST: standard back porch (no audio islands here)
#else
    .h_back_porch    = 130,
#endif
    .h_active_pixels = 640,

    .v_sync_polarity = false,
    .v_front_porch   = 10,
    .v_sync_width    = 2,
    .v_back_porch    = 33,
    .v_active_lines  = 480,

#if defined(AV_60HZ) || defined(AV_60HZ_TEST) || defined(AV_STD_TIMING)
    .bit_clk_khz     = 252000,   // 25.2 MHz pixel (~standard 25.175); 60Hz: 800x525 -> 60.0 Hz
#else
    .bit_clk_khz     = 240000,   // 24 MHz pixel clock, ~57.14 Hz refresh
#endif
};
#define DVI_TIMING   dvi_timing_640x480p_57hz_240mhz

// PIZERO-32: Audio Clock Regeneration (ACR) CTS. The value that nulls audio pitch
// is MONITOR-DEPENDENT -- the sink derives the audio rate fs = pixel_clk*N/(128*CTS)
// from ITS assumed pixel clock, which may differ from our off-spec 24 MHz. Higher
// CTS -> lower pitch. 25176 suits the dev monitor (assumes the 25.175 MHz CEA
// clock); sinks that run sharp need more (~26674 drops ~1 semitone). Override with
// -DAV_ACR_CTS=<n>. N stays 6144.
#ifndef AV_ACR_CTS
#define AV_ACR_CTS 25176
#endif

// PIZERO-14: double-buffered. Core 0 renders into the back buffer, then swaps
// g_front at a frame boundary; core 1 samples g_front once per frame -> no tearing.
// FB_SINGLE picks one framebuffer instead. Every data-island build needs it; a
// build without data islands can ask for it too (PIZERO-131: pizero_wavmeas,
// which shows no picture, so its second buffer was 150 KB spent on nothing).
#if defined(AV_DATA_ISLAND) && !defined(FB_SINGLE)
#define FB_SINGLE
#endif
#ifdef VIDEO_640
// PIZERO-134: the CoCo 3 at full horizontal resolution. One byte a pixel, a
// GIME colour (0-63) or a card colour (64 up), looked up in g_pal565 as core 1
// encodes each line: 640x240, the same 153,600 bytes as the RGB565 buffer.
#if !defined(COCO3) || !defined(FB_SINGLE) || !defined(TCC1014_PORT_FULL)
#error "VIDEO_640 needs COCO3, a single framebuffer and TCC1014_PORT_FULL"
#endif
#define FB640_W 640
#define PAL_CARD 64                    // g_pal565[64..]: the card's ink and paper
#define PAL_VDG  80                    // PIZERO-135: g_pal565[80..95]: a CoCo 2's palette
static uint8_t g_fb[FB640_W * FRAME_HEIGHT] __attribute__((aligned(4)));
static const uint8_t * volatile g_front = g_fb;
static uint16_t g_pal565[256];
#elif defined(FB_SINGLE)
// PIZERO-30 (M1): Data-island audio builds SINGLE-buffer the framebuffer. The ~150 KB
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
#ifdef VIDEO_640
    dvi_static_framebuf_main_pal8(&dvi0, &g_front, g_pal565);
#else
    dvi_static_framebuf_main_16bpp(&dvi0, &g_front);
#endif
}

// ---- Keyboard (serial + autotype, same as the AMOLED port) ---------------
// PIZERO-145: the settings in force, from /coco/settings.txt (defaults
// until it is read, which is also what a card without the file gets).
static struct coco_settings g_settings;     // settings_defaults() at the top of setup()

// PIZERO-165: a screenshot asked for (Print Screen, the pad's M button, or
// Ctrl-P over serial), taken by loop() after the next frame is drawn.
static volatile bool g_shot_request = false;
static bool g_switch_request = false;     // PIZERO-314: Ctrl-T on serial
static int8_t g_switch_want = -1;         // PIZERO-319: a saved machine change (1 CoCo 3, 0 CoCo 2), done from loop()

static int info_lines(char lines[][33], int max);   // PIZERO-156, at the end of this file

#ifdef VIDEO_BENCH
// PIZERO-175 step 1: can core 1 send a 640-wide picture? Time libdvi's
// encoders for one scanline each, on core 0 (the cores are identical) with
// its interrupts off while core 1 runs the real display, so memory
// contention is realistic. Today's output is 320 pixels doubled at 16 bpp;
// the candidates send 640 distinct pixels. Each encoded line is shown twice
// (240 lines doubled to 480), so it has 2 x 31.75 us = 63.5 us of core 1's
// time, less the audio data-island work. Printed as [bench] every 5 s.
#include "tmds_encode.h"
#include "hardware/sync.h"
#define BENCH_LINES 240
static uint32_t g_bench_pix16[640 / 2];          // 640 RGB565 pixels
static uint32_t g_bench_pix8[640 / 4];           // 640 one-byte pixels
static uint32_t g_bench_sym[3 * 640];            // symbols, all 3 channels
static uint32_t g_bench_pal[6 * 256];            // TMDS palette
struct bench_result { const char *name; uint32_t ns_per_line; };
static struct bench_result g_bench[12];
static int g_bench_n = 0;

static void bench_add(const char *name, uint32_t us_total) {
    if (g_bench_n < 12) g_bench[g_bench_n++] = { name, us_total * 1000u / BENCH_LINES };
}

// The RP2350's own TMDS encoder in the SIO block (one per core), which libdvi
// predates. One write of two RGB565 pixels, then each lane's two symbols are
// one read each, in the two-symbols-per-word format libdvi sends.
#define BENCH_TMDS_RGB565 ((13u << SIO_TMDS_CTRL_L0_ROT_LSB) | (3u << SIO_TMDS_CTRL_L1_ROT_LSB) | \
                           (8u << SIO_TMDS_CTRL_L2_ROT_LSB) | (4u << SIO_TMDS_CTRL_L0_NBITS_LSB) | \
                           (5u << SIO_TMDS_CTRL_L1_NBITS_LSB) | (4u << SIO_TMDS_CTRL_L2_NBITS_LSB) | \
                           (5u << SIO_TMDS_CTRL_PIX_SHIFT_LSB))
static void __not_in_flash_func(bench_hw_640_16bpp)(const uint32_t *pix, uint32_t *sym, int n_words) {
    sio_hw->tmds_ctrl = BENCH_TMDS_RGB565 | SIO_TMDS_CTRL_CLEAR_BALANCE_BITS;
    uint32_t *l1 = sym + n_words, *l2 = sym + 2 * n_words;
    for (int i = 0; i < n_words; i++) {
        sio_hw->tmds_wdata = pix[i];
        sym[i] = sio_hw->tmds_peek_double_l0;
        l1[i]  = sio_hw->tmds_peek_double_l1;
        l2[i]  = sio_hw->tmds_peek_double_l2;
    }
}
// Same, from one-byte palette indices: what a GIME or TMS9918A line would be.
static void __not_in_flash_func(bench_hw_640_pal8)(const uint8_t *pix, const uint16_t *pal, uint32_t *sym, int n_words) {
    sio_hw->tmds_ctrl = BENCH_TMDS_RGB565 | SIO_TMDS_CTRL_CLEAR_BALANCE_BITS;
    uint32_t *l1 = sym + n_words, *l2 = sym + 2 * n_words;
    for (int i = 0; i < n_words; i++) {
        sio_hw->tmds_wdata = pal[pix[2 * i]] | ((uint32_t)pal[pix[2 * i + 1]] << 16);
        sym[i] = sio_hw->tmds_peek_double_l0;
        l1[i]  = sio_hw->tmds_peek_double_l1;
        l2[i]  = sio_hw->tmds_peek_double_l2;
    }
}
// Today's 320 doubled to 640, on the hardware encoder (PIX2_NOSHIFT).
static void __not_in_flash_func(bench_hw_320x2_16bpp)(const uint32_t *pix, uint32_t *sym, int n_words) {
    sio_hw->tmds_ctrl = BENCH_TMDS_RGB565 | SIO_TMDS_CTRL_PIX2_NOSHIFT_BITS | SIO_TMDS_CTRL_CLEAR_BALANCE_BITS;
    uint32_t *l1 = sym + 2 * n_words, *l2 = sym + 4 * n_words;
    for (int i = 0; i < n_words; i++) {
        sio_hw->tmds_wdata = pix[i];
        sym[2 * i] = sio_hw->tmds_peek_double_l0;
        l1[2 * i]  = sio_hw->tmds_peek_double_l1;
        l2[2 * i]  = sio_hw->tmds_pop_double_l2;
        sym[2 * i + 1] = sio_hw->tmds_peek_double_l0;
        l1[2 * i + 1]  = sio_hw->tmds_peek_double_l1;
        l2[2 * i + 1]  = sio_hw->tmds_peek_double_l2;
    }
}

// Decode one 10-bit TMDS data symbol back to its 8-bit value.
static uint32_t bench_tmds_decode(uint32_t q) {
    uint32_t d = (q & 0x200) ? (~q & 0xff) : (q & 0xff), v = d & 1;
    for (int i = 1; i < 8; i++) {
        uint32_t b = ((d >> i) ^ (d >> (i - 1))) & 1;
        v |= ((q & 0x100) ? b : b ^ 1) << i;
    }
    return v;
}
// Mismatches between hw_640_16bpp's symbols and the pixels they encode, in
// libdvi's order (first pixel in the low 10 bits) and its lane order (0 blue).
static uint32_t g_bench_hw_errors = 0;
static void bench_hw_check(void) {
    bench_hw_640_16bpp(g_bench_pix16, g_bench_sym, 320);
    for (int i = 0; i < 320; i++) {
        for (int half = 0; half < 2; half++) {
            uint32_t px = (g_bench_pix16[i] >> (16 * half)) & 0xffff;
            uint32_t want[3] = { (px & 0x1f) << 3, ((px >> 5) & 0x3f) << 2, (px >> 11) << 3 };
            for (int lane = 0; lane < 3; lane++)
                if (bench_tmds_decode((g_bench_sym[lane * 320 + i] >> (10 * half)) & 0x3ff) != want[lane])
                    g_bench_hw_errors++;
        }
    }
}

static void video_bench_run(void) {
    for (int i = 0; i < 640 / 2; i++) g_bench_pix16[i] = 0x1234ABCDu * (uint32_t)(i + 1);
    for (int i = 0; i < 640 / 4; i++) g_bench_pix8[i] = 0x0F070301u + (uint32_t)i * 0x01010101u;
    static uint16_t pal16[256];
    for (int i = 0; i < 256; i++) pal16[i] = (uint16_t)(i * 0x0841u);
    tmds_setup_palette_symbols(pal16, g_bench_pal, 256);
    uint32_t irq = save_and_disable_interrupts();
    uint64_t t;

    t = time_us_64();                       // today: 320 pixels doubled, 3 channels
    for (int l = 0; l < BENCH_LINES; l++) {
        tmds_encode_data_channel_16bpp(g_bench_pix16, g_bench_sym + 0,   320, DVI_16BPP_BLUE_MSB,  DVI_16BPP_BLUE_LSB);
        tmds_encode_data_channel_16bpp(g_bench_pix16, g_bench_sym + 640, 320, DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
        tmds_encode_data_channel_16bpp(g_bench_pix16, g_bench_sym + 1280,320, DVI_16BPP_RED_MSB,   DVI_16BPP_RED_LSB);
    }
    bench_add("today_320x2_16bpp", (uint32_t)(time_us_64() - t));

    t = time_us_64();                       // 640 distinct pixels, RGB565
    for (int l = 0; l < BENCH_LINES; l++) {
        tmds_encode_data_channel_fullres_16bpp(g_bench_pix16, g_bench_sym + 0,   640, DVI_16BPP_BLUE_MSB,  DVI_16BPP_BLUE_LSB);
        tmds_encode_data_channel_fullres_16bpp(g_bench_pix16, g_bench_sym + 640, 640, DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
        tmds_encode_data_channel_fullres_16bpp(g_bench_pix16, g_bench_sym + 1280,640, DVI_16BPP_RED_MSB,   DVI_16BPP_RED_LSB);
    }
    bench_add("full_640_16bpp", (uint32_t)(time_us_64() - t));

    t = time_us_64();                       // 640 one-byte pixels, 16 colors
    for (int l = 0; l < BENCH_LINES; l++)
        tmds_encode_palette_data(g_bench_pix8, g_bench_pal, g_bench_sym, 640, 4);
    bench_add("pal_640_16col", (uint32_t)(time_us_64() - t));

    t = time_us_64();                       // 640 one-byte pixels, 256 colors
    for (int l = 0; l < BENCH_LINES; l++)
        tmds_encode_palette_data(g_bench_pix8, g_bench_pal, g_bench_sym, 640, 8);
    bench_add("pal_640_256col", (uint32_t)(time_us_64() - t));

    t = time_us_64();                       // 640 pixels, 1 bit each (80-column text)
    for (int l = 0; l < BENCH_LINES; l++)
        tmds_encode_1bpp(g_bench_pix16, g_bench_sym, 640);
    bench_add("mono_640_1bpp", (uint32_t)(time_us_64() - t));

    t = time_us_64();                       // 640 pixels, 2 bits each (4 grays)
    for (int l = 0; l < BENCH_LINES; l++)
        tmds_encode_2bpp(g_bench_pix16, g_bench_sym, 640);
    bench_add("gray_640_2bpp", (uint32_t)(time_us_64() - t));

    t = time_us_64();
    for (int l = 0; l < BENCH_LINES; l++)
        bench_hw_320x2_16bpp(g_bench_pix16, g_bench_sym, 160);
    bench_add("hw_320x2_16bpp", (uint32_t)(time_us_64() - t));

    t = time_us_64();
    for (int l = 0; l < BENCH_LINES; l++)
        bench_hw_640_16bpp(g_bench_pix16, g_bench_sym, 320);
    bench_add("hw_640_16bpp", (uint32_t)(time_us_64() - t));

    t = time_us_64();
    for (int l = 0; l < BENCH_LINES; l++)
        bench_hw_640_pal8((const uint8_t *)g_bench_pix8, pal16, g_bench_sym, 320);
    bench_add("hw_640_pal8", (uint32_t)(time_us_64() - t));

    restore_interrupts(irq);
    bench_hw_check();
}

static void video_bench_print(void) {
    Serial.printf("[bench] ns per encoded line (budget 63500 incl. audio):");
    for (int i = 0; i < g_bench_n; i++) Serial.printf(" %s=%lu", g_bench[i].name, (unsigned long)g_bench[i].ns_per_line);
    Serial.printf(" hw_check_errors=%lu/1920\r\n", (unsigned long)g_bench_hw_errors);
}
#endif  // VIDEO_BENCH

static const char *g_autotype        = nullptr;
static int         g_autotype_warmup = 0;
// PIZERO-182: the queued typing is autorun (a disk's program, or the typed
// lines of autorun.txt), and a Space or BREAK tap since the boot began
// cancels it while it is still waiting for BASIC.
static bool        g_autotype_autorun = false;
static bool        g_autorun_cancel   = false;

// PIZERO-163: the CoCo key names and the character chords live in
// key_translate.h (host-tested), shared by serial typing and the USB keyboard.
static uint32_t g_bad_fps = 0;     // PIZERO-201: seconds the machine ran off 60 fps

// PIZERO-203: the 1 Hz telemetry never waits on USB. arduino-pico's
// Serial.write spins while a host holds the port open and its buffer is full,
// a second at a time, and every bit of progress restarts the second; a host
// that has the port open and reads slowly (a monitor hub on a Mac) stalled the
// end of the frame past the watchdog. Each second prints only if the last
// second's output has drained; a line gets 10 ms to find room, else the rest
// of the second is dropped and counted (tdrop= on the [run] line).
static bool     g_tele_ok = true;
static uint32_t g_tele_dropped = 0;
static void tele_write(const char *s, size_t n) {
    if (!g_tele_ok) { g_tele_dropped++; return; }
    if (!Serial) return;                              // no host: nothing would be sent
    uint32_t t0 = micros();
    // PIZERO-220: in pieces, since a line can be longer than the USB
    // serial's transmit buffer (the [run] line passed it), and a wait for
    // that much room never ends.
    while (n) {
        size_t k = n < 64 ? n : 64;
        while (Serial.availableForWrite() < (int)k) {
            if (micros() - t0 > 10000) { g_tele_ok = false; g_tele_dropped++; return; }
        }
        Serial.write((const uint8_t *)s, k);
        s += k;
        n -= k;
    }
}
static void tprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tprintf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) tele_write(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}
static int g_kb_hold = 0;
static int g_kb_gap  = 0;
// Serial and autotype press keys on their own layer (PIZERO-163), so
// releasing one cannot drop a key the USB keyboard is holding.

static int next_keychar() {
    if (g_autotype) {
        if (g_autotype_autorun && g_autorun_cancel && g_autotype_warmup > 0) {
            Serial.print("[autorun] cancelled: Space or BREAK (PIZERO-182)\r\n");
            g_autotype = nullptr;
            g_autotype_autorun = false;
            return -1;
        }
        if (g_autotype_warmup > 0) { g_autotype_warmup--; return -1; }
        g_autotype_autorun = false;           // typing has begun: no longer cancellable
        char c = *g_autotype;
        if (c == '\0') { g_autotype = nullptr; return -1; }
        g_autotype++;
        // PIZERO-194: '~' in autorun text waits a second before typing on
        // (the CoCo has no '~' key), so a line can follow a disk boot.
        if (c == '~') { g_autotype_warmup = 60; return -1; }
        return (unsigned char)c;
    }
    if (g_settings.serial_keyboard && Serial.available()) return Serial.read();
    return -1;
}

static void pump_keyboard() {
    if (g_kb_hold > 0) {
        if (--g_kb_hold == 0) {
            coco_machine_release_all_keys_src(COCO_KEYS_TYPED);
            g_kb_gap = g_autotype ? 4 : 2;
        }
        return;
    }
    if (g_kb_gap > 0) { g_kb_gap--; return; }
    int c = next_keychar();
    if (c < 0) return;
    if (c == 0x10) { g_shot_request = true; return; }   // PIZERO-165: Ctrl-P, a screenshot
    if (c == 0x14) { g_switch_request = true; return; } // PIZERO-314: Ctrl-T, the other machine
    uint8_t dscan, shift;
    if (!kt_chord((char)c, &dscan, &shift)) return;
    if (shift == KT_SHIFT_ON) coco_machine_press_key_src(COCO_KEYS_TYPED, K_SHIFT);
    coco_machine_press_key_src(COCO_KEYS_TYPED, dscan);
    g_kb_hold = g_autotype ? 5 : 3;
}

// PIZERO-11b: hot-replug watchdog attempt: DISABLED. The premise (read
// D+/D- via gpio_get + look for SE1) doesn't work because Pico-PIO-USB's
// TX state machine holds the pins in J state between transmissions, so
// the post-INOVER reading never changes on unplug. Bypassing the SM (pad
// OD trick) doesn't disambiguate either because RP2350 E9 leak current
// makes "no device" still look like weak-high. See PIZERO-11b for the
// referenced workarounds (external 8.2k pull-downs, A3/A4 silicon, or
// transaction-failure tracking inside the lib).

// HID boot-keyboard report state. tuh_hid_report_received_cb fires from
// USBHost.task() (called once per loop()), so this runs on core 0 alongside
// the CoCo machine: no cross-core sync needed.
static bool    g_machine_running = false;
// PIZERO-81c: frames since boot (key repeat timing), and the audio pause the
// overlay asks for, read by core 1 in stream_next_sample.
static uint32_t g_frame_count = 0;
static volatile bool g_audio_paused = false;
// PIZERO-152: a key pressed while a boot page is up (before the machine runs).
static bool g_boot_key = false;
static uint8_t g_hid_prev_codes[6] = {0};
// PIZERO-163: the USB keys held and the CoCo key each chose (key_translate.h).
static struct kt_state g_kt;

// Make the USB keyboard's layer of the key matrix match what is held now.
static void hid_keys_present(uint8_t mods) {
    uint8_t keys[KT_KEYS_MAX];
    int n = kt_keys(&g_kt, mods, keys);
    coco_machine_release_all_keys_src(COCO_KEYS_USB);
    for (int i = 0; i < n; i++) coco_machine_press_key_src(COCO_KEYS_USB, keys[i]);
}

static uint8_t g_hid_mods = 0;               // the last report's modifiers
static bool g_prtsc_held = false;              // PIZERO-165
static volatile bool g_art_request = false;    // PIZERO-186: F8 pressed, for loop()
static bool g_f8_held = false;

// PIZERO-167: once a frame while the machine runs: auto-repeat the newest
// held USB key (key_translate.h kt_repeat), per the key_repeat settings.
static void hid_key_repeat(void) {
    if (kt_repeat(&g_kt, g_frame_count, g_settings.key_repeat,
                  kt_repeat_delay_frames(g_settings.key_repeat_delay),
                  kt_repeat_period_frames(g_settings.key_repeat_rate)))
        hid_keys_present(g_hid_mods);
}

// PIZERO-169: the overlay's input is the keyboard's held keys merged with
// the pad's navigation keys, fed together so its press detection sees one
// consistent set. Returns true when the overlay took the report.
static uint8_t  g_kb_codes[6];                // the keyboard's keys held now
static uint8_t  g_pad_nav[6];                 // the pad's, as overlay keys
static uint16_t g_pad_buttons_now = 0;        // the pad's buttons held now
static uint8_t g_pad_raw[10];                 // PIZERO-215: the last report's first bytes, for [pad]
static uint16_t g_pad_inert = 0;              // held when the overlay closed
static bool     g_pad_dpad_inert = false;

static bool overlay_feed(uint8_t mods) {
    uint8_t u[6] = { 0 };
    int n = 0;
    const uint8_t *src[2] = { g_kb_codes, g_pad_nav };
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 6 && n < 6; i++) {
            uint8_t c = src[s][i];
            if (!c) continue;
            bool dup = false;
            for (int j = 0; j < n; j++) if (u[j] == c) dup = true;
            if (!dup) u[n++] = c;
        }
    bool closed = false;
    if (!disk_overlay_key(mods, u, g_frame_count, &closed)) return false;
    g_audio_paused = disk_overlay_is_open();
    if (closed) {
        // Keys held NOW press nothing until released, so the ESC that
        // closed the overlay is not a BREAK (FRUITJAM-68). A held Shift is
        // restored, since the machine's keys were all released on open. The
        // pad's held buttons likewise, so launching with A is not a fire.
        kt_resync(&g_kt, g_kb_codes);
        hid_keys_present(mods);
        g_pad_inert = g_pad_buttons_now;
        g_pad_dpad_inert = true;
        memset(g_pad_nav, 0, sizeof g_pad_nav);
    }
    return true;
}

static void hid_keyboard_apply(const uint8_t *report) {
    uint8_t mods = report[0];
    g_hid_mods = mods;
    const uint8_t *codes = &report[2];

    // PIZERO-182: a Space or BREAK (Esc, or Pause) tap while the machine
    // boots, or while autorun's typing still waits for BASIC, cancels it.
    {
        static uint8_t prev[6];
        for (int i = 0; i < 6; i++) {
            uint8_t c = codes[i];
            if (c != 0x2C && c != 0x29 && c != 0x48) continue;
            bool was = false;
            for (int j = 0; j < 6; j++) if (prev[j] == c) was = true;
            if (!was && (!g_machine_running || (g_autotype && g_autotype_autorun && g_autotype_warmup > 0)))
                g_autorun_cancel = true;
        }
        memcpy(prev, codes, 6);
    }

    // PIZERO-152: before the machine runs, a key only dismisses a boot page;
    // it is tracked but not pressed, so it cannot reach BASIC as a keystroke.
    if (!g_machine_running) {
        for (int i = 0; i < 6; i++) {
            if (!codes[i]) continue;
            bool was = false;
            for (int j = 0; j < 6; j++) if (g_hid_prev_codes[j] == codes[i]) was = true;
            if (!was) g_boot_key = true;
        }
        memcpy(g_hid_prev_codes, codes, 6);
        kt_resync(&g_kt, codes);
        return;
    }

    // PIZERO-165: Print Screen saves the screen, overlay or not (0x46 presses
    // no CoCo key, so it passes on harmlessly).
    bool prtsc = false;
    for (int i = 0; i < 6; i++) if (codes[i] == 0x46) prtsc = true;
    if (prtsc && !g_prtsc_held) g_shot_request = true;
    g_prtsc_held = prtsc;

    // PIZERO-186: F8 cycles the artifact colors while the machine runs (0x41
    // presses no CoCo key). The save is loop()'s, not this callback's.
    bool f8 = false;
    for (int i = 0; i < 6; i++) if (codes[i] == 0x41) f8 = true;
    if (f8 && !g_f8_held && g_machine_running && !disk_overlay_is_open()) g_art_request = true;
    g_f8_held = f8;

    // PIZERO-81c: the F12 overlay sees every report first. Open, it takes all
    // of them, so nothing reaches BASIC; closed, it takes only its F keys.
    memcpy(g_kb_codes, codes, 6);
    if (overlay_feed(mods)) return;

    // PIZERO-163: each key types its keycap's character, SHIFT forced to suit.
    kt_update(&g_kt, mods, codes);
    hid_keys_present(mods);
}

// ---- ROM/boot (same flow as the AMOLED port) -----------------------------
#ifdef COCO3
// PIZERO-135: a build with the CoCo 3 in it can be either machine; the
// machine setting picks one at boot, and switching restarts the board.
static bool g_coco3_mode = false;
#define IS_COCO3() g_coco3_mode
#else
#define IS_COCO3() false
#endif
#ifdef COCO3
static uint8_t g_coco_rom[32768];          // PIZERO-133: coco3.rom (a CoCo 2 uses the first 16 KB)
#else
static uint8_t g_coco_rom[16384];
#endif
// PIZERO-139: room for a 16 KB cartridge. The double-buffered builds have
// no RAM to spare (two framebuffers), so they keep the old 8 KB limit.
#ifdef FB_SINGLE
#define COCO_CART_MAX 16384
#else
#define COCO_CART_MAX 8192
#endif
static uint8_t g_cart_rom[COCO_CART_MAX];

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

#ifdef AV_DATA_ISLAND
#ifdef AV_60HZ
// PIZERO-45: TRUE 640x480p60. 252 MHz sysclk -> 25.2 MHz pixel, h_total(800) x
// v_total(525) = 420000 px -> 25.2e6/420000 = 60.0 Hz exactly, period = 16667 us.
// At the CoCo's 0.894886 MHz that's 14915 cyc/frame, and 48000/60 = exactly 800
// audio samples/frame (AUDIO_SAMPLES_PER_FRAME) -- producer, encoder and sink all
// on one drift-free cadence. In-spec timing also fixes the ~13% slow game speed.
#define CYCLES_PER_FRAME 14915
#define FRAME_PERIOD_US  16667
#else
// PIZERO-30: pace emulation to the TRUE video refresh. Pixel clock 24 MHz, frame
// is h_total(880) x v_total(525) = 462000 px, so refresh = 24e6/462000 =
// 51.9481 Hz, period = 19250 us. Matching the loop keeps the audio producer, the
// per-frame encoder, and the sink's consumption on one cadence. CYCLES_PER_FRAME
// (17226 / 894886.25 Hz = 19250 us) makes the emulator produce EXACTLY
// 48000/51.9481 = 924 audio samples/frame == AUDIO_SAMPLES_PER_FRAME (no drift).
#define CYCLES_PER_FRAME 17226
#define FRAME_PERIOD_US  19250
#endif
#else
#define CYCLES_PER_FRAME 15000
#define FRAME_PERIOD_US  16762
#endif

// PIZERO-130: the 1.79 MHz benchmark. A CoCo 3 runs its 6809 at twice the
// CoCo 2's clock, so CPU_SPEED_MUL=2 (env pizero_cpu179) runs twice the cycles
// in each 60 Hz frame: the CPU load of a CoCo 3, before any GIME cost. The
// whole machine runs fast, sound included (the ring overflows and counts
// skips), so it is a measurement build, not one to play.
#ifndef CPU_SPEED_MUL
#define CPU_SPEED_MUL 1
#endif

#ifdef AUDIO_WAV_DUMP
// ---- PIZERO-18 audio validation: WAV-over-USB-CDC --------------------------
// Streams the emulator's PCM out the serial port as a base64-encoded WAV, framed
// by ---WAV-BEGIN---/---WAV-END--- markers, for a fixed window after boot. No
// hardware wiring: capture the serial log on the host, snip between the markers,
// `base64 -D > coco.wav`. Base64 (not raw) so a cooked tty/monitor can't mangle
// the bytes. All other debug prints are suppressed while dumping.
#ifdef AUDIO_WAV_DUMP_BOOT
static const uint32_t AUDIO_DUMP_SECONDS = 20;   // PIZERO-223: a stretch of a song
#else
static const uint32_t AUDIO_DUMP_SECONDS = 9;
#endif
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

#if defined(AV_DATA_ISLAND) && defined(AV_AUDIO_SWAPTEST)
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
#endif // AV_DATA_ISLAND && AV_AUDIO_SWAPTEST

#if defined(AV_DATA_ISLAND) && !defined(AV_AUDIO_SWAPTEST) && !defined(AV_AUDIO_STATIC)
// PIZERO-30 (M2, Option B final): clean LIVE data-island audio, distributed across the
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

#ifdef AV_STREAM_AUDIO
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
// PIZERO-223: whole numbers, in 480ths of a sample: each active line adds
// AUDIO_SAMPLES_PER_FRAME, and every 480 is a sample, so a frame carries
// exactly that many. The 16.16 step it replaces (800/480 truncated) sent
// 47,999.7 samples a second against the 48,000 that ACR and the Audio
// InfoFrame declare, so a TV's buffer drained and it had to keep correcting.
static int32_t  g_meter_acc  = 0;                  // 480ths of a sample (IRQ-only)
static int32_t  g_meter_step = 0;                  // samples per frame (set at init)
#ifdef AV_60HZ
#define AUDIO_SAMPLES_PER_FRAME 800                // 48000 Hz / 60 Hz refresh (PIZERO-45)
#else
#define AUDIO_SAMPLES_PER_FRAME 924                // 48000 Hz / ~51.95 Hz refresh
#endif
#ifdef AV_VBLANK_AUDIO
// PIZERO-232: audio on the vblank lines too, so the 43-line (~1.4 ms) gap each
// frame closes and the meter spreads a frame's samples over all 523 non-sync
// lines. The audio island goes in the vblank ACTIVE block, where the info
// island has always worked; islands in the vblank back porch blank the dev
// monitor (PIZERO-34). One island per line, so every AV_VBLANK_INFO_EVERY-th
// line carries AVI + Audio InfoFrame + ACR instead (6 a frame at 8, 360 ACR a
// second, down from about 2,700). A slot is in flight for one line, so three
// is margin; each is a full 640-pixel block (3 x 3 x 320 words, 11.5 KB).
#define STREAM_METER_LINES 523
#ifndef AV_VBLANK_INFO_EVERY
#define AV_VBLANK_INFO_EVERY 8
#endif
#define VPOOL 3
#define VPOOL_ISL 8                                // first island's word offset (dvi_setup_scanline_for_vblank_island)
static uint32_t g_vpool0[VPOOL][320];
static uint32_t g_vpool1[VPOOL][320];
static uint32_t g_vpool2[VPOOL][320];
static int      g_vpool_w = 0;                     // next vblank pool slot (IRQ-only)
#else
#define STREAM_METER_LINES 480
#endif
static int16_t  g_stream_last[2] = { 0, 0 };       // hold-last on ring underrun (IRQ-only)
static volatile uint32_t g_stream_under = 0;       // ring-underrun (hold-last) count (diagnostic)
#ifdef AV_AUDIO_SYNTH
#define SYNTH_TBL 512
static int16_t  g_sine[SYNTH_TBL];                 // one 440 Hz period, filled on core 0
static uint32_t g_synth_ph   = 0;                  // 32-bit phase accumulator (IRQ-only)
static uint32_t g_synth_inc  = 0;                  // per-sample phase step (set at init)
#ifdef AV_AUDIO_SYNTH_CHORD
// PIZERO-223: a C major chord (C4 E4 G4) over the HDMI link, made on core 1
// with no CoCo in the path: 4 s of three sines, 4 s of the same notes as
// square waves (what the SN76489 plays), 1 s of silence, repeating. Whether
// the TV makes the sines fuzzy too says whether it is the link or the TV.
static uint32_t g_chord_ph[3], g_chord_inc[3];
static uint32_t g_chord_n = 0;                     // samples into the 9 s cycle
#endif
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
#ifdef AV_EVEN_AUDIO
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
#endif // AV_STREAM_AUDIO

#ifdef AV_STREAM_AUDIO
#ifdef AV_AUDIO_SYNTH
// The synth's mono sample for the streaming meter: a 440 Hz wavetable (no sinf
// in the IRQ), on core 1.
static inline int16_t __not_in_flash_func(stream_next_mono)(void) {
#ifdef AV_AUDIO_SYNTH_CHORD
    uint32_t n = g_chord_n;
    g_chord_n = (n + 1 >= 9u * 48000u) ? 0 : n + 1;
    int32_t acc = 0;
    for (int v = 0; v < 3; ++v) {
        uint32_t ph = g_chord_ph[v];
        g_chord_ph[v] = ph + g_chord_inc[v];
        if (n < 4u * 48000u) {                     // sine, interpolated between table entries
            uint32_t i = ph >> 23, fr = (ph >> 7) & 0xFFFF;
            int32_t a = g_sine[i], b = g_sine[(i + 1) & (SYNTH_TBL - 1)];
            acc += (a + (((b - a) * (int32_t)fr) >> 16)) / 2;
        } else if (n < 8u * 48000u) {              // square, the same peak
            acc += (ph & 0x80000000u) ? -3000 : 3000;
        }
    }
    return (int16_t)acc;
#else
    int16_t s = g_sine[g_synth_ph >> 23];          // top 9 bits index the 512-entry table
    g_synth_ph += g_synth_inc;
    return s;
#endif
}
#endif

// One left and right pair for the streaming meter: the synth, or the CoCo
// ring (hold-last on underrun). PIZERO-250: the two sides differ for the
// Symphony-12 in stereo. Runs on core 1.
#ifdef USB_AUDIO
// PIZERO-255: while a USB DAC has the sound, the ring is its alone (one
// reader), and the video carries silence.
static volatile bool g_audio_usb = false;
static volatile uint32_t g_usb_audio_under = 0;
#endif

static inline void __not_in_flash_func(stream_next_sample)(int16_t *lr) {
#ifdef AV_AUDIO_SYNTH
    lr[0] = lr[1] = stream_next_mono();
#else
#ifdef USB_AUDIO
    if (g_audio_usb) { lr[0] = lr[1] = 0; return; }
#endif
    // PIZERO-81c: while the overlay pauses the machine, the ring is left
    // exactly as it was (the producer is stopped too, so the rate servo sees
    // no jump on resume) and the output fades to digital silence rather than
    // holding the last level as DC. It fades back in on resume. 256 samples
    // each way, about 5 ms: no click, and the stream never stops.
    static uint16_t gain = 256;
    if (g_audio_paused) {
        if (gain) gain--;
        lr[0] = (int16_t)(((int32_t)g_stream_last[0] * gain) >> 8);
        lr[1] = (int16_t)(((int32_t)g_stream_last[1] * gain) >> 8);
        return;
    }
    if (coco_machine_audio_read_lr(lr, 1) < 1) {   // PIZERO-39: ring from IRQ
        lr[0] = g_stream_last[0]; lr[1] = g_stream_last[1]; g_stream_under++;
    }
    g_stream_last[0] = lr[0]; g_stream_last[1] = lr[1];
    if (gain < 256) {
        gain++;
        lr[0] = (int16_t)(((int32_t)lr[0] * gain) >> 8);
        lr[1] = (int16_t)(((int32_t)lr[1] * gain) >> 8);
    }
#endif
}

#ifdef USB_AUDIO
// PIZERO-255: the USB DAC's packet, 48 left and right pairs a 1 ms frame,
// from the ring. Called by Pico-PIO-USB's frame handler (core 0, in an
// interrupt). The ring's rate servo (PIZERO-121) holds it half full against
// whatever drains it, so the producer follows the USB frame clock. Pause
// and underrun as the video path: a fade to silence while the overlay is
// open, and the last pair held when the ring runs short.
uint16_t __not_in_flash_func(usb_audio_fill)(uint8_t *buf, uint16_t max) {
    static uint16_t gain = 256;
    static int16_t last[2] = { 0, 0 };
    uint16_t n = 48;
    if (n * 4 > max) n = max / 4;
    int16_t *o = (int16_t *)buf;
    if (g_audio_paused) {
        for (uint16_t k = 0; k < n; k++) {
            if (gain) gain--;
            o[2 * k] = (int16_t)(((int32_t)last[0] * gain) >> 8);
            o[2 * k + 1] = (int16_t)(((int32_t)last[1] * gain) >> 8);
        }
        return (uint16_t)(n * 4);
    }
    size_t got = coco_machine_audio_read_lr(o, n);
    if (got) { last[0] = o[2 * got - 2]; last[1] = o[2 * got - 1]; }
    for (size_t k = got; k < n; k++) { o[2 * k] = last[0]; o[2 * k + 1] = last[1]; }
    if (got < n) g_usb_audio_under += (uint32_t)(n - got);
    for (uint16_t k = 0; k < n && gain < 256; k++) {
        gain++;
        o[2 * k] = (int16_t)(((int32_t)o[2 * k] * gain) >> 8);
        o[2 * k + 1] = (int16_t)(((int32_t)o[2 * k + 1] * gain) >> 8);
    }
    return (uint16_t)(n * 4);
}

void usb_audio_route(bool usb) { g_audio_usb = usb; }
#endif

// Meter one line: how many samples (0..4) it carries. Free-runs (acc stays in
// [0,1) sample): resetting at v==0 discarded the fractional carry -> 923 vs the
// producer's 924 samp/frame -> slow ring drift.
static inline int __not_in_flash_func(stream_meter)(void) {
    g_meter_acc += g_meter_step;
    int n = g_meter_acc / STREAM_METER_LINES;
    if (n > 4) n = 4;
#ifdef AV_AUDIO_FULL_PACKETS
    // PIZERO-232: send only full 4-sample packets, as an ordinary source does,
    // one every 2.4-2.6 lines, instead of 1-2 samples on nearly every line.
    if (n < 4) return 0;
#endif
    if (n > 0) g_meter_acc -= (int32_t)n * STREAM_METER_LINES;
    return n;
}

// Encode n samples as one audio island into a pool slot's three lane buffers.
static inline void __not_in_flash_func(stream_encode)(int n, uint32_t *b0, uint32_t *b1, uint32_t *b2) {
    int16_t lr[8];
    for (int f = 0; f < n; ++f) stream_next_sample(&lr[2*f]);
    dvi_data_packet_t pkt;
    dvi_di_set_audio_samples(&pkt, lr, n, g_aud_framectr);
    g_aud_framectr = (g_aud_framectr + (uint32_t)n) % 192u;
    dvi_di_compute_parity(&pkt);
    const int hv = ((!DVI_TIMING.v_sync_polarity) ? 2 : 0) | ((!DVI_TIMING.h_sync_polarity) ? 1 : 0);
    dvi_di_encode_header(&b0[5], &pkt, hv, true);  // island at offset 5 (after preamble+guard)
    dvi_di_encode_subpacket(&b1[5], &b2[5], &pkt);
}

// DMA IRQ, ACTIVE scanline (fires during the PREVIOUS line's scanout): meter this
// line's sample count, encode one audio island into the next rotating-pool slot,
// and point the back-porch DMA blocks at it -- or at the no-island framing when
// the meter yields 0 samples. The full-line budget (~33 us) covers the ~4.8 us
// encode (PIZERO-36); same-line back-porch encoding would not (Option A).
static void __not_in_flash_func(stream_audio_cb)(void) {
    g_wd_core1_lines++;                            // PIZERO-227: core 1 is alive
    const int n = stream_meter();
    if (n <= 0) {                                  // no island this line: plain framing
        dvi0.dma_list_active.l0[2].read_addr = g_bp0;
        dvi0.dma_list_active.l1[1].read_addr = g_bk1;
        dvi0.dma_list_active.l2[1].read_addr = g_bk2;
        return;
    }
    const int w = g_pool_w;
    g_pool_w = (w + 1) % APOOL;
    stream_encode(n, g_pool0[w], g_pool1[w], g_pool2[w]);
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
#endif // AV_STREAM_AUDIO

// DMA IRQ, vblank (no-sync) line: AVI/AudioIF/ACR on the first back-porch line,
// control elsewhere. (Audio no longer lives in vblank.)
static void __not_in_flash_func(audio_vblank_info_cb)(void) {
    // PIZERO-30: send AVI+AudioIF+ACR on EVERY vblank line (was just v_ctr==0 ->
    // 1/frame). ~43 ACR/frame gives the sink's audio PLL frequent clock updates;
    // once/frame let it wobble at the frame rate. (Matches the proven static build.)
#ifdef AV_EVEN_AUDIO
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
#elif defined(AV_STREAM_AUDIO) && defined(AV_VBLANK_AUDIO)
    // PIZERO-232: an audio island in the active block, or on every
    // AV_VBLANK_INFO_EVERY-th line the info island. An info line still counts
    // its share into the meter (or a frame carries ~791 samples, not 800, and
    // the ring overflows), so the next audio line takes the carry.
    const uint vs = dvi0.timing_state.v_state, vc = dvi0.timing_state.v_ctr;
    const int vbi = (vs == DVI_STATE_FRONT_PORCH) ? (int)vc : (10 + (int)vc);  // fp 0-9, bp 10-42
    const uint32_t *a0 = g_info[0], *a1 = g_info[1], *a2 = g_info[2];
    if (vbi % AV_VBLANK_INFO_EVERY == 0) {
        g_meter_acc += g_meter_step;
    } else {
        const int n = stream_meter();
        if (n > 0) {
            const int w = g_vpool_w;
            g_vpool_w = (w + 1) % VPOOL;
            stream_encode(n, &g_vpool0[w][VPOOL_ISL], &g_vpool1[w][VPOOL_ISL], &g_vpool2[w][VPOOL_ISL]);
            a0 = g_vpool0[w]; a1 = g_vpool1[w]; a2 = g_vpool2[w];
        } else {
            a0 = g_ctrl[0]; a1 = g_ctrl[1]; a2 = g_ctrl[2];
        }
    }
    dvi0.dma_list_vblank_nosync.l0[3].read_addr = a0;
    dvi0.dma_list_vblank_nosync.l1[2].read_addr = a1;
    dvi0.dma_list_vblank_nosync.l2[2].read_addr = a2;
#else
    dvi0.dma_list_vblank_nosync.l0[3].read_addr = g_info[0];
    dvi0.dma_list_vblank_nosync.l1[2].read_addr = g_info[1];   // lane 1 active block is [2] after split
    dvi0.dma_list_vblank_nosync.l2[2].read_addr = g_info[2];
#endif
}

// Core 0 (after blit): re-encode the OFF bank's audio islands from the ring, flip.
// (Streaming delivery (PIZERO-38) encodes per-line in the IRQ instead -- see
// stream_audio_cb; the bank refill + its synth_fill helper are bank-path only.)
#if defined(AV_AUDIO_SYNTH) && !defined(AV_STREAM_AUDIO)
// CONTROL EXPERIMENT (PIZERO-30): bypass the emulator entirely and feed a
// mathematically clean 440 Hz sine straight into the audio data islands. Same
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

#ifndef AV_STREAM_AUDIO
static void audio_encode_frame(void) {
    const uint32_t off = g_active_bank ^ 1u;
    const int hv = ((!DVI_TIMING.v_sync_polarity) ? 2 : 0) | ((!DVI_TIMING.h_sync_polarity) ? 1 : 0);
#ifdef AV_AUDIO_SYNTH
    int16_t mono[12];
#endif
    int16_t stereo[24];
    dvi_data_packet_t pkt;
    for (int line = 0; line < N_ALINES; ++line) {
        const int np = aline_pkts(line);
        const int want = np * 4;
#ifdef AV_AUDIO_SYNTH
        synth_fill(mono, want);                       // clean sine, no emulation
        for (int i = 0; i < want; ++i) stereo[2*i] = stereo[2*i + 1] = mono[i];
#else
        size_t n = coco_machine_audio_read_lr(stereo, (size_t)want);   // PIZERO-250
        for (size_t i = n; i < (size_t)want; ++i) {                     // hold last on underrun
            stereo[2*i] = i ? stereo[2*i - 2] : 0;
            stereo[2*i + 1] = i ? stereo[2*i - 1] : 0;
        }
#endif
        for (int k = 0; k < np; ++k) {
            dvi_di_set_audio_samples(&pkt, &stereo[k * 8], 4, g_aud_framectr);
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
#endif // !AV_STREAM_AUDIO
#endif // AV_DATA_ISLAND && !SWAPTEST && !STATIC

// ── PIZERO-33: freeze auto-recovery + root-cause ──────────────────────────
// A hardware watchdog, petted once per loop() on core 0, auto-reboots the board
// if core 0 wedges anywhere (the recurring everyday freeze; suspected USB-host
// path per PIZERO-11b). Phase markers written to watchdog SCRATCH registers
// (which SURVIVE a watchdog reset, but not power-on) record where core 0 last
// was, so after an auto-reboot we print exactly which operation hung. A magic in
// scratch[0] distinguishes a real freeze record from power-on garbage. PIZERO-33
// persistent log: scratch[3] packs a running freeze COUNT + the LAST stuck phase,
// so the tally survives across auto-reboots and is shown in the [run] telemetry.
// You can connect a monitor any time (while still powered) and see how many times
// and where it froze, without catching the recovery boot live.
//   scratch[0]=WD_MAGIC  scratch[1]=phase  scratch[2]=heartbeat
//   scratch[3]=[31:8]=freeze count  [7:0]=last-freeze phase
// Build with -DWATCHDOG_DISABLE to leave a freeze frozen (live debugging).
#ifndef WATCHDOG_TIMEOUT_MS
#define WATCHDOG_TIMEOUT_MS 3000   // >> the ~19ms frame loop; only a real hang trips it
#endif
#define WD_MAGIC 0x05330534u   // bumped for scratch[3] freeze-log layout (invalidates stale scratch)
// PIZERO-51: scratch[4] sentinel marking a DELIBERATE watchdog reboot for USB
// re-enumeration (device unplugged) so setup() reports it as a replug, not a freeze.
#define USB_REPLUG_MAGIC 0x51D15C09u
// PIZERO-203: WP_TELEM and WP_SOAKLOG split the end of the frame, so a freeze
// there names the pacing wait, the serial telemetry or the card log.
enum { WP_NONE = 0, WP_SETUP, WP_LOOP, WP_USB, WP_KBD, WP_EMU, WP_RENDER, WP_BLIT, WP_AUDIO, WP_PACE,
       WP_TELEM, WP_SOAKLOG, WP_LAST = WP_SOAKLOG };
static bool g_run_button_reset = false;    // PIZERO-116: this boot came from RUN
static uint32_t g_freeze_count = 0;        // persistent across reboots (from scratch[3])
static char g_wd_post_text[400];           // PIZERO-227: the last freeze's post-mortem, for the soak log
static uint32_t g_last_freeze_phase = WP_NONE;
static const char *wd_phase_name(uint32_t p) {
    switch (p) {
        case WP_SETUP: return "SETUP";       case WP_LOOP:   return "loop-top";
        case WP_USB:   return "USBHost.task"; case WP_KBD:   return "keyboard";
        case WP_EMU:   return "emulate";     case WP_RENDER: return "VDG-render";
        case WP_BLIT:  return "blit";        case WP_AUDIO:  return "audio-encode";
        case WP_PACE:  return "frame-pace";
        case WP_TELEM: return "telemetry";   case WP_SOAKLOG: return "soak-log";  default:        return "?";
    }
}
#ifdef WATCHDOG_DISABLE
static inline void wd_phase(uint32_t p)     { (void)p; }
static inline void wd_heartbeat(uint32_t h) { (void)h; }
#else
static inline void wd_phase(uint32_t p)     { watchdog_hw->scratch[1] = p; }
static inline void wd_heartbeat(uint32_t h) { watchdog_hw->scratch[2] = h; }
#endif


// PIZERO-92: show a diagnostic on the SCREEN, not just on a serial port nobody
// has. Every pre-machine failure leaves libdvi running and the framebuffer
// black, and a monitor that syncs to a black picture looks exactly like dead
// hardware. We ship no ROMs by design, so a first power-up hits this path.
//
// The serial messages stay: they are how WE debug. This adds the half the
// recipient can see. The idle loop keeps petting the watchdog, so the board
// does not reboot under the message.
// A whole page, not a one-liner. We have 32x16 characters and no 64 KB budget
// to defend, so a failure gets the space to say what happened, what to do
// about it, and what we actually saw on the card. Three blocks:
//
//   title    one line, centred, the headline the reader repeats on the phone
//   body     what happened and what to do, wrapped
//   detail   the evidence, so the next person does not have to guess
static void settings_apply(void);

// PIZERO-154: the game running now (its .bin, .ccc or .dsk path; "" for
// none) and its own settings file, if it has one. Every launch starts again
// from settings.txt, so one game's settings never leak into the next.
static char g_game_path[96];
static char g_game_settings[96];

static void settings_for_game(const char *game) {
    g_game_path[0] = g_game_settings[0] = '\0';
    char p[96];
    bool have_game = false;
    if (game && game[0] && strlen(game) < sizeof g_game_path) {
        strcpy(g_game_path, game);
        have_game = settings_game_path(game, p, sizeof p);
    }
    // PIZERO-242: the machine's profile, settings.txt, then the game's file.
    bool found = false;
    coco_boot_load_settings_for(have_game ? p : nullptr, &g_settings, &found);
    if (found) {
        strcpy(g_game_settings, p);
        Serial.printf("[settings] with %s\r\n", p);
    }
    settings_apply();
}

// PIZERO-135: switching machine. The two lay out RAM differently, so a
// switch restarts the board (a full watchdog reset, tagged in scratch[4] so
// it is not counted as a freeze), carrying the machine to boot and what to do
// next in a small file on the card: RAM does not survive the restart (the
// boot reuses SRAM, wd_post.cpp) and the scratch registers are all in use.
// The file is used once: setup() reads it and deletes it, and only believes
// it after a tagged restart, so RUN or a power cycle boots as settings.txt
// says.
static void present_card(void);
enum { HO_NORMAL = 0,      // boot as usual (autorun too) on the given machine
       HO_BASIC = 1,       // the BASIC prompt: a settings change, nothing to run
       HO_LAUNCH = 2 };    // launch path as the overlay would (kind is CAT_*)
#define MACHINE_SWITCH_MAGIC 0x5C0C0135u    // scratch[4]: a deliberate switch restart
#define HO_FILE "0:/coco/log/switch.txt"
struct machine_handoff {
    bool     valid;
    uint8_t  coco3, what;
    int8_t   kind;
    char     path[96];
};
static struct machine_handoff g_boot_handoff;   // this boot's, valid only after a switch
static bool g_switch_restart = false;           // setup(): scratch[4] said so

// The record, read and deleted. One line: coco3 what kind path.
static void handoff_read(void) {
    char buf[160];
    uint32_t n = 0;
    if (!coco_boot_load_text(HO_FILE, buf, sizeof buf - 1, &n)) return;
    f_unlink(HO_FILE);
    if (!g_switch_restart) return;              // a leftover, not this restart's
    buf[n] = '\0';
    unsigned c3 = 0, what = 0;
    int kind = -1, used = 0;
    if (sscanf(buf, "%u %u %d %n", &c3, &what, &kind, &used) < 3) return;
    char *p = buf + used;
    p[strcspn(p, "\r\n")] = '\0';
    g_boot_handoff.valid = true;
    g_boot_handoff.coco3 = c3 != 0;
    g_boot_handoff.what = (uint8_t)what;
    g_boot_handoff.kind = (int8_t)kind;
    snprintf(g_boot_handoff.path, sizeof g_boot_handoff.path, "%s", p);
}

// Whether the card holds what that machine needs to start.
// PIZERO-319: the FILINFO (about 280 bytes) in a function of its own, so it
// is not on the stack while coco_boot_resolve, with one of its own, runs.
static FRESULT __attribute__((noinline)) file_size(const char *p, FSIZE_t *size) {
    FILINFO fi;
    FRESULT r = f_stat(p, &fi);
    *size = r == FR_OK ? fi.fsize : 0;
    return r;
}

static bool machine_available(bool coco3) {
    char p[80];
    const char *rom = coco3 ? "coco3.rom" : "bas12.rom";
    const FSIZE_t want = coco3 ? 32768u : 8192u;
    // PIZERO-261: say why not, so a missing or wrong-size ROM is found at once.
    if (!coco_boot_resolve("rom", rom, p, sizeof p)) {
        Serial.printf("[machine] %s not found in /coco/roms/ or /coco/\r\n", rom);
        return false;
    }
    FSIZE_t size = 0;
    FRESULT r = file_size(p, &size);
    if (r != FR_OK) {
        Serial.printf("[machine] %s: cannot read it (%d)\r\n", p, (int)r);
        return false;
    }
    if (size != want) {
        Serial.printf("[machine] %s is %lu bytes, needs %lu\r\n", p, (unsigned long)size, (unsigned long)want);
        return false;
    }
    return true;
}

#ifdef COCO3
static bool install_cart_file(const char *path);
static void release_big_cart(void);

// PIZERO-314: build the other machine in place, with no restart: USB, the
// card and the display carry on, and the drives stay mounted. The machine
// is taken down (coco_machine_shutdown), its ROM loaded and the new one
// built and set up as setup() does: settings, then Disk BASIC with the
// drives, at its power-on. Returns false if the ROM or the memory is not
// there; the old machine is gone by then, so the caller restarts instead.
static bool machine_rebuild(bool coco3) {
    uint32_t t0 = millis();
    coco_boot_flush_drives();                      // a written disk reaches the card first
    g_machine_running = false;
    g_audio_paused = true;
    coco_machine_install_cart(nullptr);
    release_big_cart();
    coco_machine_shutdown();
    Serial.printf("[mem] free heap %d bytes with no machine\r\n", rp2040.getFreeHeap());
    bool rom = coco3 ? coco_boot_load_coco3_rom(g_coco_rom) : coco_boot_load_rom_from_sd(g_coco_rom);
    if (!rom || !coco_machine_init(g_coco_rom, coco3 ? 32768 : 16384)) {
        Serial.printf("[machine] cannot build the %s in place (%s, free heap %d bytes)\r\n",
                      coco3 ? "CoCo 3" : "CoCo 2", rom ? "no memory" : "no ROM", rp2040.getFreeHeap());
        return false;
    }
    g_coco3_mode = coco3;
    if (coco3) coco_machine_set_framebuffer(g_fb);
    settings_apply();
    char cart[80];
    if (coco_boot_resolve_cart("disk11.rom", cart, sizeof cart) && install_cart_file(cart)) {
        coco_machine_install_disk_reader(coco_boot_disk_read_sector);
        coco_machine_install_disk_writer(coco_boot_disk_write_sector);
        coco_machine_install_disk_idle(coco_boot_flush_drives);
    }
    g_kt.coco3 = coco3;                            // PIZERO-215
    coco_machine_video_invalidate();
    g_audio_paused = disk_overlay_is_open();
    g_machine_running = true;
    Serial.printf("[machine] now a %s, built in place in %lu ms; free heap %d bytes\r\n",
                  coco3 ? "CoCo 3" : "CoCo 2", (unsigned long)(millis() - t0), rp2040.getFreeHeap());
    return true;
}
#endif

// Become the other machine, then do `what`. PIZERO-314: in place when it
// can (HO_BASIC and HO_LAUNCH return, and the caller carries on with the
// new machine); otherwise, and for HO_NORMAL, whose boot is not finished,
// restart as it. Returns without a change only if the restart's record
// cannot be written.
static void switch_machine(bool coco3, uint8_t what, int kind, const char *path) {
#ifdef COCO3
    if (what != HO_NORMAL && machine_rebuild(coco3)) return;
#endif
    Serial.printf("[machine] switching to %s, restarting\r\n", coco3 ? "CoCo 3" : "CoCo 2");
    coco_boot_flush_drives();                      // a written disk reaches the card first
    char line[160];
    int n = snprintf(line, sizeof line, "%u %u %d %s\n", coco3 ? 1u : 0u, (unsigned)what, kind,
                     path ? path : "");
    f_mkdir("0:/coco/log");
    if (n <= 0 || n >= (int)sizeof line || !coco_boot_save_text(HO_FILE, line, (uint32_t)n)) {
        Serial.print("[machine] cannot write the switch record: not switching\r\n");
        return;
    }
    coco_boot_card_clear();
    coco_boot_card_center(7, coco3 ? "STARTING THE COCO 3" : "STARTING THE COCO 2");
    present_card();
    Serial.flush();
    delay(300);
    // A full reset, like PIZERO-51's: a core reset alone left the display's
    // DMA and PIO running and the next boot stalled starting them.
    // watchdog_reboot clears scratch[4] for a plain reboot, so the tag goes
    // in after it; the reset comes 30 ms later.
    watchdog_reboot(0, 0, 30);
    watchdog_hw->scratch[4] = MACHINE_SWITCH_MAGIC;
    while (true) tight_loop_contents();
}

// The machine `game` (settings.txt with the game's own file on top) asks for.
static bool machine_wanted(const char *game) {
    static struct coco_settings t;
    char p[96];
    bool g = game && game[0] && settings_game_path(game, p, sizeof p);
    coco_boot_load_settings_for(g ? p : nullptr, &t, nullptr);
    return settings_is_coco3(&t);
}

// PIZERO-255: the volume keys of a keyboard or a USB headphone DAC. Up and
// Down step the volume setting (0-15); Mute toggles to silence and back, and
// Up or Down while muted unmute first. Each change is put into effect at
// once and written into settings.txt two seconds after the last press, so a
// run of presses is one write to the card. Mute itself is not saved.
static uint8_t  g_vol_unmuted = 10;
static bool     g_vol_muted = false;
static uint32_t g_vol_save_at = 0;        // millis() to save at, 0 for none
#define HIDC_N 8                          // HID interfaces (CFG_TUH_HID)
static struct hidc_map g_hidc[HIDC_N];
static uint8_t g_hidc_held[HIDC_N];

static void volume_keys(uint8_t pressed) {
    uint8_t v = g_settings.volume;
    if (pressed & HIDC_MUTE) {
        if (g_vol_muted) { v = g_vol_unmuted; g_vol_muted = false; }
        else { g_vol_unmuted = v; v = 0; g_vol_muted = true; }
    }
    if (pressed & (HIDC_UP | HIDC_DOWN)) {
        if (g_vol_muted) { v = g_vol_unmuted; g_vol_muted = false; }
        if ((pressed & HIDC_UP) && v < 15) v++;
        if ((pressed & HIDC_DOWN) && v > 0) v--;
    }
    g_settings.volume = v;
    coco_machine_set_volume(v);
    if (!g_vol_muted) { uint32_t t = millis() + 2000u; g_vol_save_at = t ? t : 1u; }   // 0 means none
    Serial.printf("[settings] volume = %u%s\r\n", v, g_vol_muted ? " (muted)" : "");
}

static void volume_save_due(void) {
    if (!g_vol_save_at || (int32_t)(millis() - g_vol_save_at) < 0) return;
    g_vol_save_at = 0;
    const char *path = "0:/coco/settings.txt";
    const int cap = 4096;                       // as the editor's whole-file limit
    char *buf = (char *)malloc(cap);
    bool ok = false;
    if (buf) {
        uint32_t n = 0;
        int rd = coco_boot_read_text(path, buf, cap, &n);   // PIZERO-296: never a cut-short file
        if (rd == COCO_TEXT_MISSING) n = 0;
        char val[4];
        snprintf(val, sizeof val, "%u", (unsigned)g_settings.volume);
        int m = (rd == COCO_TEXT_OK || rd == COCO_TEXT_MISSING)
                ? settings_set_line(buf, (int)n, cap, "volume", val) : -1;
        ok = m >= 0 && coco_boot_save_text(path, buf, (uint32_t)m);
        free(buf);
    }
    Serial.printf("[settings] volume = %u %s %s\r\n", (unsigned)g_settings.volume,
                  ok ? "saved to" : "NOT saved to", path);
}

// PIZERO-186: F8. The next artifact color setting (on, swapped, off, on),
// put into effect and written into the running game's own settings file, or
// settings.txt when nothing was launched, keeping the rest of the file.
static void artifact_cycle_and_save(void) {
    static const char *const names[3] = { "off", "on", "swapped" };   // ART_OFF, ART_ON, ART_SWAPPED
    uint8_t a = g_settings.artifact;
    a = (a == ART_ON) ? ART_SWAPPED : (a == ART_SWAPPED) ? ART_OFF : ART_ON;
    g_settings.artifact = a;
    coco_machine_set_artifact(a);
    char gp[96];
    const char *path = (g_game_path[0] && settings_game_path(g_game_path, gp, sizeof gp))
                       ? gp : "0:/coco/settings.txt";
    const int cap = 4096;                       // as the editor's whole-file limit
    char *buf = (char *)malloc(cap);
    bool ok = false;
    if (buf) {
        uint32_t n = 0;
        int rd = coco_boot_read_text(path, buf, cap, &n);   // PIZERO-296: never a cut-short file
        if (rd == COCO_TEXT_MISSING) n = 0;                 // a new file
        int m = (rd == COCO_TEXT_OK || rd == COCO_TEXT_MISSING)
                ? settings_set_line(buf, (int)n, cap, "artifact_colors", names[a]) : -1;
        ok = m >= 0 && coco_boot_save_text(path, buf, (uint32_t)m);
        free(buf);
    }
    if (ok && strcmp(path, "0:/coco/settings.txt") && !g_game_settings[0])
        snprintf(g_game_settings, sizeof g_game_settings, "%s", path);   // the game has a file now
    Serial.printf("[settings] F8: artifact_colors = %s, %s %s\r\n", names[a],
                  ok ? "saved to" : "NOT saved to", path);
}

// PIZERO-146/147/154: after the editor saves a file. settings.txt, or the
// running game's own file, is re-read and applied at once, so a change needs
// no reboot. autorun.txt only matters at power-on: running it now would
// restart the machine unannounced. Another game's file waits for that game.
// Is path NAME.TXT beside a NAME.BIN, .CCC, .DSK or .DMK? PIZERO-319: its own
// function, so its FILINFO and name (about 400 bytes) are on the stack only
// for a file that is not settings.txt, not under every save.
static bool __attribute__((noinline)) is_game_settings(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || strcasecmp(dot, ".txt")) return false;
    static const char *const games[] = { ".BIN", ".CCC", ".DSK", ".DMK" };
    char g[100];
    FILINFO fi;
    for (unsigned i = 0; i < sizeof games / sizeof games[0]; i++) {
        snprintf(g, sizeof g, "%.*s%s", (int)(dot - path), path, games[i]);
        if (f_stat(g, &fi) == FR_OK) return true;
    }
    return false;
}

static const char *on_file_saved(const char *path) {
    char mine[96];
    bool running_game = g_game_path[0] && settings_game_path(g_game_path, mine, sizeof mine)
                        && strcasecmp(path, mine) == 0;
    if (strcmp(path, "0:/coco/settings.txt") == 0 || running_game) {
        settings_for_game(g_game_path);
#ifdef COCO3
        // PIZERO-135: machine changed: become it, at the BASIC prompt.
        // PIZERO-319: not here. A save runs inside the USB keyboard
        // callback, and rebuilding the machine there crashed in the USB
        // timer interrupt; the editor's buffer also splits the heap, so the
        // CoCo 3's 64 KB does not fit. loop() does it once the overlay is
        // closed, as it does a launch.
        if (settings_is_coco3(&g_settings) != IS_COCO3() && machine_available(settings_is_coco3(&g_settings)))
            g_switch_want = settings_is_coco3(&g_settings) ? 1 : 0;
        else
            g_switch_want = -1;
        if (g_switch_want >= 0) return TEK_MSG_SWITCH;
#endif
        return TEK_MSG_APPLIED;
    }
    if (strcmp(path, "0:/coco/autorun.txt") == 0) return TEK_MSG_NEXT;
    // PIZERO-296: FILES opens any text file now. Only a game's own settings
    // file (NAME.TXT beside NAME.BIN, .CCC or .DSK) waits for its game.
    return is_game_settings(path) ? TEK_MSG_GAME : nullptr;   // nullptr: TEK_MSG_SAVED
}

// PIZERO-145: put every setting into effect. Called at boot and again
// whenever settings.txt is re-read (the on-screen editor, PIZERO-146).
static void settings_apply(void) {
    const struct coco_settings *st = &g_settings;
    coco_machine_csg_always(st->sn76489);
    coco_machine_set_ay_carts(st->coco_psg, st->symphony12, st->deluxe_psg, st->symphony12_stereo);   // PIZERO-149/250
    coco_machine_set_orchestra90(st->orchestra90);        // PIZERO-148
    coco_machine_set_midi_pak(st->midi_pak);              // PIZERO-287
#ifdef USB_AUDIO
    usb_audio_set_db(st->usb_audio_db);                   // PIZERO-255, before a DAC is set up
    usb_audio_enable(st->usb_audio);                      // PIZERO-255
#endif
    coco_machine_set_sound_sources(st->sound_all);        // PIZERO-253
    coco_machine_set_joystick_quiet(st->joystick_quiet);
    coco_machine_set_volume(st->volume);
    coco_machine_set_lowpass(st->lowpass_hz);   // PIZERO-212
    coco_machine_set_artifact(st->artifact);
    coco_machine_set_gime_palette(st->gime_palette);
    coco_machine_set_gime_timer(st->gime_timer);
    coco_machine_set_font(st->font);          // PIZERO-166
    coco_machine_set_lowercase(st->lowercase);
    coco_machine_palette_set_default(st->color_set ? st->color : nullptr, st->color_set);
    dvi_sio_tmds_encode = st->video_hw_encode;   // PIZERO-175: from the next scanline
    Serial.printf("[settings] machine=%s sn76489=%s coco_psg=%s symphony12=%s deluxe_psg=%s orchestra90=%s midi_pak=%s usb_audio=%s/%ddB sound_sources=%s joystick_quiet=%s volume=%u audio_lowpass=%u artifact_colors=%s gime_palette=%s gime_timer=%s "
                  "autorun=%s reset_button=%s serial_keyboard=%s joystick_swap=%s font=%s lowercase=%s key_repeat=%s/%ums/%u video_encoder=%s soak_log=%s colors_overridden=%04x\r\n",
                  SETTINGS_MACHINE_NAMES[st->machine], st->sn76489 ? "on" : "off",
                  st->coco_psg ? "on" : "off", st->symphony12 ? (st->symphony12_stereo ? "stereo" : "mono") : "off", st->deluxe_psg ? "on" : "off", st->orchestra90 ? "on" : "off", st->midi_pak ? "on" : "off", st->usb_audio ? "on" : "off", (int)st->usb_audio_db,
                  st->sound_all ? "all" : "switched", st->joystick_quiet ? "on" : "off", st->volume, (unsigned)st->lowpass_hz,
                  st->artifact == ART_OFF ? "off" : st->artifact == ART_SWAPPED ? "swapped" : "on",
                  st->gime_palette ? "on" : "off", st->gime_timer ? "on" : "off",
                  st->autorun ? "on" : "off", st->reset_to_basic ? "basic" : "autorun", st->serial_keyboard ? "on" : "off",
                  st->joystick_swap ? "on" : "off",
                  st->font == FONT_CLASSIC ? "classic" : st->font == FONT_6847T1 ? "6847t1" : "6847t2",
                  st->lowercase ? "on" : "off",
                  st->key_repeat ? "on" : "off", (unsigned)st->key_repeat_delay,
                  (unsigned)st->key_repeat_rate,
                  st->video_hw_encode ? "hardware" : "software", st->soak_log ? "on" : "off",
                  (unsigned)st->color_set);
}

// Put the finished card on screen. Shared by the boot pages and the F12
// overlay (PIZERO-81), which both draw while the machine is not running.
static void present_card(void) {
#ifdef VIDEO_640
    coco_boot_card_present8(g_fb, g_pal565, PAL_CARD);
#elif defined(FB_SINGLE)
    coco_boot_card_present(g_fb);
#else
    coco_boot_card_present(g_fb[0]);
    coco_boot_card_present(g_fb[1]);
    g_front = g_fb[0];
#endif
}

// PIZERO-152: hold a boot page until a key is pressed or max_ms passes.
// Runs before loop(), so it services the USB host itself; a byte on the
// serial console counts as a key too. The timeout keeps a board with no
// keyboard from waiting forever.
static uint32_t g_bt_pages = 0;          // PIZERO-315: ms the boot spent on pages

static void boot_wait(uint32_t max_ms) {
    g_boot_key = false;
    uint32_t t0 = millis();
    while (!g_boot_key && (millis() - t0) < max_ms) {
        USBHost.task();
        if (Serial.available()) { while (Serial.available()) Serial.read(); break; }
        delay(2);
    }
    g_bt_pages += millis() - t0;
}

static void boot_page(const char *title, const char *body, const char *detail) {
    coco_boot_card_clear();
    coco_boot_card_center(1, title);
    int r = coco_boot_card_wrap(2, 4, 28, 7, body);
    if (detail) coco_boot_card_wrap(2, (r < 12 ? 12 : r + 1), 28, 3, detail);
    present_card();
}

// ---- Launching from the F12 overlay (PIZERO-81d, PIZERO-136) --------------
// The overlay asks through overlay_launch_request() while it is still open,
// so anything that can be checked up front (a missing disk11.rom, a cart of
// the wrong size, a .bin that is not LOADM format) is reported on its status
// line and the running program is left alone. Once accepted, the overlay
// closes and loop() performs the launch before its next frame.
//
//   DISK  that disk into drive 0 (other drives kept), Disk BASIC, cold boot,
//         then type RUN"X" or LOADM"X":EXEC for the disk's first program.
//   BIN   no cartridge, cold boot, let BASIC settle, load and jump: the same
//         as @DIRECT at power-on.
//   CART  install it, cold boot; the cart FIRQ takes the machine to $C000.
static int  g_launch_kind = -1;          // -1 = nothing pending
static char g_launch_path[96];
static char g_launch_cmd[32];            // the autotype command for DISK
static int  g_bin_settle = 0;            // frames until a pending BIN is loaded

static void noop_loadm_cb(uint16_t, const uint8_t *, uint16_t, void *) {}

// PIZERO-142: one way to install any cartridge file. Up to 16 KB goes in the
// fixed buffer; a bank-switched image (32 KB and up, cart_gmc.h) gets a heap
// buffer of its own size, only while it is installed, so the RAM stays free
// for everything else. The old big buffer is released before the new one is
// taken (the cart is unplugged first, so nothing reads freed memory), which
// lets one 64 KB cart replace another.
static uint8_t *g_cart_big = nullptr;
static uint32_t g_cart_big_len = 0;
static bool     g_cart_big_lent = false;   // PIZERO-319: in the machine's kept block, not the heap
#define CART_HEAP_MARGIN 16384      // left free for FatFs, USB and stacks

static bool banked_cart_fits(uint32_t len) {
    uint32_t room = 0;
    if (coco_machine_cart_room(&room) && len <= room) return true;     // PIZERO-319
    return (uint32_t)rp2040.getFreeHeap() + (g_cart_big_lent ? 0 : g_cart_big_len) >= len + CART_HEAP_MARGIN;
}

static void release_big_cart(void) {
    if (!g_cart_big_lent) free(g_cart_big);
    g_cart_big = nullptr;
    g_cart_big_len = 0;
    g_cart_big_lent = false;
}

static bool install_cart_file(const char *path) {
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) return false;
    uint32_t n = (uint32_t)fi.fsize;
    if (cart_is_banked_size(n)) {
        coco_machine_install_cart(nullptr);
        release_big_cart();
        uint32_t room = 0;
        uint8_t *lend = coco_machine_cart_room(&room);   // PIZERO-319: a CoCo 2's spare 40 KB first
        bool lent = lend && n <= room;
        uint8_t *img = lent ? lend : (uint8_t *)malloc(n);
        if (!img) {
            Serial.printf("[cart] no memory for %lu bytes\r\n", (unsigned long)n);
            return false;
        }
        if (!coco_boot_load_file(path, img, n)) { if (!lent) free(img); return false; }
        g_cart_big = img;
        g_cart_big_len = n;
        g_cart_big_lent = lent;
        coco_machine_install_cart_banked(img, n);
        Serial.printf("[cart] banked, %lu banks\r\n", (unsigned long)(n / CART_BANK_SIZE));
        return true;
    }
    uint32_t len = 0;
    if (!coco_boot_load_cart_path(path, g_cart_rom, COCO_CART_MAX, &len)) return false;
    coco_machine_install_cart_sized(g_cart_rom, len);
    release_big_cart();
    return true;
}

static bool overlay_launch_request(int kind, const char *path, char *msg, size_t msg_sz) {
    if (strlen(path) >= sizeof g_launch_path) return false;
    FILINFO fi;
    switch (kind) {
    case CAT_DSK: {
        char rom[80];
        if (!coco_boot_resolve("rom", "disk11.rom", rom, sizeof rom)) {
            snprintf(msg, msg_sz, "NEEDS DISK11.ROM");
            return false;
        }
        break;
    }
    case CAT_BIN: {
        uint16_t entry = 0;
        if (!coco_boot_parse_loadm(path, noop_loadm_cb, nullptr, &entry)) {
            snprintf(msg, msg_sz, "NOT A LOADM PROGRAM");
            return false;
        }
        break;
    }
    case CAT_CART:
        if (f_stat(path, &fi) != FR_OK) { snprintf(msg, msg_sz, "CANNOT OPEN IT"); return false; }
        if (cat_cart_size_ok((unsigned long)fi.fsize, COCO_CART_MAX)) break;
        if (cart_is_banked_size((uint32_t)fi.fsize)) {
            if (!banked_cart_fits((uint32_t)fi.fsize)) {
                snprintf(msg, msg_sz, "NOT ENOUGH MEMORY FOR %luK",
                         (unsigned long)(fi.fsize / 1024));
                return false;
            }
            break;
        }
        snprintf(msg, msg_sz, cat_cart_size_ok((unsigned long)fi.fsize, 16384)
                                  ? "16K CARTS NOT IN THIS BUILD"
                                  : "NOT A CARTRIDGE IMAGE");
        return false;
    default:
        return false;
    }
#ifdef COCO3
    // PIZERO-261: a game set to the other machine needs that machine's ROM.
    // Without it, say which and stay in the list, rather than run the game on
    // the wrong machine (machine_available logs the reason on serial too).
    bool want = machine_wanted(path);
    if (want != IS_COCO3() && !machine_available(want)) {
        snprintf(msg, msg_sz, "NEEDS %s IN /COCO/ROMS", want ? "COCO3.ROM" : "BAS12.ROM");
        return false;
    }
#endif
    g_launch_kind = kind;
    strcpy(g_launch_path, path);
    return true;
}

// The first runnable program on drive 0, as the command that starts it.
static bool disk_run_command(char *out, size_t out_sz) {
    uint8_t sec[256];
    char name[9];
    int type = 0;
    for (int s = RSDOS_DIR_FIRST_SEC; s <= RSDOS_DIR_LAST_SEC; s++) {
        if (coco_boot_disk_read_sector(0, RSDOS_DIR_TRACK, (unsigned)s, sec) != 0) return false;
        int r = rsdos_scan_sector(sec, name, &type);
        if (r < 0) return false;
        if (r > 0) return rsdos_run_command(name, type, out, out_sz) > 0;
    }
    return false;
}

static void perform_launch(void) {
    int kind = g_launch_kind;
    g_launch_kind = -1;
#ifdef COCO3
    // PIZERO-135: a game for the other machine restarts the board as it.
    bool want = machine_wanted(g_launch_path);
    if (want != IS_COCO3() && machine_available(want))
        switch_machine(want, HO_LAUNCH, kind, g_launch_path);   // PIZERO-314: returns as it
    if (want != IS_COCO3()) {
        Serial.printf("[machine] %s wants the %s, which the card cannot start: running it here\r\n",
                      g_launch_path, want ? "CoCo 3" : "CoCo 2");
    }
#endif
    g_autotype = nullptr;                         // drop anything still queued
    g_bin_settle = 0;
    Serial.printf("[launch] %s %s\r\n",
                  kind == CAT_DSK ? "disk" : kind == CAT_BIN ? "program" : "cart",
                  g_launch_path);
    switch (kind) {
    case CAT_DSK: {
        if (!coco_boot_mount_drive(0, g_launch_path)) return;
        char rom[80];
        if (!coco_boot_resolve_cart("disk11.rom", rom, sizeof rom) || !install_cart_file(rom)) return;
        coco_machine_install_disk_reader(coco_boot_disk_read_sector);
        coco_machine_install_disk_writer(coco_boot_disk_write_sector);   // PIZERO-66
        coco_machine_install_disk_idle(coco_boot_flush_drives);
        settings_for_game(g_launch_path);         // PIZERO-154
        coco_machine_cold_reset();
        // PIZERO-182: a boot like any other, so autorun decides: on, type
        // the disk's first program once BASIC is up (a Space or BREAK tap
        // before then cancels it); off, the BASIC prompt with the disk in.
        g_autorun_cancel = false;
        if (!g_settings.autorun) {
            Serial.print("[launch] autorun = off: BASIC prompt\r\n");
        } else if (disk_run_command(g_launch_cmd, sizeof g_launch_cmd)) {
            g_autotype = g_launch_cmd;
            g_autotype_warmup = 180;              // same wait as autorun.txt
            g_autotype_autorun = true;
            Serial.printf("[launch] will type %s\n", g_launch_cmd);
        } else {
            Serial.print("[launch] no program on the disk: BASIC prompt\r\n");
        }
        break;
    }
    case CAT_BIN:
        coco_machine_install_cart(nullptr);
        settings_for_game(g_launch_path);         // PIZERO-154
        coco_machine_cold_reset();
        g_bin_settle = 30;                        // as at power-on: PIA DDRs settle
        break;
    case CAT_CART:
        if (!install_cart_file(g_launch_path)) return;
        settings_for_game(g_launch_path);         // PIZERO-154
        coco_machine_cold_reset();
        break;
    }
}

// A pending BIN, once BASIC has had its settling frames.
static void bin_settle_tick(void) {
    if (g_bin_settle <= 0 || --g_bin_settle > 0) return;
    uint16_t entry = 0;
    if (coco_boot_parse_loadm(g_launch_path, loadm_write_cb, nullptr, &entry)) {
        Serial.printf("[launch] jump $%04X\r\n", entry);
        coco_machine_jump(entry);
    }
}

// PIZERO-203: why the chip last reset, so a crash (a fault handler's
// software reset), a supply dip and a real power-on are told apart.
static const char *reset_reason_name(void) {
    switch (rp2040.getResetReason()) {
    case RP2040::PWRON_RESET:    return "power-on";
    case RP2040::RUN_PIN_RESET:  return "run-pin";
    case RP2040::SOFT_RESET:     return "software";
    case RP2040::WDT_RESET:      return "watchdog";
    case RP2040::DEBUG_RESET:    return "debugger";
    case RP2040::GLITCH_RESET:   return "glitch";
    case RP2040::BROWNOUT_RESET: return "brownout";
    default:                     return "unknown";
    }
}

// PIZERO-287: the MIDI Pak's other end. The native USB-C port is a USB MIDI
// device beside the serial console, so the computer it is plugged into sees
// a MIDI port; bytes the CoCo writes to the pak go out on it, and bytes in
// land in the pak at MIDI's rate (coco_machine.cpp does the timing).
static Adafruit_USBD_MIDI g_usb_midi;
static uint32_t g_midi_out, g_midi_in;     // bytes moved, for the [midi] log line
static uint32_t g_midi_out_lost;           // packets USB had no room for

// The core starts USB before setup(), and TinyUSBDevice.begin() clears any
// interface added earlier (so initVariant() is too early). So it is added at
// the top of setup() and the board then ALWAYS drops off the bus and rejoins,
// so the host enumerates the finished configuration from scratch. The
// library's examples rejoin only if mounted(); setup() runs before the host
// has finished enumerating, so that skipped the rejoin and changed the
// configuration under the host's feet, which left the Mac with a device and
// no interfaces at all, no serial console and no flashing without the BOOT
// button (2026-11-01).
static void midi_usb_begin(void) {
    // The name the computer shows for the board (and so for its MIDI port);
    // the board definition's own is "RP2350 PiZero". Set before the rejoin
    // below, so the host reads it fresh.
    TinyUSBDevice.setProductDescriptor("CoCo Zero RP2350");
    g_usb_midi.setStringDescriptor("CoCo Zero MIDI Pak");
    g_usb_midi.begin();
    TinyUSBDevice.detach();
    delay(50);
    TinyUSBDevice.attach();
}

// Move bytes both ways. Called between the slices of a frame while software
// has the pak set up, and once a frame otherwise.
static void midi_pump(void) {
    if (!TinyUSBDevice.mounted()) {
        uint8_t b;
        while (coco_machine_midi_tx_pop(&b)) {}      // nobody to hear them
        return;
    }
    // Our own packing both ways (midi_usb_pack.h), straight on TinyUSB's
    // packet calls: the library's stream parser garbled a message after an
    // unfinished one, and the fewer third-party layers the better.
    static struct midi_pack pack = { 0, 0, 0, { 0, 0, 0 }, false };
    uint8_t b, pk[4];
    while (coco_machine_midi_tx_pop(&b)) {
        g_midi_out++;
        if (midi_pack_byte(&pack, b, pk) && !tud_midi_packet_write(pk)) g_midi_out_lost++;
    }
    while (tud_midi_packet_read(pk)) {
        uint8_t m[3];
        int n = midi_unpack(pk, m);
        for (int i = 0; i < n; i++) { coco_machine_midi_rx_push(m[i]); g_midi_in++; }
    }
    TinyUSB_Device_Task();
}

// PIZERO-287: a frame in slices while the pak is in use, so a byte the CoCo
// writes leaves within about 2 ms instead of in 16 ms bursts.
#define MIDI_SLICES 8
static void run_frame(uint32_t cycles) {
    if (!coco_machine_midi_active()) {
        coco_machine_run_cycles(cycles);
        midi_pump();
        return;
    }
    uint32_t done = 0;
    for (int i = 0; i < MIDI_SLICES; i++) {
        uint32_t part = (cycles * (uint32_t)(i + 1)) / MIDI_SLICES - done;
        coco_machine_run_cycles(part);
        done += part;
        midi_pump();
    }
}

// PIZERO-315: boot time, phase by phase, in ms since reset. Each mark is
// when that phase ended. 'basic' is the first frame a text-screen row
// starts with OK; a program started without BASIC never shows one, and the
// boot is logged without it after BT_GIVE_UP_MS. Boot pages wait for a
// person, so the time they were up is kept apart (pages).
enum { BT_START, BT_USB_VIDEO, BT_CARD, BT_MONITOR, BT_ROM, BT_BUILT, BT_SETTINGS,
       BT_CATALOG, BT_LISTED, BT_RUNNING, BT_FRAME, BT_BASIC, BT_N };
static const char *const BT_NAMES[BT_N] = {
    "start", "usb_video", "card", "monitor", "rom", "built", "settings",
    "catalog", "listed", "running", "frame", "basic" };
#define BT_GIVE_UP_MS 30000u
static uint32_t g_bt[BT_N];
static bool     g_bt_host = false;       // a computer answered on USB serial
static bool     g_bt_done = false;
static const char *g_bt_reset = "unknown";   // read in setup(): the watchdog changes it later
static char     g_bt_clock[24];          // when the boot finished, for printing it again
static inline void bt_mark(int i) {
    if (!g_bt[i]) { uint32_t t = (uint32_t)(time_us_64() / 1000u); g_bt[i] = t ? t : 1; }
}

// The OK BASIC prints at the start of a row, as screen codes ('O' 'K', then
// a space or the end of the row), on the 32-column screen at $0400.
static bool bt_basic_ready(void) {
    const uint8_t *scr = coco_machine_peek_ram(0x0400);
    for (int r = 0; r < 16; r++) {
        const uint8_t *row = scr + r * 32;
        if (row[0] == 0x4F && row[1] == 0x4B && row[2] == 0x60) return true;
    }
    return false;
}

// Once per frame until the boot is logged: one serial line, and the same
// line added to /coco/log/boottime.txt.
static int bt_format(char *line, size_t cap);

// PIZERO-319: noinline, so its 300-byte line is on the stack only while it
// runs, not in loop()'s frame under everything loop() calls.
static void __attribute__((noinline)) bt_frame(void) {
    if (g_bt_done) return;
    bt_mark(BT_FRAME);
    uint32_t now = (uint32_t)(time_us_64() / 1000u);
    if (bt_basic_ready()) bt_mark(BT_BASIC);
    else if (now < BT_GIVE_UP_MS) return;
    g_bt_done = true;
    char src[24];
    coco_boot_clock_text(g_bt_clock, sizeof g_bt_clock, src, sizeof src);
    char line[300];
    int n = bt_format(line, sizeof line);
    if (n <= 0) return;
    Serial.printf("[boot] times ms: %s", line);
    if (!g_settings.debug) return;             // PIZERO-316: the card log only when debugging
    FRESULT fr = f_mkdir("0:/coco/log");
    FIL f;
    if ((fr == FR_OK || fr == FR_EXIST) && f_open(&f, "0:/coco/log/boottime.txt", FA_WRITE | FA_OPEN_APPEND) == FR_OK) {
        UINT bw = 0;
        f_write(&f, line, (UINT)n, &bw);
        f_close(&f);
    }
}

// The boot's line, built from the marks (on the stack: nothing kept but
// the marks). Returns its length, or 0 if it did not fit.
static int bt_format(char *line, size_t cap) {
    int n = snprintf(line, cap, "%s fw=%s machine=%s reset=%s host=%s", g_bt_clock, FW_VERSION,
                     IS_COCO3() ? "coco3" : "coco2", g_bt_reset, g_bt_host ? "yes" : "no");
    for (int i = 0; i < BT_N && n > 0 && n < (int)cap; i++) {
        if (g_bt[i]) n += snprintf(line + n, cap - n, " %s=%lu", BT_NAMES[i], (unsigned long)g_bt[i]);
        else n += snprintf(line + n, cap - n, " %s=none", BT_NAMES[i]);
    }
    if (n > 0 && n < (int)cap)
        n += snprintf(line + n, cap - n, " pages=%lu\r\n", (unsigned long)g_bt_pages);
    return (n > 0 && n < (int)cap) ? n : 0;
}

// PIZERO-315: a computer that opens the serial port after the boot is told
// how the boot went. noinline for the same reason as bt_frame.
#define STACK0_BYTES 4096u
static uint32_t g_stack0_deepest = 0, g_stack1_deepest = 0;   // PIZERO-319, kept by stack_watch
static void __attribute__((noinline)) bt_replay(void) {
    static bool host_was = true;
    bool host = (bool)Serial;
    if (host && !host_was && g_bt_done) {
        char line[300];
        if (bt_format(line, sizeof line)) Serial.printf("[boot] times ms: %s", line);
        Serial.printf("[stack] core 0 deepest %lu of %u bytes, core 1 %lu (so far)\r\n",   // PIZERO-319
                      (unsigned long)g_stack0_deepest, STACK0_BYTES, (unsigned long)g_stack1_deepest);
    }
    host_was = host;
}

// PIZERO-319: core 0's stack is 4 KB (__StackLimit of scratch Y, 0x20081000,
// up to __StackTop), and core 1's 2 KB stack sits directly below it, so
// going deeper than 4 KB on core 0 overwrites core 1's interrupt frames and
// core 1 faults. setup() fills the unused part with a pattern; once a
// second the deepest point reached is found, and a new deepest is printed:
// '[stack] core 0 deepest N of 4096 bytes'. Past 3,584 it says so loudly.
extern "C" char __StackTop[];
#define STACK0_PAINT 0x5AC0FFEEu
// Core 1's 2 KB (__StackOneBottom to __StackOneTop) is painted the same
// way before core 1 starts, and watched too.
extern "C" char __StackOneTop[], __StackOneBottom[];
static void stack_paint(void) {
    uint32_t *lo = (uint32_t *)(__StackTop - STACK0_BYTES);
    uint32_t *hi = (uint32_t *)__builtin_frame_address(0) - 64;   // 256 bytes below this frame
    for (uint32_t *p = lo; p < hi; p++) *p = STACK0_PAINT;
}
static void stack1_paint(void) {          // before multicore_launch_core1
    for (uint32_t *p = (uint32_t *)__StackOneBottom; p < (uint32_t *)__StackOneTop; p++) *p = STACK0_PAINT;
}
static uint32_t stack_used(const char *bottom, const char *top) {
    const uint32_t *p = (const uint32_t *)bottom;
    while (p < (const uint32_t *)top && *p == STACK0_PAINT) p++;
    return (uint32_t)(top - (const char *)p);
}
static void __attribute__((noinline)) stack_watch(void) {
    static uint32_t next = 0;
    uint32_t now = millis();
    if ((int32_t)(now - next) < 0) return;
    next = now + 1000;
    uint32_t used = stack_used(__StackTop - STACK0_BYTES, __StackTop);
    uint32_t size1 = (uint32_t)(__StackOneTop - __StackOneBottom);
    uint32_t used1 = stack_used(__StackOneBottom, __StackOneTop);
    if (used > g_stack0_deepest + 32 || used1 > g_stack1_deepest + 32) {
        g_stack0_deepest = used;
        g_stack1_deepest = used1;
        Serial.printf("[stack] core 0 deepest %lu of %u bytes, core 1 %lu of %lu%s\r\n",
                      (unsigned long)used, STACK0_BYTES, (unsigned long)used1, (unsigned long)size1,
                      (used > STACK0_BYTES - 512 || used1 > size1 - 256) ? " *** NEAR THE END ***" : "");
    }
}

// PIZERO-316: with debug = on, wait up to 4 s for a monitor to open the
// serial port, so it sees the whole boot. Either way the held output goes
// out now if a monitor is there, and its buffer is freed.
static void boot_monitor_wait(bool wait) {
    if (wait) {
        uint32_t t0 = millis();
        while (!Serial && (millis() - t0) < 4000) delay(50);
        if (Serial) delay(200);            // a little room after the host attaches
    }
    g_bt_host = (bool)Serial;              // PIZERO-315
    Serial.release();
}

void setup() {
    bt_mark(BT_START);
    stack_paint();                    // PIZERO-319
    midi_usb_begin();                 // PIZERO-287: first, then a clean re-enumeration
    settings_defaults(&g_settings);   // PIZERO-145: in force until settings.txt is read
    Serial.begin(115200);
#if defined(FDC_TRACE) && defined(FDC_TRACE_UART)
    Serial1.begin(115200);            // PIZERO-191: trace on UART0 for the emulator
#endif
    // PIZERO-316: no wait here. What the boot prints is held until a monitor
    // opens the port, or until settings.txt says debug = on and the boot
    // waits for one (boot_monitor_wait, once the card is read).
    Serial.hold(g_coco_rom, sizeof g_coco_rom);   // PIZERO-319: free until the ROMs load
    Serial.print("\r\nXRoar on RP2350-PiZero (PIZERO-09)\r\n");
    // PIZERO-116: RUN is a hardware reset, so the press itself is invisible;
    // POWMAN records it as the cause of THIS reset (HAD_RUN_LOW, read-only,
    // reflects the last reset only). Watchdog reboots, flashes and power-on
    // set other bits. The raw value is printed to check that on hardware.
    uint32_t chip_reset = powman_hw->chip_reset;
    g_run_button_reset = (chip_reset & POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS) != 0;
    Serial.printf("[boot] chip_reset=0x%08lx%s\r\n", (unsigned long)chip_reset,
                  g_run_button_reset ? " (RUN button)" : "");
    {
        // PIZERO-208: which board this is, matching its USB serial number
        // and the board_serial column of qc/qc-log.csv. Early, so it prints
        // with no card in.
        char board[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(board, sizeof board);
        Serial.printf("[boot] board %s\r\n", board);
    }
    Serial.flush();

#ifndef WATCHDOG_DISABLE
    // PIZERO-33: carry the freeze tally across reboots (scratch survives a watchdog
    // reset, not power-on; WD_MAGIC guards validity).
    // A real freeze = a genuine watchdog TIMER timeout (loop() stopped petting).
    // A FORCEd reboot (power-on / manual reset / picotool / flash) is NOT TIMER, so
    // it starts a FRESH session (count back to 0) -- the tally means "freezes since
    // I last started it", and this also auto-clears the self-test's count on deploy.
    // PIZERO-51: a deliberate USB-replug reboot is also a TIMER reset: distinguish
    // it by the scratch[4] sentinel so it isn't logged/counted as a freeze. It
    // carries the running freeze tally across unchanged (not incremented).
    bool usb_replug_reboot = (watchdog_hw->reason & WATCHDOG_REASON_TIMER_BITS)
                             && watchdog_hw->scratch[0] == WD_MAGIC
                             && watchdog_hw->scratch[4] == USB_REPLUG_MAGIC;
    // PIZERO-135: a machine switch is deliberate too, and not a freeze.
    g_switch_restart = (watchdog_hw->reason & WATCHDOG_REASON_TIMER_BITS)
                       && watchdog_hw->scratch[4] == MACHINE_SWITCH_MAGIC;
    if (g_switch_restart) usb_replug_reboot = true;
    watchdog_hw->scratch[4] = 0;   // consume the sentinel

    // PIZERO-98: a UF2 flash also reboots THROUGH THE WATCHDOG, so "timer reset
    // plus a surviving magic" is not enough to call something a freeze. Observed
    // 2026-09-18 right after a flash: "stuck in 'loop-top' after ~268435456
    // frames (~51 days)", which is 0x10000000, i.e. uninitialised scratch
    // reported as a real event. That pollutes soak data, because the analyser
    // counts these messages.
    //
    // So sanity-check the payload as well as the magic: a real freeze carries a
    // plausible frame count (a week at 60 Hz is 36.3 M) and a phase that is
    // actually one of ours.
    bool scratch_sane = watchdog_hw->scratch[2] < 40000000u
                        && watchdog_hw->scratch[1] <= WP_LAST;
    bool freeze_reboot = (watchdog_hw->reason & WATCHDOG_REASON_TIMER_BITS)
                         && watchdog_hw->scratch[0] == WD_MAGIC
                         && scratch_sane
                         && !usb_replug_reboot;
    if ((watchdog_hw->reason & WATCHDOG_REASON_TIMER_BITS)
        && watchdog_hw->scratch[0] == WD_MAGIC && !scratch_sane && !usb_replug_reboot) {
        Serial.printf("[watchdog] scratch invalid after a timer reset "
                      "(frames=%lu phase=%lu) -- treating as a fresh start, not a freeze\r\n",
                      (unsigned long)watchdog_hw->scratch[2],
                      (unsigned long)watchdog_hw->scratch[1]);
        watchdog_hw->scratch[3] = 0;   // drop the carried tally too
    }
    if (usb_replug_reboot) {
        g_freeze_count = watchdog_hw->scratch[3] >> 8;
        if (g_freeze_count > 1000000u) g_freeze_count = 0;
        g_last_freeze_phase = watchdog_hw->scratch[1];
        if (g_switch_restart) Serial.print("[machine] restart to switch machine (PIZERO-135)\r\n");
        else Serial.print("[usb] re-enumeration reboot (device hot-replug, PIZERO-51) -> "
                          "mounting whatever is attached now\r\n");
        Serial.flush();
    } else if (freeze_reboot) {
        g_freeze_count = watchdog_hw->scratch[3] >> 8;
        if (g_freeze_count > 1000000u) g_freeze_count = 0;      // garbage guard
        g_last_freeze_phase = watchdog_hw->scratch[1];
        g_freeze_count++;
        Serial.printf("[watchdog] *** FREEZE RECOVERED *** core 0 was stuck in '%s' after ~%lu frames (~%lus); freeze #%lu this session -> auto-rebooted\r\n",
                      wd_phase_name(g_last_freeze_phase),
                      (unsigned long)watchdog_hw->scratch[2],
                      // PIZERO-98: frames/52 was left over from the off-spec
                      // ~52 Hz build; at 60 Hz it overstates uptime by 15%,
                      // which is enough to make a freeze look like it belongs
                      // to a different session.
                      (unsigned long)(watchdog_hw->scratch[2] / 60u),
                      (unsigned long)g_freeze_count);
        // PIZERO-227: where core 0 really was, and the context around it.
        if (wd_post_describe(g_wd_post_text, sizeof g_wd_post_text))
            Serial.printf("%s\r\n", g_wd_post_text);
        Serial.flush();
    } else {
        g_freeze_count = 0;
        g_last_freeze_phase = WP_NONE;
    }
    watchdog_hw->scratch[0] = WD_MAGIC;
    watchdog_hw->scratch[3] = (g_freeze_count << 8) | (g_last_freeze_phase & 0xFF);
    wd_phase(WP_SETUP);
    wd_heartbeat(0);
    wd_post_arm();                           // PIZERO-227
#endif

    // Bring up DVI first so we have a display even if SD/ROM fails.
    // Clear both buffers so the (static) black border is set in each.
    memset(g_fb, 0, sizeof(g_fb));
#ifdef VIDEO_640
    for (int i = 0; i < 64; i++) g_pal565[i] = coco_pal_gime_to_rgb565((uint8_t)i);
#endif
#if defined(AV_60HZ) || defined(AV_STD_TIMING) || defined(AV_60HZ_TEST)
    vreg_set_voltage(VREG_VOLTAGE_1_25);   // extra headroom for 252 MHz
#else
    vreg_set_voltage(VREG_VOLTAGE_1_20);
#endif
    delay(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);        // 240 MHz (PIZERO-02b) / 252 MHz std-timing

    // Read RP2350 chip revision: relevant to errata E9 (pull-down input
    // reads HIGH due to leak current). Pico-PIO-USB applies a workaround
    // only when chip_version <= 2 (A0..A2). Print so we know our part.
    uint32_t chip_id = *((volatile uint32_t *)(SYSINFO_BASE + SYSINFO_CHIP_ID_OFFSET));
    uint32_t chip_rev = (chip_id & SYSINFO_CHIP_ID_REVISION_BITS) >>
                        SYSINFO_CHIP_ID_REVISION_LSB;
    Serial.printf("RP2350 chip_id=%08lx revision=%lu (E9 workaround active: %s)\r\n",
                  (unsigned long)chip_id, (unsigned long)chip_rev,
                  chip_rev <= 2 ? "yes" : "NO, may explain pull-down-reads-HIGH");
    Serial.flush();


    // PIZERO-11: PIO-USB host on core 0, PIO 1 (libdvi will own PIO 0).
    // Init BEFORE multicore_launch_core1: alarm_pool_create() needs
    // cross-core sync that deadlocks if core 1 is already in libdvi's
    // DMA-IRQ loop.
#ifndef AV_STD_TIMING   // PIO-USB asserts CPU==120||240MHz; skip at 252 MHz std-timing test
    {
        pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
        pio_cfg.pin_dp     = HOST_PIN_DP;
        pio_cfg.pio_tx_num = 1;
        pio_cfg.pio_rx_num = 1;
        USBHost.configure_pio_usb(1, &pio_cfg);
        // Default any HID interface to boot protocol at mount, that's how the
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
#ifdef AV_DATA_ISLAND
    // M2 (PIZERO-28): inject one benign NULL data island into every vertical-
    // blanking (no-sync) line. Off-screen + identical pixel count, so a
    // malformed island cannot disturb the picture. Acceptance test: video stays
    // rock-solid on the monitor (proves data-island injection doesn't break sync
    // on this board's wiring/timing). Must run before core 1 starts the DVI.
    {
        dvi_di_init();
#ifdef AV_ENCODE_BENCH
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
#if defined(AV_AUDIO_SWAPTEST) || defined(AV_AUDIO_STATIC)
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
#if defined(AV_AUDIO_SWAPTEST)
        for (int b = 0; b < 2; ++b)
            dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false,
                                                 &dvi0.dma_list_vblank_nosync, pkts, 6,
                                                 g_isl2[b][0], g_isl2[b][1], g_isl2[b][2]);
        dvi0.vblank_callback = swaptest_vblank_cb;
        dvi_setup_active_hdmi_framing(&DVI_TIMING, dvi0.dma_cfg, &dvi0.dma_list_active, bp0, bk1, bk2);
        Serial.print("[disp] M0 swaptest: per-line read_addr ping-pong\r\n");
#elif defined(AV_AUDIO_STATIC)
        static uint32_t isl0[320], isl1[320], isl2[320];
        dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false,
                                             &dvi0.dma_list_vblank_nosync, pkts, 6, isl0, isl1, isl2);
        dvi_setup_active_hdmi_framing(&DVI_TIMING, dvi0.dma_cfg, &dvi0.dma_list_active, bp0, bk1, bk2);
        Serial.print("[disp] M4 step2a: static test tone in vblank\r\n");
#else
        // M2 final: vblank carries AVI/AudioIF/ACR; live audio is on ACTIVE lines.
        dvi_data_packet_t ipk[3];
#ifndef AV_AVI_VIC
#define AV_AVI_VIC 0   // PIZERO-234: Video Identification Code; 1 = CEA 640x480p60
#endif
        dvi_di_set_avi_infoframe(&ipk[0], AV_AVI_VIC);
#ifdef AV_AIF_REFER_STREAM
        // PIZERO-234: CEA-861 wants SF and SS 0 (refer to stream header) for
        // L-PCM over HDMI; the channel status carries 48 kHz and 16 bits.
        dvi_di_set_audio_infoframe(&ipk[1], 1 /*2ch*/, 0, 0);
#else
        dvi_di_set_audio_infoframe(&ipk[1], 1 /*2ch*/, DVI_AUDIO_SF_48K, DVI_AUDIO_SS_16);
#endif
#if defined(AV_STD_TIMING) || defined(AV_60HZ)
        dvi_di_set_acr(&ipk[2], 25200, 6144);    // in-spec 25.2 MHz pixel -> CTS=25200 (STD-TIMING / PIZERO-45 60Hz)
#else
        dvi_di_set_acr(&ipk[2], AV_ACR_CTS, 6144);   // PIZERO-32: monitor-tunable (default 25176)
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
#ifdef AV_STREAM_AUDIO
        // PIZERO-38: lay framing + a silence island into each rotating-pool slot
        // (the per-line IRQ rewrites only the island words). Pool-init repoints
        // dma_list transiently; framing last leaves the no-island default until
        // stream_audio_cb takes over on the first active line.
        for (int w = 0; w < APOOL; ++w)
            dvi_setup_active_audio_line(&DVI_TIMING, dvi0.dma_cfg, &dvi0.dma_list_active,
                                        g_pool0[w], g_pool1[w], g_pool2[w], sil, 1, true);
        dvi_setup_active_hdmi_framing(&DVI_TIMING, dvi0.dma_cfg, &dvi0.dma_list_active, g_bp0, g_bk1, g_bk2);
#ifdef AV_VBLANK_AUDIO
        // PIZERO-232: lay each vblank pool slot as a one-island active block,
        // the same layout as the info island; the IRQ rewrites the island words.
        for (int w = 0; w < VPOOL; ++w)
            dvi_setup_scanline_for_vblank_island(&DVI_TIMING, dvi0.dma_cfg, false, &dvi0.dma_list_vblank_nosync,
                                                 sil, 1, g_vpool0[w], g_vpool1[w], g_vpool2[w]);
        Serial.printf("[disp] PIZERO-232: audio on vblank lines too, info every %d\r\n", AV_VBLANK_INFO_EVERY);
#endif
        // AUDIO_SAMPLES_PER_FRAME spread over the STREAM_METER_LINES lines.
        g_meter_step = AUDIO_SAMPLES_PER_FRAME;   // PIZERO-223: exact, see g_meter_acc
#ifdef AV_AUDIO_SYNTH
        for (int i = 0; i < SYNTH_TBL; ++i)
            g_sine[i] = (int16_t)(6000.0f * sinf((float)i * (2.0f * 3.14159265f / SYNTH_TBL)));   // ~18% FS
        g_synth_inc = (uint32_t)((double)440.0 / 48000.0 * 4294967296.0);   // 440 Hz @ 48 kHz, 32-bit phase
#ifdef AV_AUDIO_SYNTH_CHORD
        { static const double hz[3] = { 261.63, 329.63, 392.00 };   // C4 E4 G4
          for (int v = 0; v < 3; ++v) g_chord_inc[v] = (uint32_t)(hz[v] / 48000.0 * 4294967296.0); }
#endif
        Serial.print("[disp] M2 STREAM: 440 Hz synth, per-line IRQ encode (PIZERO-38)\r\n");
#else
        Serial.print("[disp] M2 STREAM: live per-line IRQ encode (PIZERO-38)\r\n");
#endif
        dvi0.active_line_callback = stream_audio_cb;
#else
#ifdef AV_EVEN_AUDIO
        // Capture the control-only back-porch buffers the base vblank setup left
        // (dvi_init set these; the info-island setup above only touched the active
        // block), so non-audio vblank lines can restore them in the IRQ.
        g_vbp_dflt0 = dvi0.dma_list_vblank_nosync.l0[2].read_addr;
        g_vbp_dflt1 = dvi0.dma_list_vblank_nosync.l1[1].read_addr;
        g_vbp_dflt2 = dvi0.dma_list_vblank_nosync.l2[1].read_addr;
#endif
        for (int b = 0; b < 2; ++b)
            for (int line = 0; line < N_ALINES; ++line) {
#ifdef AV_EVEN_AUDIO
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
#ifdef AV_EVEN_AUDIO
        // Spread N_ALINES audio lines across ALL 523 lines (active 0-479, fp/bp).
        for (int i = 0; i < 480; ++i) g_aslot[i]  = -1;
        for (int i = 0; i < 43;  ++i) g_vaslot[i] = -1;
        for (int j = 0; j < N_ALINES; ++j) {
            const int gi = (int)((long)j * 523 / N_ALINES);
            if (gi < 480) g_aslot[gi] = (int16_t)j; else g_vaslot[gi - 480] = (int16_t)j;
        }
        Serial.print("[disp] M2: EVEN audio across active+vblank (PIZERO-34)\r\n");
#else
        // Spread N_ALINES audio lines evenly across the 480 active scanlines.
        for (int i = 0; i < 480; ++i) g_aslot[i] = -1;
        for (int j = 0; j < N_ALINES; ++j) g_aslot[(j * 480) / N_ALINES] = (int16_t)j;
        Serial.print("[disp] M2: active-line audio, ~924 samp/frame @ 51.95Hz\r\n");
#endif
        dvi0.active_line_callback = active_audio_cb;
#endif // AV_STREAM_AUDIO
#endif
    }
#endif
    dvi_sio_tmds_encode = g_settings.video_hw_encode;   // PIZERO-175: the default until settings.txt
    stack1_paint();                    // PIZERO-319
    multicore_launch_core1(core1_main);
    Serial.printf("DVI up: %dx%d -> 640x480 ~57Hz, sys=%lu kHz (PIZERO-02b)\r\n",
                  FRAME_WIDTH, FRAME_HEIGHT, (unsigned long)(clock_get_hz(clk_sys) / 1000));
    Serial.flush();
#ifdef VIDEO_BENCH
    video_bench_run();                     // PIZERO-175: core 1 is scanning out now
#endif
    bt_mark(BT_USB_VIDEO);                 // PIZERO-315

    if (!mount_sd()) {
        boot_monitor_wait(true);               // PIZERO-316: no card to say; someone is looking
        Serial.print("SD mount FAILED: no ROMs, idle.\r\n");
        boot_page(MSG_NOSD_TITLE, MSG_NOSD_BODY, MSG_NOSD_DETAIL);
        return;
    }
    coco_boot_clock_init();                    // PIZERO-201: before anything writes a file
    bt_mark(BT_CARD);                          // PIZERO-315
    {
        // PIZERO-135: which machine, from settings.txt, before its ROMs load;
        // PIZERO-316: and whether to wait for a monitor. The full settings
        // are read again once the machine exists.
        static struct coco_settings pre;
        coco_boot_load_settings(&pre);
        g_settings.debug = pre.debug;
        boot_monitor_wait(pre.debug);
        bt_mark(BT_MONITOR);
#ifdef COCO3
        g_coco3_mode = settings_is_coco3(&pre);
#endif
    }
#ifdef COCO3
    // A switch (switch_machine) chose the machine and what to do next.
    handoff_read();
    if (g_boot_handoff.valid) {
        g_coco3_mode = g_boot_handoff.coco3;
        g_run_button_reset = false;            // the reset can read as RUN; it was us
        Serial.printf("[machine] restarted to switch: %s, then %s %s\r\n",
                      g_coco3_mode ? "CoCo 3" : "CoCo 2",
                      g_boot_handoff.what == HO_LAUNCH ? "launch" : g_boot_handoff.what == HO_BASIC ? "BASIC" : "boot",
                      g_boot_handoff.path);
    }
    // PIZERO-133: a CoCo 3 runs Super Extended Color BASIC only. Without a
    // good coco3.rom the page says so, and the board carries on as a CoCo 2.
    if (g_coco3_mode && !coco_boot_load_coco3_rom(g_coco_rom)) {
        const struct coco_rom_status *rs = coco_boot_rom_status();
        static char detail[96];
        if (rs->c3_found) {
            snprintf(detail, sizeof detail,
                     "FOUND COCO3.ROM BUT IT IS %lu BYTES. IT MUST BE 32768.",
                     (unsigned long)rs->c3_bytes);
            boot_page(MSG_BADROM_TITLE, MSG_BADROM3_BODY, detail);
        } else {
            boot_page(MSG_NOROM_TITLE, MSG_NOROM3_BODY, MSG_NOROM_DETAIL);
        }
        boot_wait(6000);                       // a key skips it (PIZERO-152)
        g_coco3_mode = false;
    }
    Serial.printf("[boot] machine: %s\r\n", g_coco3_mode ? "CoCo 3" : "CoCo 2");
#endif
    if (!IS_COCO3() && !coco_boot_load_rom_from_sd(g_coco_rom)) {
        Serial.print("ROM load FAILED (need /coco/roms/bas12.rom [+extbas11.rom])\r\n");
        // A present-but-wrong-size ROM is NOT the same problem as a missing
        // one, and saying "NO ROM FOUND" over a file sitting right there sends
        // someone hunting for what they already have. The resolver prefers
        // /coco/roms/ and falls back to flat /coco/, so quote the preferred one.
        const struct coco_rom_status *rs = coco_boot_rom_status();
        static char detail[96];
        if (rs->bas_found) {
            snprintf(detail, sizeof detail,
                     "FOUND BAS12.ROM BUT IT IS %lu BYTES. IT MUST BE 8192.",
                     (unsigned long)rs->bas_bytes);
            boot_page(MSG_BADROM_TITLE, MSG_BADROM_BODY, detail);
        } else {
            boot_page(MSG_NOROM_TITLE, MSG_NOROM_BODY, MSG_NOROM_DETAIL);
        }
        return;
    }
    bt_mark(BT_ROM);                           // PIZERO-315
    Serial.printf("[mem] free heap %d bytes before the machine\r\n", rp2040.getFreeHeap());
    if (!coco_machine_init(g_coco_rom, IS_COCO3() ? 32768 : 16384)) {
        // PIZERO-262: the CoCo 3 takes 64 KB of its RAM from the heap.
        Serial.printf("coco_machine_init failed: free heap %d bytes now; a CoCo 3 needs 65536 and a few KB more\r\n",
                      rp2040.getFreeHeap());
        // Deliberately different wording from the missing-ROM case: this one
        // is a real fault, and confusing the two would send someone hunting
        // for a file that is already there.
        boot_page(MSG_INIT_TITLE, MSG_INIT_BODY, MSG_INIT_DETAIL);
        return;
    }
    if (IS_COCO3()) {
        // PIZERO-133: the GIME draws straight into the one framebuffer.
        coco_machine_set_framebuffer(g_fb);
        Serial.printf("[mem] free heap %d bytes with the CoCo 3's 128 KB\r\n", rp2040.getFreeHeap());
    }

    // PIZERO-145: settings from the card, now that the machine exists to
    // take them and before AUTORUN, which reset_button governs.
    coco_boot_recover_text("0:/coco/settings.txt");   // PIZERO-146: finish a cut-off save
    bt_mark(BT_BUILT);                         // PIZERO-315
    if (!coco_boot_load_settings(&g_settings))
        Serial.print("[settings] no /coco/settings.txt: defaults\r\n");
    settings_apply();
    bt_mark(BT_SETTINGS);
    g_bt_reset = reset_reason_name();          // PIZERO-315: before loop() starts the watchdog
    Serial.printf("[boot] reset reason: %s\r\n", g_bt_reset);   // PIZERO-203
    soak_log_note_board(FW_VERSION);           // PIZERO-208
    if (g_settings.soak_log)                 // PIZERO-201
        soak_log_begin(FW_VERSION, FW_ENV, watchdog_caused_reboot(), g_freeze_count,
                       g_freeze_count ? wd_phase_name(g_last_freeze_phase) : "none",
                       reset_reason_name(), g_wd_post_text);

    // PIZERO-92: a missing extbas11.rom is NOT fatal, it just silently costs
    // Extended and Disk BASIC. Someone who notices DISK commands failing has
    // no way to connect that to a file they never copied, so say it once, on
    // the screen, before handing over to the machine.
    if (!IS_COCO3()) {                   // PIZERO-133: coco3.rom is the whole BASIC
        const struct coco_rom_status *rs = coco_boot_rom_status();
        if (rs->ecb_bytes != 8192) {
            boot_page(MSG_CBONLY_TITLE, MSG_CBONLY_BODY, MSG_CBONLY_DETAIL);
            boot_wait(4000);      // long enough to read; a key skips it (PIZERO-152)
        }
    }

    // PIZERO-81a: the disk catalogue the F12 overlay lists, dumped once so it
    // can be checked over serial before anything is drawn.
    {
        int n = coco_boot_rescan(CAT_DSK);
        bt_mark(BT_CATALOG);                   // PIZERO-315
        const struct dsk_catalog *cat = coco_boot_dsk_catalog();
        Serial.printf("[dsk] %d image(s)", n);
        if (cat->skipped_long) Serial.printf(", %d skipped (name too long)", cat->skipped_long);
        if (cat->skipped_full) Serial.printf(", %d skipped (over %d)", cat->skipped_full, DSK_CAT_MAX);
        Serial.print("\r\n");
        char p[96];
        for (int i = 0; i < n; i++)
            if (dsk_cat_path(cat, i, p, sizeof p)) Serial.printf("[dsk]   %s\r\n", p);
        bt_mark(BT_LISTED);
    }

    // Boot strategy from /coco/autorun.txt (see AUTORUN.md); default = Disk BASIC.
    static struct coco_autorun autorun = {};
    static char path[80];
#if defined(AUDIO_WAV_DUMP) && !defined(AUDIO_WAV_DUMP_BOOT)
    // Validation build: ignore autorun and boot a clean BASIC OK prompt so the
    // injected SOUND test program actually runs. (The SD autorun.txt otherwise
    // DIRECT-loads a game, e.g. spacewarp.bin, and our keystrokes go nowhere.)
    (void)autorun; (void)path;
#else
    coco_boot_recover_text("0:/coco/autorun.txt");    // PIZERO-147: finish a cut-off save

    // PIZERO-182: autorun is one boot-time feature. On, this boot reads
    // autorun.txt and, once BASIC is up, runs drive 0's program (or the
    // typed lines). Off, autorun.txt is not even read: Disk BASIC, the
    // default disk in drive 0, nothing typed. A RUN press with
    // reset_button = basic turns it off for that boot only. A Space or BREAK
    // tap before the typing starts cancels it (hid_keyboard_apply).
    bool autorun_on = g_settings.autorun && !(g_run_button_reset && g_settings.reset_to_basic);
    if (g_boot_handoff.what != HO_NORMAL) autorun_on = false;   // PIZERO-135: a launch or BASIC
    bool have_autorun = false;
    if (autorun_on) have_autorun = coco_boot_load_autorun(&autorun);
    else Serial.printf("[autorun] off for this boot (%s): autorun.txt not read\r\n",
                       g_settings.autorun ? "RUN button, reset_button = basic" : "autorun = off");

    // PIZERO-152: a name autorun.txt gives that is not on the card is
    // reported (serial, and a boot page for the first one), and boot carries
    // on as if that line were not there: Disk BASIC for a missing @CART, a
    // normal boot for a missing @DIRECT.
    const char *miss_kind = nullptr, *miss_name = nullptr;
    auto missing = [&](const char *kind, const char *name) {
        Serial.printf("[autorun] %s %s: not found on the card, skipped\r\n", kind, name);
        if (!miss_kind) { miss_kind = kind; miss_name = name; }
    };

    // PIZERO-154: what autorun starts, for its own settings file: the
    // @DIRECT program, else a named cartridge, else drive 0's disk.
    static char game[96];
    game[0] = '\0';

    bool direct = false;
    if (have_autorun && autorun.direct_name[0]) {
        direct = coco_boot_resolve("bin", autorun.direct_name, path, sizeof(path));
        if (!direct) missing("@DIRECT", autorun.direct_name);
    }
    if (direct) {
        // PIZERO-174: typed lines cannot be typed with no BASIC to take them.
        if (autorun.autotype[0])
            Serial.print("[autorun] @DIRECT: the typed lines in autorun.txt are ignored\r\n");
        snprintf(game, sizeof game, "%s", path);
        for (int i = 0; i < 30; i++) coco_machine_run_cycles(15000);  // let PIA DDRs settle
        uint16_t entry = 0;
        if (coco_boot_parse_loadm(path, loadm_write_cb, nullptr, &entry)) {
            Serial.printf("[autorun] DIRECT %s, jump $%04X\r\n", path, entry);
            coco_machine_jump(entry);
        }
    } else {
        char cart_path[80];
        bool cart_ok = false;
        if (have_autorun && autorun.cart_name[0]) {
            cart_ok = coco_boot_resolve_cart(autorun.cart_name, cart_path, sizeof cart_path)
                      && install_cart_file(cart_path);
            if (!cart_ok) missing("@CART", autorun.cart_name);
            else snprintf(game, sizeof game, "%s", cart_path);
        }
        if (!cart_ok)
            cart_ok = coco_boot_resolve_cart("disk11.rom", cart_path, sizeof cart_path)
                      && install_cart_file(cart_path);
        if (cart_ok) {
            // PIZERO-183: the drives as they were at power-off; on a card
            // with nothing remembered, the first disk alphabetically.
            bool dsk = coco_boot_restore_drives(path, sizeof path);
            if (!dsk)
                dsk = coco_boot_find_default_dsk(path, sizeof(path)) && coco_boot_attach_dsk(path);
            // PIZERO-114: install the reader even with nothing mounted, so
            // the four drives work when filled later from the F12 overlay;
            // an empty drive reports NOT READY rather than having no FDC.
            coco_machine_install_disk_reader(coco_boot_disk_read_sector);
            coco_machine_install_disk_writer(coco_boot_disk_write_sector);   // PIZERO-66
            coco_machine_install_disk_idle(coco_boot_flush_drives);
            if (dsk) Serial.printf("[autorun] disk: %s\r\n", path);
            if (have_autorun && autorun.autotype[0]) {
                g_autotype = autorun.autotype;
                g_autotype_warmup = 180;
                g_autotype_autorun = true;
            } else if (autorun_on && dsk && disk_run_command(g_launch_cmd, sizeof g_launch_cmd)) {
                // PIZERO-152/182: nothing typed in autorun.txt (or no file):
                // run drive 0's first program, named disk or default disk,
                // as the F12 overlay's ENTER does.
                g_autotype = g_launch_cmd;
                g_autotype_warmup = 180;
                g_autotype_autorun = true;
                if (!game[0]) snprintf(game, sizeof game, "%s", path);   // its own settings, as ENTER
                Serial.printf("[autorun] will type %s\n", g_launch_cmd);
            }
        }
    }
    if (game[0]) settings_for_game(game);    // PIZERO-154: layered on settings.txt
#ifdef COCO3
    // PIZERO-135: the game autorun starts asks for the other machine.
    if (game[0] && settings_is_coco3(&g_settings) != IS_COCO3() && machine_available(settings_is_coco3(&g_settings)))
        switch_machine(settings_is_coco3(&g_settings), HO_NORMAL, -1, nullptr);
#endif
    if (miss_kind) {
        char body[160];
        snprintf(body, sizeof body, MSG_ARMISS_BODY, miss_kind, miss_name);
        boot_page(MSG_ARMISS_TITLE, body, MSG_ARMISS_DETAIL);
        boot_wait(30000);         // until a key; boot then carries on
    }
#endif // AUDIO_WAV_DUMP && !AUDIO_WAV_DUMP_BOOT
    disk_overlay_init(present_card, overlay_launch_request);   // PIZERO-81: F12 overlay
    disk_overlay_set_info(info_lines);                         // PIZERO-156: F1 INFO
    text_editor_init(present_card, on_file_saved);             // PIZERO-146
    g_kt.coco3 = coco_machine_is_coco3();       // PIZERO-215: CTRL, ALT, F1, F2
    if (g_boot_handoff.what == HO_LAUNCH) {      // PIZERO-135: what the switch was for
        g_launch_kind = g_boot_handoff.kind;
        snprintf(g_launch_path, sizeof g_launch_path, "%s", g_boot_handoff.path);
    }
    g_machine_running = true;
    bt_mark(BT_RUNNING);                         // PIZERO-315
    Serial.print("[main] coco_machine running\r\n");
    Serial.printf("[mem] free heap %d bytes (bank-switched carts need their size + %d)\r\n",
                  rp2040.getFreeHeap(), CART_HEAP_MARGIN);
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
    if (!wd_armed) { wd_armed = true; watchdog_enable(WATCHDOG_TIMEOUT_MS, true); wd_post_watchdog_started(); }
    watchdog_update();
    static uint32_t hb = 0; wd_heartbeat(++hb);
#ifdef WATCHDOG_SELFTEST
    // PIZERO-33 validation: after ~10s, deliberately wedge core 0 in a known phase.
    // Expect: ~3s later the watchdog reboots and the banner reports stuck in 'emulate'.
    // PIZERO-227: each freeze moves to the next kind, so one flash shows every
    // post-mortem: 0 spin in loop(), 1 spin with interrupts off, 2 spin inside
    // a timer interrupt, 3 a hard fault on core 0.
    if (hb == 520) { wd_phase(WP_EMU);
        unsigned kind = g_freeze_count % 4;
        static const char *what[4] = { "spin in loop()", "spin with interrupts off",
                                       "spin inside a timer interrupt", "hard fault" };
        Serial.printf("[selftest] forcing a core-0 %s in phase 'emulate' (watchdog should reboot in ~3s)...\r\n",
                      what[kind]);
        Serial.flush();
        if (kind == 1) __asm volatile ("cpsid i");
        if (kind == 2) add_alarm_in_ms(1, [](alarm_id_t, void *) -> int64_t {
                           for (;;) tight_loop_contents(); return 0; }, nullptr, true);
        if (kind == 3) __asm volatile ("udf #0");
        for (;;) tight_loop_contents();
    }
#endif
#endif
    wd_phase(WP_USB);
    USBHost.task();   // PIZERO-11: service USB host transfers (prime freeze suspect).
    wd_phase(WP_LOOP);

#ifdef USB_HOTPLUG_RECOVER
    // PIZERO-51: a phantom flood (see hotplug_note_report) flagged that a device
    // was unplugged. RECOVERY = REBOOT into the cold-boot enumeration path.
    //
    // Why reboot rather than re-enumerate in place (learned the hard way on HW,
    // 2026-07-22): on this rev3 board the USB line is a pathological liar, the SM
    // pins D+/D- at J/FS whether a device is present OR absent (E9 leak), so pio-usb
    // never sees SE0 and the tuh connect-check cannot tell "empty" from "present".
    //   - stop()/restart() only cycles the PIO timer; tuh never re-enumerates.
    //   - Forcing the pio-usb disconnect branch clears `connected`, but the very
    //     next connect-check re-latches it from the phantom J line -> tuh tries to
    //     enumerate a non-existent device, fails, and WEDGES (connected=1, usb=0,
    //     no polling, unrecoverable), confirmed by a 30 s flat capture.
    //   - A clean full re-init is impossible: the pio-usb HCD implements no
    //     hcd_deinit and pio-usb has no host_deinit (a 2nd init re-claims PIO SMs).
    // The ONLY proven enumeration path on this board is a cold boot. A watchdog
    // reboot re-runs it and mounts whatever is plugged in at that moment (~2 s,
    // auto), which is strictly better than today's manual power-cycle. Cost: the
    // running emulator state is lost on a device swap. Gated behind USB_HOTPLUG_RECOVER.
    if (g_usb_recover_pending) {
        g_usb_recover_pending = false;
        Serial.printf("[usb] phantom flood (>=%d identical HID reports [%02x %02x %02x %02x "
                      "%02x %02x %02x %02x]) -> device unplugged; rebooting to re-enumerate\r\n",
                      USB_PHANTOM_FLOOD_N,
                      g_hp_last[0], g_hp_last[1], g_hp_last[2], g_hp_last[3],
                      g_hp_last[4], g_hp_last[5], g_hp_last[6], g_hp_last[7]);
        Serial.flush();
        watchdog_reboot(0, 0, 30);                     // reboot in ~30 ms (lets serial drain)
#ifndef WATCHDOG_DISABLE
        // PIZERO-135: after the call, which clears scratch[4] for a plain reboot.
        watchdog_hw->scratch[4] = USB_REPLUG_MAGIC;   // tag: deliberate reboot, NOT a freeze
#endif
        for (;;) tight_loop_contents();                // wait for the reset
    }
#endif

    // PIZERO-11b diag: sample the PIO-USB root port every frame and log on ANY
    // change, so a sub-second connect/disconnect that the 1 Hz [run] line would
    // miss is still caught. `connected` is the lib's own debounced view (the
    // bisection: if it stays 1 after an unplug, the host stack never saw the
    // disconnect -> E9/SM masking; if it flips but no re-enum follows, the bug
    // is higher up in TinyUSB). line_state uses the lib's reader, which applies
    // the RP2350-E9 input-drain ONLY on chip rev<=2 (this board is rev3 -> plain
    // read, so a masked SE0 reading here is itself a data point).
    {
        static const char *LS[4] = { "SE0", "J/FS", "K/LS", "SE1" };
        uint8_t ls = usbdiag_line_state();
        int conn = usbdiag_connected();
        static int pls = -2, pconn = -2;
        if ((int)ls != pls || conn != pconn) {
            Serial.printf("[usb-evt] line=%s conn=%d fs=%d evt=%u usb=%lu (was line=%s conn=%d)\r\n",
                          LS[ls & 3], conn, usbdiag_fullspeed(),
                          usbdiag_event(), (unsigned long)g_usb_devices,
                          (pls >= 0) ? LS[pls & 3] : "?", pconn);
            pls = (int)ls; pconn = conn;
        }
    }
    if (!g_machine_running) { delay(1000); return; }
    static uint32_t next_us = 0;
    if (next_us == 0) next_us = micros();

    g_frame_count++;
    uint32_t a, b, c, d;
    if (disk_overlay_is_open()) {
        // PIZERO-81c: the machine is paused under the overlay. No keys, no
        // cycles, no render or blit; the overlay redraws only on change. USB,
        // the watchdog, pacing and core 1's scanout and audio all carry on.
        a = b = c = micros();
        wd_phase(WP_RENDER);
        disk_overlay_frame(g_frame_count);
        coco_machine_video_invalidate();         // PIZERO-213: the CoCo 3 redraws under it
        // PIZERO-165: the serial pump is paused, but a Ctrl-P waiting at the
        // front of the queue still takes a screenshot of the list.
        if (g_settings.serial_keyboard && Serial.available() && Serial.peek() == 0x10) {
            Serial.read();
            g_shot_request = true;
        }
        d = micros();
    } else {
    if (g_launch_kind >= 0) perform_launch();     // PIZERO-81d: chosen in the overlay
#ifdef COCO3
    // PIZERO-314: a test of the switch from the serial link: the other
    // machine at its BASIC prompt, settings.txt left as it is.
    if (g_switch_request) {
        g_switch_request = false;
        if (machine_available(!IS_COCO3())) switch_machine(!IS_COCO3(), HO_BASIC, -1, nullptr);
    }
    if (g_switch_want >= 0) {                     // PIZERO-319: a machine change saved in F11
        bool c3 = g_switch_want == 1;
        g_switch_want = -1;
        if (c3 != IS_COCO3()) switch_machine(c3, HO_BASIC, -1, nullptr);
    }
#endif
    bin_settle_tick();
    wd_phase(WP_KBD);
    pump_keyboard();
    volume_save_due();                 // PIZERO-255
    hid_key_repeat();                  // PIZERO-167
    a = micros();
    wd_phase(WP_EMU);
    run_frame(CYCLES_PER_FRAME * CPU_SPEED_MUL);   // PIZERO-287: in slices while MIDI is in use
    wd_post_frame(coco_machine_cpu_pc(), g_usb_devices);   // PIZERO-227
    coco_boot_disk_tick();                       // PIZERO-66: sync an idle dirty drive
    bt_frame();                                  // PIZERO-315: until the boot is logged
    bt_replay();
    stack_watch();                               // PIZERO-319
    b = micros();
    if (IS_COCO3()) {
    // PIZERO-133: the GIME drew each scanline into g_fb as it went.
    c = d = b;
    } else {
    wd_phase(WP_RENDER);
    coco_machine_render_frame();                  // regenerate VDG buffer (SUPPRESS_RENDER_SCANLINE)
    c = micros();
    wd_phase(WP_BLIT);
#ifdef VIDEO_640
    coco_boot_blit_vdg_pizero8(g_fb, g_pal565, PAL_VDG);   // PIZERO-135: each pixel twice
#elif defined(FB_SINGLE)
    coco_boot_blit_vdg_pizero(g_fb);              // single buffer; g_front already points at it
#else
    coco_boot_blit_vdg_pizero(g_fb[g_back]);      // render into the back buffer
    g_front = g_fb[g_back];                        // publish: core 1 picks it up at its next frame
    g_back ^= 1;
#endif
    d = micros();
    }
    }
    if (g_shot_request) {
        // PIZERO-165: save what is on the screen now. The machine and its
        // sound pause while the card is written, then pacing starts afresh
        // so the emulator does not race to catch up.
        g_shot_request = false;
        g_audio_paused = true;
#ifdef VIDEO_640
        coco_boot_screenshot8(g_fb, g_pal565, FB640_W, FRAME_HEIGHT, nullptr, 0);
#else
        coco_boot_screenshot((const uint16_t *)g_front, FRAME_WIDTH, FRAME_HEIGHT, nullptr, 0);
#endif
        g_audio_paused = disk_overlay_is_open();
        next_us = micros();
    }
    if (g_art_request) {
        // PIZERO-186: as the screenshot, pause while the card is written.
        g_art_request = false;
        g_audio_paused = true;
        artifact_cycle_and_save();
        g_audio_paused = disk_overlay_is_open();
        next_us = micros();
    }
    wd_phase(WP_AUDIO);
#if defined(AV_DATA_ISLAND) && !defined(AV_AUDIO_SWAPTEST) && !defined(AV_AUDIO_STATIC) && !defined(AV_STREAM_AUDIO)
    audio_encode_frame();                          // M2 (bank path): refill the OFF bank's audio islands
    // (AV_STREAM_AUDIO encodes per-line in the IRQ instead -- nothing to do here.)
#endif
    uint32_t e = micros();
    (void)e;
    wd_phase(WP_PACE);

#ifdef AUDIO_WAV_DUMP
    // Arm on USB-CDC attach (DTR), then autotype a deterministic ascending-tone
    // program (SOUND = core Color BASIC, no ECB) and stream a base64 WAV.
    static bool dump_armed = false;
#ifdef AUDIO_WAV_DUMP_BOOT
    // PIZERO-223: the machine boots as usual (autorun and all) and the
    // recording starts when Ctrl-W arrives on USB serial, once whatever is
    // to be heard is playing. The backlog in the ring is dropped first.
    if (!g_audio_dumping && Serial && Serial.available() && Serial.peek() == 0x17) {
        Serial.read();
        int16_t junk[256];
        while (coco_machine_audio_read(junk, 256) > 0) {}
        audio_dump_begin();
    }
    dump_armed = true;
#endif
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
    // PIZERO-219: the least time any frame had to spare this second, and how
    // many missed their slot. A late frame drops emulated time (the sound
    // comes up short) while fps, a count per second, can still read 60.
    static int32_t s_slack = INT32_MAX;
    static uint32_t s_late = 0;
    if (rem < s_slack) s_slack = rem;
    if (rem <= 0) s_late++;
    // PIZERO-220: the worst emulation time and the worst whole frame's work
    // (from the end of the last frame's wait to here) this second, so a late
    // frame can be put down to the emulation or to the rest of the loop.
    static uint32_t s_cmax = 0, s_wmax = 0, s_frame_start = 0;
    if (b - a > s_cmax) s_cmax = b - a;
    { uint32_t w = micros() - s_frame_start; if (s_frame_start && w > s_wmax) s_wmax = w; }
    if (rem > 0) delayMicroseconds((uint32_t)rem);
    else         next_us = micros();
    s_frame_start = micros();

    static uint32_t last = 0, frames = 0;
    frames++;
    uint32_t now = millis();
    if (now - last >= 1000) {
#ifdef AUDIO_WAV_DUMP
        if (g_audio_dumping) { frames = 0; last = now; return; }  // don't corrupt the base64 stream
#endif
        wd_phase(WP_TELEM);                                // PIZERO-203
        g_tele_ok = !Serial || Serial.availableForWrite() >= 192;
        if (!g_tele_ok) g_tele_dropped++;
        // PIZERO-11 diagnostic: D+/D- (post-INOVER, as PIO-USB sees them),
        // SOF frame counter (proves the 1 ms timer is firing), USB devices
        // mounted. With a FS device the lib expects D+=1 D-=0 (J state);
        // with an LS device D+=0 D-=1 (K state); empty bus = both 0.
        int dp = gpio_get(HOST_PIN_DP);
        int dm = gpio_get(HOST_PIN_DP + 1);
        uint32_t sof = pio_usb_host_get_frame_number();
        // PIZERO-11b: the lib's own line-state read + debounced connection flag.
        static const char *LS[4] = { "SE0", "J/FS", "K/LS", "SE1" };
        uint8_t ls = usbdiag_line_state();
        // PIZERO-119: name the render path, since the three differ by an
        // order of magnitude and a render time alone cannot be read.
        // PIZERO-33: if the event-queue runaway guard ever trips, say so
        // loudly and name the callback. This is the suspected cause of the
        // freezes in phase 'emulate'.
        if (event_runaway_count) {
            tprintf("[runaway] *** EVENT QUEUE RUNAWAY x%lu *** callback=%p tick=%lu\r\n",
                          (unsigned long)event_runaway_count,
                          event_runaway_fn, (unsigned long)event_runaway_tick);
        }
        uint8_t vdg = coco_machine_vdg_mode_bits();
        char mode[12];
        if (!(vdg & 0x80)) snprintf(mode, sizeof mode, "alpha");
        else snprintf(mode, sizeof mode, "gm%u%s", (unsigned)((vdg >> 4) & 7),
                      (vdg & 0x08) ? "c" : "");
        uint32_t pbw; uint8_t pbl; bool ext;
        coco_machine_pia1b_trace(&pbw, &pbl, &ext);
        tprintf("[vdg] mode=%s bits=%02x sam=%u pb_writes=%lu pb_last=%02x lower=%u\r\n",
                      mode, (unsigned)vdg, (unsigned)coco_machine_sam_v(),
                      (unsigned long)pbw, (unsigned)pbl, (unsigned)ext);
        // PIZERO-143: only while a program is using the GMC's sound chip.
        if (coco_machine_cart_csg_writes())
            tprintf("[csg] writes=%lu dropped=%lu\r\n",
                          (unsigned long)coco_machine_cart_csg_writes(),
                          (unsigned long)coco_machine_cart_csg_dropped());
        // PIZERO-149: likewise the AY chips.
        if (coco_machine_ay_writes())
            tprintf("[ay] writes=%lu chips=%x\r\n", (unsigned long)coco_machine_ay_writes(),
                    (unsigned)coco_machine_ay_active());
#ifdef USB_AUDIO
        usb_audio_telemetry(g_usb_audio_under);             // PIZERO-255
#endif
        if (coco_machine_orch_writes())                     // PIZERO-148
            tprintf("[orch] writes=%lu\r\n", (unsigned long)coco_machine_orch_writes());
        if (coco_machine_joystick_probes())                 // PIZERO-253
            tprintf("[snd] joystick_probes=%lu\r\n", (unsigned long)coco_machine_joystick_probes());
#ifdef VIDEO_BENCH
        { static uint32_t n = 0; if (n++ % 5 == 0) video_bench_print(); }
#endif
#ifdef FDC_TRACE
        FDC_TRACE_PORT.printf("[fdc] pc=%04x crb=%02x\r\n", (unsigned)coco_machine_cpu_pc(),
                              (unsigned)coco_machine_pia0_crb());   // PIZERO-190, 191
        { static uint32_t n = 0;                   // PIZERO-191: the screen every 5 s
          if (++n % 5 == 0) {
              char row[33];
              for (unsigned r = 0; r < 16; r++) {
                  coco_machine_screen_row(r, row);
                  FDC_TRACE_PORT.printf("[scr] |%s|\r\n", row);
              }
          } }
#endif
        // PIZERO-201: built in a buffer, so the soak log can take the same
        // line; badfps counts the seconds the machine ran off 60 fps since
        // boot, so a once-a-minute sample loses no video-fault windows.
        if (g_machine_running && frames != 60) g_bad_fps++;
        char g_run_line[256], g_aud_line[160];   // this second's lines, on the stack
        g_aud_line[0] = '\0';
        snprintf(g_run_line, sizeof g_run_line,
                      "[run] fps=%lu cpu=%luus render=%luus blit=%luus aud=%luus "
                      "| ls=%s conn=%d sof=%lu usb=%lu rpts=%lu rfail=%lu eperr=%u ints=%x "
                      "| freezes=%lu last=%s badfps=%lu tdrop=%lu slack=%ldus late=%lu cmax=%luus wmax=%luus",
                      (unsigned long)frames, (unsigned long)(b - a),
                      (unsigned long)(c - b), (unsigned long)(d - c),
                      (unsigned long)(e - d),
                      LS[ls & 3], usbdiag_connected(),
                      (unsigned long)sof,
                      (unsigned long)g_usb_devices,
                      (unsigned long)g_hid_reports,
                      (unsigned long)g_hid_recv_fail,
                      usbdiag_ep_error(), usbdiag_ints(),
                      (unsigned long)g_freeze_count,
                      g_freeze_count ? wd_phase_name(g_last_freeze_phase) : "none",
                      (unsigned long)g_bad_fps, (unsigned long)g_tele_dropped,
                      (long)s_slack, (unsigned long)s_late,
                      (unsigned long)s_cmax, (unsigned long)s_wmax);
        s_slack = INT32_MAX;
        s_late = 0;
        s_cmax = s_wmax = 0;
        tprintf("%s\r\n", g_run_line);
        // PIZERO-287: the MIDI Pak, when anything has moved or software set it up.
        { static uint32_t last_out = 0, last_in = 0;
          if (coco_machine_midi_active() || g_midi_out != last_out || g_midi_in != last_in) {
              uint32_t txd, rxd;
              coco_machine_midi_counters(&txd, &rxd);
              tprintf("[midi] active=%d usb=%d out=%lu in=%lu dropped_out=%lu dropped_in=%lu usb_full=%lu\r\n",
                      (int)coco_machine_midi_active(), (int)TinyUSBDevice.mounted(),
                      (unsigned long)g_midi_out, (unsigned long)g_midi_in,
                      (unsigned long)txd, (unsigned long)rxd, (unsigned long)g_midi_out_lost);
              last_out = g_midi_out; last_in = g_midi_in;
          } }
#ifdef CPU_PROF
        {
            // PIZERO-209: per emulated cycle, in tenths of a host clock.
            // bus = latch stores + call + hook; core = the rest of the run;
            // each with the timing windows' own cost taken out.
            struct cpu_prof p; uint32_t win_in, win_full;
            coco_machine_cpu_prof(&p, &win_in, &win_full);
            if (p.bus_n && p.insn_n) {
                uint64_t n = p.bus_n;
                uint32_t total = (uint32_t)((uint64_t)p.run_clk * 10 / n);
                uint32_t bus_raw = (uint32_t)((uint64_t)p.bus_clk * 10 / n);
                uint32_t bus = bus_raw - win_in * 10;
                uint32_t core = total - bus_raw - (win_full - win_in) * 10;
                tprintf("[prof] cyc=%lu clk/cyc: bus=%lu.%lu core=%lu.%lu sum=%lu.%lu (raw %lu.%lu) "
                        "| nvma=%lu.%lu%% cyc/insn=%lu.%02lu | win in=%lu full=%lu "
                        "| hook dec=%lu.%lu data=%lu.%lu ev+aud=%lu.%lu irq=%lu.%lu\r\n",
                        (unsigned long)p.bus_n,
                        (unsigned long)(bus / 10), (unsigned long)(bus % 10),
                        (unsigned long)(core / 10), (unsigned long)(core % 10),
                        (unsigned long)((bus + core) / 10), (unsigned long)((bus + core) % 10),
                        (unsigned long)(total / 10), (unsigned long)(total % 10),
                        (unsigned long)(p.nvma_n * 1000ull / n / 10), (unsigned long)(p.nvma_n * 1000ull / n % 10),
                        (unsigned long)(p.bus_n / p.insn_n), (unsigned long)(p.bus_n * 100ull / p.insn_n % 100),
                        (unsigned long)win_in, (unsigned long)win_full,
                        (unsigned long)(p.sec_clk[0] * 10ull / n / 10), (unsigned long)(p.sec_clk[0] * 10ull / n % 10),
                        (unsigned long)(p.sec_clk[1] * 10ull / n / 10), (unsigned long)(p.sec_clk[1] * 10ull / n % 10),
                        (unsigned long)(p.sec_clk[2] * 10ull / n / 10), (unsigned long)(p.sec_clk[2] * 10ull / n % 10),
                        (unsigned long)(p.sec_clk[3] * 10ull / n / 10), (unsigned long)(p.sec_clk[3] * 10ull / n % 10));
                // PIZERO-220: the CoCo 3's slow cycles, events and sink, per frame.
                // PIZERO-248: the CoCo 2 too, for its audio samples' cost.
                if (p.slow_n || p.samp_n)
                    tprintf("[prof3] per frame: slow=%lu (%lu clk: bus %lu tail %lu) ev=%lu (%lu clk) lines=%lu sink=%lu clk run=%lu clk | samples=%lu (%lu clk) catch=%lu clk\r\n",
                            (unsigned long)(p.slow_n / 60), (unsigned long)(p.slow_clk / 60),
                            (unsigned long)(p.cb_bus_clk / 60), (unsigned long)(p.cb_tail_clk / 60),
                            (unsigned long)(p.ev_n / 60), (unsigned long)(p.ev_clk / 60),
                            (unsigned long)(p.line_n / 60), (unsigned long)(p.line_clk / 60),
                            (unsigned long)(p.run_clk / 60),
                            (unsigned long)(p.samp_n / 60), (unsigned long)(p.samp_clk / 60),
                            (unsigned long)(p.catch_clk / 60));
                if (p.pr_n)
                    tprintf("[prof3] port render per drawn line: gather+hash=%lu decode=%lu clk (%lu lines/s)\r\n",
                            (unsigned long)(p.pr_gather / p.pr_n), (unsigned long)(p.pr_decode / p.pr_n),
                            (unsigned long)p.pr_n);
                tprintf("[prof3] worst frame: run=%lu clk cyc=%lu slow=%lu ev=%lu lines=%lu sink=%lu\r\n",
                        (unsigned long)p.wf_run, (unsigned long)p.wf_cyc, (unsigned long)p.wf_slow,
                        (unsigned long)p.wf_ev, (unsigned long)p.wf_lines, (unsigned long)p.wf_sink);
            }
        }
#endif
        (void)dp; (void)dm;
        { static uint32_t last_pad = 0;      // PIZERO-13: only while a pad talks
          uint32_t n = g_pad_decoded;
          if (n != last_pad)
              tprintf("[pad] reports=%lu/s right-joystick x=%u y=%u fire=%u buttons=%04x "
                      "raw=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
                            (unsigned long)(n - last_pad), (unsigned)(g_pad_x >> 10),
                            (unsigned)(g_pad_y >> 10), (unsigned)g_pad_fire,
                            (unsigned)g_pad_buttons_now,    // PIZERO-215: PAD_BIT(PAD_B_*)
                            g_pad_raw[0], g_pad_raw[1], g_pad_raw[2], g_pad_raw[3], g_pad_raw[4],
                            g_pad_raw[5], g_pad_raw[6], g_pad_raw[7], g_pad_raw[8], g_pad_raw[9]);
          last_pad = n; }
#ifdef AV_STREAM_AUDIO
        // PIZERO-38/39 audio-ring health: fill should hover near AUDIO_RING/2 with
        // skips (overflow) and under (underrun) staying ~flat once primed.
        // PIZERO-118: cumulative counters could not say whether the machine had
        // nothing to play or whether the stream came up short, so report RATES
        // and what the source actually produced. prod/s against the nominal
        // sample rate is the real health number: a steady shortfall means the
        // consumer is being fed less than it asks for and is holding the last
        // sample to cover the gap. tone/s says whether any of it was audible.
        { uint32_t fill = 0, skips = 0, prod = 0, tone = 0;
          coco_machine_audio_stats(&fill, &skips);
          coco_machine_audio_counters(&prod, &tone);
          static uint32_t prod_p = 0, tone_p = 0, under_p = 0;
          uint32_t ms = now - last ? now - last : 1;
          uint32_t dprod  = prod - prod_p, dtone = tone - tone_p;
          uint32_t dunder = (uint32_t)g_stream_under - under_p;
          prod_p = prod; tone_p = tone; under_p = (uint32_t)g_stream_under;
          uint32_t prod_s  = (uint32_t)((uint64_t)dprod  * 1000u / ms);
          uint32_t tone_s  = (uint32_t)((uint64_t)dtone  * 1000u / ms);
          uint32_t under_s = (uint32_t)((uint64_t)dunder * 1000u / ms);
          uint32_t want    = coco_machine_audio_rate();
          uint32_t srv     = coco_machine_audio_rate_now();   // PIZERO-121 servo
          // Tenths of a percent of the nominal rate that never arrived.
          uint32_t short_x10 = want ? (uint32_t)((uint64_t)under_s * 1000u / want) : 0;
          snprintf(g_aud_line, sizeof g_aud_line,
                        "[aud] fill=%lu prod=%lu/s want=%lu/s srv=%lu under=%lu/s "
                        "short=%lu.%lu%% tone=%lu/s skips=%lu under_total=%lu",
                        (unsigned long)fill, (unsigned long)prod_s, (unsigned long)want,
                        (unsigned long)srv, (unsigned long)under_s,
                        (unsigned long)(short_x10 / 10), (unsigned long)(short_x10 % 10),
                        (unsigned long)tone_s, (unsigned long)skips,
                        (unsigned long)g_stream_under);
          tprintf("%s\r\n", g_aud_line); }
#endif
        wd_phase(WP_SOAKLOG);                                // PIZERO-203
        soak_log_tick(g_run_line, g_aud_line);           // PIZERO-201
        frames = 0; last = now;
    }
}

// PIZERO-164: press exactly these keys on the pad's layer of the key matrix,
// if they differ from what it holds now (pads report ~60 times a second).
static uint8_t g_pad_keys[PAD_KEYS_MAX];
static uint8_t g_pad_nkeys = 0;
static void pad_keys_present(const uint8_t *keys, uint8_t n) {
    if (n == g_pad_nkeys && (n == 0 || !memcmp(keys, g_pad_keys, n))) return;
    coco_machine_release_all_keys_src(COCO_KEYS_PAD);
    for (uint8_t i = 0; i < n; i++) coco_machine_press_key_src(COCO_KEYS_PAD, keys[i]);
    if (n) memcpy(g_pad_keys, keys, n);
    g_pad_nkeys = n;
}

// PIZERO-13: the bound gamepad (0 = none), and the pad identity of each
// device address, so no pad's reports reach the keyboard. A pad binds on the
// first interface whose report its decoder accepts: the GameSir identities
// carry the gamepad on a different interface in different modes.
#define USB_PAD_MAX_DADDR 15
// PIZERO-13 bring-up: print a known pad's report descriptor at mount and its
// reports as they change. Build with -DPAD_PROBE=1 to measure a new identity.
#ifndef PAD_PROBE
#define PAD_PROBE 0
#endif
static uint8_t g_pad_daddr = 0;
static uint8_t g_pad_idx = 0;
static enum pad_kind g_pad_kind_of[USB_PAD_MAX_DADDR + 1];

static bool pad_addr(uint8_t daddr) {
    return daddr <= USB_PAD_MAX_DADDR && g_pad_kind_of[daddr] != PAD_NONE;
}

// ---- USB host HID callbacks (PIZERO-11) ---------------------------------
// TinyUSB looks up these weak symbols by C name; extern "C" prevents mangling.
extern "C" {

#ifdef USB_DESC_DUMP
// PIZERO-255 spike: every device's configuration descriptor, decoded where
// it is audio, and in raw hex, so a USB headphone DAC's formats, rates and
// endpoint sync types can be read off the serial log. One fetch at a time
// (the buffer is shared), queued as devices mount.
static uint8_t  g_dd_buf[1024];
static uint8_t  g_dd_queue[8];
static uint8_t  g_dd_n = 0;
static bool     g_dd_busy = false;

static void dd_start(void);

static void dd_print(uint8_t daddr, const uint8_t *p, uint16_t len) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    static const char *const sp[] = { "full", "low", "high" };
    tusb_speed_t s = tuh_speed_get(daddr);
    Serial.printf("[usbdesc] addr=%u vid=%04X pid=%04X speed=%s config_len=%u\r\n", daddr, vid, pid,
                  (unsigned)s < 3 ? sp[s] : "?", len);
    uint8_t cls = 0, sub = 0;
    for (uint16_t i = 0; i + 2 <= len && p[i] >= 2 && i + p[i] <= len; i += p[i]) {
        const uint8_t *d = p + i;
        uint8_t dl = d[0], ty = d[1];
        if (ty == 0x04 && dl >= 9) {
            cls = d[5]; sub = d[6];
            Serial.printf("[usbdesc]  interface %u alt %u class %02X/%02X/%02X endpoints %u%s\r\n",
                          d[2], d[3], d[5], d[6], d[7], d[4],
                          d[5] == 1 ? (d[6] == 1 ? " (audio control)" : d[6] == 2 ? " (audio streaming)" : " (audio)") :
                          d[5] == 3 ? " (HID)" : d[5] == 9 ? " (hub)" : "");
        } else if (ty == 0x05 && dl >= 7) {
            static const char *const tt[] = { "control", "isochronous", "bulk", "interrupt" };
            static const char *const st[] = { "none", "async", "adaptive", "sync" };
            static const char *const ut[] = { "data", "feedback", "implicit fb", "?" };
            uint16_t mps = (uint16_t)(d[4] | (d[5] << 8));
            Serial.printf("[usbdesc]   endpoint %02X %s %s sync=%s usage=%s max=%u interval=%u\r\n",
                          d[2], (d[2] & 0x80) ? "IN" : "OUT", tt[d[3] & 3],
                          st[(d[3] >> 2) & 3], ut[(d[3] >> 4) & 3], mps & 0x7FF, d[6]);
        } else if (ty == 0x24 && cls == 1 && sub == 2 && dl >= 6 && d[2] == 2) {
            // AS FORMAT_TYPE: UAC1 carries the rates here; UAC2 does not.
            if (dl >= 8 && d[3] == 1 && dl == 8 + 3 * d[7]) {
                Serial.printf("[usbdesc]   UAC1 format type I: channels=%u subframe=%u bits=%u rates=",
                              d[4], d[5], d[6]);
                if (d[7] == 0) Serial.printf("%lu-%lu (continuous)",
                    (unsigned long)(d[8] | d[9] << 8 | d[10] << 16), (unsigned long)(d[11] | d[12] << 8 | d[13] << 16));
                for (unsigned k = 0; k < d[7]; k++)
                    Serial.printf("%s%lu", k ? "," : "", (unsigned long)(d[8+3*k] | d[9+3*k] << 8 | d[10+3*k] << 16));
                Serial.print("\r\n");
            } else if (dl == 6 && d[3] == 1) {
                Serial.printf("[usbdesc]   UAC2 format type I: subslot=%u bits=%u\r\n", d[4], d[5]);
            }
        } else if (ty == 0x24 && cls == 1 && sub == 1 && dl >= 3) {
            Serial.printf("[usbdesc]   audio control subtype %02X%s\r\n", d[2],
                          d[2] == 1 ? (dl >= 5 && d[3] == 0 && d[4] == 2 ? " header UAC2" : " header UAC1") : "");
        }
        Serial.print("[usbdesc]    raw");
        for (uint8_t k = 0; k < dl; k++) Serial.printf(" %02X", d[k]);
        Serial.print("\r\n");
    }
}

static void dd_done(tuh_xfer_t *x) {
    uint8_t daddr = (uint8_t)x->user_data;
    if (x->result == XFER_RESULT_SUCCESS) {
        uint16_t total = (uint16_t)(g_dd_buf[2] | (g_dd_buf[3] << 8));
        if (total > sizeof g_dd_buf) total = sizeof g_dd_buf;
        dd_print(daddr, g_dd_buf, total);
    } else {
        Serial.printf("[usbdesc] addr=%u configuration descriptor fetch failed (%d)\r\n", daddr, (int)x->result);
    }
    g_dd_busy = false;
    dd_start();
}

static void dd_start(void) {
    if (g_dd_busy || !g_dd_n) return;
    uint8_t daddr = g_dd_queue[0];
    for (uint8_t k = 1; k < g_dd_n; k++) g_dd_queue[k - 1] = g_dd_queue[k];
    g_dd_n--;
    g_dd_busy = tuh_descriptor_get_configuration(daddr, 0, g_dd_buf, sizeof g_dd_buf, dd_done, daddr);
    if (!g_dd_busy) { Serial.printf("[usbdesc] addr=%u fetch not started\r\n", daddr); dd_start(); }
}
#endif

void tuh_mount_cb(uint8_t daddr) {
    g_usb_devices++;
    Serial.printf("[usb] device attached, addr=%u\r\n", daddr);
#ifdef USB_DESC_DUMP
    if (g_dd_n < sizeof g_dd_queue) g_dd_queue[g_dd_n++] = daddr;
    dd_start();
#endif
#ifdef USB_AUDIO
    usb_audio_mount(daddr);
#endif
}

void tuh_umount_cb(uint8_t daddr) {
    if (g_usb_devices) g_usb_devices--;
    Serial.printf("[usb] device removed, addr=%u\r\n", daddr);
#ifdef USB_AUDIO
    usb_audio_umount(daddr);
#endif
}

void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx,
                      uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    uint8_t proto = tuh_hid_interface_protocol(daddr, idx);
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    enum pad_kind pad = pad_identify(vid, pid);
    const char *kind = pad == PAD_DS4    ? "gamepad, DS4"
                     : pad == PAD_XINPUT ? "gamepad, XInput"
                     : pad == PAD_HIDGP  ? "gamepad, HID"
                     : pad == PAD_SILENT ? "gamepad, no input in this mode"
                     : (proto == HID_ITF_PROTOCOL_KEYBOARD) ? "keyboard"
                     : (proto == HID_ITF_PROTOCOL_MOUSE)    ? "mouse"
                                                            : "generic";
    Serial.printf("[usb] HID mount addr=%u idx=%u vid=%04X pid=%04X proto=%u (%s) desc_len=%u\r\n",
                  daddr, idx, vid, pid, proto, kind, desc_len);
    if (idx < HIDC_N && hidc_parse(desc_report, desc_len, &g_hidc[idx]))   // PIZERO-255
        Serial.printf("[usb] volume keys on addr=%u idx=%u\r\n", daddr, idx);
#ifdef USB_DESC_DUMP
    Serial.printf("[usbhid] addr=%u idx=%u report descriptor %u bytes:", daddr, idx, desc_len);
    for (uint16_t i = 0; i < desc_len; i++)
        Serial.printf("%s%02X", (i % 32) ? " " : "\r\n[usbhid]   ", desc_report[i]);
    Serial.print("\r\n");
#endif
    if (idx < HIDC_N) g_hidc_held[idx] = 0;
    if (daddr <= USB_PAD_MAX_DADDR) g_pad_kind_of[daddr] = pad;
#if PAD_PROBE
    if (pad != PAD_NONE) {                     // the layout, for decoding by hand
        Serial.printf("[pad] descriptor %u bytes:", desc_len);
        for (uint16_t i = 0; i < desc_len; i++)
            Serial.printf("%s%02X", (i % 32) ? " " : "\r\n[pad]   ", desc_report[i]);
        Serial.printf("\r\n");
    }
#endif
    if (!tuh_hid_receive_report(daddr, idx)) {
        Serial.printf("[usb] tuh_hid_receive_report FAILED idx=%u\r\n", idx);
    } else {
        Serial.printf("[usb] receive_report armed idx=%u\r\n", idx);
    }
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx) {
    Serial.printf("[usb] HID unmount addr=%u idx=%u\r\n", daddr, idx);
    if (idx < HIDC_N) g_hidc[idx].has = false;   // PIZERO-255
    if (daddr <= USB_PAD_MAX_DADDR) g_pad_kind_of[daddr] = PAD_NONE;
    // PIZERO-13: an unplugged pad must not leave a stick held over or fire down.
    if (g_pad_daddr && daddr == g_pad_daddr && idx == g_pad_idx) {
        g_pad_daddr = 0;
        coco_machine_release_all_joysticks();
        pad_keys_present(nullptr, 0);             // PIZERO-164: and its keys
    }
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t idx,
                                uint8_t const *report, uint16_t len) {
    // PIZERO-12: treat any 8-byte report as boot keyboard (our test dongle
    // mislabels its keyboard interface as proto=2/mouse). 3-byte report is
    // a boot mouse (no consumer yet: PIZERO-13). Other lengths: ignore.
    // PIZERO-13: the bound pad goes to the joysticks. Any known pad identity
    // is kept off the keyboard path: its 64-byte reports would type junk.
#ifdef USB_DESC_DUMP
    // PIZERO-255 diagnosis: each report from a non-pad, non-boot-keyboard
    // interface, when it changes, with what the volume decoder made of it.
    if (!pad_addr(daddr) && len != 8 && idx < HIDC_N) {
        static uint8_t last[HIDC_N][16];
        static uint8_t last_len[HIDC_N];
        uint8_t n = (uint8_t)(len < 16 ? len : 16);
        if (n != last_len[idx] || memcmp(report, last[idx], n)) {
            memcpy(last[idx], report, n);
            last_len[idx] = n;
            bool mine = false;
            uint8_t k = g_hidc[idx].has ? hidc_decode(&g_hidc[idx], report, len, &mine) : 0;
            Serial.printf("[usbhid] addr=%u idx=%u len=%u:", daddr, idx, len);
            for (uint8_t i = 0; i < n; i++) Serial.printf(" %02X", report[i]);
            Serial.printf("  -> mine=%u keys=%u\r\n", mine, k);
        }
    }
#endif
    // PIZERO-255: a report carrying volume keys goes to them and no further.
    if (idx < HIDC_N && g_hidc[idx].has) {
        bool mine;
        uint8_t k = hidc_decode(&g_hidc[idx], report, len, &mine);
        if (mine) {
            uint8_t pressed = (uint8_t)(k & ~g_hidc_held[idx]);
            g_hidc_held[idx] = k;
            if (pressed) volume_keys(pressed);
            g_hid_reports++;
            if (!tuh_hid_receive_report(daddr, idx)) g_hid_recv_fail++;
            return;
        }
    }
    if (pad_addr(daddr)) {
        // PIZERO-165: the GameSir's M button sends Print Screen (usage 0x46) as
        // a keyboard report (ID 3) from its second interface: a screenshot.
        if (len >= 9 && report[0] == 0x03) {
            static bool m_held = false;
            bool m = false;
            for (int i = 3; i < 9; i++) if (report[i] == 0x46) m = true;
            if (m && !m_held) g_shot_request = true;
            m_held = m;
        }
        bool mine = g_pad_daddr && daddr == g_pad_daddr && idx == g_pad_idx;
        struct pad_state st;
        struct pad_out ps;
        if ((mine || !g_pad_daddr) &&
            pad_decode(g_pad_kind_of[daddr], report, len, &st)) {
            if (!mine) {                       // first decodable report: bind
                g_pad_daddr = daddr;
                g_pad_idx = idx;
                Serial.printf("[usb] gamepad bound addr=%u idx=%u\r\n", daddr, idx);
            }
            // PIZERO-169: Home opens the overlay; while it is open the pad
            // drives it and does nothing to the machine.
            g_pad_buttons_now = st.buttons;
            memcpy(g_pad_raw, report, len < sizeof g_pad_raw ? len : sizeof g_pad_raw);   // PIZERO-215
            static bool home_held = false, home_armed = false;
            bool home = st.buttons & PAD_BIT(PAD_B_HOME);
            bool home_new = home && !home_held;
            home_held = home;
            if (!disk_overlay_is_open() && home_new && g_machine_running) {
                disk_overlay_open();
                g_audio_paused = true;
                home_armed = false;                // this press opened it
                pad_nav_codes(st.buttons, st.dpad, g_pad_nav);
            } else if (disk_overlay_is_open()) {
                if (!home) home_armed = true;      // let go: the next press closes
                uint8_t nav[6];
                int n = pad_nav_codes(st.buttons, st.dpad, nav);
                if (home && home_armed && n < 6) nav[n] = PAD_HK_ESC;
                if (memcmp(nav, g_pad_nav, 6)) {
                    memcpy(g_pad_nav, nav, 6);
                    overlay_feed(g_hid_mods);
                }
            }
            if (disk_overlay_is_open()) {
                pad_centered(&ps);
            } else {
                // Buttons (and the D-pad) held when the overlay closed stay
                // inert until let go.
                g_pad_inert &= st.buttons;
                st.buttons &= (uint16_t)~g_pad_inert;
                if (st.dpad >= 8) g_pad_dpad_inert = false;
                if (g_pad_dpad_inert) st.dpad = 8;
                pad_resolve(&st, &g_settings.pad, &ps);
            }
            // PIZERO-160: joystick_swap sends each pad stick, with its fire
            // buttons, to the other CoCo joystick.
            int sw = g_settings.joystick_swap ? 1 : 0;
            for (int p = 0; p < 2; p++) {
                coco_machine_set_joystick_axis(p ^ sw, 0, ps.axis[p][0]);
                coco_machine_set_joystick_axis(p ^ sw, 1, ps.axis[p][1]);
                coco_machine_set_joystick_fire(p ^ sw, ps.fire[p]);
                coco_machine_set_joystick_fire2(p ^ sw, ps.fire2[p]);   // PIZERO-215
            }
            // PIZERO-164: the keys the pad's buttons are mapped to, on the
            // pad's own layer of the key matrix; redrawn only on a change.
            pad_keys_present(ps.keys, ps.nkeys);
            g_pad_decoded++;
            g_pad_x = ps.axis[sw][0];                  // what CoCo port 0 now holds
            g_pad_y = ps.axis[sw][1];
            g_pad_fire = (uint8_t)((ps.fire[sw] ? 1 : 0) | (ps.fire[sw ^ 1] ? 2 : 0));
        }
    } else if (len >= 8) {
        hid_keyboard_apply(report);
    }
#if PAD_PROBE
    // Any known pad: print each report that differs from the last, so moving
    // one control at a time shows which bytes it owns.
    if (pad_addr(daddr) && idx < 4) {
        static uint8_t last[4][64];
        static uint16_t last_len[4];
        uint16_t n = len < sizeof last[0] ? len : (uint16_t)sizeof last[0];
        if (n != last_len[idx] || memcmp(report, last[idx], n) != 0) {
            memcpy(last[idx], report, n);
            last_len[idx] = n;
            Serial.printf("[pad] i%u %u:", idx, len);
            for (uint16_t i = 0; i < n; i++) Serial.printf(" %02X", report[i]);
            Serial.printf("\r\n");
        }
    }
#endif
    g_hid_reports++;   // PIZERO-11b diag: proves the device is alive & polled
#ifdef USB_HOTPLUG_RECOVER
    // PIZERO-51: detect the unplug phantom-flood (byte-identical reports) and ask
    // loop() to re-enumerate. Kept OUT of hid_keyboard_apply so real keystrokes
    // (which change the report, resetting the run) never trip it.
    // A pad at rest sends identical reports without end, which is not a flood.
    if (!(g_pad_daddr && daddr == g_pad_daddr) &&
        hotplug_note_report(daddr, idx, report, len)) g_usb_recover_pending = true;
#endif
    if (!tuh_hid_receive_report(daddr, idx)) g_hid_recv_fail++;
}

} // extern "C"

// PIZERO-156: the F1 INFO page, a line at a time (32 columns at most; the
// labels are 7 wide so a git describe with -dirty still fits).
static int info_lines(char lines[][33], int max) {
    int n = 0;
#define INFO(...) do { if (n < max) snprintf(lines[n++], 33, __VA_ARGS__); } while (0)
    INFO("FW     %s", FW_VERSION);
    INFO("DATE   %s", FW_DATE);
    INFO("BUILD  %s", FW_ENV);
    {                                            // PIZERO-242: the machine profile
        static const char *const m[3] = { "COCO ZERO", "COCO 2", "COCO 3" };
        INFO("MACH   %s", m[g_settings.machine < 3 ? g_settings.machine : 0]);
    }
    INFO("%s", "");
    uint32_t chip_id = *((volatile uint32_t *)(SYSINFO_BASE + SYSINFO_CHIP_ID_OFFSET));
    uint32_t rev = (chip_id & SYSINFO_CHIP_ID_REVISION_BITS) >> SYSINFO_CHIP_ID_REVISION_LSB;
    INFO("CHIP   RP2350 REVISION %lu", (unsigned long)rev);
    INFO("ID     %08lX", (unsigned long)chip_id);
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(serial, sizeof serial);
    INFO("SERIAL %s", serial);
    INFO("CLOCK  %lu MHZ", (unsigned long)(clock_get_hz(clk_sys) / 1000000u));
    INFO("VIDEO  %s ENCODER", dvi_sio_tmds_encode ? "HARDWARE" : "SOFTWARE");   // PIZERO-175
    INFO("%s", "");
    uint32_t up = millis() / 1000u;
    INFO("UP     %lu:%02lu:%02lu", (unsigned long)(up / 3600u),
         (unsigned long)((up / 60u) % 60u), (unsigned long)(up % 60u));
    { char clk[40], src[24];                   // PIZERO-201
      coco_boot_clock_text(clk, sizeof clk, src, sizeof src);
      INFO("CLOCK  %.16s %s", clk, src); }
    INFO("MEMORY %d KB FREE", rp2040.getFreeHeap() / 1024);
    INFO("USB    %lu DEVICE%s%s", (unsigned long)g_usb_devices,
         g_usb_devices == 1 ? "" : "S", g_pad_daddr ? ", GAMEPAD" : "");
#undef INFO
    return n;
}
