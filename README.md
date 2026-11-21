# Filesystem-APFS

The installed APFS provider exports the Common V2 filesystem API. Its hosted profile probes containers and provides read-only volume metadata for the subset handled by the native checkpoint, space manager and catalogue code. It installs at `/System/Filesystems/APFS.so`.

The carrier embeds a freestanding ELF which recognises APFS containers. Both profiles check the NX object type and storage flags, identity, geometry and full Fletcher checksum. The firmware profile currently implements CORE probe. It has no namespace or boot-file operations. The independently formatted container tests check the extracted firmware and the installed carrier on 512-byte, 4 KiB and 8 KiB hosts.

The portable Debian and native RUN candidates install the provider and its PDF manual. Historical worker and formatter fixtures are retained in the repository for regression testing.
