/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "apfs_disk.h"
#include <string.h>
int apfs_v2_probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant) {
    uint8_t initial[4096];
    uint8_t *block = initial;
    int rc;
    if (!variant) return IFS_FS_INVALID;
    *variant = 0u;
    if (!ifs_fs_host_v2_valid(h)) return IFS_FS_INVALID;
    if (h->volume_bytes < sizeof(initial) ||
        h->volume_bytes < h->logical_sector_bytes)
        return IFS_FS_NOT_FOUND;
    /* Read the superblock prefix through Common V2's sector-safe byte
     * reader. A 4096-byte APFS prefix on 8K/16K/64K logical media must
     * not issue an unaligned 4096-byte BlockIo request. */
    rc = ifs_fs_host_read_bytes_v2(h, 0, initial, sizeof(initial));
    if (rc) return rc;
    if (memcmp(initial + 32, "NXSB", 4)) return IFS_FS_NOT_FOUND;
    if (infiltratr_load_le32(initial + 24)!=APFS_NX_OBJECT_TYPE ||
        infiltratr_load_le32(initial + 28)!=0)
        return IFS_FS_CORRUPT;
    uint32_t block_size = infiltratr_load_le32(initial + 36);
    uint64_t count = infiltratr_load_le64(initial + 40);
    if (block_size < 4096 || block_size > 65536 ||
        (block_size & (block_size - 1u)) || count == 0 ||
        count > h->volume_bytes / block_size)
        return IFS_FS_CORRUPT;
    if (block_size > sizeof(initial)) {
        block = h->allocate(h->context, block_size);
        if (!block) return IFS_FS_NO_MEMORY;
        memcpy(block, initial, sizeof(initial));
        rc = ifs_fs_host_read_bytes_v2(h, sizeof(initial),
                                       block + sizeof(initial),
                                       block_size - sizeof(initial));
        if (rc) {
            h->release(h->context, block);
            return rc;
        }
    }
    rc = checksum_valid(block, block_size)
         ? IFS_FS_OK : IFS_FS_CORRUPT;
    /* A checksummed NXSB can still contain impossible checkpoint geometry.
     * Validate physical descriptor bounds before allowing a BOOT probe.
     * Non-contiguous rings are legitimate APFS but not supported by the
     * current reader; do not misclassify them as corrupt media. */
    if (rc == IFS_FS_OK) {
        const uint32_t descriptors = infiltratr_load_le32(block + 104u);
        const uint64_t descriptor_base = infiltratr_load_le64(block + 112u);
        const uint32_t index = infiltratr_load_le32(block + 136u);
        const uint32_t length = infiltratr_load_le32(block + 140u);
        if (descriptors) {
            if (descriptor_base & (UINT64_C(1) << 63u))
                rc = IFS_FS_UNSUPPORTED;
            else if (descriptor_base >= count ||
                     descriptors > count - descriptor_base ||
                     index >= descriptors || length > descriptors)
                rc = IFS_FS_CORRUPT;
        }
    }
    /* NX_SUPERBLOCK_OID is 1. A matching signature and Fletcher checksum
     * are insufficient if the physical container identity is impossible:
     * an NXSB must have a transaction ID and a nonzero container UUID. */
    if (rc==IFS_FS_OK) {
        uint8_t uuid_nonzero=0u;
        for (unsigned i=0u;i<16u;++i)
            uuid_nonzero|=block[72u+i];
        if (infiltratr_load_le64(block+8u)!=1u || !infiltratr_load_le64(block+16u) || !uuid_nonzero)
            rc=IFS_FS_CORRUPT;
    }
    if (block != initial) h->release(h->context, block);
    if (!rc) *variant = 1u;
    return rc;
}
