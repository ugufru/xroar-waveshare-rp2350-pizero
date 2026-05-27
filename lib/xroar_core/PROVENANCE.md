# xroar_core provenance

## Upstream

- Project: XRoar — Dragon/Tandy CoCo emulator
- Author: Ciaran Anscomb
- Homepage: https://www.6809.org.uk/xroar/
- Source: https://www.6809.org.uk/git/xroar.git
- License: GNU GPL v3 or later (see COPYING.GPL in upstream tree)

## Pinned revision

- Commit: `a2d31903eeabb31b0d42481587364db49ef0ec70`
- Date:   2026-05-14
- Title:  *mpi: remove unused global and thus mpi_free()*

Copied to this tree on 2026-05-18.

## Files vendored

From `src/`:
- `mc6809/mc6809.c`, `mc6809.h`, `mc6809_common.c` (included by mc6809.c), `mc6809_trace.[ch]`
- `mc680x/mc680x_ops.c` (included by mc6809.c)
- `mc6883.[ch]` (SAM)
- `mc6847/mc6847.[ch]`, `font-6847.[ch]`, `font-6847t1.[ch]`
- `mc6821.[ch]` (PIA)
- `ram.[ch]`, `part.[ch]`, `serialise.[ch]`, `events.[ch]`
- `logging.h` (verbatim — its no-op path is used; `LOGGING` is never defined)
- `breakpoint.h`, `dkbd.h`, `debug_cpu.h`, `fs.h` (declarations only)

From `portalib/`:
- All `.c`/`.h` files except `sdsx.c` (excluded — pulls POSIX `<regex.h>`, not available on newlib for the Pico)

## Files explicitly NOT vendored

- `rombank.c` / `crc32.c` — host file I/O. We embed ROMs as const arrays (see COCO-22).
- `dragon.c` / `coco.c` / `dragon32.c` etc. — XRoar machine glue. We write our own coco_machine.c (COCO-25).
- Host backends (`sdl2/`, `gtk2/`, `alsa/`, `pulseaudio/`, `evdev/`, `joydev/`, `wasm/`, `macosx/`, etc.).
- ROM loaders, cartridge code, disk/tape — deferred to later issues.

## Stubs written for the port

- `include/config.h` — replaces the autotools-generated config. Enables GCC attribute macros; intentionally does **not** define `LOGGING` (so logging.h's macros are no-ops).
- `include/top-config.h` — verbatim copy of XRoar's top-config.h.
- `src/xroar.h` — tiny stub. Exposes `machine_event_list_global` and the
  `MACHINE_EVENT_LIST` macro.
- `src/machine.h` — minimal types `machine_partdb_entry`, `cart_partdb_entry`, signals.
- `src/xroar_stubs.c` — definitions for the globals our stub headers declare.

## Build wiring

PlatformIO library config in `library.json`:
- `srcDir` = `.`
- `srcFilter` selects only the .c files we want compiled
- `flags` add `-Iinclude -Isrc -Isrc/mc6809 -Isrc/mc6847 -Iportalib`
