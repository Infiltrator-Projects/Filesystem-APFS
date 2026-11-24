#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
for sector in 512 4096; do
    "$1" "$2" "$work/$sector.img" 1 "$sector"
    if [ "$#" -gt 2 ]; then
        "$3" "$work/$sector.img"
    fi
done
