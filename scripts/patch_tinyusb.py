# patch_tinyusb.py: let our build flags set TinyUSB's host limits (PIZERO-54).
#
# The Adafruit TinyUSB library's RP2040 config hard-codes the host sizes with
# no #ifndef guard, so a -D flag cannot change them: our -DCFG_TUH_HID=4 was
# silently overridden to 12 from the start. A multi-port USB-C hub is often two
# hub chips chained, and with CFG_TUH_HUB 1 the second one is refused ("All
# addresses are occupied"), so nothing behind it ever enumerates.
#
# This wraps those three defines in #ifndef guards in the downloaded library
# before it compiles. It is idempotent, and prints what it changed. Wired in
# as a pre: extra_script in platformio.ini.

Import("env")  # noqa: F821  (provided by PlatformIO)

import os
import re

GUARDED = ("CFG_TUH_HUB", "CFG_TUH_DEVICE_MAX", "CFG_TUH_HID")
REL = os.path.join("src", "arduino", "ports", "rp2040", "tusb_config_rp2040.h")


def patch(path):
    with open(path) as f:
        text = f.read()
    changed = []
    for name in GUARDED:
        pat = re.compile(r"^#define %s\b(.*)$" % name, re.M)
        m = pat.search(text)
        if not m:
            continue
        before = text[:m.start()].rstrip().splitlines()
        if before and before[-1].strip() == "#ifndef %s" % name:
            continue                                  # already guarded
        text = text[:m.start()] + "#ifndef %s\n%s\n#endif" % (name, m.group(0)) + text[m.end():]
        changed.append(name)
    if changed:
        with open(path, "w") as f:
            f.write(text)
        print("patch_tinyusb: guarded %s in %s" % (", ".join(changed), path))


libdeps = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV")  # noqa: F821
lib = os.path.join(libdeps, "Adafruit TinyUSB Library")
cfg = os.path.join(lib, REL)
if os.path.isfile(cfg):
    patch(cfg)
