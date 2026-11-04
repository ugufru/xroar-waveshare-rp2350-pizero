// PIZERO-320: host tests for the DMK sector lookup.
//
// The images here are built the way a WD2797 formats a double-density track
// (gaps, A1 A1 A1 sync, ID field, data field, CRCs), with the sector order
// interleaved as Disk BASIC's DSKINI leaves it, so a lookup that assumed
// sectors in order would fail.
//
//   pio test -e native -f test_dmk

#include <unity.h>

#include <cstring>
#include <vector>

#include "../../src/dmk.h"

void setUp(void) {}
void tearDown(void) {}

static const unsigned TRACK_LEN = 0x1900;   // 6400, the usual 5.25" DD length

struct img {
    std::vector<uint8_t> b;
};

static unsigned img_read(void *ctx, uint32_t off, uint8_t *buf, unsigned n) {
    img *im = (img *)ctx;
    if (off >= im->b.size()) return 0;
    if (off + n > im->b.size()) n = (unsigned)(im->b.size() - off);
    memcpy(buf, &im->b[off], n);
    return n;
}

// The byte a sector's data is filled with, so each one can be told apart.
static uint8_t fill(unsigned track, unsigned side, unsigned sector) {
    return (uint8_t)(track * 7 + side * 50 + sector);
}

// One formatted track, sectors 1-18 in Disk BASIC's interleave.
static void put_track(uint8_t *t, unsigned track, unsigned side, bool f8_on_5 = false) {
    static const uint8_t order[18] = { 1, 12, 5, 16, 9, 2, 13, 6, 17, 10, 3, 14, 7, 18, 11, 4, 15, 8 };
    memset(t, 0, TRACK_LEN);
    unsigned p = 128;
    for (int i = 0; i < 32; i++) t[p++] = 0x4E;            // gap 1
    for (int s = 0; s < 18; s++) {
        for (int i = 0; i < 8; i++) t[p++] = 0x00;
        for (int i = 0; i < 3; i++) t[p++] = 0xA1;
        uint16_t ptr = (uint16_t)(p | DMK_IDAM_DD);
        t[2 * s] = (uint8_t)ptr;
        t[2 * s + 1] = (uint8_t)(ptr >> 8);
        uint8_t id[5] = { 0xFE, (uint8_t)track, (uint8_t)side, order[s], 1 };
        uint16_t crc = 0xFFFF;
        for (int i = 0; i < 3; i++) crc = dmk_crc_byte(crc, 0xA1);
        for (int i = 0; i < 5; i++) { crc = dmk_crc_byte(crc, id[i]); t[p++] = id[i]; }
        t[p++] = (uint8_t)(crc >> 8);
        t[p++] = (uint8_t)crc;
        for (int i = 0; i < 22; i++) t[p++] = 0x4E;        // gap 2
        for (int i = 0; i < 12; i++) t[p++] = 0x00;
        for (int i = 0; i < 3; i++) t[p++] = 0xA1;
        uint8_t mark = (f8_on_5 && order[s] == 5) ? 0xF8 : 0xFB;
        t[p++] = mark;
        uint8_t data[256];
        memset(data, fill(track, side, order[s]), sizeof data);
        memcpy(t + p, data, 256);
        p += 256;
        crc = dmk_data_crc(mark, data, 256);
        t[p++] = (uint8_t)(crc >> 8);
        t[p++] = (uint8_t)crc;
        for (int i = 0; i < 24; i++) t[p++] = 0x4E;        // gap 3
    }
    TEST_ASSERT_TRUE(p <= TRACK_LEN);
}

static img make_image(unsigned tracks, unsigned sides, uint8_t b0 = 0, uint8_t b11 = 0) {
    img im;
    im.b.assign(DMK_HEADER + (size_t)tracks * sides * TRACK_LEN, 0);
    im.b[0] = b0;
    im.b[1] = (uint8_t)tracks;
    im.b[2] = (uint8_t)TRACK_LEN;
    im.b[3] = (uint8_t)(TRACK_LEN >> 8);
    im.b[4] = sides == 1 ? 0x10 : 0x00;
    im.b[11] = b11;
    for (unsigned t = 0; t < tracks; t++)
        for (unsigned s = 0; s < sides; s++)
            put_track(&im.b[DMK_HEADER + (t * sides + s) * TRACK_LEN], t, s);
    return im;
}

