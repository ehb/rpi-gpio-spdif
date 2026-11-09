#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""LibreELEC facts for the CI scripts, read from the LibreELEC tree of a release.

  le_kernel.py derive 12.2.1 --config FILE   print KEY=VALUE lines (kernel commit, checksum,
                                             config path) and write the kernel config to FILE
  le_kernel.py versions                      JSON array from ci/libreelec-versions.txt
  le_kernel.py watch                         tag=/versions= of the newest release of this
                                             repository and the LibreELEC versions without a zip
  le_kernel.py rev v1.2.3                    add-on revision for a project tag

The kernel commit comes from packages/linux/package.mk, the config from the same search order
as kernel_config_path() in config/functions, for project RPi, device RPi4, arch aarch64.

Set GITHUB_TOKEN to get the higher API rate limit. Standard library only.
"""
import argparse
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.request

LE_REPO = "LibreELEC/LibreELEC.tv"
RAW = "https://raw.githubusercontent.com/%s/%%s/%%s" % LE_REPO
ADDON = "driver.audio.rpi-gpio-spdif"
HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT, DEVICE, ARCH = "RPi", "RPi4", "aarch64"


def fetch(url, api=False):
    """Bytes of url, None on 404. Other errors end the run."""
    headers = {"User-Agent": "rpi-gpio-spdif-ci"}
    token = os.environ.get("GITHUB_TOKEN")
    if api and token:
        headers["Authorization"] = "Bearer " + token
    if api:
        headers["Accept"] = "application/vnd.github+json"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        sys.exit("%s: HTTP %d %s" % (url, e.code, e.reason))
    except urllib.error.URLError as e:
        sys.exit("%s: %s" % (url, e.reason))


def le_file(tag, path):
    return fetch(RAW % (tag, path))


def derive(tag, config_out):
    pkg = le_file(tag, "packages/linux/package.mk")
    if pkg is None:
        sys.exit("LibreELEC %s: packages/linux/package.mk not found (is the tag right?)" % tag)
    pkg = pkg.decode()
    options = le_file(tag, "projects/%s/options" % PROJECT).decode()
    device_options = (le_file(tag, "projects/%s/devices/%s/options" % (PROJECT, DEVICE)) or b"").decode()
    linux = re.findall(r'^\s*LINUX="([^"]+)"', device_options + "\n" + options, re.M)
    if not linux:
        sys.exit("LibreELEC %s: no LINUX= in the project options" % tag)
    linux = linux[0]

    block = re.search(r"^\s*%s\)\s*\n(.*?)^\s*;;" % re.escape(linux), pkg, re.M | re.S)
    if not block:
        sys.exit("LibreELEC %s: no case for LINUX=%s in package.mk" % (tag, linux))
    block = block.group(1)
    version = re.search(r'PKG_VERSION="([^"]+)"(?:\s*#\s*(\S+))?', block)
    sha = re.search(r'PKG_SHA256="([0-9a-f]{64})"', block)
    url = re.search(r'PKG_URL="([^"]+)"', block)
    if not (version and sha and url):
        sys.exit("LibreELEC %s: PKG_VERSION/PKG_SHA256/PKG_URL not found in the %s case" % (tag, linux))
    commit, kver_hint = version.group(1), version.group(2) or ""
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        sys.exit("LibreELEC %s: PKG_VERSION %r is not a commit" % (tag, commit))
    kurl = url.group(1).replace("${PKG_VERSION}", commit)
    if "$" in kurl:
        sys.exit("LibreELEC %s: cannot resolve PKG_URL %s" % (tag, url.group(1)))

    name = "linux.%s.conf" % ARCH
    project = "projects/" + PROJECT
    candidates = [
        "%s/devices/%s/linux/%s/%s" % (project, DEVICE, commit, name),
        "%s/devices/%s/linux/%s/%s" % (project, DEVICE, linux, name),
        "%s/devices/%s/linux/%s" % (project, DEVICE, name),
        "%s/linux/%s/%s" % (project, commit, name),
        "%s/linux/%s/%s" % (project, linux, name),
        "%s/linux/%s" % (project, name),
        "packages/linux/config/%s/%s" % (commit, name),
        "packages/linux/config/%s/%s" % (linux, name),
        "packages/linux/config/%s" % name,
    ]
    for path in candidates:
        config = le_file(tag, path)
        if config is not None:
            break
    else:
        sys.exit("LibreELEC %s: no kernel config found:\n  %s" % (tag, "\n  ".join(candidates)))

    if config_out:
        os.makedirs(os.path.dirname(config_out) or ".", exist_ok=True)
        with open(config_out, "wb") as f:
            f.write(config)
    for key, value in (("LE", tag), ("KCOMMIT", commit), ("KSHA256", sha.group(1)), ("KURL", kurl),
                       ("KVER_HINT", kver_hint), ("KCONFIG_PATH", path),
                       ("KCONFIG_SHA256", hashlib.sha256(config).hexdigest())):
        print("%s=%s" % (key, value))


def vkey(v):
    return tuple(int(x) for x in v.split("."))


def listed():
    out = []
    with open(os.path.join(HERE, "libreelec-versions.txt"), encoding="ascii") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if line:
                out.append(line)
    if not out:
        sys.exit("ci/libreelec-versions.txt is empty")
    return sorted(out, key=vkey)


def stable_releases():
    """Stable LibreELEC tags, x.y.z with y < 90 (x.9y are alpha/beta numbers)."""
    data = fetch("https://api.github.com/repos/%s/releases?per_page=50" % LE_REPO, api=True)
    if data is None:
        sys.exit("%s: release list not found" % LE_REPO)
    out = []
    for r in json.loads(data):
        tag = r["tag_name"]
        if r["draft"] or r["prerelease"] or not re.fullmatch(r"\d+\.\d+\.\d+", tag):
            continue
        if int(tag.split(".")[1]) < 90:
            out.append(tag)
    return sorted(out, key=vkey)


def with_new():
    have = listed()
    return have + [v for v in stable_releases() if vkey(v) > vkey(have[-1])]


def watch():
    repo = os.environ.get("GITHUB_REPOSITORY")
    if not repo:
        sys.exit("GITHUB_REPOSITORY is not set")
    data = fetch("https://api.github.com/repos/%s/releases/latest" % repo, api=True)
    if data is None:
        print("no published release in %s, nothing to add" % repo, file=sys.stderr)
        print("tag=")
        print("versions=[]")
        return
    latest = json.loads(data)
    names = [a["name"] for a in latest["assets"]]
    missing = [v for v in with_new() if not any(n.startswith("%s-%s." % (ADDON, v)) for n in names)]
    print("tag=%s" % latest["tag_name"])
    print("versions=%s" % json.dumps(missing, separators=(",", ":")))


def rev(tag):
    m = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", tag)
    if not m:
        sys.exit("tag %r is not vX.Y.Z" % tag)
    a, b, c = (int(x) for x in m.groups())
    if b > 99 or c > 99:
        sys.exit("tag %s: minor and patch must stay below 100" % tag)
    print(a * 10000 + b * 100 + c)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("derive")
    d.add_argument("le")
    d.add_argument("--config", required=True)
    sub.add_parser("versions")
    sub.add_parser("watch")
    r = sub.add_parser("rev")
    r.add_argument("tag")
    a = ap.parse_args()
    if a.cmd == "derive":
        derive(a.le, a.config)
    elif a.cmd == "versions":
        print(json.dumps(listed(), separators=(",", ":")))
    elif a.cmd == "watch":
        watch()
    else:
        rev(a.tag)


if __name__ == "__main__":
    main()
