# Filesystem-APFS
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

- `/System/Filesystems/APFS.so`
- `/System/Resources/Documentation/Filesystem-APFS.pdf`
- `/System/Kernel/Filesystems/apfs.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.

## Common V2 migration (1.0.10)

APFS.so exports canonical Common V2 bounded-host probing, checksum and
checkpoint validation, read-only mount/unmount and volume accounting.
The format-specific worker and fixture interface remains exported until
Defragmenter's APFS loader migrates, preserving working functionality
rather than falsely treating probe-only V2 as a complete replacement.
V2 path I/O, mutation, defragmentation and recovery are not yet
qualified or advertised.
