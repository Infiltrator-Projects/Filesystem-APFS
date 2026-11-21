#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Extract only into the CI runner's temporary directory.
set -euo pipefail
work="${RUNNER_TEMP:?}/apfs-independent-tools"
mkdir -p "$work"
file=apfsprogs_0.2.0-1_amd64.deb
curl -fsSL --retry 3 "https://deb.debian.org/debian/pool/main/a/apfsprogs/$file" -o "$work/$file"
echo "ec7a61d97f9f71fc10767806e416b76a71582ce7d71e7dda6a7464834cd42055  $work/$file" | sha256sum -c -
dpkg-deb -x "$work/$file" "$work/root"
test -x "$work/root/usr/sbin/mkapfs"
test -x "$work/root/usr/sbin/apfsck"
echo "$work/root/usr/sbin" >> "${GITHUB_PATH:?}"
