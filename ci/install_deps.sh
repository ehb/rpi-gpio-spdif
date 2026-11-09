#!/bin/sh
# Debian 12 packages for the CI jobs (runs as root in the container, or with sudo).
set -e
SUDO=
[ "$(id -u)" = 0 ] || SUDO=sudo
export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update -qq
$SUDO apt-get install -y --no-install-recommends \
    bc binutils bison build-essential ca-certificates curl device-tree-compiler flex \
    gcc-aarch64-linux-gnu libc6-dev libelf-dev libssl-dev make perl python3 tar xz-utils zstd
