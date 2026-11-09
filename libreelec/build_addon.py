#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package the LibreELEC add-on driver.audio.rpi-gpio-spdif as an installable zip.

The module only loads into the kernel it was built against, so one zip serves exactly one
LibreELEC version. Version scheme as LibreELEC's own driver add-ons: <LE version>.<revision>.

  python libreelec/build_addon.py --ko rpi_gpio_spdif.ko --dtbo rpi-gpio-spdif.dtbo \\
      --kver 6.12.56 --le 12.2.1 [--rev 1] [--out build]

--ko    module built against that LibreELEC kernel (vermagic is checked)
--dtbo  overlay compiled from driver/rpi-gpio-spdif-overlay.dts (dtc -@ -I dts -O dtb)
"""
import argparse
import os
import re
import sys
import zipfile

ID = "driver.audio.rpi-gpio-spdif"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def vermagic(ko):
    data = open(ko, "rb").read()
    m = re.search(rb"vermagic=([^\x00]*)", data)
    return m.group(1).decode() if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ko", required=True)
    ap.add_argument("--dtbo", required=True)
    ap.add_argument("--kver", required=True, help="uname -r of the target LibreELEC")
    ap.add_argument("--le", required=True, help="LibreELEC version, e.g. 12.2.1")
    ap.add_argument("--rev", default="1")
    ap.add_argument("--out", default=os.path.join(ROOT, "build"))
    a = ap.parse_args()

    vm = vermagic(a.ko)
    if not vm or vm.split()[0] != a.kver:
        sys.exit("module vermagic %r does not match --kver %s" % (vm, a.kver))
    if open(a.dtbo, "rb").read(4) != b"\xd0\x0d\xfe\xed":
        sys.exit("%s is not a compiled device tree overlay" % a.dtbo)

    version = "%s.%s" % (a.le, a.rev)
    xml = open(os.path.join(HERE, "addon", "addon.xml.in"), encoding="ascii").read()
    xml = xml.replace("@VERSION@", version).replace("@LEVERSION@", a.le).replace("@KVER@", a.kver)

    files = [  # (path in the add-on, bytes, executable)
        ("addon.xml", xml.encode("ascii"), False),
        ("KVER", (a.kver + "\n").encode("ascii"), False),
        ("default.py", open(os.path.join(HERE, "addon", "default.py"), "rb").read(), False),
        ("LICENSE.txt", open(os.path.join(ROOT, "LICENSE"), "rb").read(), False),
        ("bin/rpi-gpio-spdif", open(os.path.join(HERE, "addon", "bin", "rpi-gpio-spdif"), "rb").read(), True),
        ("system.d/%s.service" % ID,
         open(os.path.join(HERE, "addon", "system.d", ID + ".service"), "rb").read(), False),
        ("alsa/asound.conf", open(os.path.join(ROOT, "alsa", "asound-libreelec.conf"), "rb").read(), False),
        ("overlays/rpi-gpio-spdif.dtbo", open(a.dtbo, "rb").read(), False),
        ("kernel-overlay/lib/modules/%s/updates/%s/rpi_gpio_spdif.ko" % (a.kver, ID),
         open(a.ko, "rb").read(), False),
    ]
    for name, data, _ in files:
        if name.endswith((".service", ".conf", ".py", ".xml")) or name.startswith("bin/"):
            if b"\r" in data:
                sys.exit("%s has CR line endings" % name)

    os.makedirs(a.out, exist_ok=True)
    zpath = os.path.join(a.out, "%s-%s.zip" % (ID, version))
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data, exe in files:
            zi = zipfile.ZipInfo("%s/%s" % (ID, name), date_time=(2026, 1, 1, 0, 0, 0))
            zi.external_attr = ((0o755 if exe else 0o644) | 0o100000) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, data)
    print(zpath)


if __name__ == "__main__":
    main()
