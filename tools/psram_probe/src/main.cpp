// PSRAM probe for the RP2350-PiZero U1 footprint (PIZERO-282). Standalone
// PlatformIO project: cd tools/psram_probe && pio run -t upload, then open the
// serial port and send any key to rerun. Reports the core's detected size; if
// zero, checks the CS line (GPIO 47) and dumps the raw 0x9F ID reply; if found,
// runs walking bits, address-in-address over the whole chip, and timings.

#include <Arduino.h>
#include <hardware/structs/qmi.h>
#include <hardware/structs/xip_ctrl.h>
#include <hardware/sync.h>
extern size_t __psram_size;

// Raw JEDEC ID read on CS1 (only used when the core found nothing).
static void __no_inline_not_in_flash_func(cmd1)(uint8_t c, int div) {
  qmi_hw->direct_csr = div << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
  while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  qmi_hw->direct_tx = c;
  while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
  (void)qmi_hw->direct_rx;
  qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
}
static int g_div = 30;
static void __no_inline_not_in_flash_func(raw_id)(uint8_t *out) {
  uint32_t s = save_and_disable_interrupts();
  qmi_hw->direct_csr = g_div << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
  while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS | QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB | 0xf5;
  while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
  (void)qmi_hw->direct_rx;
  qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  for (int i = 0; i < 12; i++) {
    qmi_hw->direct_tx = i == 0 ? 0x9f : 0xff;
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)) {}
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    out[i] = qmi_hw->direct_rx;
  }
  qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
  restore_interrupts(s);
}

// Drive CS1 by hand only from RAM with interrupts off: with CS low, a live chip
// would answer the flash's own XIP fetches and corrupt them.
static void __no_inline_not_in_flash_func(cs_probe)(int *idle, int *lo, int *hi) {
  const uint32_t m = 1u << (47 - 32);
  uint32_t st = save_and_disable_interrupts();
  *idle = !!(sio_hw->gpio_hi_in & m);
  sio_hw->gpio_hi_clr = m; sio_hw->gpio_hi_oe_set = m;
  for (volatile int i = 0; i < 200; i++) {}
  *lo = !!(sio_hw->gpio_hi_in & m);
  sio_hw->gpio_hi_set = m;
  for (volatile int i = 0; i < 200; i++) {}
  *hi = !!(sio_hw->gpio_hi_in & m);
  sio_hw->gpio_hi_oe_clr = m;
  for (volatile int i = 0; i < 200; i++) {}
  restore_interrupts(st);
}

static void run() {
  Serial.printf("\n[psram] sys clk %lu Hz, core detected %u bytes\n", (unsigned long)rp2040.f_cpu(), (unsigned)__psram_size);
  if (!__psram_size) {
    Serial.println("[psram] GPIO47 check..."); Serial.flush(); delay(200);
    // CS line check: as SIO input with R39 pull-up it should read 1; drive low/high and read back.
    gpio_init(47); gpio_disable_pulls(47); delayMicroseconds(50);
    int idle, lo, hi; cs_probe(&idle, &lo, &hi);
    Serial.printf("[psram] GPIO47 idle=%d (want 1 via R39)  driven low reads %d  driven high reads %d\n", idle, lo, hi); Serial.flush(); delay(200);
    gpio_set_function(47, GPIO_FUNC_XIP_CS1);
    Serial.println("[psram] raw ID..."); Serial.flush(); delay(200);
    uint8_t id[12]; raw_id(id);
    Serial.print("[psram] raw 0x9F reply:");
    for (int i = 0; i < 12; i++) Serial.printf(" %02X", id[i]);
    Serial.println("\n[psram] expected bytes 4/5 = 0D 5D (AP Memory, known-good die)");
    return;
  }
  const uint32_t n = __psram_size / 4;
  volatile uint32_t *u = (volatile uint32_t *)0x15000000; // uncached alias, every access hits the chip
  uint32_t errs = 0, t0;
  // Walking bits on a few addresses
  static const uint32_t probe[] = {0, 4, 0x1000, 0x10000, 0x100000, 0x400000, __psram_size - 4};
  for (uint32_t a : probe) for (int b = 0; b < 32; b++) {
    uint32_t v = 1u << b; u[a/4] = v; if (u[a/4] != v) errs++;
    u[a/4] = ~v; if (u[a/4] != ~v) errs++;
  }
  Serial.printf("[psram] walking bits: %lu errors\n", (unsigned long)errs);
  // Address-in-address over the whole chip, then inverted (catches address-line shorts/opens)
  for (int pass = 0; pass < 2; pass++) {
    uint32_t x = pass ? 0xFFFFFFFF : 0, e = 0, first = 0xFFFFFFFF;
    t0 = micros(); for (uint32_t i = 0; i < n; i++) u[i] = (i * 4) ^ x; uint32_t tw = micros() - t0;
    t0 = micros();
    for (uint32_t i = 0; i < n; i++) { uint32_t r = u[i]; if (r != ((i * 4) ^ x)) { if (!e) first = i * 4; e++; } }
    uint32_t tr = micros() - t0;
    Serial.printf("[psram] addr-in-addr pass %d: %lu bad words of %lu (first at 0x%06lX)  write %.1f MB/s  read %.1f MB/s\n",
      pass, (unsigned long)e, (unsigned long)n, (unsigned long)first, __psram_size / (float)tw, __psram_size / (float)tr);
    errs += e;
  }
  // Random byte-read latency (the CoCo access pattern): uncached
  volatile uint8_t *b = (volatile uint8_t *)0x15000000; uint32_t s = 1, acc = 0;
  t0 = micros(); for (int i = 0; i < 200000; i++) { s = s * 1664525 + 1013904223; acc += b[s & 0x1FFFF]; }
  uint32_t tl = micros() - t0;
  Serial.printf("[psram] uncached random byte read in 128 KB: %.0f ns each (%lu)\n", tl * 1000.0 / 200000, (unsigned long)acc & 1);
  Serial.printf("[psram] RESULT: %s\n", errs ? "FAIL" : "PASS");
}

void setup() { Serial.begin(115200); uint32_t t = millis(); while (!Serial && millis() - t < 8000) {} delay(500); run(); }
void loop() { if (Serial.available()) { while (Serial.available()) Serial.read(); run(); } delay(50); }
