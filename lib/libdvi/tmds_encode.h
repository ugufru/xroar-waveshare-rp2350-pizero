#ifndef _TMDS_ENCODE_H_
#define _TMDS_ENCODE_H_

#include "hardware/interp.h"
#include "dvi_config_defs.h"

// Functions from tmds_encode.c
void tmds_encode_data_channel_16bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint channel_msb, uint channel_lsb);
void tmds_encode_data_channel_8bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint channel_msb, uint channel_lsb);
void tmds_encode_data_channel_fullres_16bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint channel_msb, uint channel_lsb);
void tmds_setup_palette_symbols(const uint16_t *palette, uint32_t *symbuf, size_t n_palette);
void tmds_setup_palette24_symbols(const uint32_t *palette, uint32_t *symbuf, size_t n_palette);
void tmds_encode_palette_data(const uint32_t *pixbuf, const uint32_t *tmds_palette, uint32_t *symbuf, size_t n_pix, uint32_t palette_bits);

#if !PICO_RP2040
// PIZERO-175: the same 16bpp doubled encode as tmds_encode_data_channel_16bpp,
// all three lanes in one pass, on the RP2350's SIO hardware TMDS encoder.
// n_pix source pixels; lane i's symbols start at symbuf + i * lane_words.
void tmds_encode_16bpp_sio_doubled(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, size_t lane_words);
#endif

// Functions from tmds_encode.S. Assembly symbols are unmangled, so they need C
// linkage now that this file is compiled as C++ (PIZERO-175: only the two
// loops in use had it, so the fullres, palette and 1/2 bpp encoders could
// not link).
extern "C" {

void tmds_encode_1bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_encode_2bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);

// Uses interp0:
void tmds_encode_loop_16bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_encode_loop_16bpp_leftshift(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint leftshift);

// Uses interp0 and interp1:
void tmds_encode_loop_8bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_encode_loop_8bpp_leftshift(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint leftshift);

// Uses interp0 and interp1:
// (Note a copy is provided in scratch memories X and Y)
void tmds_fullres_encode_loop_16bpp_x(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_fullres_encode_loop_16bpp_y(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_fullres_encode_loop_16bpp_leftshift_x(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint leftshift);
void tmds_fullres_encode_loop_16bpp_leftshift_y(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint leftshift);
void tmds_palette_encode_loop_x(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_palette_encode_loop_y(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
}

#endif
