// usb_audio.cpp: see usb_audio.h (PIZERO-255).

#include <Arduino.h>
#include "pio_usb.h"
#include "Adafruit_TinyUSB.h"
#include "usb_audio.h"

#ifdef USB_AUDIO

// What the search found: the playback interface and alternate setting,
// its isochronous OUT endpoint descriptor, the class version and, for
// UAC2, the clock source that sets its rate.
struct uac_out {
    bool    found;
    bool    uac2;
    uint8_t ac_itf;        // the AudioControl interface (UAC2 clock requests)
    uint8_t itf, alt;
    uint8_t clock_id;      // UAC2: the clock source feeding the terminal
    uint8_t ep[7];         // the endpoint descriptor, as TinyUSB opens it
    // The feature unit feeding the analog output, and its channels (bit 0
    // master, bits 1-2 left and right) with a mute or a volume control.
    uint8_t fu_id, fu_mute, fu_vol;
};

static uint8_t  g_ua_buf[512];
static uint8_t  g_ua_daddr;          // the streaming device, 0 when none
static bool     g_ua_busy;           // a device's setup in progress
static struct uac_out g_ua;
static uint8_t  g_ua_rate[4];        // 48,000, little-endian
static const char *g_ua_state = "none";
static bool     g_ua_enabled = true; // the usb_audio setting
static bool     g_ua_ready;          // set up, the endpoint open: can stream
static bool     g_ua_streaming;

// UAC1 and UAC2 both: a type I PCM format, 2 channels of 16 bits in 2-byte
// slots, on an isochronous OUT endpoint. UAC1 lists its rates in the format
// descriptor and must offer 48 kHz; UAC2 rates belong to the clock source,
// which is set to 48 kHz and checked by the request's result.
static bool ua_find(const uint8_t *p, uint16_t len, struct uac_out *o) {
    uint8_t cls = 0, sub = 0, itf = 0, alt = 0;
    uint8_t term_clock[16] = {0};        // UAC2 input terminal id -> clock id
    uint8_t link = 0, channels = 0, slot = 0, bits = 0;
    bool rate_ok = false;
    uint8_t fu_mute[16] = {0}, fu_vol[16] = {0}, out_src = 0;
    bool fu[16] = {false};
    memset(o, 0, sizeof *o);
    for (uint16_t i = 0; i + 2 <= len && p[i] >= 2 && i + p[i] <= len; i += p[i]) {
        const uint8_t *d = p + i;
        uint8_t dl = d[0], ty = d[1];
        if (ty == 0x04 && dl >= 9) {
            itf = d[2]; alt = d[3]; cls = d[5]; sub = d[6];
            if (cls == 1 && sub == 1) { o->ac_itf = itf; o->uac2 = d[7] == 0x20; }
            link = channels = slot = bits = 0;
            rate_ok = false;
        } else if (ty == 0x24 && cls == 1 && sub == 1 && dl >= 8 && d[2] == 0x02 && o->uac2) {
            if (d[3] < 16) term_clock[d[3]] = d[7];             // UAC2 input terminal's clock
        } else if (ty == 0x24 && cls == 1 && sub == 1 && dl >= 8 && d[2] == 0x03) {
            uint16_t tt = (uint16_t)(d[4] | d[5] << 8);         // output terminal
            if (tt != 0x0101 && !out_src) out_src = d[7];        // the analog one, not USB streaming
        } else if (ty == 0x24 && cls == 1 && sub == 1 && dl >= 7 && d[2] == 0x06 && d[3] < 16) {
            uint8_t id = d[3];                                  // feature unit: master, ch1, ch2
            fu[id] = true;
            for (uint8_t ch = 0; ch < 3; ch++) {
                if (o->uac2) {                                  // 4 bytes a channel, 2 bits a control
                    uint16_t at = (uint16_t)(5 + 4 * ch);
                    if (at >= dl) break;
                    if (d[at] & 0x03) fu_mute[id] |= (uint8_t)(1u << ch);
                    if (d[at] & 0x0C) fu_vol[id] |= (uint8_t)(1u << ch);
                } else {                                        // bControlSize bytes, 1 bit a control
                    uint8_t n = d[5];
                    uint16_t at = (uint16_t)(6 + n * ch);
                    if (!n || at >= dl - 1) break;
                    if (d[at] & 0x01) fu_mute[id] |= (uint8_t)(1u << ch);
                    if (d[at] & 0x02) fu_vol[id] |= (uint8_t)(1u << ch);
                }
            }
        } else if (ty == 0x24 && cls == 1 && sub == 2 && d[2] == 0x01 && dl >= 4) {
            link = d[3];                                        // AS_GENERAL: terminal link
            if (o->uac2 && dl >= 11) channels = d[10];
        } else if (ty == 0x24 && cls == 1 && sub == 2 && d[2] == 0x02 && dl >= 6 && d[3] == 1) {
            if (!o->uac2 && dl >= 8) {                          // UAC1 type I
                channels = d[4]; slot = d[5]; bits = d[6];
                uint8_t n = d[7];
                if (n == 0 && dl >= 14) {
                    uint32_t lo = d[8] | d[9] << 8 | d[10] << 16, hi = d[11] | d[12] << 8 | d[13] << 16;
                    rate_ok = lo <= 48000 && 48000 <= hi;
                }
                for (uint8_t k = 0; k < n && 8 + 3 * k + 2 < dl; k++)
                    if ((uint32_t)(d[8+3*k] | d[9+3*k] << 8 | d[10+3*k] << 16) == 48000) rate_ok = true;
            } else if (o->uac2) {                               // UAC2 type I
                slot = d[4]; bits = d[5];
                rate_ok = true;
            }
        } else if (ty == 0x05 && dl >= 7 && cls == 1 && sub == 2 && alt) {
            bool iso_out = !(d[2] & 0x80) && (d[3] & 3) == 1;
            if (iso_out && channels == 2 && slot == 2 && bits == 16 && rate_ok && !o->found) {
                o->found = true;
                o->itf = itf;
                o->alt = alt;
                memcpy(o->ep, d, 7);
                o->clock_id = (o->uac2 && link < 16) ? term_clock[link] : 0;
            }
        }
    }
    if (out_src < 16 && fu[out_src]) {
        o->fu_id = out_src;
        o->fu_mute = fu_mute[out_src];
        o->fu_vol = fu_vol[out_src];
    }
    return o->found;
}

