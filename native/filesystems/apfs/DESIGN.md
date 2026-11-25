# APFS native Common V2 design

## Production architecture

The canonical installed provider is `APFS.so`. Its Common V2 descriptor is implemented in [the module](../../../module/apfs_v2_probe.c), with [native namespace decoding](../../../native/apfs_v2_reader.c). Hosted and freestanding profiles must be qualified separately; neither the existence of a callback nor a freestanding ELF establishes production BOOT handoff or general mutation support.

## On-disk integrity

Validate container geometry, NX checkpoint identity and Fletcher checksums, object map mappings and catalogue records before trusting offsets. B-tree keys and values must lie within the record area, not the footer. Distinct records must not overlap; ambiguous object mappings must be rejected. Preserve host I/O errors and bound all partition-relative reads. Unsupported feature combinations fail closed. The reader's supported tree shapes and object/extent representations are limited by the native implementation and its exact regression tests, not the complete APFS format.

The hosted descriptor includes read-only namespace operations and a separate format operation; this does not imply arbitrary writable mounting, encryption, snapshots, multivolume or recovery support. See [README](../../../README.md) and [namespace regression tests](../../../tests/test_apfs_v2_namespace.c).

## Distribution

One portable amd64 DEB and one revision-pinned source-download native RUN, with GitHub-generated source archives. The installed module and PDF are the only filesystem-specific package files. No Linux VFS, separate kernel or EFI module or FUSE runtime substitutes for the V2 provider. The [release workflow](../../../.github/workflows/native-release.yml) defines the exact qualification.

## Historical source provenance

The original, actual out-of-tree Linux APFS implementation from
[linux-apfs/linux-apfs-rw](https://github.com/linux-apfs/linux-apfs-rw)
is preserved in `native/filesystems/apfs/linux/` for direct development.
The imported source is the exact Git tree
`03187127cd2e62cf78321cdb942eb3a7b17981f9`; all 38 source,
build and licence files were verified against the original blob hashes.

This is working implementation code, **not** a discarded reference. The
upstream version supplied its own `obj-m = apfs.o` Kbuild and DKMS support.
Its existing copyright and licence notices are retained verbatim.

The imported Linux driver is retained as historical source and evidence, not the current production deployment route. Its inherited copyright and licence notices remain intact. Importing it does not qualify write safety or any Common V2 capability.
