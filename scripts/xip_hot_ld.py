# PIZERO-331: put the hot code that runs from flash in one block at the start
# of .text, so where it sits in the XIP cache no longer depends on what else
# the linker happens to put around it.
#
# The XIP cache is 16 KB, two-way, 8-byte lines: two flash addresses 8 KB
# apart share a set. The GIME's per-line code (about 10 KB) runs from flash
# (RAM is too short for it, the CoCo 3 takes 64 KB of the heap). Left to the
# linker, it lands wherever its object file falls, among other code that
# runs as often (the frame loop, newlib's memset, the USB stack), and an
# unrelated change elsewhere moved the CoCo 3's speed by 8% (171 against 158
# host clocks per emulated cycle). Laid out contiguously and under 16 KB, the
# block's own lines cannot evict each other.
#
# Each function is picked by its -ffunction-sections input section name,
# placed ahead of the generic *(.text*) so it wins. Patched into the
# memmap_default.ld the framework generates, after it is generated.
#
# memset: the SDK script means to keep newlib's mem functions out of flash
# (*lib_a-mem*.o), but this newlib names them libc_a-mem*.o, so memset is in
# flash, called per line (the port line's border). Here it joins the block.

Import("env")
import os

HOT = [
    # GIME (tcc1014.c): per line, per frame, per timer tick, per bus cycle
    ".text.port_render_scanline",
    ".text.render_scanline",
    ".text.do_hs_fall",
    ".text.do_hs_rise",
    ".text.do_hb_irq",
    ".text.do_vb_irq",
    ".text.do_update_timer",
    ".text.schedule_timer",
    ".text.fetch_byte_vram",
    ".text.tcc1014_poll_lines",
    ".text.tcc1014_mem_cycle",
    ".text.tcc1014_set_register",
    # The CoCo 3 machine (coco_machine.cpp): the GIME's line signals, audio
    ".text._ZL15coco3_signal_hsPvb",
    ".text._ZL15coco3_signal_fsPvb",
    ".text._ZL18audio_update_levelv",
    # USB host CRC, per packet in the frame interrupt (Pico-PIO-USB)
    ".text.calc_usb_crc16",
]

MARK = "/* PIZERO-331 xip_hot */"


def patch_file(path):
    s = open(path).read()
    if MARK in s:
        return
    anchor = "        KEEP (*(.reset))\n"
    if anchor not in s:
        raise SystemExit("xip_hot_ld.py: no KEEP (*(.reset)) in " + path)
    block = (
        "        " + MARK + "\n"
        "        . = ALIGN(8);\n"
        "        __xip_hot_start = .;\n"
        + "".join("        *(%s)\n" % h for h in HOT)
        + "        *libc.a:libc_a-memset.o(.text*)\n"
        "        __xip_hot_end = .;\n"
        "        ASSERT(__xip_hot_end - __xip_hot_start <= 16384, \"PIZERO-331: the xip_hot block is over the 16 KB XIP cache\");\n"
    )
    s = s.replace(anchor, anchor + block, 1)
    open(path, "w").write(s)
    print("xip_hot_ld.py: patched " + path)


LD = os.path.join("$BUILD_DIR", "memmap_default.ld")
# When the framework generates it, and again before the link, as a build
# directory may hold one generated before this script existed (it is only
# regenerated when the framework's template changes).
env.AddPostAction(LD, lambda target, source, env: patch_file(str(target[0])))
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf",
                 lambda target, source, env: patch_file(env.subst(LD)))
