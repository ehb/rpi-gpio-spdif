#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package the LibreELEC add-on service.rpi-gpio-spdif as an installable zip.

The add-on holds the aarch64 spdifbmc plugin, the device tree overlays spdif-gpio21 and
spdif-gpio21-pi5 (only GPIO21), a start/stop script, a systemd unit and the settings. It is
tied to the architecture, not to a kernel or LibreELEC version.
Version: --version, else the newest entry of CHANGELOG.md.

  python libreelec/build_addon.py --so build/aarch64/libasound_module_pcm_spdifbmc.so \\
      --dtbo build/overlay/spdif-gpio21.dtbo build/overlay/spdif-gpio21-pi5.dtbo \\
      [--version 0.2.0] [--out build]

The plugin is checked for ELF class 64-bit, little endian, machine AArch64; the overlays for
the flattened device tree magic and their names.
"""
import argparse
import os
import re
import struct
import sys
import zipfile

ID = "service.rpi-gpio-spdif"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ADDON = os.path.join(HERE, "addon")
EM_AARCH64 = 183
OVERLAYS = ["spdif-gpio21.dtbo", "spdif-gpio21-pi5.dtbo"]


def elf_problem(path):
    """None if path is a little-endian 64-bit AArch64 ELF, else the reason."""
    with open(path, "rb") as f:
        head = f.read(20)
    if len(head) < 20 or head[:4] != b"\x7fELF":
        return "not an ELF file"
    if head[4] != 2 or head[5] != 1:
        return "not a 64-bit little-endian ELF"
    machine = struct.unpack_from("<H", head, 18)[0]
    if machine != EM_AARCH64:
        return "machine %d, not AArch64 (%d)" % (machine, EM_AARCH64)
    return None


def dtb_problem(path):
    """None if path starts with the flattened device tree magic, else the reason."""
    with open(path, "rb") as f:
        head = f.read(4)
    if head != b"\xd0\x0d\xfe\xed":
        return "not a device tree blob (.dtbo)"
    return None


def changelog_version():
    text = open(os.path.join(ROOT, "CHANGELOG.md"), encoding="ascii").read()
    m = re.search(r"^## (\d+\.\d+\.\d+)\s*$", text, re.M)
    if not m:
        sys.exit("CHANGELOG.md has no '## x.y.z' entry; pass --version")
    return m.group(1)


def read(*parts):
    with open(os.path.join(*parts), "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--so", required=True, help="aarch64 libasound_module_pcm_spdifbmc.so")
    ap.add_argument("--dtbo", required=True, nargs="+", help=" ".join(OVERLAYS) + " (make overlay)")
    ap.add_argument("--version", help="add-on version x.y.z (default: CHANGELOG.md)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build"))
    a = ap.parse_args()

    if not os.path.isfile(a.so):
        sys.exit("--so: no such file: %s" % a.so)
    why = elf_problem(a.so)
    if why:
        sys.exit("--so %s: %s -- this add-on is for 64-bit Raspberry Pi (aarch64) only" % (a.so, why))
    if sorted(os.path.basename(d) for d in a.dtbo) != sorted(OVERLAYS):
        sys.exit("--dtbo needs exactly " + " and ".join(OVERLAYS))
    for d in a.dtbo:
        if not os.path.isfile(d):
            sys.exit("--dtbo: no such file: %s" % d)
        why = dtb_problem(d)
        if why:
            sys.exit("--dtbo %s: %s" % (d, why))
    version = a.version or changelog_version()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        sys.exit("version %r is not x.y.z" % version)
    xml = open(os.path.join(ADDON, "addon.xml.in"), encoding="ascii").read()
    xml = xml.replace("@VERSION@", version)

    files = [  # (path in the add-on, bytes, executable)
        ("addon.xml", xml.encode("ascii"), False),
        ("default.py", read(ADDON, "default.py"), False),
        ("LICENSE.txt", read(ROOT, "LICENSE"), False),
        ("bin/rpi-gpio-spdif", read(ADDON, "bin", "rpi-gpio-spdif"), True),
        ("lib/libasound_module_pcm_spdifbmc.so", read(a.so), False),
    ] + [("overlays/" + os.path.basename(d), read(d), False) for d in sorted(a.dtbo)] + [
        ("system.d/%s.service" % ID, read(ADDON, "system.d", ID + ".service"), False),
        ("resources/settings.xml", read(ADDON, "resources", "settings.xml"), False),
        ("resources/language/resource.language.en_gb/strings.po",
         read(ADDON, "resources", "language", "resource.language.en_gb", "strings.po"), False),
    ]
    for name, data, _ in files:
        if name.endswith((".service", ".py", ".xml", ".po")) or name.startswith("bin/"):
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
