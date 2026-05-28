/*
 * Hardware configuration for the on-board microSD slot on the Waveshare
 * RP2350-PiZero. Pin map from the Waveshare RP2350-PiZero demo
 * (Arduino/03-MicroSD/hw_config.c) — PIZERO-01/08:
 *
 *   spi1 SCK  = GPIO 30
 *   spi1 MOSI = GPIO 31
 *   spi1 MISO = GPIO 40
 *        CS   = GPIO 43  (software-driven, plain GPIO)
 *   card-detect = GPIO 22 (unused here; SD is polled on mount)
 *
 * Uses the carlk3 no-OS-FatFS-SD v3.x API (sd_spi_if_t / SD_IF_SPI), same as
 * the AMOLED port so coco_boot.cpp's ff.h/f_util.h usage is unchanged.
 */

#include "hw_config.h"
#include "hardware/spi.h"

static spi_t g_sd_spi = {
    .hw_inst    = spi1,
    .miso_gpio  = 40,
    .mosi_gpio  = 31,
    .sck_gpio   = 30,
    .set_drive_strength       = true,
    .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_2MA,
    .sck_gpio_drive_strength  = GPIO_DRIVE_STRENGTH_12MA,
    .no_miso_gpio_pull_up     = true,
    .baud_rate  = 12500 * 1000,  // 12.5 MHz — matches Waveshare demo.
};

static sd_spi_if_t spi_if = {
    .spi    = &g_sd_spi,
    .ss_gpio = 43,
    .set_drive_strength     = true,
    .ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_2MA,
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
};

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num) {
    return (num == 0) ? &sd_card : NULL;
}