static void test_crc_matches_the_wd279x(void) {
    // A1 A1 A1 from $FFFF is $CDB4, the value every DD field starts from.
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 3; i++) crc = dmk_crc_byte(crc, 0xA1);
    TEST_ASSERT_EQUAL_HEX16(0xCDB4, crc);
    // CRC-16/CCITT-FALSE of "123456789" is $29B1.
    crc = 0xFFFF;
    for (const char *s = "123456789"; *s; s++) crc = dmk_crc_byte(crc, (uint8_t)*s);
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc);
}

static void test_header_parses(void) {
    img im = make_image(35, 1);
    struct dmk_geom g;
    TEST_ASSERT_EQUAL(DMK_OK, dmk_parse_header(&im.b[0], (uint32_t)im.b.size(), &g));
    TEST_ASSERT_EQUAL(35, g.tracks);
    TEST_ASSERT_EQUAL(1, g.sides);
    TEST_ASSERT_EQUAL(TRACK_LEN, g.track_len);
    TEST_ASSERT_EQUAL(0, g.protect);
    TEST_ASSERT_TRUE(dmk_sniff(&im.b[0], (uint32_t)im.b.size()));
}

static void test_header_protect_bytes(void) {
    struct dmk_geom g;
    img a = make_image(1, 1, 0xFF, 0x00);
    TEST_ASSERT_EQUAL(DMK_OK, dmk_parse_header(&a.b[0], (uint32_t)a.b.size(), &g));
    TEST_ASSERT_EQUAL(1, g.protect);                       // no write back
    img b = make_image(1, 1, 0x00, 0xFF);
    TEST_ASSERT_EQUAL(DMK_OK, dmk_parse_header(&b.b[0], (uint32_t)b.b.size(), &g));
    TEST_ASSERT_EQUAL(1, g.protect);                       // the override
}

static void test_header_rejects(void) {
    struct dmk_geom g;
    uint8_t h[16] = { 0, 35, 0x00, 0x10, 0x10 };           // track length $1000, fine
    TEST_ASSERT_EQUAL(DMK_SHORT, dmk_parse_header(h, 15, &g));
    TEST_ASSERT_EQUAL(DMK_SHORT, dmk_parse_header(h, 16 + 0x0FFF, &g));
    TEST_ASSERT_EQUAL(DMK_OK, dmk_parse_header(h, 16 + 0x1000, &g));
    h[3] = 0x01;                                           // $0100: too short a track
    TEST_ASSERT_EQUAL(DMK_BAD_LENGTH, dmk_parse_header(h, 1 << 20, &g));
    h[3] = 0x30;                                           // $3000: too long
    TEST_ASSERT_EQUAL(DMK_BAD_LENGTH, dmk_parse_header(h, 1 << 20, &g));
    h[3] = 0x10; h[1] = 0;
    TEST_ASSERT_EQUAL(DMK_NO_TRACKS, dmk_parse_header(h, 1 << 20, &g));
}

static void test_sniff_rejects_a_jvc(void) {
    // A blank DECB disk: every byte $FF, track length $FFFF.
    std::vector<uint8_t> jvc(161280, 0xFF);
    TEST_ASSERT_FALSE(dmk_sniff(&jvc[0], (uint32_t)jvc.size()));
    // Zeros: track length 0.
    std::vector<uint8_t> z(161280, 0x00);
    TEST_ASSERT_FALSE(dmk_sniff(&z[0], (uint32_t)z.size()));
    // A real DMK whose size does not match its header is not sniffed as one.
    img im = make_image(2, 1);
    TEST_ASSERT_FALSE(dmk_sniff(&im.b[0], (uint32_t)im.b.size() - 1));
}