#ifdef USB_AUDIO_TONE
// 440 Hz at about -12 dBFS, the same on both sides, in RAM since the fill
// runs in the USB frame interrupt.
#define UA_SINE 256
static int16_t g_ua_sine[UA_SINE];
static uint32_t g_ua_ph, g_ua_inc;
static uint16_t __not_in_flash_func(ua_fill)(uint8_t *buf, uint16_t max) {
    uint16_t n = 48;
    if (n * 4 > max) n = max / 4;
    int16_t *o = (int16_t *)buf;
    for (uint16_t k = 0; k < n; k++) {
        int16_t s = g_ua_sine[g_ua_ph >> 24];
        g_ua_ph += g_ua_inc;
        o[2 * k] = s;
        o[2 * k + 1] = s;
    }
    return (uint16_t)(n * 4);
}
#endif

#ifndef USB_AUDIO_TONE
#define ua_fill usb_audio_fill       // the machine's sound (main.cpp)
#endif

// Start or stop the stream, and move the sound to match.
static void ua_stream(bool on) {
    if (on == g_ua_streaming) return;
    if (on) {
        if (!pio_usb_host_iso_out_stream(0, g_ua_daddr, g_ua.ep[2], ua_fill)) {
            g_ua_state = "stream not started";
            return;
        }
        g_ua_streaming = true;
        g_ua_state = "streaming";
        usb_audio_route(true);
    } else {
        pio_usb_host_iso_out_stream(0, 0, 0, NULL);
        g_ua_streaming = false;
        usb_audio_route(false);
    }
}

