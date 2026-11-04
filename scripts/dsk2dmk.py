#!/usr/bin/env python3
"""Convert a headerless JVC .DSK (18 sectors of 256 bytes a track, single
sided) to a DMK, laid out as a WD2797 formats a double-density track, with
Disk BASIC's DSKINI sector interleave (PIZERO-320).

  python3 scripts/dsk2dmk.py TESTTEXT.DSK TESTTEXT.DMK

For testing the DMK path with a disk of our own: build one with
scripts/mktestdisk.py, convert it, and copy both to /coco/dsk. The layout
matches test/test_dmk's: gap 1 of 32 x $4E, then per sector 8 x $00,
A1 A1 A1, the ID field and its CRC, 22 x $4E, 12 x $00, A1 A1 A1, $FB,
the data and its CRC, 24 x $4E; the rest of the 6400-byte track is $4E."""
import sys

SEC, SPT = 256, 18
TRACK_LEN = 0x1900
ORDER = [1, 12, 5, 16, 9, 2, 13, 6, 17, 10, 3, 14, 7, 18, 11, 4, 15, 8]


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def track(img, t):
    raw = bytearray(b'\x4e' * (TRACK_LEN - 128))
    ptrs = bytearray(128)
    p = 0
    def put(bs):
        nonlocal p
        raw[p:p + len(bs)] = bs
        p += len(bs)
    put(b'\x4e' * 32)
    for i, s in enumerate(ORDER):
        put(b'\x00' * 8)
        put(b'\xa1' * 3)
        ptr = (128 + p) | 0x8000                  # offset of $FE from the track start
        ptrs[2 * i:2 * i + 2] = ptr.to_bytes(2, 'little')
        idf = bytes([0xFE, t, 0, s, 1])
        put(idf + crc16(b'\xa1' * 3 + idf).to_bytes(2, 'big'))
        put(b'\x4e' * 22)
        put(b'\x00' * 12)
        put(b'\xa1' * 3)
        data = img[(t * SPT + s - 1) * SEC:(t * SPT + s) * SEC]
        put(b'\xfb' + data + crc16(b'\xa1' * 3 + b'\xfb' + data).to_bytes(2, 'big'))
        put(b'\x4e' * 24)
    assert p <= len(raw), 'track overflow'
    return bytes(ptrs) + bytes(raw)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img = open(sys.argv[1], 'rb').read()
    if len(img) % (SPT * SEC):
        sys.exit('%s: %d bytes is not whole 18-sector tracks (a headered JVC?)' % (sys.argv[1], len(img)))
    tracks = len(img) // (SPT * SEC)
    hdr = bytearray(16)
    hdr[1] = tracks
    hdr[2:4] = TRACK_LEN.to_bytes(2, 'little')
    hdr[4] = 0x10                                 # single-sided
    out = bytes(hdr) + b''.join(track(img, t) for t in range(tracks))
    open(sys.argv[2], 'wb').write(out)
    print('wrote', sys.argv[2], len(out), 'bytes,', tracks, 'tracks')


if __name__ == '__main__':
    main()
