# Filesystem-APFS
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

- `/System/Filesystems/APFS.so`
- `/System/Resources/Documentation/Filesystem-APFS.pdf`
- `/System/Kernel/Filesystems/apfs.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.

## Common V2 runtime (1.0.9)

The installed APFS.so exports `infiltrator_filesystem_api_v2`, without
its former V1 exported module interface. The canonical APFS engine is
retained, with V2 bounded host-I/O probing, checksum and checkpoint
validation, read-only mount/unmount, and verified volume allocation query.
Path-level reads, mutation, formatting, defragmentation and recovery are
**not** yet qualified as Common V2 operations; they are not advertised
by this API. The old path-based fixture functionality remains source
code, not an exported production V1 descriptor.
