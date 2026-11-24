/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native Common V2 APFS media probe. No legacy callbacks, path or fd I/O.
 * Hosted mount and format are implemented; mounted file mutation is not.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "apfs_native.h"
#include "apfs_disk.h"
#include "apfs_v2_reader.h"
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

static int native_mount_impl(const IfsFsHostRuntimeV2 *host, void **output,
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
static int native_mount(const IfsFsHostRuntimeV2 *host, void **output, IfsFsVolumeInfoV2 *info)
{
    if (output) *output=NULL;
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(native_mount_impl(host,output,info));
}

static int native_unmount_impl(void *handle)
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
static int native_unmount(void *handle)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(native_unmount_impl(handle));
}

static int volume_query_impl(void *handle, IfsFsVolumeInfoV2 *info)
{
    return volume_fill(find_session(handle), info);
}
static int volume_query(void *handle,IfsFsVolumeInfoV2 *info)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(volume_query_impl(handle,info));
}

/* Hosted, single-device, unencrypted APFS v2 creation. The initial
 * profile is case- and normalization-sensitive, with 4 KiB blocks and
 * one volume. On-disk structures follow the Apple APFS reference and
 * the independently documented apfsprogs layout (including spaceman
 * internal-pool accounting and the two reserved directory records).
 * Formatting is not a writable mount or a general CoW transaction API.
 * The descriptor is published only after independent apfsck qualification.
 */
enum { F_BLOCK=4096u,F_CHUNK=32768u,F_DESC=8u,F_DATA=9u,
       F_SM=1024u,F_REAPER=1025u,F_VOL=1026u,F_CAT=1027u,
       F_IPQ=1028u,F_MAINQ=1029u };
