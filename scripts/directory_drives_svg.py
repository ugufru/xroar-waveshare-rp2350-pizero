#!/usr/bin/env python3
"""Generate docs/directory-drives.svg (PIZERO-288). Run: python3 scripts/directory_drives_svg.py docs/directory-drives.svg"""
import sys
from html import escape as e

W = 1200
OPTS = [
    ("1  Our own DOS", "replace the ROM, DECB-like syntax"),
    ("2  Extended DECB", "DECB plus an extension"),
    ("3  Virtual floppy", "unmodified DECB, 8.3 rule"),
]
S, P, X = 3, 2, 1  # strong, partial, weak
WORD = {S: "Strong", P: "Partial", X: "Weak"}
ROWS = [
    ("Existing BASIC programs", "LOAD, SAVE, OPEN, RUN as written",
     [(P, "only as far as the syntax matches"), (S, "DECB is still there"), (S, "DECB is not touched")]),
    ("Sector-level software", "DSKCON, DSKI$, DSKO$, sector editors",
     [(X, "needs option 3 underneath"), (P, ".DSK yes; directories need 3"), (S, "sectors are what it serves")]),
    ("Long file names", "",
     [(S, "native"), (S, "through the extension"), (X, "8.3 only, by design")]),
    ("A host directory just works", "no conversion, no sidecar files",
     [(S, "every file"), (S, "every file, extended commands"), (S, "8.3 files, byte for byte")]),
    ("Capacity", "files per drive, size per file",
     [(S, "host limits"), (P, "DECB limits on plain commands"), (P, "156K a file, 72 a view, no disk full")]),
    ("6809 ROM space needed", "",
     [(P, "a new 8K ROM, assembler in build"), (X, "DECB is full; CoCo 3 room unknown"), (S, "none")]),
    ("Same on CoCo 1, 2 and 3", "",
     [(P, "must survive CoCo 3 patching"), (P, "CoCo 3 room unverified"), (S, "it sits below the ROM")]),
    ("Build effort", "what the hard part is",
     [(X, "a DOS's BASIC side, from scratch"), (P, "hooks inside a full 8K ROM"), (P, "write inference, refresh rule")]),
    ("Shares work with native/Forth", "the PIZERO-72 hypercall",
     [(S, "same hypercall"), (S, "same hypercall"), (P, "shares the storage backend only")]),
]
C, H = "core", "helps"
WINS = [
    ("Host-file storage on the SD card now, USB later", "PIZERO-70 backend; USB is PIZERO-78", [C, C, C]),
    ("DECB file rules, host-tested", "8.3 filter, type and ASCII sniffing, extensions", [H, C, C]),
    ("Editor reads any line ending", "reads CR, LF, CRLF; new files CR", [H, H, H]),
    ("Unpack GAME.DSK into GAME/, pack it back", "the same mapping option 3 runs on", [H, H, C]),
    ("ZIP import", "open the zipped .DSK files most archives ship", [H, H, H]),
    ("Check the DECB facts in the disassembly", "filename parser, FAT re-read, CoCo 3 room", [H, C, C]),
    ("Overlay conveniences", "text files, Markdown, boot flag, DIR popup, hex editor", [H, H, H]),
]

X0, CW0, CW, GAP = 40, 300, 280, 2
RH = 56
out = []
a = out.append

top = 150
mat_h = RH * len(ROWS)
wins_top = top + mat_h + 120
WH = 46
Hh = wins_top + 70 + WH * len(WINS) + 60

a(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {Hh}" width="{W}" height="{Hh}" '
  'font-family="system-ui, -apple-system, Segoe UI, sans-serif">')
a('<title>Directory drives: three ways to give Disk BASIC a host directory</title>')
a('<desc>PIZERO-288. Compares option 1 (our own DOS), option 2 (Disk BASIC with an extension) and '
  'option 3 (an emulated floppy built from a host directory, 8.3 names only) on nine criteria, then '
  'lists short-term work that pays off for all three. The same content is in '
  'docs/directory-drives-proposal.md as tables. Hover a cell for its full text.</desc>')