static void test_every_sector_found_in_interleave(void) {
    img im = make_image(35, 1);
    struct dmk_geom g;
    dmk_parse_header(&im.b[0], (uint32_t)im.b.size(), &g);
    struct dmk_map m;
    for (unsigned t = 0; t < 35; t++) {
        TEST_ASSERT_EQUAL(18, dmk_build_map(&g, t, 0, img_read, &im, &m));
        for (unsigned s = 1; s <= 18; s++) {
            uint8_t mark = 0;
            uint32_t off = dmk_map_find(&g, &m, s, &mark);
            TEST_ASSERT_NOT_EQUAL(0, off);
            TEST_ASSERT_EQUAL_HEX8(0xFB, mark);
            for (unsigned i = 0; i < 256; i++) TEST_ASSERT_EQUAL_HEX8(fill(t, 0, s), im.b[off + i]);
            // And the CRC after it is the data field's.
            uint16_t crc = dmk_data_crc(0xFB, &im.b[off], 256);
            TEST_ASSERT_EQUAL_HEX8(crc >> 8, im.b[off + 256]);
            TEST_ASSERT_EQUAL_HEX8(crc & 0xFF, im.b[off + 257]);
        }
        TEST_ASSERT_EQUAL(0u, dmk_map_find(&g, &m, 19, nullptr));
        TEST_ASSERT_EQUAL(0u, dmk_map_find(&g, &m, 0, nullptr));
    }
}

static void test_double_sided_reads_each_side(void) {
    img im = make_image(40, 2);
    struct dmk_geom g;
    TEST_ASSERT_EQUAL(DMK_OK, dmk_parse_header(&im.b[0], (uint32_t)im.b.size(), &g));
    TEST_ASSERT_EQUAL(2, g.sides);
    struct dmk_map m;
    TEST_ASSERT_EQUAL(18, dmk_build_map(&g, 39, 1, img_read, &im, &m));
    uint32_t off = dmk_map_find(&g, &m, 7, nullptr);
    TEST_ASSERT_EQUAL_HEX8(fill(39, 1, 7), im.b[off]);
    TEST_ASSERT_EQUAL(18, dmk_build_map(&g, 39, 0, img_read, &im, &m));
    off = dmk_map_find(&g, &m, 7, nullptr);
    TEST_ASSERT_EQUAL_HEX8(fill(39, 0, 7), im.b[off]);
    TEST_ASSERT_EQUAL(-1, dmk_build_map(&g, 40, 0, img_read, &im, &m));
    TEST_ASSERT_EQUAL(-1, dmk_build_map(&g, 0, 2, img_read, &im, &m));
}

static void test_deleted_mark_is_kept(void) {
    img im = make_image(1, 1);
    put_track(&im.b[DMK_HEADER], 0, 0, true);
    struct dmk_geom g;
    dmk_parse_header(&im.b[0], (uint32_t)im.b.size(), &g);
    struct dmk_map m;
    TEST_ASSERT_EQUAL(18, dmk_build_map(&g, 0, 0, img_read, &im, &m));
    uint8_t mark = 0;
    TEST_ASSERT_NOT_EQUAL(0, dmk_map_find(&g, &m, 5, &mark));
    TEST_ASSERT_EQUAL_HEX8(0xF8, mark);
    dmk_map_find(&g, &m, 6, &mark);
    TEST_ASSERT_EQUAL_HEX8(0xFB, mark);
}

static void test_single_density_and_odd_sizes_are_left_out(void) {
    img im = make_image(1, 1);
    uint8_t *t = &im.b[DMK_HEADER];
    // Pointer 0 (sector 1) to single density, pointer 1 (sector 12) to 512 bytes.
    t[1] &= 0x7F;
    unsigned p1 = (t[2] | (t[3] << 8)) & DMK_IDAM_OFF;
    t[p1 + 4] = 2;
    struct dmk_geom g;
    dmk_parse_header(&im.b[0], (uint32_t)im.b.size(), &g);
    struct dmk_map m;
    TEST_ASSERT_EQUAL(16, dmk_build_map(&g, 0, 0, img_read, &im, &m));
    TEST_ASSERT_EQUAL(0u, dmk_map_find(&g, &m, 1, nullptr));
    TEST_ASSERT_EQUAL(0u, dmk_map_find(&g, &m, 12, nullptr));
    TEST_ASSERT_NOT_EQUAL(0u, dmk_map_find(&g, &m, 5, nullptr));
}