#define F_PHYS UINT32_C(0x40000000)
#define F_EPH UINT32_C(0x80000000)
static void f16(uint8_t *p,uint32_t x)
{p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void f32(uint8_t *p,uint32_t x)
{f16(p,x);f16(p+2,x>>16);}
static void f64(uint8_t *p,uint64_t x)
{f32(p,(uint32_t)x);f32(p+4,(uint32_t)(x>>32));}
static uint32_t fget32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void fmt_object(uint8_t *p,uint64_t oid,uint32_t type,uint32_t subtype)
{
    memset(p,0,F_BLOCK);f64(p+8,oid);f64(p+16,1u);
    f32(p+24,type);f32(p+28,subtype);
}
static int fmt_write(const IfsFsHostRuntimeV2 *h,uint64_t block,uint8_t *p,int object)
{
    if(h->cancelled && h->cancelled(h->context))return IFS_FS_BUSY;
    if(object) {
        uint64_t a=0u,b=0u,mod=UINT64_C(0xffffffff);
        for(unsigned i=8u;i<F_BLOCK;i+=4u){a=(a+fget32(p+i))%mod;b=(b+a)%mod;}
        const uint64_t c=mod-(a+b)%mod,d=mod-(a+c)%mod;
        f64(p,(d<<32)|c);
    }
    return ifs_fs_host_write_bytes_v2(h,block*F_BLOCK,p,F_BLOCK);
}
typedef struct {
    uint8_t raw[F_BLOCK];
    uint16_t toc,keys,values,count,maxkey,maxval;
    int fixed;
} FormatNode;
static void node_begin(FormatNode *n,uint64_t oid,uint32_t storage,uint32_t subtype,
                       uint32_t flags,uint16_t keysize,uint16_t valsize)
{
    memset(n,0,sizeof(*n));fmt_object(n->raw,oid,storage|2u,subtype);
    n->fixed=keysize!=0u;
    n->toc=n->fixed?(uint16_t)(4u*((F_BLOCK-56u)/(4u+keysize+valsize))):128u;
    f16(n->raw+32,n->fixed?7u:3u);f16(n->raw+42,n->toc);
    f16(n->raw+48,0xffffu);f16(n->raw+52,0xffffu);
    uint8_t *footer=n->raw+F_BLOCK-40u;
    f32(footer,flags);f32(footer+4,F_BLOCK);
    f32(footer+8,keysize);f32(footer+12,valsize);
    n->maxkey=keysize;n->maxval=valsize;f64(footer+32,1u);
}
static int node_add(FormatNode *n,const uint8_t *key,uint16_t kl,const uint8_t *value,uint16_t vl)
{
    const uint32_t es=n->fixed?4u:8u,base=56u+n->toc,end=F_BLOCK-40u;
    if((n->count+1u)*es>n->toc || base+n->keys+kl+n->values+vl>end)
        return IFS_FS_CORRUPT;
    uint8_t *slot=n->raw+56u+es*n->count++;
    f16(slot,n->keys);memcpy(n->raw+base+n->keys,key,kl);n->keys+=kl;
    n->values+=vl;memcpy(n->raw+end-n->values,value,vl);
    if(n->fixed)f16(slot+2,n->values);
    else {f16(slot+2,kl);f16(slot+4,n->values);f16(slot+6,vl);}
    if(kl>n->maxkey)n->maxkey=kl;
    if(vl>n->maxval)n->maxval=vl;
    return IFS_FS_OK;
}
static int node_write(const IfsFsHostRuntimeV2 *h,FormatNode *n,uint64_t at)
{
    f32(n->raw+36,n->count);f16(n->raw+44,n->keys);
    f16(n->raw+46,F_BLOCK-40u-56u-n->toc-n->keys-n->values);
    uint8_t *footer=n->raw+F_BLOCK-40u;
    f32(footer+16,n->maxkey);f32(footer+20,n->maxval);
    f64(footer+24,n->count);
    return fmt_write(h,at,n->raw,1);
}
static int format_apfs_impl(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                            const char *label,uint32_t flags)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    if(variant!=1u || flags || h->logical_sector_bytes>F_BLOCK ||
       h->volume_bytes%F_BLOCK || h->volume_bytes<128ull*1024u*1024u ||
       h->volume_bytes>256ull*1024u*1024u)return IFS_FS_UNSUPPORTED;
    for(unsigned i=0;i<APFS_V2_MAX_SESSIONS;++i)
        if(sessions[i].token)return IFS_FS_BUSY;
    if(!label || !*label)label="Untitled";
    if(strlen(label)>127u)return IFS_FS_INVALID;
    for(const char *p=label;*p;++p)
        if((unsigned char)*p<32u || (unsigned char)*p>126u ||
           *p=='/' || *p==':')return IFS_FS_INVALID;
    const uint32_t total=(uint32_t)(h->volume_bytes/F_BLOCK);
    const uint32_t chunks=(total+F_CHUNK-1u)/F_CHUNK;
    /* Reserve a complete descriptor and data checkpoint ring. */
    const uint32_t data_blocks=total<36353u?160u+36u*((total-13633u)/4544u):
                                 308u+4u*((total-36353u)/4544u);
    const uint32_t co=F_DATA+data_blocks,ib=co+10u,ip=ib+16u;
    const uint32_t ipblocks=3u*(chunks+1u),used=ip+ipblocks-2u;
    const uint32_t free_blocks=total-used;
    const uint32_t cib=ip+1u;
    uint8_t uuids[32],raw[F_BLOCK]={0},key[32]={0},value[128]={0};
    int rc=ifs_fs_host_random_bytes_v2(h,uuids,sizeof(uuids));
    if(rc!=IFS_FS_OK)return rc;
    for(unsigned i=0;i<2u;++i) {
        uuids[i*16u+6]=(uint8_t)((uuids[i*16u+6]&15u)|64u);
        uuids[i*16u+8]=(uint8_t)((uuids[i*16u+8]&63u)|128u);
    }
    uint64_t now=h->realtime_ns?h->realtime_ns(h->context):0u;
    FormatNode *node=h->allocate(h->context,sizeof(*node));
    if(!node)return IFS_FS_NO_MEMORY;
#define WRITE(at,obj) do{rc=fmt_write(h,(at),raw,(obj));if(rc!=IFS_FS_OK)goto done;}while(0)
#define NODE(at) do{rc=node_write(h,node,(at));if(rc!=IFS_FS_OK)goto done;}while(0)
#define ADD(k,kl,v,vl) do{rc=node_add(node,(k),(kl),(v),(vl));if(rc!=IFS_FS_OK)goto done;}while(0)
    /* Remove old checkpoint signatures, then populate the complete new
     * reserved region. The primary identity is the final commit point. */
    WRITE(0u,0);
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}
    for(uint32_t i=1u;i<ip+ipblocks;++i)WRITE(i,0);

    /* Reaper and both ephemeral free queues. The internal pool is
     * reserved in the device bitmap, but only two pool blocks are live. */
    fmt_object(raw,F_REAPER,F_EPH|17u,0u);
    f64(raw+32,1u);f32(raw+64,1u);f32(raw+108,F_BLOCK-112u);
    WRITE(F_DATA,1);
    node_begin(node,F_IPQ,F_EPH,9u,12u,16u,8u);NODE(F_DATA+2u);
    node_begin(node,F_MAINQ,F_EPH,9u,12u,16u,8u);NODE(F_DATA+3u);
    fmt_object(raw,F_SM,F_EPH|5u,0u);
    f32(raw+32,F_BLOCK);f32(raw+36,F_CHUNK);f32(raw+40,126u);f32(raw+44,507u);
    f64(raw+48,total);f64(raw+56,chunks);f32(raw+64,1u);
    f64(raw+72,free_blocks);f32(raw+80,384u);f32(raw+128,392u);
    f32(raw+148,16u);f64(raw+152,ipblocks);f32(raw+160,1u);
    f32(raw+164,16u);f64(raw+168,ib);f64(raw+176,ip);
    f64(raw+208,F_IPQ);f16(raw+224,1u);
    f64(raw+248,F_MAINQ);f16(raw+264,1u+(total-1u)/4544u);
    f16(raw+320,1u);f16(raw+322,15u);f32(raw+324,336u);
    f32(raw+328,344u);f32(raw+332,352u);f64(raw+336,1u);
    for(unsigned i=0;i<16u;++i)f16(raw+352u+2u*i,i==0u||i==15u?0xffffu:i+1u);
    f64(raw+384,cib);WRITE(F_DATA+1u,1);
    memset(raw,0,sizeof(raw));raw[0]=3u;WRITE(ib,0);
    memset(raw,0,sizeof(raw));
    for(uint32_t i=0;i<ip+ipblocks;++i)
        if(i!=co+8u && i!=co+9u)raw[i/8u]|=(uint8_t)(1u<<(i%8u));
    WRITE(ip,0);
    fmt_object(raw,cib,F_PHYS|7u,0u);f32(raw+36,chunks);
    for(uint32_t i=0;i<chunks;++i) {
        uint8_t *c=raw+40u+32u*i;
        uint32_t count=total-i*F_CHUNK;
        if(count>F_CHUNK)count=F_CHUNK;
        f64(c,1u);f64(c+8,(uint64_t)i*F_CHUNK);f32(c+16,count);
        f32(c+20,count-(i==0u?used:0u));f64(c+24,i==0u?ip:0u);
    }
    WRITE(cib,1);

    /* Both object maps, including their fixed-size B-tree root records. */
    for(unsigned i=0;i<2u;++i) {
        uint32_t omap=i?co+3u:co,tree=omap+1u;
        fmt_object(raw,omap,F_PHYS|11u,0u);f32(raw+32,i?0u:1u);
        f32(raw+40,F_PHYS|2u);f32(raw+44,F_PHYS|2u);f64(raw+48,tree);
        WRITE(omap,1);
        node_begin(node,tree,F_PHYS,11u,16u,16u,16u);
        memset(key,0,sizeof(key));memset(value,0,sizeof(value));
        f64(key,i?F_CAT:F_VOL);f64(key+8,1u);
        f32(value+4,F_BLOCK);f64(value+8,i?co+5u:co+2u);
        ADD(key,16u,value,16u);NODE(tree);
    }
    /* Catalogue contains the root parent dentries and both reserved
     * directory inodes, including mandatory primary-name xfields. */
    node_begin(node,F_CAT,0u,14u,64u,0u,0u);
    const char *names[]={"private-dir","root"};
    const uint32_t inodes[]={3u,2u};
    for(unsigned i=0;i<2u;++i) {
        const uint16_t len=(uint16_t)(strlen(names[i])+1u);
        memset(key,0,sizeof(key));memset(value,0,sizeof(value));
        f64(key,(UINT64_C(9)<<60)|1u);f16(key+8,len);
        memcpy(key+10,names[i],len);f64(value,inodes[i]);f64(value+8,now);
        f16(value+16,4u);ADD(key,10u+len,value,18u);
    }
    for(unsigned i=0;i<2u;++i) {
        const char *name=names[1u-i];const uint16_t len=(uint16_t)(strlen(name)+1u);
        const uint16_t padded=(uint16_t)((len+7u)&~7u);
        memset(key,0,sizeof(key));memset(value,0,sizeof(value));
        f64(key,(UINT64_C(3)<<60)|(2u+i));f64(value,1u);f64(value+8,2u+i);
        for(unsigned t=0;t<4u;++t)f64(value+16u+8u*t,now);
        f64(value+48,0x8000u);f16(value+80,0040755u);
        f16(value+92,1u);f16(value+94,padded);
        value[96]=4u;value[97]=2u;f16(value+98,len);memcpy(value+100,name,len);
        ADD(key,8u,value,100u+padded);
    }
    NODE(co+5u);
    node_begin(node,co+6u,F_PHYS,15u,80u,0u,0u);NODE(co+6u);
    node_begin(node,co+7u,F_PHYS,16u,80u,0u,0u);NODE(co+7u);
    fmt_object(raw,F_VOL,13u,0u);memcpy(raw+32,"APSB",4u);
    /* No optional hardlink-map feature: this profile matches the
     * current reader's supported feature mask. */
    f64(raw+88,5u);f16(raw+96,5u);f32(raw+104,6u);
    f16(raw+112,1u);f32(raw+116,2u);f32(raw+120,F_PHYS|2u);
    f32(raw+124,F_PHYS|2u);f64(raw+128,co+3u);f64(raw+136,F_CAT);
    f64(raw+144,co+6u);f64(raw+152,co+7u);f64(raw+176,16u);
    memcpy(raw+240,uuids+16,16u);f64(raw+264,1u);
    memcpy(raw+272,"InfiltratorOS APFS",18u);f64(raw+304,now);f64(raw+312,1u);
    memcpy(raw+704,label,strlen(label));f32(raw+960,3u);WRITE(co+2u,1);

    /* The checkpoint map records all four ephemeral objects, not only
     * the space manager. A single transaction establishes the container. */
    fmt_object(raw,1u,F_PHYS|12u,0u);f32(raw+32,1u);f32(raw+36,4u);
    const uint32_t types[]={17u,5u,2u,2u};
    const uint32_t oids[]={F_REAPER,F_SM,F_IPQ,F_MAINQ};
    for(unsigned i=0;i<4u;++i) {
        uint8_t *m=raw+40u+40u*i;
        f32(m,F_EPH|types[i]);f32(m+4,i>=2u?9u:0u);f32(m+8,F_BLOCK);
        f64(m+24,oids[i]);f64(m+32,F_DATA+i);
    }
    WRITE(1u,1);
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}
    fmt_object(raw,1u,F_EPH|1u,0u);memcpy(raw+32,"NXSB",4u);
    f32(raw+36,F_BLOCK);f64(raw+40,total);f64(raw+64,2u);
    memcpy(raw+72,uuids,16u);f64(raw+88,1124u);f64(raw+96,2u);
    f32(raw+104,F_DESC);f32(raw+108,data_blocks);f64(raw+112,1u);f64(raw+120,F_DATA);
    f32(raw+128,2u);f32(raw+132,4u);f32(raw+140,2u);f32(raw+148,4u);
    f64(raw+152,F_SM);f64(raw+160,co);f64(raw+168,F_REAPER);
    f32(raw+180,1u);f64(raw+184,F_VOL);f64(raw+1312,UINT64_C(0x800040001));
    WRITE(2u,1);
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}
    WRITE(0u,1);
    if(h->flush(h->context)!=0) {
        memset(raw,0,sizeof(raw));(void)ifs_fs_host_write_bytes_v2(h,0,raw,sizeof(raw));
        (void)h->flush(h->context);rc=IFS_FS_IO_ERROR;goto done;
    }
    /* Use the same production analyser and namespace reader, without
     * dropping optional host callbacks/capabilities. */
    IfsFsHostRuntimeV2 ro={0};
    const size_t host_bytes=h->structure_bytes<sizeof(ro)?h->structure_bytes:sizeof(ro);
    memcpy(&ro,h,host_bytes);ro.structure_bytes=(uint32_t)host_bytes;
    ro.capabilities=(h->capabilities&~(IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE|
                      IFS_FS_HOST_DISCARD))|IFS_FS_HOST_READ_ONLY;
    ro.write_at=NULL;ro.flush=NULL;ro.discard=NULL;
    void *mounted=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    rc=native_mount_impl(&ro,&mounted,&info);
    if(rc==IFS_FS_OK) {
        const int urc=native_unmount_impl(mounted);
        rc=urc!=IFS_FS_OK?urc:info.free_bytes!=(uint64_t)free_blocks*F_BLOCK?
           IFS_FS_CORRUPT:IFS_FS_OK;
    }