static void ua_fail(const char *what) {
    g_ua_state = what;
    Serial.printf("[uac] addr=%u %s\r\n", g_ua_daddr, what);
    g_ua_daddr = 0;
    g_ua_busy = false;
}

static void ua_start(void);

// PIZERO-255: the DAC's own volume. A chip powers up at a level of its own
// (the hub's at its top, 0 dB, which with the machine's TV-level sound is
// hot in headphones). Set every volume channel of the feature unit feeding
// the analog output to the usb_audio_db setting (default -10 dB, held to the
// chip's range) and its mute off. The range and the level it came up at are
// logged. A refused step is logged and the next one tried; the stream
// starts whatever happens. Values are 1/256 dB, signed.
static int16_t g_ua_db_q8 = -10 * 256;
#define UA_VOL_DB_Q8 g_ua_db_q8
static tusb_control_request_t g_vreq;
static uint8_t  g_vbuf[8];
static uint8_t  g_vstep;
static int16_t  g_vmin = -32768, g_vmax = 32767, g_vcur;
static bool     g_vcur_ok;

static bool ua_ctl(bool get, uint8_t req, uint8_t cs, uint8_t ch, uint16_t len, tuh_xfer_cb_t cb) {
    g_vreq.bmRequestType = get ? 0xA1 : 0x21;          // class, interface
    g_vreq.bRequest = req;
    g_vreq.wValue = (uint16_t)(cs << 8 | ch);
    g_vreq.wIndex = (uint16_t)(g_ua.fu_id << 8 | g_ua.ac_itf);
    g_vreq.wLength = len;
    tuh_xfer_t xfer = {};
    xfer.daddr = g_ua_daddr;
    xfer.ep_addr = 0;
    xfer.setup = &g_vreq;
    xfer.buffer = g_vbuf;
    xfer.complete_cb = cb;
    return tuh_control_xfer(&xfer);
}

static uint8_t ua_first(uint8_t mask) {
    for (uint8_t ch = 0; ch < 3; ch++) if (mask & (1u << ch)) return ch;
    return 0;
}

// The steps, one request each: 0 the range (UAC2 RANGE, or UAC1 MIN then
// MAX), 2 the current level, 3-5 set each volume channel, 6-8 unmute each
// mute channel, 9 done.
static void ua_vol_next(tuh_xfer_t *x);

static void ua_vol_run(void) {
    for (;;) {
        uint8_t st = g_vstep++;
        bool sent = false;
        uint8_t vch = ua_first(g_ua.fu_vol);
        if (st == 0 && g_ua.fu_vol) {
            sent = g_ua.uac2 ? ua_ctl(true, 0x02, 0x02, vch, 8, ua_vol_next)    // RANGE
                             : ua_ctl(true, 0x82, 0x02, vch, 2, ua_vol_next);   // GET_MIN
        } else if (st == 1 && g_ua.fu_vol && !g_ua.uac2) {
            sent = ua_ctl(true, 0x83, 0x02, vch, 2, ua_vol_next);               // GET_MAX
        } else if (st == 2 && g_ua.fu_vol) {
            sent = ua_ctl(true, g_ua.uac2 ? 0x01 : 0x81, 0x02, vch, 2, ua_vol_next);   // CUR
        } else if (st >= 3 && st <= 5 && (g_ua.fu_vol & (1u << (st - 3)))) {
            int16_t v = UA_VOL_DB_Q8;
            if (v > g_vmax) v = g_vmax;
            if (v < g_vmin) v = g_vmin;
            g_vbuf[0] = (uint8_t)v; g_vbuf[1] = (uint8_t)((uint16_t)v >> 8);
            sent = ua_ctl(false, 0x01, 0x02, (uint8_t)(st - 3), 2, ua_vol_next);
        } else if (st >= 6 && st <= 8 && (g_ua.fu_mute & (1u << (st - 6)))) {
            g_vbuf[0] = 0;
            sent = ua_ctl(false, 0x01, 0x01, (uint8_t)(st - 6), 1, ua_vol_next);
        } else if (st >= 9) {
            int16_t set = UA_VOL_DB_Q8 > g_vmax ? g_vmax : UA_VOL_DB_Q8 < g_vmin ? g_vmin : UA_VOL_DB_Q8;
            if (g_ua.fu_vol)
                Serial.printf("[uac] addr=%u volume: range %d.%02u to %d.%02u dB, came up at %s%d.%02u dB, set to %d.%02u dB\r\n",
                              g_ua_daddr, g_vmin / 256, (unsigned)(abs(g_vmin) % 256) * 100 / 256,
                              g_vmax / 256, (unsigned)(abs(g_vmax) % 256) * 100 / 256,
                              g_vcur_ok ? "" : "? ", g_vcur / 256, (unsigned)(abs(g_vcur) % 256) * 100 / 256,
                              set / 256, (unsigned)(abs(set) % 256) * 100 / 256);
            ua_start();
            return;
        } else {
            continue;                                   // a step this unit has no control for
        }
        if (sent) return;
        Serial.printf("[uac] addr=%u volume step %u not sent\r\n", g_ua_daddr, st);
    }
}

