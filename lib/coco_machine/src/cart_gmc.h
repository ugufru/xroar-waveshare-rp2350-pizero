// cart_gmc.h: bank switching for cartridges over 16 KB (PIZERO-142).
//
// The cartridge window ($C000-$FEFF) is 16 KB, so bigger cartridges switch
// 16 KB banks into it. This follows XRoar's Games Master Cartridge
// (~/github/xroar/src/gmc.c), which XRoar uses for every image over 16 KB:
//   * reads of $C000-$FEFF come from rom[bank | (A & 0x3FFF)]
//   * a write to an EVEN address in $FF40-$FF5F selects the bank:
//     bank = (D << 14) & ((size - 1) & 0x3C000), so up to sixteen banks
//   * a write to an ODD address there goes to its SN76489 sound chip
//   * reset selects bank 0
// Pure logic, host-tested; the machine keeps the image and the pointer.

#ifndef CART_GMC_H
#define CART_GMC_H

#include <stdbool.h>
#include <stdint.h>

#define CART_BANK_SIZE   16384u
#define CART_BANKED_MAX  (16u * CART_BANK_SIZE)   // 256 KB: the mask's reach

// A banked image: a power of two from 32 KB to 256 KB. Anything that is not
// a power of two would leave the mask pointing past the end of the image.
static inline bool cart_is_banked_size(uint32_t n) {
    return n >= 2 * CART_BANK_SIZE && n <= CART_BANKED_MAX && (n & (n - 1)) == 0;
}

// Byte offset of the bank a write of D selects, for an image of len bytes.
static inline uint32_t gmc_bank_offset(uint8_t d, uint32_t len) {
    return ((uint32_t)d << 14) & ((len - 1) & 0x3C000u);
}

// Which of the GMC's two registers an $FF40-$FF5F write reaches.
static inline bool gmc_is_bank_register(uint16_t a) { return (a & 1) == 0; }

// PIZERO-143: the SN76489 is available all the time, not only with a GMC in
// (a settings switch will turn that off). Without a banked cart, $FF40-$FF5F
// belongs to the disk controller: A3 = 0 is its drive latch, which repeats
// across $FF40-$FF47 (and $FF50-$FF57); A3 = 1 is the WD279x at $FF48-$FF4B.
// Disk BASIC and OS-9 write the latch only at $FF40 itself, so its ODD
// mirrors ($FF41, 43, 45, 47 and 51-57) go to the chip instead. That puts it
// at $FF41, where GMC software writes it, with Disk BASIC still working.
static inline bool csg_on_latch_mirror(uint16_t a) {
    return (a & 0x09) == 0x01;               // odd, and in the latch half
}

#endif  // CART_GMC_H
