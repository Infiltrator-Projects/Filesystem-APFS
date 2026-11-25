/* SPDX-License-Identifier: GPL-3.0-or-later
 * Firmware Common V2 APFS provider. The same native filesystem reader
 * is compiled into the hosted carrier and this freestanding ELF payload.
 */
#include "apfs_v2_reader.h"
#include "apfs_disk.h"
#include <stdint.h>
#include <stdatomic.h>

/* Protect the native reader and session registry against concurrent and
 * reentrant host callbacks. Return BUSY rather than waiting inside host
 * I/O or freeing a live reader. This is an exclusion gate, not a claim
 * of parallel I/O across mounted volumes. */
static atomic_flag operation_gate=ATOMIC_FLAG_INIT;
static int enter_operation(void)
{
    return !atomic_flag_test_and_set_explicit(&operation_gate,memory_order_acquire);
}
static int leave_operation(int rc)
{
    atomic_flag_clear_explicit(&operation_gate,memory_order_release);
    return rc;
}

#include <stddef.h>

enum { MAX_SESSIONS = 32 };
static struct { uintptr_t token; IfsApfsReader *reader; } sessions[MAX_SESSIONS];
static uintptr_t sequence;
static IfsApfsReader *find_reader(void *volume)
{
    uintptr_t id=(uintptr_t)volume;
    if(!id)return NULL;
    for(unsigned i=0;i<MAX_SESSIONS;++i)
        if(sessions[i].token==id)return sessions[i].reader;
    return NULL;
}
static int mount_impl(const IfsFsHostRuntimeV2 *host,void **output,IfsFsVolumeInfoV2 *info)
{
    if(!output || !info || info->structure_bytes<sizeof(*info) ||
       !ifs_fs_host_v2_valid(host))return IFS_FS_INVALID;
    *output=NULL;
    if(!(host->capabilities&IFS_FS_HOST_READ_ONLY))return IFS_FS_ACCESS;
    unsigned slot=0u;
    while(slot<MAX_SESSIONS&&sessions[slot].token)++slot;
    if(slot==MAX_SESSIONS||sequence==UINTPTR_MAX)return IFS_FS_BUSY;
    IfsApfsReader *reader=NULL;
    uint32_t block=0u;uint64_t bytes=0u;
    int rc=ifs_apfs_reader_open(host,&reader,&block,&bytes);
    if(rc!=IFS_FS_OK)return rc;
    uintptr_t token=++sequence;
    sessions[slot].token=token;sessions[slot].reader=reader;
    info->variant=1u;info->max_name_bytes=255u;
    info->block_bytes=block;info->total_bytes=bytes;
    info->free_bytes=0u; /* No unsupported free-space estimate. */
    *output=(void *)token;
    return IFS_FS_OK;
}
static int mount(const IfsFsHostRuntimeV2 *host, void **output, IfsFsVolumeInfoV2 *info)
{
    if (output) *output=NULL;
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(mount_impl(host,output,info));
}
static int unmount_impl(void *volume)
{
    uintptr_t id=(uintptr_t)volume;
    for(unsigned i=0;i<MAX_SESSIONS;++i){
        if(id&&sessions[i].token==id){
            IfsApfsReader *reader=sessions[i].reader;
            sessions[i].token=0u;sessions[i].reader=NULL;
            ifs_apfs_reader_close(reader);
            return IFS_FS_OK;
        }
    }
    return IFS_FS_INVALID;
}
static int unmount(void *handle)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(unmount_impl(handle));
}
static int stat_path_impl(void *volume,const char *path,IfsFsEntryV2 *entry)
{
    IfsApfsReader *r=find_reader(volume);
    return r?ifs_apfs_reader_stat(r,path,entry):IFS_FS_INVALID;
}
static int stat_path(void *handle,const char *path,IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(stat_path_impl(handle,path,entry));
}
static int list_path_impl(void *volume,const char *path,uint32_t index,IfsFsEntryV2 *entry)
{
    IfsApfsReader *r=find_reader(volume);
    return r?ifs_apfs_reader_list(r,path,index,entry):IFS_FS_INVALID;
}
static int list_path(void *handle,const char *path,uint32_t index,IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(list_path_impl(handle,path,index,entry));
}
static int read_file_impl(void *volume,const char *path,uint64_t offset,void *data,uint32_t n)
{
    IfsApfsReader *r=find_reader(volume);
    return r?ifs_apfs_reader_read(r,path,offset,data,n):IFS_FS_INVALID;
}
static int read_file(void *handle,const char *path,uint64_t offset,void *data,uint32_t length)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(read_file_impl(handle,path,offset,data,length));
}
static const IfsFsCoreOpsV2 core = {
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),IFS_FS_GROUP_CORE,0u,
             IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
             IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
    .probe=apfs_v2_probe,.mount=mount,.unmount=unmount,
    .stat=stat_path,.directory_entry=list_path,.read=read_file
};
/* Freestanding metadata and label reads share the hosted native reader.
 * No free-space estimate or write operations are advertised here. */
static int volume_label(void *handle,char *out,uint32_t cap,uint32_t *bytes)
{
    if(!bytes)return IFS_FS_INVALID;
    *bytes=0u;
    if(!enter_operation())return IFS_FS_BUSY;
    IfsApfsReader *r=find_reader(handle);
    return leave_operation(r?ifs_apfs_reader_label(r,out,cap,bytes):IFS_FS_INVALID);
}
static int metadata_get(void *handle,const char *path,IfsFsMetadataV2 *out)
{
    if(!enter_operation())return IFS_FS_BUSY;
    IfsApfsReader *r=find_reader(handle);
    return leave_operation(r?ifs_apfs_reader_metadata(r,path,out):IFS_FS_INVALID);
}
static const IfsFsVolumeOpsV2 volume={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(volume),IFS_FS_GROUP_VOLUME,0u,IFS_FS_VOLUME_LABEL_GET},
    .label=volume_label
};
static const IfsFsMetadataOpsV2 metadata_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(metadata_ops),IFS_FS_GROUP_METADATA,0u,IFS_FS_META_GET_MODE},
    .get=metadata_get
};
static const IfsFsGroupHeaderV2 *query(uint32_t id,uint32_t low,uint32_t high)
{
    if(low>IFS_FS_GROUP_ABI_V2 || high<IFS_FS_GROUP_ABI_V2)return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume.header;
    if(id==IFS_FS_GROUP_METADATA)return &metadata_ops.header;
    return NULL;
}
static const IfsFsModuleApiV2 api={
    IFS_FS_COMMON_ABI_V2,sizeof(IfsFsModuleApiV2),"apfs",
    IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,
    IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|
    IFS_FS_GROUP_BIT(IFS_FS_GROUP_METADATA),query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
