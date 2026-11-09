#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check a built module against the kernel release and config it is meant for.

  check_vermagic.py MODULE.ko CONFIG KVER [KVER_HINT]

The expected vermagic is computed the way include/linux/vermagic.h does for arm64:
<release> SMP [preempt |preempt_rt ]mod_unload [modversions ]aarch64. KVER is the release
of the tree, KVER_HINT the version LibreELEC names next to the commit in package.mk.
MODVERSIONS and module signing would need more than this build provides, so either fails.
"""
import re
import sys


def main():
    ko, config_path, kver = sys.argv[1:4]
    hint = sys.argv[4] if len(sys.argv) > 4 else ""
    config = dict(re.findall(r"^(CONFIG_\w+)=(.*)$", open(config_path, encoding="ascii").read(), re.M))
    for opt in ("CONFIG_MODVERSIONS", "CONFIG_MODULE_SIG"):
        if config.get(opt) == "y":
            sys.exit("%s=y: not supported by this build" % opt)
    if hint and hint != kver:
        sys.exit("kernel release %s does not match the version %s named in package.mk" % (kver, hint))

    expected = kver + " "
    if config.get("CONFIG_SMP") == "y":
        expected += "SMP "
    if config.get("CONFIG_PREEMPT_RT") == "y":
        expected += "preempt_rt "
    elif config.get("CONFIG_PREEMPT_BUILD") == "y":
        expected += "preempt "
    if config.get("CONFIG_MODULE_UNLOAD") == "y":
        expected += "mod_unload "
    expected += "aarch64"

    m = re.search(rb"vermagic=([^\x00]*)", open(ko, "rb").read())
    found = m.group(1).decode().strip() if m else None
    if found != expected:
        sys.exit("vermagic mismatch: module %r, expected %r" % (found, expected))
    print("vermagic=%s" % found)


if __name__ == "__main__":
    main()