done:
    h->release(h->context,node);return rc;
#undef WRITE
#undef NODE
#undef ADD
}
static int format_volume(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                          const char *label,uint32_t flags)
{
    if(!enter_operation())return IFS_FS_BUSY;
    return leave_operation(format_apfs_impl(h,variant,label,flags));
}

static const IfsFsVolumeOpsV2 volume_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
               IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_FORMAT},
    .query = volume_query,
    .format = format_volume
};

static int native_stat_impl(void *handle,const char *path,IfsFsEntryV2 *entry)
{
    MountedApfs *v=find_session(handle);
    return v ? ifs_apfs_reader_stat(v->reader,path,entry):IFS_FS_INVALID;
}
static int native_stat(void *handle,const char *path,IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(native_stat_impl(handle,path,entry));
}
static int native_list_impl(void *handle,const char *path,uint32_t index,IfsFsEntryV2 *entry)
{
    MountedApfs *v=find_session(handle);
    return v ? ifs_apfs_reader_list(v->reader,path,index,entry):IFS_FS_INVALID;
}
static int native_list(void *handle,const char *path,uint32_t index,IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(native_list_impl(handle,path,index,entry));
}
static int native_read_impl(void *handle,const char *path,uint64_t at,void *data,uint32_t bytes)
{
    MountedApfs *v=find_session(handle);
    return v ? ifs_apfs_reader_read(v->reader,path,at,data,bytes):IFS_FS_INVALID;
}
static int native_read(void *handle,const char *path,uint64_t offset,void *data,uint32_t length)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(native_read_impl(handle,path,offset,data,length));
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
