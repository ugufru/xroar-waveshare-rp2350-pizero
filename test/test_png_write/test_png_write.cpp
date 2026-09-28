// PIZERO-165: host tests for the streaming PNG writer.
//
//   pio test -e native

#include <unity.h>

#include <cstring>
#include <vector>

#include "../../src/png_write.h"

void setUp(void) {}
void tearDown(void) {}

static std::vector<uint8_t> g_out;
static void sink(void *, const uint8_t *p, size_t n) { g_out.insert(g_out.end(), p, p + n); }

struct img { int w, h; };
static void row_pattern(void *ctx, int y, uint8_t *out) {
    img *im = (img *)ctx;
    for (int x = 0; x < im->w; x++) out[x] = (uint8_t)((x + 3 * y) % 5);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Parse what the writer made, checking everything a PNG reader would, and
// return the unfiltered pixel indexes.
static std::vector<uint8_t> decode(int w, int h, int npal) {
    const uint8_t *p = g_out.data();
    size_t n = g_out.size();
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    TEST_ASSERT_EQUAL_MEMORY(sig, p, 8);
    size_t i = 8;
    std::vector<uint8_t> idat;
    int chunks = 0;
    bool iend = false;
    while (i + 12 <= n) {
        uint32_t len = be32(p + i);
        const uint8_t *type = p + i + 4, *data = p + i + 8;
        uint32_t crc = png_crc_update(0xFFFFFFFFu, type, 4 + len) ^ 0xFFFFFFFFu;
        TEST_ASSERT_EQUAL_HEX32(crc, be32(data + len));
        if (!memcmp(type, "IHDR", 4)) {
            TEST_ASSERT_EQUAL_UINT32(13, len);
            TEST_ASSERT_EQUAL_UINT32((uint32_t)w, be32(data));
            TEST_ASSERT_EQUAL_UINT32((uint32_t)h, be32(data + 4));
            TEST_ASSERT_EQUAL_UINT8(8, data[8]);                 // bit depth
            TEST_ASSERT_EQUAL_UINT8(3, data[9]);                 // indexed
        } else if (!memcmp(type, "PLTE", 4)) {
            TEST_ASSERT_EQUAL_UINT32((uint32_t)npal * 3u, len);
        } else if (!memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), data, data + len);
        } else if (!memcmp(type, "IEND", 4)) {
            iend = true;
        }
        i += 12 + len;
        chunks++;
    }
    TEST_ASSERT_TRUE(iend);
    TEST_ASSERT_EQUAL_size_t(n, i);                              // nothing after IEND
    // zlib: header, stored blocks, Adler-32.
    TEST_ASSERT_EQUAL_UINT8(0x78, idat[0]);
    TEST_ASSERT_EQUAL_UINT(0, ((idat[0] << 8) | idat[1]) % 31);
    std::vector<uint8_t> raw;
    size_t k = 2;
    bool final = false;
    while (!final) {
        final = idat[k] & 1;
        TEST_ASSERT_EQUAL_UINT8(0, (idat[k] >> 1) & 3);          // stored
        uint16_t len = (uint16_t)(idat[k + 1] | (idat[k + 2] << 8));
        uint16_t nlen = (uint16_t)(idat[k + 3] | (idat[k + 4] << 8));
        TEST_ASSERT_EQUAL_HEX16((uint16_t)~len, nlen);
        raw.insert(raw.end(), idat.begin() + (long)k + 5, idat.begin() + (long)k + 5 + len);
        k += 5u + len;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    TEST_ASSERT_EQUAL_HEX32((b << 16) | a, be32(&idat[k]));
    TEST_ASSERT_EQUAL_size_t(idat.size(), k + 4);
    TEST_ASSERT_EQUAL_size_t((size_t)h * (size_t)(w + 1), raw.size());
    std::vector<uint8_t> px;
    for (int y = 0; y < h; y++) {
        TEST_ASSERT_EQUAL_UINT8(0, raw[(size_t)y * (size_t)(w + 1)]);   // filter "None"
        px.insert(px.end(), raw.begin() + (long)y * (w + 1) + 1, raw.begin() + (long)(y + 1) * (w + 1));
    }
    return px;
}

static void check(int w, int h) {
    g_out.clear();
    uint8_t pal[5 * 3] = { 0,0,0, 255,0,0, 0,255,0, 0,0,255, 255,255,255 };
    img im = { w, h };
    std::vector<uint8_t> row((size_t)w);
    size_t n = png_write_indexed(sink, nullptr, w, h, pal, 5, row_pattern, &im, row.data());
    TEST_ASSERT_EQUAL_size_t(g_out.size(), n);
    std::vector<uint8_t> px = decode(w, h, 5);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            TEST_ASSERT_EQUAL_UINT8((x + 3 * y) % 5, px[(size_t)y * (size_t)w + (size_t)x]);
}

static void test_crc_matches_the_png_standard(void) {
    // Every PNG ends with an IEND chunk whose CRC is AE 42 60 82.
    const uint8_t iend[4] = { 'I', 'E', 'N', 'D' };
    TEST_ASSERT_EQUAL_HEX32(0xAE426082u, png_crc_update(0xFFFFFFFFu, iend, 4) ^ 0xFFFFFFFFu);
}

static void test_a_small_image_round_trips(void) { check(7, 3); }

static void test_the_screen_size_crosses_a_stored_block_boundary(void) {
    // 320x240 is 77,040 raw bytes: two stored blocks, the first exactly 65535.
    check(320, 240);
}

static void test_rgb565_white_stays_white(void) {
    uint8_t c[3];
    png_rgb565_to_888(0xFFFF, c);
    TEST_ASSERT_EQUAL_UINT8(255, c[0]); TEST_ASSERT_EQUAL_UINT8(255, c[1]); TEST_ASSERT_EQUAL_UINT8(255, c[2]);
    png_rgb565_to_888(0x0000, c);
    TEST_ASSERT_EQUAL_UINT8(0, c[0]);
}

static void test_palette_builds_and_fills(void) {
    uint16_t pal[3]; int n = 0;
    TEST_ASSERT_EQUAL_INT(0, png_palette_index(pal, &n, 3, 0x1234));
    TEST_ASSERT_EQUAL_INT(1, png_palette_index(pal, &n, 3, 0xFFFF));
    TEST_ASSERT_EQUAL_INT(0, png_palette_index(pal, &n, 3, 0x1234));
    TEST_ASSERT_EQUAL_INT(2, png_palette_index(pal, &n, 3, 0x0001));
    TEST_ASSERT_EQUAL_INT(-1, png_palette_index(pal, &n, 3, 0x0002));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_crc_matches_the_png_standard);
    RUN_TEST(test_a_small_image_round_trips);
    RUN_TEST(test_the_screen_size_crosses_a_stored_block_boundary);
    RUN_TEST(test_rgb565_white_stays_white);
    RUN_TEST(test_palette_builds_and_fills);
    return UNITY_END();
}