static void ua_vol_next(tuh_xfer_t *x) {
    uint8_t st = (uint8_t)(g_vstep - 1);
    if (x->result != XFER_RESULT_SUCCESS) {
        Serial.printf("[uac] addr=%u volume step %u refused (%d)\r\n", g_ua_daddr, st, (int)x->result);
    } else if (st == 0) {
        if (g_ua.uac2) {                                // wNumSubRanges, then MIN, MAX, RES
            g_vmin = (int16_t)(g_vbuf[2] | g_vbuf[3] << 8);
            g_vmax = (int16_t)(g_vbuf[4] | g_vbuf[5] << 8);
        } else {
            g_vmin = (int16_t)(g_vbuf[0] | g_vbuf[1] << 8);
        }
    } else if (st == 1) {
        g_vmax = (int16_t)(g_vbuf[0] | g_vbuf[1] << 8);
    } else if (st == 2) {
        g_vcur = (int16_t)(g_vbuf[0] | g_vbuf[1] << 8);
        g_vcur_ok = true;
    }
    ua_vol_run();
}

static void ua_rate_done(tuh_xfer_t *x) {
    if (x->result != XFER_RESULT_SUCCESS)
        Serial.printf("[uac] addr=%u rate request not taken (%d), going on\r\n", g_ua_daddr, (int)x->result);
    g_vstep = 0;
    g_vmin = -32768; g_vmax = 32767; g_vcur = 0; g_vcur_ok = false;
    if (g_ua.fu_id) { ua_vol_run(); return; }
    ua_start();
}

static void ua_start(void) {
    if (!tuh_edpt_open(g_ua_daddr, (tusb_desc_endpoint_t const *)g_ua.ep)) { ua_fail("endpoint open failed"); return; }
#ifdef USB_AUDIO_TONE
    for (int k = 0; k < UA_SINE; k++) g_ua_sine[k] = (int16_t)(8000.0 * sin(2 * M_PI * k / UA_SINE));
    g_ua_inc = (uint32_t)(440.0 * 4294967296.0 / 48000.0);
#endif
    g_ua_ready = true;
    g_ua_busy = false;
    g_ua_state = "ready, usb_audio off";
    if (g_ua_enabled) ua_stream(true);
    Serial.printf("[uac] addr=%u %s: UAC%u interface %u alt %u endpoint %02X max %u\r\n",
                  g_ua_daddr, g_ua_state, g_ua.uac2 ? 2 : 1, g_ua.itf, g_ua.alt, g_ua.ep[2], g_ua.ep[4] | g_ua.ep[5] << 8);
}

