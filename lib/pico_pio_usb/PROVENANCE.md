# pico_pio_usb provenance

## Upstream

- Project: Pico-PIO-USB, USB host and device over PIO on the RP2040/RP2350
- Author: sekigon-gonnoc
- Source: https://github.com/sekigon-gonnoc/Pico-PIO-USB
- Version: 0.7.2, commit `dc9193fed510da5f81f4db520e98282f61db9054`
  (2026-05-19), the revision this project had been building from `lib_deps`
- License: MIT (see `LICENSE` in this directory)

Vendored on 2026-10-23 for PIZERO-255 (sound out of a USB headphone DAC),
which needs isochronous OUT transfers the upstream host does not have. Until
then the library came unpinned from git through `lib_deps`; vendoring also
pins it. `src/`, `LICENSE`, `README.md` and `library.properties` are copied
as they were; the examples and CI files are left out.

## Local modifications (this project)

The first commit of this directory (29f10b0) is the upstream files unchanged,
so the changes read as a diff against it. All are marked `PIZERO-255 (this
project)` in the source:

- `pio_usb.c`, `pio_usb_ll.h`: `pio_usb_ll_encode_tx_data` takes and returns
  16-bit lengths and indexes its output with a 16-bit index. The upstream
  `uint8_t` wrapped for any packet that encodes past 255 bytes, which a
  192-byte isochronous audio packet does (about 460 bytes).
- `usb_definitions.h`: `endpoint_t.encoded_data_len` is `uint16_t`.
- `pio_usb_host.c`, `pio_usb.h`: an isochronous OUT stream,
  `pio_usb_host_iso_out_stream(root, address, endpoint, fill)` and
  `pio_usb_host_iso_frames()`. The 1 ms frame handler calls `fill` for the
  frame's packet, builds DATA0 and its CRC16, encodes it into its own buffer
  (up to `PIO_USB_ISO_MAX` bytes) before sending the OUT token, then sends the
  packet and waits for no handshake. Upstream had no isochronous support: an
  OUT transaction always waited for a handshake.
- `pio_usb.c`, `pio_usb_ll.h`, `pio_usb.h`, `pio_usb_host.c` (PIZERO-322):
  the audio packet no longer holds core 0 while it goes out (about 136 us of
  every 1 ms frame, spent waiting for a packet that gets no reply).
  `pio_usb_bus_usb_transfer_nowait` starts it and returns; the frame sends it
  after every root port's transactions (two ports here, so no other port's
  setup moves the transmitter's pins mid-packet), and anything that next
  drives the bus calls `pio_usb_bus_tx_wait` first (the send routines, PRE,
  the root port setup, the frame's start, a port reset). That wait counts
  the packet done when EOP has been seen and the DMA and TX FIFO are empty,
  not by upstream's `pc >= COMP` test, which a frame later can fail: the last
  encoded byte's spare symbols may have run the state machine round to an
  earlier address. It gives up after 1 ms and counts it
  (`pio_usb_bus_tx_wait_timeouts`, on the `[uac]` line; it should stay 0).
- `pio_usb_encode.h` (new), `pio_usb.c` (PIZERO-322): `pio_usb_ll_encode_tx_data`
  encodes four bits at a time. Four bits with no stuffed bit inside them are
  exactly one output byte, looked up from the line level they start at;
  a nibble where the run of 1s could reach six goes bit by bit, as does the
  end of packet. Same bytes as upstream's bit-at-a-time loop
  (`test/test_usb_encode`, which keeps a copy of it, checks every length to
  200, runs of 1s before every byte value and 20,000 random packets), and
  smaller (2.8 KB less RAM). The tables and the slow path are kept in RAM.
  A 196-byte audio packet took about 100 us every 1 ms frame before.
