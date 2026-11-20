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

The original APFS mutation/defragmentation worker, fixture formatters and
their former V1 interface remain in source **for regression tests only**.
They are excluded from the installed `APFS.so`. No Linux kernel `.ko` is built
or bundled as a runtime substitute. Native Linux source is retained for
provenance and independent future migration work.

The `.deb` and source-embedded `.run` are CI qualification artifacts while
firmware/path-I/O acceptance remains incomplete; they are not automatically
published as newly qualified BOOT releases.
