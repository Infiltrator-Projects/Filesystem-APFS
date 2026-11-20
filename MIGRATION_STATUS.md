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
