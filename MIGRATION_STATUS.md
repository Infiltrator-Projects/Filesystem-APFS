# APFS Common V2 implementation and qualification — 22 November 2026

## Authoritative production interface

The only installed filesystem provider is `/System/Filesystems/APFS.so`,
exporting `infiltrator_filesystem_api_v2`. Its hosted read-only Common V2
CORE implementation provides PROBE, MOUNT/UNMOUNT, STAT, LIST and READ,
and the VOLUME group provides QUERY. File contents and directories are read
through `native/apfs_v2_reader.c`; hosted mounting also checks the source-owned
checkpoint/spaceman summary in `native/apfs_native.c`.

The same portable APFS reader and probe are compiled into a no-import,
freestanding ELF by `module/firmware`, embedded byte-for-byte into the
canonical hosted carrier as `.infiltrator_boot`. The firmware descriptor
uses the Common V2 CORE group with read-only session handles, not Linux VFS,
FUSE, a Linux kernel filesystem module or an independent substitute decoder.

### Actually supported APFS layout

The current firmware/host reader explicitly restricts itself to a
checksum-valid, clean, unencrypted, single-volume checkpoint with supported
flat root/leaf object-map and filesystem-catalogue B-trees. Path resolution
uses native catalogue directory and inode keys; regular-file contents use
on-disk extent records. Unsupported tree heights, encryption, snapshots,
clones, compression, sparse/unmapped streams and unsupported features must
fail closed. This is **not** comprehensive APFS coverage and must not be
promoted by loosening a checksum, inventing a file, or replacing the reader
with a Linux filesystem driver.

Source-owned catalogue corruption checks reject repeated exact parent/name
directory keys and repeated inode keys, including conflicting records that
still carry a valid APFS object checksum. The same reader is built into both
runtime profiles. The independent `tests/test_apfs_v2_namespace.c` exercises
its real Common V2 entry point with clean, deliberately malformed, and
checksummed duplicate-key images, file/directory traversal, I/O failure
propagation, rejection of unqualified writes, unmount-handle invalidation
and no host-write callbacks.

## Qualification boundaries

- Hosted and freestanding compilation, the exact embedded ELF byte comparison
  and disposable Common V2 image tests are separate gates. The hosted module
  and its native RUN/portable DEB are checked as installed carriers.
- The current workflow additionally probes independently formatted APFS
  containers, but the deliberately restricted flat-catalogue namespace test
  does **not** establish support for arbitrary real-world APFS datasets.
- No current production BOOT V2 validator result exists because BOOT itself
  is not yet Common V2-capable. An earlier validator report must not be
  relabelled as current V2 acceptance.
- A disposable OVMF boot reading the actual kernel and initramfs via the
  packaged APFS.so has not been qualified. No firmware-compatible release
  designation follows from a successful test compile.
- No writable mounting, recovery, formatting or filesystem-specific `.ko`
  substitution is claimed. Historical workers and fixture APIs are
  non-installed, test-only source; no V1 runtime ABI is exported.

### Pending exact-head result

The 22 November duplicate-key repairs are committed to `main` in
`530b711`, `2b85b7a`, `7756b3d` and `654865c`. Their GitHub Actions
completion must be checked before marking this revision qualified or issuing
a new release. No BOOT repository, physical disk, ESP or firmware settings
were modified.
