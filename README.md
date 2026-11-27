# Filesystem-APFS

## 1. Purpose and format scope

Apple File System, with container and volume object structures. This repository owns the format-specific implementation of `APFS.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

NX superblocks, checkpoint descriptors, object maps, space manager and catalogue data govern container interpretation. Validate object flags, object identity, geometry and Fletcher checksums. Object-map duplicate keys and values overlapping the B-tree footer are rejected.

## 3. Common V2 implementation and capabilities

The hosted descriptor declares probe, mount, stat, directory enumeration, read, query, label retrieval and format. The carrier also embeds a freestanding ELF; the documented firmware profile recognises containers, and must not be assumed to implement the full hosted namespace. Independent container tests exercise multiple transfer sizes. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

Encrypted volumes, complex checkpoint and snapshot combinations and arbitrary namespace mutation require explicit qualification. Check the feature set and profile mask rather than inferring production BOOT eligibility from the carrier. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/APFS.so` and `/System/Resources/Documentation/Filesystem-APFS.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-APFS/actions/runs/45526913001) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-APFS/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](module/apfs_v2_probe.c)
- [Native design or source provenance](native/filesystems/apfs/DESIGN.md)
- [Migration and source history](MIGRATION_STATUS.md)
- [Original-format reference source](docs/APFS.pdfstreams) and [PDF](docs/APFS.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.

The [engineering reference](docs/APFS-TECHNICAL-REFERENCE.md) adds field-level original-format guidance. The PDF and editable reference contain one integrated manual.
