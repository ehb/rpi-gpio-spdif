#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Debian 12 packages for the CI jobs (runs as root in the container, or with sudo).
set -e
SUDO=
[ "$(id -u)" = 0 ] || SUDO=sudo
export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update -qq
$SUDO apt-get install -y --no-install-recommends \
    alsa-utils binutils-aarch64-linux-gnu binutils-arm-linux-gnueabihf build-essential \
    ca-certificates device-tree-compiler gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf libc6-dev-arm64-cross \
    libc6-dev-armhf-cross libasound2-dev make python3 tar unzip
