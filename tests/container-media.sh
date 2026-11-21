#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
reader=$(realpath "${1:?Common V2 exerciser}")
module=$(realpath "${2:?installed APFS provider}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
truncate -s 128M clean.img
mkapfs -L CommonV2 clean.img
apfsck clean.img
objcopy --dump-section .infiltrator_boot=reader.elf "$module" carrier-copy.so
test -s reader.elf
test -z "$(nm -Du reader.elf)"
if readelf -dW reader.elf | grep '(NEEDED)' >/dev/null; then exit 1; fi
python3 - <<'PY'
from pathlib import Path
import subprocess, struct
with open('clean.img','rb') as f:
    raw=f.read(4096)
assert raw[32:36]==b'NXSB' and struct.unpack_from('<I',raw,24)[0]==0x80000001
mod=(1<<32)-1
for name in ('wrong-class','bad-fletcher'):
    subprocess.run(['cp','--sparse=always','--reflink=auto','clean.img',name+'.img'],check=True)
    data=bytearray(raw)
    if name=='wrong-class':
        struct.pack_into('<I',data,24,0x40000001)
        a=b=0
        for (word,) in struct.iter_unpack('<I',data[8:]):
            a=(a+word)%mod; b=(b+a)%mod
        x=mod-(a+b)%mod
        struct.pack_into('<II',data,0,x,mod-(a+x)%mod)
    else:
        data[-1]^=1
    with Path(name+'.img').open('r+b') as f: f.write(data)
PY
sha256sum *.img > media.sha256
for binary in "$module" "$work/reader.elf"; do
    for size in 512 4096 8192; do
        "$reader" "$binary" clean.img "$size" clean
        "$reader" "$binary" wrong-class.img "$size" corrupt
        "$reader" "$binary" bad-fletcher.img "$size" corrupt
    done
done
sha256sum -c media.sha256
