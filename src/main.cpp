// xroar-waveshare-rp2350-pizero — DVI bring-up test pattern (PIZERO-05).
//
// Brings up the mini-HDMI output via the vendored libdvi (Wren6991/PicoDVI)
// and paints a static 320x240 RGB565 test pattern that libdvi scans out
// pixel/line-doubled to 640x480p 60 Hz. No XRoar yet — once this image is on
// an HDMI monitor we know the hardware path (clocks, PIO TMDS, DMA, pin map)
// is good and can layer the emulator on top.
//
// Display path is fixed by the board wiring: TMDS on GPIO 32-39, so libdvi
// (PIO) — HSTX (GPIO 12-19) is not wired to the connector. See README.md.

#include <Arduino.h>
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"

extern "C" {
#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"   // provides pico_sock_cfg (the PiZero TMDS map)
}

#define FRAME_WIDTH  320
#define FRAME_HEIGHT 240
#define DVI_TIMING   dvi_timing_640x480p_60hz

// 320x240 RGB565 framebuffer (~154 KB). libdvi doubles it to 640x480.
static uint16_t g_fb[FRAME_WIDTH * FRAME_HEIGHT] __attribute__((aligned(4)));

static struct dvi_inst dvi0;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Vertical colour bars + a 1px white border, so we can confirm geometry,
// colour order (RGB565, no byte-swap for libdvi), and stable sync.
static void fill_test_pattern() {
    static const uint16_t bars[8] = {
        rgb565(0,0,0),       rgb565(0,0,255),     rgb565(0,255,0),    rgb565(0,255,255),
        rgb565(255,0,0),     rgb565(255,0,255),   rgb565(255,255,0),  rgb565(255,255,255),
    };
    for (int y = 0; y < FRAME_HEIGHT; y++) {
        for (int x = 0; x < FRAME_WIDTH; x++) {
            uint16_t c = bars[(x * 8) / FRAME_WIDTH];
            if (y == 0 || y == FRAME_HEIGHT - 1 || x == 0 || x == FRAME_WIDTH - 1)
                c = rgb565(255, 255, 255);
            g_fb[y * FRAME_WIDTH + x] = c;
        }
    }
}

// Core 1 runs the libdvi TMDS encoder/scanout and never returns.
static void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    while (queue_is_empty(&dvi0.q_colour_valid))
        tight_loop_contents();
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) delay(10);
    Serial.println("\nRP2350-PiZero DVI bring-up (PIZERO-05)");

    fill_test_pattern();

    vreg_set_voltage(VREG_VOLTAGE_1_20);
    delay(10);

    // TMDS pins are GPIO 32-39 (> 31), so PIO needs its GPIO window based at 16.
    pio_set_gpio_base(DVI_DEFAULT_SERIAL_CONFIG.pio, 16);

    dvi0.timing  = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;     // pico_sock_cfg: TMDS 32/34/36, clk 38
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    multicore_launch_core1(core1_main);

    Serial.printf("DVI up: %dx%d fb -> 640x480p60, sys=%lu kHz\n",
                  FRAME_WIDTH, FRAME_HEIGHT,
                  (unsigned long)(clock_get_hz(clk_sys) / 1000));
}

void loop() {
    // Stream the static framebuffer's scanlines to libdvi forever. The
    // blocking queue paces this loop to the 60 Hz scanout.
    for (uint y = 0; y < FRAME_HEIGHT; y++) {
        const uint16_t *scanline = &g_fb[y * FRAME_WIDTH];
        queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline);
        const uint16_t *unused;
        while (queue_try_remove_u32(&dvi0.q_colour_free, &unused))
            ;
    }
}
