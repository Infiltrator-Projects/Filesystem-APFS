/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native Common V2 APFS media probe. No legacy callbacks, path or fd I/O.
 * This does not claim mount, format, write or defragmentation support.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "apfs_native.h"
#include "apfs_disk.h"
#include "apfs_v2_reader.h"
#include <stdint.h>
#include <string.h>
/* The host-backed mount invokes the same complete checkpoint, spaceman
 * and catalogue validator used by the APFS native engine. The mounted state
 * reports verified allocation totals; no unimplemented path or writer API
 * is exposed as a successful callback. */
typedef struct {
    IfsFsHostRuntimeV2 host;
    ApfsAnalysis media;
    IfsApfsReader *reader;
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
    int result = apfs_v2_probe(host, &variant);
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
    uint32_t reader_block = 0u;
    uint64_t reader_bytes = 0u;
    result = ifs_apfs_reader_open(&volume->host, &volume->reader,
                                  &reader_block, &reader_bytes);
    if (result != IFS_FS_OK) {
        apfs_analysis_free(&volume->media);
        host->release(host->context, volume);
        return result;
    }
    /* The namespace must see exactly the geometry already validated by
     * the hosted checkpoint/spaceman analyser. */
    if (reader_block != volume->media.block_size ||
        reader_bytes != volume->media.block_count *
                        (uint64_t)volume->media.block_size) {
        ifs_apfs_reader_close(volume->reader);
        apfs_analysis_free(&volume->media);
        host->release(host->context, volume);
        return IFS_FS_CORRUPT;
    }
    result = volume_fill(volume, info);
    if (result != IFS_FS_OK) {
        ifs_apfs_reader_close(volume->reader);
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
    ifs_apfs_reader_close(volume->reader);
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

static int native_stat(void *handle,const char *path,IfsFsEntryV2 *entry)
{
    MountedApfs *v=find_session(handle);
    return v ? ifs_apfs_reader_stat(v->reader,path,entry):IFS_FS_INVALID;
}
static int native_list(void *handle,const char *path,uint32_t index,IfsFsEntryV2 *entry)
{
    MountedApfs *v=find_session(handle);
    return v ? ifs_apfs_reader_list(v->reader,path,index,entry):IFS_FS_INVALID;
}
static int native_read(void *handle,const char *path,uint64_t at,void *data,uint32_t bytes)
{
    MountedApfs *v=find_session(handle);
    return v ? ifs_apfs_reader_read(v->reader,path,at,data,bytes):IFS_FS_INVALID;
}
static const IfsFsCoreOpsV2 core = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
                IFS_FS_GROUP_CORE, 0, IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT |
                IFS_FS_CORE_STAT | IFS_FS_CORE_LIST | IFS_FS_CORE_READ },
    .probe = apfs_v2_probe,
    .mount = native_mount,
    .unmount = native_unmount,
    .stat = native_stat,
    .directory_entry = native_list,
    .read = native_read
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