static void test_bad_pointers_and_missing_marks_are_skipped(void) {
    img im = make_image(1, 1);
    uint8_t *t = &im.b[DMK_HEADER];
    // Sector 1's pointer into the pointer table; sector 12's pointer past the
    // end; sector 5's data mark wiped.
    t[0] = 0x10; t[1] = 0x80;
    t[2] = 0xFF; t[3] = 0xBF;
    unsigned p2 = (t[4] | (t[5] << 8)) & DMK_IDAM_OFF;
    for (unsigned j = DMK_ID_LEN; j < DMK_ID_LEN + DMK_DAM_WINDOW; j++)
        if (t[p2 + j] == 0xFB) t[p2 + j] = 0x00;
    struct dmk_geom g;
    dmk_parse_header(&im.b[0], (uint32_t)im.b.size(), &g);
    struct dmk_map m;
    TEST_ASSERT_EQUAL(15, dmk_build_map(&g, 0, 0, img_read, &im, &m));
    TEST_ASSERT_EQUAL(0u, dmk_map_find(&g, &m, 1, nullptr));
    TEST_ASSERT_EQUAL(0u, dmk_map_find(&g, &m, 12, nullptr));
    TEST_ASSERT_EQUAL(0u, dmk_map_find(&g, &m, 5, nullptr));
}

static void test_duplicate_sector_first_wins(void) {
    img im = make_image(1, 1);
    uint8_t *t = &im.b[DMK_HEADER];
    // Make the second ID (sector 12) claim to be sector 1 too.
    unsigned p1 = (t[2] | (t[3] << 8)) & DMK_IDAM_OFF;
    t[p1 + 3] = 1;
    struct dmk_geom g;
    dmk_parse_header(&im.b[0], (uint32_t)im.b.size(), &g);
    struct dmk_map m;
    TEST_ASSERT_EQUAL(17, dmk_build_map(&g, 0, 0, img_read, &im, &m));
    uint32_t off = dmk_map_find(&g, &m, 1, nullptr);
    TEST_ASSERT_EQUAL_HEX8(fill(0, 0, 1), im.b[off]);
}

static void test_a_short_last_track_does_not_read_past_the_file(void) {
    img im = make_image(2, 1);
    im.b.resize(im.b.size() - TRACK_LEN / 2);               // half of track 1 gone
    struct dmk_geom g;
    TEST_ASSERT_EQUAL(DMK_OK, dmk_parse_header(&im.b[0], (uint32_t)im.b.size(), &g));
    struct dmk_map m;
    int n = dmk_build_map(&g, 1, 0, img_read, &im, &m);
    TEST_ASSERT_TRUE(n >= 0 && n < 18);
    for (unsigned k = 0; k < m.n; k++) {
        uint32_t off = dmk_map_find(&g, &m, m.id[k], nullptr);
        TEST_ASSERT_TRUE(off + 258 <= im.b.size());
    }
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_crc_matches_the_wd279x);
    RUN_TEST(test_header_parses);
    RUN_TEST(test_header_protect_bytes);
    RUN_TEST(test_header_rejects);
    RUN_TEST(test_sniff_rejects_a_jvc);
    RUN_TEST(test_every_sector_found_in_interleave);
    RUN_TEST(test_double_sided_reads_each_side);
    RUN_TEST(test_deleted_mark_is_kept);
    RUN_TEST(test_single_density_and_odd_sizes_are_left_out);
    RUN_TEST(test_bad_pointers_and_missing_marks_are_skipped);
    RUN_TEST(test_duplicate_sector_first_wins);
    RUN_TEST(test_a_short_last_track_does_not_read_past_the_file);
    return UNITY_END();
}
