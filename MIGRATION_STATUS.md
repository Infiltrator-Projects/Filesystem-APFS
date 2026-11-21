# APFS Common V2 migration status

The native APFS engine and its checksum/checkpoint/spaceman/catalog parser remain
in this repository. On-disk validation is not a replacement for an actual
read-only boot namespace.

## Hosted provider

- Common V2 CORE probe, mount, unmount: implemented and regression tested
- Common V2 VOLUME query: implemented and regression tested
- Common V2 file stat, directory enumeration, path traversal and file reads:
  **not implemented**
- Free-standing ELF/UEFI profile and embedded `.infiltrator_boot` payload:
  **not implemented**
- BOOT production validator: expected rejection for this hosted-only artifact
- OVMF kernel/initramfs read through APFS.so: **not qualified**

## Runtime ABI and packaging boundary

The installed APFS.so is built from the native on-disk analyser and
`module/apfs_v2_probe.c`, with only the Common V2 exported entry point.
The historical V1 worker and fixture interfaces are retained in a test-only
shared library and are not installed. Package qualification explicitly rejects
`.ko` and V1 runtime exports. Source of the original Linux adapter remains in
tree but is not shipped.

The hosted-only CI pipeline deliberately does **not publish** a new firmware
release. Before the module may be called BOOT-compatible, implement and test
real Common V2 namespace reads through a freestanding shared engine, test
valid/corrupt APFS images and nonmutation, and pass the unmodified production
validator and OVMF Linux kernel/initramfs handoff.

## 21 November 2026 — sector-aligned APFS probe repair

The Common V2 APFS NXSB probe now handles valid 8 KiB and larger logical
sectors by using the canonical Common V2 byte-range host reader for both
the 4 KiB prefix and the remainder of the on-disk block. It no longer
issues illegal unaligned 4 KiB BlockIo reads against an 8 KiB sector or
wrongly reports the filesystem missing solely for large host sectors.
The production exported CORE.PROBE is tested with checksum-valid 8 KiB
APFS NXSB media, a checksummed negative case with deliberate corruption,
sector-aligned host callbacks, and a byte-for-byte nonmutation assertion.
This fixes the probe only: APFS namespace, freestanding ELF and real BOOT
validator/OVMF kernel reads remain incomplete.

## 21 November 2026 — checksum-valid NXSB identity rejection

The hosted Common V2 probe now rejects an NX superblock whose physical
object ID is not the NX root object, whose transaction ID is zero or whose
container UUID is all zeros, even when its complete Fletcher checksum is
valid. Production CORE.PROBE regression tests use tampered checksummed
media for the missing UUID and transaction ID cases. This does not add
any APFS file namespace or firmware export.