static void ua_alt_done(tuh_xfer_t *x) {
    if (x->result != XFER_RESULT_SUCCESS) { ua_fail("SET_INTERFACE failed"); return; }
    static tusb_control_request_t req;
    g_ua_rate[0] = 48000 & 0xFF; g_ua_rate[1] = (48000 >> 8) & 0xFF; g_ua_rate[2] = 48000 >> 16; g_ua_rate[3] = 0;
    if (g_ua.uac2) {
        // UAC2 CUR of the clock source's SAM_FREQ control, 4 bytes.
        req.bmRequestType = 0x21;
        req.bRequest = 0x01;
        req.wValue = 0x0100;
        req.wIndex = (uint16_t)(g_ua.clock_id << 8 | g_ua.ac_itf);
        req.wLength = 4;
    } else {
        // UAC1 SET_CUR of the endpoint's SAMPLING_FREQ control, 3 bytes.
        req.bmRequestType = 0x22;
        req.bRequest = 0x01;
        req.wValue = 0x0100;
        req.wIndex = g_ua.ep[2];
        req.wLength = 3;
    }
    tuh_xfer_t xfer = {};
    xfer.daddr = g_ua_daddr;
    xfer.ep_addr = 0;
    xfer.setup = &req;
    xfer.buffer = g_ua_rate;
    xfer.complete_cb = ua_rate_done;
    if (!tuh_control_xfer(&xfer)) ua_fail("rate request not sent");
}

static void ua_config_done(tuh_xfer_t *x) {
    uint8_t daddr = (uint8_t)x->user_data;
    if (x->result != XFER_RESULT_SUCCESS) { g_ua_busy = false; return; }
    uint16_t total = (uint16_t)(g_ua_buf[2] | (g_ua_buf[3] << 8));
    if (total > sizeof g_ua_buf) total = sizeof g_ua_buf;
    if (!ua_find(g_ua_buf, total, &g_ua)) { g_ua_busy = false; return; }   // not an audio device
    g_ua_daddr = daddr;
    g_ua_state = "setting up";
    Serial.printf("[uac] addr=%u UAC%u playback: interface %u alt %u endpoint %02X clock %u feature unit %u (mute %X volume %X)\r\n",
                  daddr, g_ua.uac2 ? 2 : 1, g_ua.itf, g_ua.alt, g_ua.ep[2], g_ua.clock_id,
                  g_ua.fu_id, g_ua.fu_mute, g_ua.fu_vol);
    if (!tuh_interface_set(daddr, g_ua.itf, g_ua.alt, ua_alt_done, 0)) ua_fail("SET_INTERFACE not sent");
}

void usb_audio_mount(uint8_t daddr) {
    if (g_ua_daddr || g_ua_busy) return;      // one DAC at a time
    g_ua_busy = tuh_descriptor_get_configuration(daddr, 0, g_ua_buf, sizeof g_ua_buf, ua_config_done, daddr);
}

void usb_audio_umount(uint8_t daddr) {
    if (daddr != g_ua_daddr) return;
    ua_stream(false);
    g_ua_daddr = 0;
    g_ua_ready = false;
    g_ua_state = "unplugged";
}

void usb_audio_set_db(int db) {
    if (db > 0) db = 0;
    if (db < -60) db = -60;
    g_ua_db_q8 = (int16_t)(db * 256);
}

// The setting can change while running (the settings editor): stop or
// restart the stream on the device already set up.
void usb_audio_enable(bool on) {
    g_ua_enabled = on;
    if (!g_ua_ready) return;
    ua_stream(on);
    if (!on) g_ua_state = "ready, usb_audio off";
}

void usb_audio_telemetry(uint32_t under) {
    static uint32_t last, last_under;
    uint32_t f = pio_usb_host_iso_frames();
    if (g_ua_daddr || f)
        Serial.printf("[uac] %s packets=%lu (%lu/s) under=%lu/s tx_timeouts=%lu\r\n", g_ua_state, (unsigned long)f,
                      (unsigned long)(f - last), (unsigned long)(under - last_under),
                      (unsigned long)pio_usb_bus_tx_wait_timeouts());   // PIZERO-322
    last = f;
    last_under = under;
}

#endif  // USB_AUDIO
