#!/usr/bin/env python3
"""Build TESTTEXT.DSK, a Disk BASIC disk of test files for the F11 FILES list
(PIZERO-309: text files on a mounted disk, read-only; PIZERO-310: F5 wrap).

  python3 scripts/mktestdisk.py TESTTEXT.DSK

Copy it to /coco/dsk on the card and put it in a drive (F12, or a line in
/coco/drives.txt). F11 then lists README.TXT:N, WRAP.TXT:N, WIDE32.TXT:N,
LFLINES.TXT:N, HELLO.BAS:N, BIG.TXT:N (over 4 KB, refused) and NOEXT:N;
BINARY.BIN is binary and must not appear. README.TXT on the disk says what
each file tests. Used and passed on hardware 2026-11-01."""
import sys

SEC, SPT, TRK = 256, 18, 35
img = bytearray(b'\xff' * (SEC * SPT * TRK))
fat = [0xFF] * 68
dirents = []

def gran_ts(g):
    t = g // 2
    if t >= 17:
        t += 1
    return t, (10 if g & 1 else 1)

def put(t, s, data):
    off = (t * SPT + (s - 1)) * SEC
    img[off:off + len(data)] = data

next_gran = [0]

def add(name, ext, typ, ascii_flag, data):
    n = len(data)
    grans = max(1, -(-n // (9 * SEC)))
    gs = list(range(next_gran[0], next_gran[0] + grans))
    next_gran[0] += grans
    for i, g in enumerate(gs):
        chunk = data[i * 9 * SEC:(i + 1) * 9 * SEC]
        t, s = gran_ts(g)
        for k in range(9):
            part = chunk[k * SEC:(k + 1) * SEC]
            if part:
                put(t, s + k, part.ljust(SEC, b'\x00'))
        if i + 1 < len(gs):
            fat[g] = gs[i + 1]
        else:
            in_last = n - i * 9 * SEC
            used = max(1, -(-in_last // SEC))
            fat[g] = 0xC0 + used
            last = in_last - (used - 1) * SEC
    e = bytearray(32)
    e[0:8] = name.ljust(8).encode()
    e[8:11] = ext.ljust(3).encode()
    e[11] = typ
    e[12] = ascii_flag
    e[13] = gs[0]
    e[14] = (last >> 8) & 0xFF
    e[15] = last & 0xFF
    dirents.append(bytes(e))

CR = b'\r'
def crtext(lines):
    return CR.join(l.encode() for l in lines) + CR

add('README', 'TXT', 1, 0xFF, crtext([
    'TESTTEXT.DSK: F11 TEST FILES',
    '',
    'EACH FILE ON THIS DISK TESTS ONE',
    'THING IN THE F11 FILES LIST.',
    '',
    'README.TXT  CR LINE ENDS (COCO)',
    'WRAP.TXT    LONG LINES: PRESS F5',
    'WIDE32.TXT  32-WIDE LINES',
    'LFLINES.TXT LF LINE ENDS (MAC)',
    'HELLO.BAS   ASCII BASIC',
    'BIG.TXT     OVER 4 KB: REFUSED',
    'BINARY.BIN  NOT LISTED (BINARY)',
    'NOEXT       NO EXTENSION',
]))
add('WRAP', 'TXT', 1, 0xFF, crtext([
    'SHORT LINE.',
    'THIS LINE IS MUCH LONGER THAN THIRTY-TWO CHARACTERS, SO WITH F5 ON IT WRAPS ONTO THE NEXT ROWS, AND WITH F5 OFF IT RUNS OFF THE RIGHT EDGE.',
    'ANOTHER SHORT LINE.',
    'X' * 100,
    'END.',
]))
add('WIDE32', 'TXT', 1, 0xFF, crtext([
    '12345678901234567890123456789012',
    'EVERY LINE HERE IS EXACTLY 32 CH',
    'ARACTERS WIDE, SO WITH F5 ON EAC',
    'H TAKES ONE ROW, NOT TWO........',
]))
add('LFLINES', 'TXT', 1, 0xFF, b'THIS FILE ENDS ITS LINES WITH LF\nLIKE A FILE FROM A MAC.\nIT SHOULD SHOW AS LINES TOO.\n')
add('HELLO', 'BAS', 0, 0xFF, crtext([
    '10 CLS',
    '20 PRINT "HELLO FROM TESTTEXT.DSK"',
    '30 FOR I=1 TO 10:PRINT I;:NEXT I',
    '40 END',
]))
big = crtext(['LINE %04d OF A FILE OVER 4 KB, WHICH F11 REFUSES.' % i for i in range(1, 121)])
assert len(big) > 4096
add('BIG', 'TXT', 1, 0xFF, big)
# LOADM file: one segment at $0E00 (RTS), exec $0E00
add('BINARY', 'BIN', 2, 0x00, bytes([0x00, 0x00, 0x01, 0x0E, 0x00, 0x39, 0xFF, 0x00, 0x00, 0x0E, 0x00]))
add('NOEXT', '', 1, 0xFF, crtext(['A FILE WITH NO EXTENSION.', 'IT SHOWS AS NOEXT:1.']))

# FAT: track 17 sector 2; directory: sectors 3-11
fs = bytearray(SEC)
fs[0:68] = bytes(fat)
put(17, 2, bytes(fs))
d = bytearray(b'\xff' * (9 * SEC))
for i, e in enumerate(dirents):
    d[i * 32:(i + 1) * 32] = e
for k in range(9):
    put(17, 3 + k, bytes(d[k * SEC:(k + 1) * SEC]))
open(sys.argv[1], 'wb').write(img)
print('wrote', sys.argv[1], len(img), 'bytes,', len(dirents), 'files, granules used', next_gran[0], ', BIG.TXT', len(big), 'bytes')
