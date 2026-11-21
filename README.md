# Filesystem-APFS

The independently owned `APFS.so` installs at
`/System/Filesystems/APFS.so`. Its documentation installs at
`/System/Resources/Documentation/Filesystem-APFS.pdf`.

## Qualified scope and limitations

The current production shared object exposes **Common V2 only** and implements
APFS container probing, checksum/checkpoint validation, read-only mount/unmount,
and volume information through the native APFS analysis engine.

**This is not a BOOT-qualified reader.** Directory enumeration, pathname lookup,
file-content reads, freestanding UEFI execution, and genuine BOOT
`validate-module` acceptance are outstanding. Do not label this release as a
bootable filesystem provider or bypass the validator.

## Native Common V2 safety qualification

The production native reader now verifies that the active container checkpoint
is a genuine physical NX superblock with the expected object identity and that
the referenced APFS volume object has its correct on-disk type. The bounded
read-only subset refuses every unimplemented APFS incompatible-feature bit
(rather than only testing sealed-volume metadata). Valid but unsupported
volume layouts return `IFS_FS_UNSUPPORTED`, distinct from checksum/structure
corruption and physical I/O failure.

The `apfs-real-v2-checkpoint-feature-policy` test constructs an independent
disposable APFS checkpoint, spaceman, object-map, volume and catalog, mounts it
through the production Common V2 API, checks unchanged image bytes and stale
handles, then injects checksum-correct unsupported features and malformed
object identity. The test-only fixture callbacks are never in the shipped
`APFS.so`.

These checks do not implement filesystem pathname traversal, file reads,
directory enumeration, compressed or sealed system volumes, or a freestanding
firmware ELF. **BOOT compatibility remains unqualified**, and no new release
should be published merely because the hosted regression and package CI pass.

The original APFS mutation/defragmentation worker, fixture formatters and
their former V1 interface remain in source **for regression tests only**.
They are excluded from the installed `APFS.so`. No Linux kernel `.ko` is built
or bundled as a runtime substitute. Native Linux source is retained for
provenance and independent future migration work.

The `.deb` and source-embedded `.run` are CI qualification artifacts while
firmware/path-I/O acceptance remains incomplete; they are not automatically
published as newly qualified BOOT releases.
