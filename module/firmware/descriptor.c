/* SPDX-License-Identifier: GPL-3.0-or-later
 * Firmware Common V2 APFS provider. The same native filesystem reader
 * is compiled into the hosted carrier and this freestanding ELF payload.
 */
#include "apfs_v2_reader.h"
#include "apfs_disk.h"
#include <stdint.h>
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
static int mount(const IfsFsHostRuntimeV2 *host,void **output,IfsFsVolumeInfoV2 *info)
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
static int unmount(void *volume)
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
static int stat_path(void *volume,const char *path,IfsFsEntryV2 *entry)
{
    IfsApfsReader *r=find_reader(volume);
    return r?ifs_apfs_reader_stat(r,path,entry):IFS_FS_INVALID;
}
static int list_path(void *volume,const char *path,uint32_t index,IfsFsEntryV2 *entry)
{
    IfsApfsReader *r=find_reader(volume);
    return r?ifs_apfs_reader_list(r,path,index,entry):IFS_FS_INVALID;
}
static int read_file(void *volume,const char *path,uint64_t offset,void *data,uint32_t n)
{
    IfsApfsReader *r=find_reader(volume);
    return r?ifs_apfs_reader_read(r,path,offset,data,n):IFS_FS_INVALID;
}
static const IfsFsCoreOpsV2 core = {
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),IFS_FS_GROUP_CORE,0u,
             IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
             IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
    .probe=apfs_v2_probe,.mount=mount,.unmount=unmount,
    .stat=stat_path,.directory_entry=list_path,.read=read_file
};
static const IfsFsGroupHeaderV2 *query(uint32_t id,uint32_t low,uint32_t high)
{
    return id==IFS_FS_GROUP_CORE && low<=IFS_FS_GROUP_ABI_V2 &&
           high>=IFS_FS_GROUP_ABI_V2 ? &core.header:NULL;
}
static const IfsFsModuleApiV2 api={
    IFS_FS_COMMON_ABI_V2,sizeof(IfsFsModuleApiV2),"apfs",
    IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,
    IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE),query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
