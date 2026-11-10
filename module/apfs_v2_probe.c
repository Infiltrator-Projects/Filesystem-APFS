/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native Common V2 APFS media probe. No legacy callbacks, path or fd I/O.
 * This does not claim mount, format, write or defragmentation support.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "apfs_native.h"
#include <stdint.h>
#include <string.h>
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    return le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static uint64_t checksum(const uint8_t *p, uint32_t bytes) {
    const uint64_t mod = UINT64_C(0xffffffff);
    uint64_t a = 0, b = 0;
    for (uint32_t i = 8; i + 4 <= bytes; i += 4) {
        a = (a + le32(p + i)) % mod;
        b = (b + a) % mod;
    }
    uint64_t x = mod - ((a + b) % mod);
    return ((mod - ((a + x) % mod)) << 32) | x;
}
static int bounded_read(const IfsFsHostRuntimeV2 *h, uint64_t pos,
                        uint8_t *out, uint32_t bytes) {
    const uint32_t step = h->maximum_transfer_bytes -
        h->maximum_transfer_bytes % h->logical_sector_bytes;
    if (!step || bytes % h->logical_sector_bytes ||
        pos > h->volume_bytes || bytes > h->volume_bytes - pos)
        return IFS_FS_INVALID;
    while (bytes) {
        uint32_t n = bytes < step ? bytes : step;
        if (h->read_at(h->context, pos, out, n) != 0)
            return IFS_FS_IO_ERROR;
        out += n; pos += n; bytes -= n;
    }
    return IFS_FS_OK;
}
static int probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant) {
    uint8_t initial[4096];
    uint8_t *block = initial;
    int rc;
    if (!variant || !ifs_fs_host_v2_valid(h)) return IFS_FS_INVALID;
    if (h->volume_bytes < sizeof(initial) ||
        h->logical_sector_bytes > sizeof(initial))
        return IFS_FS_NOT_FOUND;
    rc = bounded_read(h, 0, initial, sizeof(initial));
    if (rc) return rc;
    if (memcmp(initial + 32, "NXSB", 4)) return IFS_FS_NOT_FOUND;
    /* Reject a checksummed NXSB-like payload with the wrong APFS object
     * type or storage class: this must be the physical NX superblock. */
    if ((le32(initial + 24) & UINT32_C(0x0000ffff)) != 1u ||
        (le32(initial + 24) & UINT32_C(0xc0000000)) != UINT32_C(0x40000000))
        return IFS_FS_CORRUPT;
    uint32_t block_size = le32(initial + 36);
    uint64_t count = le64(initial + 40);
    if (block_size < 4096 || block_size > 65536 ||
        (block_size & (block_size - 1u)) || count == 0 ||
        count > h->volume_bytes / block_size)
        return IFS_FS_CORRUPT;
    if (block_size > sizeof(initial)) {
        block = h->allocate(h->context, block_size);
        if (!block) return IFS_FS_NO_MEMORY;
        memcpy(block, initial, sizeof(initial));
        rc = bounded_read(h, sizeof(initial), block + sizeof(initial),
                          block_size - sizeof(initial));
        if (rc) {
            h->release(h->context, block);
            return rc;
        }
    }
    rc = le64(block) == checksum(block, block_size)
         ? IFS_FS_OK : IFS_FS_CORRUPT;
    if (block != initial) h->release(h->context, block);
    if (!rc) *variant = 1u;
    return rc;
}
/* The host-backed mount invokes the same complete checkpoint, spaceman
 * and catalogue validator used by the APFS native engine. The mounted state
 * reports verified allocation totals; no unimplemented path or writer API
 * is exposed as a successful callback. */
typedef struct {
    IfsFsHostRuntimeV2 host;
    ApfsAnalysis media;
} MountedApfs;

static int mounted_valid(const MountedApfs *volume)
{
    return volume && volume->media.block_size &&
        ifs_fs_host_v2_valid(&volume->host);
}

static int volume_fill(const MountedApfs *volume, IfsFsVolumeInfoV2 *info)
{
    if (!mounted_valid(volume) || !info ||
        info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    info->variant = 1u;
    info->max_name_bytes = 255u;
    info->block_bytes = volume->media.block_size;
    info->total_bytes = volume->media.block_count *
                        (uint64_t)volume->media.block_size;
    info->free_bytes = volume->media.free_blocks *
                       (uint64_t)volume->media.block_size;
    return IFS_FS_OK;
}

static int native_mount(const IfsFsHostRuntimeV2 *host, void **output,
                        IfsFsVolumeInfoV2 *info)
{
    MountedApfs *volume;
    char error[256] = {0};
    uint32_t variant = 0u;
    if (!output || !info || info->structure_bytes < sizeof(*info) ||
        !ifs_fs_host_v2_valid(host))
        return IFS_FS_INVALID;
    *output = NULL;
    int result = probe(host, &variant);
    if (result != IFS_FS_OK) return result;
    volume = host->allocate(host->context, sizeof(*volume));
    if (!volume) return IFS_FS_NO_MEMORY;
    memset(volume, 0, sizeof(*volume));
    volume->host = *host;
    if (apfs_analyse_host(host, &volume->media, error, sizeof(error)) != 0) {
        host->release(host->context, volume);
        return IFS_FS_CORRUPT;
    }
    result = volume_fill(volume, info);
    if (result != IFS_FS_OK) {
        apfs_analysis_free(&volume->media);
        host->release(host->context, volume);
        return result;
    }
    *output = volume;
    return IFS_FS_OK;
}

static int native_unmount(void *handle)
{
    MountedApfs *volume = handle;
    if (!mounted_valid(volume)) return IFS_FS_INVALID;
    IfsFsHostRuntimeV2 host = volume->host;
    apfs_analysis_free(&volume->media);
    host.release(host.context, volume);
    return IFS_FS_OK;
}

static const IfsFsVolumeOpsV2 volume_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
               IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY},
    .query = (int (*)(void *, IfsFsVolumeInfoV2 *))volume_fill
};

static const IfsFsCoreOpsV2 core = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
                IFS_FS_GROUP_CORE, 0, IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT },
    .probe = probe,
    .mount = native_mount,
    .unmount = native_unmount
};
static const IfsFsGroupHeaderV2 *query(uint32_t id, uint32_t lo, uint32_t hi) {
    if (lo > IFS_FS_GROUP_ABI_V2 || hi < IFS_FS_GROUP_ABI_V2)
        return NULL;
    if (id == IFS_FS_GROUP_CORE) return &core.header;
    if (id == IFS_FS_GROUP_VOLUME) return &volume_ops.header;
    return NULL;
}
static const IfsFsModuleApiV2 api = {
    IFS_FS_COMMON_ABI_V2, sizeof(IfsFsModuleApiV2), "apfs",
    IFS_FS_PROFILE_HOSTED, IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME), query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) { return &api; }
