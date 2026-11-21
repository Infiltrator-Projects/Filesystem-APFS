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
static int probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant) {
    uint8_t initial[4096];
    uint8_t *block = initial;
    int rc;
    if (!variant || !ifs_fs_host_v2_valid(h)) return IFS_FS_INVALID;
    if (h->volume_bytes < sizeof(initial) ||
        h->volume_bytes < h->logical_sector_bytes)
        return IFS_FS_NOT_FOUND;
    /* Read the superblock prefix through Common V2's sector-safe byte
     * reader. A 4096-byte APFS prefix on 8K/16K/64K logical media must
     * not issue an unaligned 4096-byte BlockIo request. */
    rc = ifs_fs_host_read_bytes_v2(h, 0, initial, sizeof(initial));
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
        rc = ifs_fs_host_read_bytes_v2(h, sizeof(initial),
                                       block + sizeof(initial),
                                       block_size - sizeof(initial));
        if (rc) {
            h->release(h->context, block);
            return rc;
        }
    }
    rc = le64(block) == checksum(block, block_size)
         ? IFS_FS_OK : IFS_FS_CORRUPT;
    /* NX_SUPERBLOCK_OID is 1. A matching signature and Fletcher checksum
     * are insufficient if the physical container identity is impossible:
     * an NXSB must have a transaction ID and a nonzero container UUID. */
    if (rc==IFS_FS_OK) {
        uint8_t uuid_nonzero=0u;
        for (unsigned i=0u;i<16u;++i)
            uuid_nonzero|=block[72u+i];
        if (le64(block+8u)!=1u || !le64(block+16u) || !uuid_nonzero)
            rc=IFS_FS_CORRUPT;
    }
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

/* The native APFS state is heap allocated but an ABI handle is an opaque,
 * monotonic token. Never dereference a stale pointer after its unmount. */
#define APFS_V2_MAX_SESSIONS 32u
static struct {
    uintptr_t token;
    MountedApfs *volume;
} sessions[APFS_V2_MAX_SESSIONS];
static uintptr_t token_sequence;
static MountedApfs *find_session(void *handle)
{
    const uintptr_t token=(uintptr_t)handle;
    if (!token) return NULL;
    for (size_t i=0;i<APFS_V2_MAX_SESSIONS;++i)
        if (sessions[i].token==token) return sessions[i].volume;
    return NULL;
}
static size_t unused_session(void)
{
    for (size_t i=0;i<APFS_V2_MAX_SESSIONS;++i)
        if (!sessions[i].token) return i;
    return APFS_V2_MAX_SESSIONS;
}

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
    /* APFS checkpoint/catalogue validation is a read-only V2 snapshot.
     * Writable mounts require separately qualified CoW transaction and
     * recovery operations, not merely an exclusive host descriptor. */
    if (!(host->capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS;
    const size_t slot=unused_session();
    if (slot==APFS_V2_MAX_SESSIONS || token_sequence==UINTPTR_MAX)
        return IFS_FS_BUSY;
    int result = probe(host, &variant);
    if (result != IFS_FS_OK) return result;
    volume = host->allocate(host->context, sizeof(*volume));
    if (!volume) return IFS_FS_NO_MEMORY;
    memset(volume, 0, sizeof(*volume));
    /* Earlier V2 hosts do not allocate later optional fields. Retain only
     * the prefix they supplied, bounded by this volume's storage. */
    const size_t host_bytes = host->structure_bytes < sizeof(volume->host) ?
        host->structure_bytes : sizeof(volume->host);
    memcpy(&volume->host, host, host_bytes);
    volume->host.structure_bytes = (uint32_t)host_bytes;
    const int analysis_status =
        apfs_analyse_host_v2(host, &volume->media, error, sizeof(error));
    if (analysis_status != IFS_FS_OK) {
        host->release(host->context, volume);
        return analysis_status;
    }
    result = volume_fill(volume, info);
    if (result != IFS_FS_OK) {
        apfs_analysis_free(&volume->media);
        host->release(host->context, volume);
        return result;
    }
    const uintptr_t token=++token_sequence;
    sessions[slot].token=token;
    sessions[slot].volume=volume;
    *output=(void *)token;
    return IFS_FS_OK;
}

static int native_unmount(void *handle)
{
    MountedApfs *volume=find_session(handle);
    if (!mounted_valid(volume)) return IFS_FS_INVALID;
    for (size_t i=0;i<APFS_V2_MAX_SESSIONS;++i) {
        if (sessions[i].token==(uintptr_t)handle) {
            sessions[i].token=0u;
            sessions[i].volume=NULL;
            break;
        }
    }
    IfsFsHostRuntimeV2 host=volume->host;
    apfs_analysis_free(&volume->media);
    host.release(host.context, volume);
    return IFS_FS_OK;
}

static int volume_query(void *handle, IfsFsVolumeInfoV2 *info)
{
    return volume_fill(find_session(handle), info);
}

static const IfsFsVolumeOpsV2 volume_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
               IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY},
    .query = volume_query
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