a('''<style>
  svg { --bg:#fcfcfb; --fg:#0b0b0b; --muted:#52514e; --line:#e1e0d9;
        --r1:#86b6ef; --r2:#3987e5; --r3:#184f95; --t1:#0b0b0b; --t2:#0b0b0b; --t3:#ffffff; }
  @media (prefers-color-scheme: dark) {
    svg { --bg:#1a1a19; --fg:#f0efec; --muted:#c3c2b7; --line:#2c2c2a;
          --r1:#184f95; --r2:#3987e5; --r3:#9ec5f4; --t1:#ffffff; --t2:#0b0b0b; --t3:#0b0b0b; }
  }
  .bg { fill:var(--bg); } .fg { fill:var(--fg); } .muted { fill:var(--muted); }
  .h1 { font-size:24px; font-weight:650; fill:var(--fg); }
  .h2 { font-size:13px; font-weight:600; fill:var(--muted); letter-spacing:.05em; text-transform:uppercase; }
  .colh { font-size:15px; font-weight:650; fill:var(--fg); }
  .crit { font-size:14px; font-weight:600; fill:var(--fg); }
  .sub { font-size:12px; fill:var(--muted); }
  .r1 { fill:var(--r1); } .r2 { fill:var(--r2); } .r3 { fill:var(--r3); }
  .t1 { fill:var(--t1); } .t2 { fill:var(--t2); } .t3 { fill:var(--t3); }
  .word { font-size:13px; font-weight:700; } .note { font-size:12px; }
  .rule { stroke:var(--line); stroke-width:1; }
  .cell:hover rect { stroke:var(--fg); stroke-width:2; }
</style>''')
a(f'<rect class="bg" width="{W}" height="{Hh}"/>')
a(f'<text class="h1" x="{X0}" y="48">Directory drives: three ways to put a USB directory behind Disk BASIC</text>')
a(f'<text class="sub" x="{X0}" y="72">PIZERO-288, docs/directory-drives-proposal.md. How each option does on what matters, '
  'and the short-term work that pays off whichever wins.</text>')

# column headers
a(f'<text class="h2" x="{X0}" y="{top-40}">How they compare</text>')
for i, (t, s) in enumerate(OPTS):
    x = X0 + CW0 + i * CW + 12
    a(f'<text class="colh" x="{x}" y="{top-18}">{e(t)}</text>')
    a(f'<text class="sub" x="{x}" y="{top-2}">{e(s)}</text>')

for r, (crit, sub, cells) in enumerate(ROWS):
    y = top + 8 + r * RH
    a(f'<text class="crit" x="{X0}" y="{y+24 if sub else y+32}">{e(crit)}</text>')
    if sub:
        a(f'<text class="sub" x="{X0}" y="{y+42}">{e(sub)}</text>')
    for i, (lvl, note) in enumerate(cells):
        x = X0 + CW0 + i * CW
        a(f'<g class="cell"><title>{e(OPTS[i][0].split("  ")[1])}, {e(crit)}: {WORD[lvl]}. {e(note)}</title>')
        a(f'<rect class="r{lvl}" x="{x+GAP}" y="{y+GAP}" width="{CW-2*GAP}" height="{RH-2*GAP}" rx="4"/>')
        a(f'<text class="word t{lvl}" x="{x+14}" y="{y+24}">{WORD[lvl]}</text>')
        a(f'<text class="note t{lvl}" x="{x+14}" y="{y+42}">{e(note)}</text></g>')

# legend
ly = top + 8 + mat_h + 28
lx = X0 + CW0
for i, lvl in enumerate((S, P, X)):
    x = lx + i * 130
    a(f'<rect class="r{lvl}" x="{x}" y="{ly-12}" width="16" height="16" rx="3"/>')
    a(f'<text class="sub" x="{x+24}" y="{ly+1}" style="fill:var(--fg)">{WORD[lvl]}</text>')
a(f'<text class="sub" x="{lx+400}" y="{ly+1}">The word in each cell carries the rating, not the color.</text>')

# wins
a(f'<text class="h2" x="{X0}" y="{wins_top}">Short-term wins that pay off for all three</text>')
for i, (t, _) in enumerate(OPTS):
    x = X0 + CW0 + i * CW + 12
    a(f'<text class="colh" x="{x}" y="{wins_top+40}">{e(t)}</text>')
for r, (name, sub, marks) in enumerate(WINS):
    y = wins_top + 56 + r * WH
    a(f'<line class="rule" x1="{X0}" x2="{X0+CW0+3*CW}" y1="{y}" y2="{y}"/>')
    a(f'<text class="crit" x="{X0}" y="{y+20}">{e(name)}</text>')
    a(f'<text class="sub" x="{X0}" y="{y+37}">{e(sub)}</text>')
    for i, m in enumerate(marks):
        lvl = S if m == C else P
        x = X0 + CW0 + i * CW
        a(f'<g class="cell"><title>{e(name)}: {m} for {e(OPTS[i][0].split("  ")[1])}</title>')
        a(f'<rect class="r{lvl}" x="{x+GAP}" y="{y+8}" width="{120}" height="{30}" rx="4"/>')
        a(f'<text class="word t{lvl}" x="{x+14}" y="{y+28}">{"Core piece" if m == C else "Helps"}</text></g>')
fy = wins_top + 56 + len(WINS) * WH + 26
a(f'<text class="sub" x="{X0}" y="{fy}">Core piece: the option is partly built out of this work. '
  'Helps: the option benefits, or users do whichever option ships.</text>')
a('</svg>')

s = "\n".join(out) + "\n"
assert chr(0x2014) not in s and "&" + "mdash;" not in s
open(sys.argv[1], "w", encoding="utf-8").write(s)
print("wrote", sys.argv[1], len(s), "bytes, height", Hh)
