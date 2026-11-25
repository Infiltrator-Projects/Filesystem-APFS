/* SPDX-License-Identifier: GPL-3.0-or-later
 * Read-only APFS namespace for the Common V2 host and UEFI ABI.
 *
 * A mounted volume is a single unencrypted volume whose active checkpoint,
 * physical object maps, and filesystem catalogue can be represented by
 * checksum-valid flat root/leaf B-trees. All I/O is partition-relative and
 * performed solely through the supplied Common V2 host; no Linux VFS or
 * POSIX calls, firmware services, global allocator, or legacy filesystem ABI.
 * Unsupported tree heights and encrypted/snapshot/cloned/compressed layouts
 * fail closed, never exposing imaginary file contents.
 */
#include "apfs_v2_reader.h"
#include "apfs_disk.h"
#include <infiltratr/endian.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define LE16 infiltratr_load_le16
#define LE32 infiltratr_load_le32
#define LE64 infiltratr_load_le64
#define APFS_OBJ_MASK UINT32_C(0x0000ffff)
#define APFS_OBJ_NX UINT32_C(0x80000001)
#define APFS_OBJ_OMAP 11u
#define APFS_OBJ_BTREE 2u
#define APFS_OBJ_BTREE_NODE 3u
#define APFS_OBJ_FSTREE 14u
#define APFS_OBJ_FS 13u
#define APFS_KEY_ID_MASK UINT64_C(0x0fffffffffffffff)
#define APFS_KEY_INODE 3u
#define APFS_KEY_EXTENT 8u
#define APFS_KEY_DREC 9u
#define APFS_MAX_SESSIONS 32u
#define APFS_ROOT_INODE 2u
#define APFS_DIR_MODE 0040000u
#define APFS_REG_MODE 0100000u
#define APFS_LINK_MODE 0120000u
#define APFS_FEATURE_UNENCRYPTED UINT64_C(1)
#define APFS_INODE_CLONED UINT64_C(0x10)
#define APFS_INODE_SPARSE UINT64_C(0x200)
#define APFS_INODE_EVER_CLONED UINT64_C(0x400)
typedef struct {
    const uint8_t *key, *value;
    uint16_t key_len, value_len;
} Record;
typedef struct {
    uint64_t private_id, size, flags;
    uint32_t type,mode,owner,group;
} Inode;
struct IfsApfsReader {
    IfsFsHostRuntimeV2 host;
    uint8_t *catalog;
    uint64_t blocks, xid;
    uint32_t block_size, records;
    uint8_t label[256];
};
static void *allocate(IfsApfsReader *r,size_t n)
{
    return r->host.allocate(r->host.context,n);
}
static void release(IfsApfsReader *r,void *p)
{
    if(p)r->host.release(r->host.context,p);
}
static int read_block(IfsApfsReader *r,uint64_t addr,uint8_t *out)
{
    if(addr>=r->blocks || (addr && r->block_size>UINT64_MAX/addr))
        return IFS_FS_CORRUPT;
    return ifs_fs_host_read_bytes_v2(
        &r->host,addr*(uint64_t)r->block_size,out,r->block_size);
}
static int tree_load(IfsApfsReader *r,uint64_t addr,uint32_t subtype,int fixed,
                     uint8_t **out,uint32_t *count)
{
    uint8_t *b=allocate(r,r->block_size);
    if(!b)return IFS_FS_NO_MEMORY;
    int rc=read_block(r,addr,b);
    if(rc!=IFS_FS_OK){release(r,b);return rc;}
    uint32_t type=LE32(b+24)&APFS_OBJ_MASK;
    uint16_t flags=LE16(b+32), level=LE16(b+34);
    uint32_t n=LE32(b+36);
    uint32_t table_off=LE16(b+40),table_len=LE16(b+42);
    uint32_t stride=fixed?4u:8u;
    if(!checksum_valid(b,r->block_size) ||
       (type!=APFS_OBJ_BTREE && type!=APFS_OBJ_BTREE_NODE) ||
       LE32(b+28)!=subtype ||
       (flags&3u)!=3u || (flags&~7u)!=0u ||
       !!(flags&4u)!=!!fixed ||
       (uint64_t)56u+table_off+table_len>r->block_size-40u ||
       n>(r->block_size-96u)/stride ||
       (uint64_t)n*stride>table_len){
        release(r,b);return IFS_FS_CORRUPT;
    }
    if(level!=0u){release(r,b);return IFS_FS_UNSUPPORTED;}
    *out=b;*count=n;return IFS_FS_OK;
}
static int record_at(const uint8_t *b,uint32_t block_size,uint32_t records,
                     uint32_t index,int fixed,Record *out)
{
    if(index>=records)return IFS_FS_INVALID;
    const uint32_t table=56u+LE16(b+40),len=LE16(b+42);
    const uint64_t key_base=(uint64_t)table+len;
    const uint64_t value_base=block_size-40u;
    const uint8_t *e=b+table+(size_t)index*(fixed?4u:8u);
    uint32_t klen=fixed?16u:LE16(e+2);
    uint32_t vlen=fixed?16u:LE16(e+6);
    uint64_t k=key_base+LE16(e);
    const uint32_t backwards=LE16(e+(fixed?2u:4u));
    if(backwards>value_base)return IFS_FS_CORRUPT;
    uint64_t v=value_base-backwards;
    if(!klen||k>block_size||klen>block_size-k ||
       v>block_size||vlen>block_size-v||k+klen>v)
        return IFS_FS_CORRUPT;
    out->key=b+k;out->key_len=(uint16_t)klen;
    out->value=b+v;out->value_len=(uint16_t)vlen;
    return IFS_FS_OK;
}
static int object_map(IfsApfsReader *r,uint64_t map_addr,
                      uint64_t oid,uint64_t xid,uint64_t *addr)
{
    uint8_t *b=allocate(r,r->block_size);
    if(!b)return IFS_FS_NO_MEMORY;
    int rc=read_block(r,map_addr,b);
    if(rc!=IFS_FS_OK){release(r,b);return rc;}
    if(!checksum_valid(b,r->block_size) ||
       (LE32(b+24)&APFS_OBJ_MASK)!=APFS_OBJ_OMAP){
        release(r,b);return IFS_FS_CORRUPT;
    }
    uint64_t root=LE64(b+48);release(r,b);
    uint32_t n=0;
    rc=tree_load(r,root,APFS_OBJ_OMAP,1,&b,&n);
    if(rc!=IFS_FS_OK)return rc;
    int found=0;
    uint64_t best=0,physical=0;
    for(uint32_t i=0;i<n;++i){
        Record item;
        rc=record_at(b,r->block_size,n,i,1,&item);
        if(rc!=IFS_FS_OK)break;
        uint64_t key_oid=LE64(item.key),key_xid=LE64(item.key+8);
        if(key_oid!=oid||key_xid>xid)continue;
        uint32_t flags=LE32(item.value),size=LE32(item.value+4);
        uint64_t paddr=LE64(item.value+8);
        if((flags&5u)!=0u||size!=r->block_size||paddr>=r->blocks){
            rc=IFS_FS_UNSUPPORTED;break;
        }
        if(!found||key_xid>=best){
            found=1;best=key_xid;physical=paddr;
        }
    }
    release(r,b);
    if(rc!=IFS_FS_OK)return rc;
    if(!found)return IFS_FS_CORRUPT;
    *addr=physical;return IFS_FS_OK;
}
static int active_nx(IfsApfsReader *r,uint8_t **result)
{
    uint8_t *chosen=allocate(r,r->block_size);
    uint8_t *item=allocate(r,r->block_size);
    if(!chosen||!item){
        release(r,chosen);release(r,item);return IFS_FS_NO_MEMORY;
    }
    int rc=read_block(r,0u,chosen);
    if(rc!=IFS_FS_OK)goto failure;
    if(!checksum_valid(chosen,r->block_size) ||
       LE32(chosen+24)!=APFS_OBJ_NX ||
       LE64(chosen+8)!=1u || !LE64(chosen+16) ||
       LE32(chosen+36)!=r->block_size ||
       LE64(chosen+40)!=r->blocks){
        rc=IFS_FS_CORRUPT;goto failure;
    }
    uint32_t n=LE32(chosen+104);
    uint64_t base=LE64(chosen+112);
    if(!n || n>4096u || base>=r->blocks ||
       (base&(UINT64_C(1)<<63)) || n>r->blocks-base){
        rc=IFS_FS_UNSUPPORTED;goto failure;
    }
    uint64_t chosen_xid=LE64(chosen+16);
    for(uint32_t i=0;i<n;++i){
        rc=read_block(r,base+i,item);
        if(rc!=IFS_FS_OK)goto failure;
        if(!checksum_valid(item,r->block_size) ||
           LE32(item+24)!=APFS_OBJ_NX ||
           LE64(item+8)!=1u || !LE64(item+16) ||
           LE32(item+36)!=r->block_size ||
           LE64(item+40)!=r->blocks ||
           memcmp(chosen+72,item+72,16u))
            continue;
        if(LE64(item+16)>chosen_xid){
            chosen_xid=LE64(item+16);
            memcpy(chosen,item,r->block_size);
        }
    }
    release(r,item);
    *result=chosen;return IFS_FS_OK;
failure:
    release(r,chosen);release(r,item);
    return rc;
}
static int decode_drec(const Record *record,uint64_t parent,
                       const uint8_t **name,uint32_t *name_bytes,
                       uint64_t *child)
{
    if(record->key_len<10u || record->value_len<18u)
        return IFS_FS_CORRUPT;
    uint64_t hdr=LE64(record->key);
    if((hdr&APFS_KEY_ID_MASK)!=parent ||
       hdr>>60u!=APFS_KEY_DREC)return IFS_FS_NOT_FOUND;
    uint32_t length=LE16(record->key+8);
    if(!length || length>256u || length!=(uint32_t)record->key_len-10u ||
       record->key[10u+length-1u]!=0u ||
       memchr(record->key+10u,0,length-1u) ||
       memchr(record->key+10u,'/',length-1u))
        return IFS_FS_CORRUPT;
    uint64_t target=LE64(record->value);
    if(!target)return IFS_FS_CORRUPT;
    *name=record->key+10u;*name_bytes=length-1u;
    *child=target;return IFS_FS_OK;
}
static int inode_for(IfsApfsReader *r,uint64_t target,Inode *result)
{
    int found=0;
    Inode matched={0};
    /* The APFS inode key is unique in a committed catalogue snapshot.
     * A second instance is malformed even if its values happen to agree:
     * accepting the first entry would make path results order-dependent. */
    for(uint32_t i=0;i<r->records;++i){
        Record record;
        int rc=record_at(r->catalog,r->block_size,r->records,i,0,&record);
        if(rc!=IFS_FS_OK)return rc;
        if(record.key_len!=8u ||
           (LE64(record.key)&APFS_KEY_ID_MASK)!=target ||
           LE64(record.key)>>60u!=APFS_KEY_INODE)continue;
        if(record.value_len<92u)return IFS_FS_CORRUPT;
        const uint16_t mode=LE16(record.value+80u);
        Inode f={.private_id=LE64(record.value+8u),.flags=LE64(record.value+48u),
                 .mode=mode,.owner=LE32(record.value+72u),.group=LE32(record.value+76u)};
        if((mode&0170000u)==APFS_DIR_MODE)
            f.type=IFS_FS_ENTRY_DIRECTORY;
        else if((mode&0170000u)==APFS_REG_MODE)
            f.type=IFS_FS_ENTRY_FILE;
        else if((mode&0170000u)==APFS_LINK_MODE)
            f.type=IFS_FS_ENTRY_LINK;
        else return IFS_FS_UNSUPPORTED;
        if(f.type==IFS_FS_ENTRY_FILE){
            if(!f.private_id)return IFS_FS_CORRUPT;
            if(f.flags&(APFS_INODE_CLONED|APFS_INODE_SPARSE|
                        APFS_INODE_EVER_CLONED))return IFS_FS_UNSUPPORTED;
            if(record.value_len==92u)return IFS_FS_UNSUPPORTED;
            if(record.value_len<96u)return IFS_FS_CORRUPT;
            uint32_t count=LE16(record.value+92u);
            uint32_t used=LE16(record.value+94u);
            if(count>64u||96u+4u*count>record.value_len)
                return IFS_FS_CORRUPT;
            uint32_t data=96u+4u*count;
            if(used>record.value_len-data)return IFS_FS_CORRUPT;
            int have_stream=0;
            for(uint32_t k=0;k<count;++k){
                const uint8_t *entry=record.value+96u+k*4u;
                const uint32_t size=LE16(entry+2u);
                if(size>record.value_len-data)return IFS_FS_CORRUPT;
                if(entry[0]==8u){
                    if(size<16u||have_stream)return IFS_FS_CORRUPT;
                    f.size=LE64(record.value+data);
                    have_stream=1;
                }
                if(size>UINT32_MAX-7u)return IFS_FS_CORRUPT;
                uint32_t padded=(size+7u)&~7u;
                if(padded>record.value_len-data)return IFS_FS_CORRUPT;
                data+=padded;
            }
            if(!have_stream)return IFS_FS_UNSUPPORTED;
        }
        if(found)return IFS_FS_CORRUPT;
        matched=f;
        found=1;
    }
    if(found){
        *result=matched;
        return IFS_FS_OK;
    }
    return IFS_FS_NOT_FOUND;
}
static int locate(IfsApfsReader *r,const char *path,uint64_t *oid,
                  const char **base,uint32_t *base_bytes,Inode *inode)
{
    if(!path||path[0]!='/')return IFS_FS_INVALID;
    uint64_t current=APFS_ROOT_INODE;
    const char *p=path;
    *base="/";*base_bytes=1u;
    if(!*++p)goto done;
    for(unsigned depth=0u;depth<64u;++depth){
        const char *start=p;
        while(*p && *p!='/'){
            if((size_t)(p-start)>=255u)return IFS_FS_INVALID;
            ++p;
        }
        uint32_t n=(uint32_t)(p-start);
        if(!n|| (n==1u&&start[0]=='.') ||
           (n==2u&&start[0]=='.'&&start[1]=='.'))
            return IFS_FS_INVALID;
        Inode parent;
        int rc=inode_for(r,current,&parent);
        if(rc!=IFS_FS_OK)return rc;
        if(parent.type!=IFS_FS_ENTRY_DIRECTORY)return IFS_FS_NOT_FOUND;
        int found=0;
        uint64_t matched_child=0u;
        /* A checksummed catalogue can still contain conflicting records.
         * An exact parent/name key must identify precisely one child.
         * Inspect the entire leaf before choosing a pathname component;
         * returning the first match would silently conceal corruption. */
        for(uint32_t i=0;i<r->records;++i){
            Record record;
            rc=record_at(r->catalog,r->block_size,r->records,i,0,&record);
            if(rc!=IFS_FS_OK)return rc;
            if(record.key_len<8u||LE64(record.key)>>60u!=APFS_KEY_DREC)
                continue;
            const uint8_t *name;uint32_t length;uint64_t child;
            rc=decode_drec(&record,current,&name,&length,&child);
            if(rc==IFS_FS_NOT_FOUND)continue;
            if(rc!=IFS_FS_OK)return rc;
            if(length==n&&!memcmp(name,start,n)){
                if(found)return IFS_FS_CORRUPT;
                matched_child=child;
                found=1;
            }
        }
        if(!found)return IFS_FS_NOT_FOUND;
        current=matched_child;
        *base=start;*base_bytes=n;
        if(!*p)goto done;
        ++p;
    }
    return IFS_FS_INVALID;
done:
    *oid=current;
    return inode_for(r,current,inode);
}
static int entry_fill(const Inode *node,const char *name,uint32_t n,
                      IfsFsEntryV2 *entry)
{
    if(!entry || entry->structure_bytes<sizeof(*entry) ||
       !entry->name || !entry->name_capacity)return IFS_FS_INVALID;
    if(n>=entry->name_capacity)return IFS_FS_NO_SPACE;
    memcpy(entry->name,name,n);
    entry->name[n]=0;
    entry->name_bytes=n;
    entry->type=node->type;
    entry->size_bytes=node->size;
    entry->attributes=0u;
    return IFS_FS_OK;
}
/* The catalogue inode's POSIX fields are authoritative. Preserve the
 * native type and permission bits; do not substitute host ownership. */
int ifs_apfs_reader_metadata(IfsApfsReader *r,const char *path,IfsFsMetadataV2 *out)
{
    if(!r || !out || out->structure_bytes<sizeof(*out))return IFS_FS_INVALID;
    uint64_t oid;const char *base;uint32_t n;Inode inode;
    int rc=locate(r,path,&oid,&base,&n,&inode);
    if(rc!=IFS_FS_OK)return rc;
    *out=(IfsFsMetadataV2){.structure_bytes=sizeof(*out),.mask=IFS_FS_META_GET_MODE,
        .mode=inode.mode,.owner=inode.owner,.group=inode.group};
    return IFS_FS_OK;
}
/* APFS volume names are NUL-terminated UTF-8. Validate shortest form,
 * scalar range and surrogate exclusion before copying any bytes. */
int ifs_apfs_reader_label(const IfsApfsReader *r,char *out,uint32_t cap,uint32_t *bytes)
{
    if(!bytes)return IFS_FS_INVALID;
    *bytes=0u;
    if(!r || !out || !cap)return IFS_FS_INVALID;
    uint32_t n=0;
    while(n<sizeof(r->label) && r->label[n])++n;
    if(n==sizeof(r->label))return IFS_FS_CORRUPT;
    for(uint32_t i=0;i<n;) {
        uint32_t cp=r->label[i++],need=0,min=0;
        if(cp<0x80u)continue;
        if(cp>=0xc2u && cp<=0xdfu){cp&=31u;need=1;min=0x80u;}
        else if(cp>=0xe0u && cp<=0xefu){cp&=15u;need=2;min=0x800u;}
        else if(cp>=0xf0u && cp<=0xf4u){cp&=7u;need=3;min=0x10000u;}
        else return IFS_FS_CORRUPT;
        if(need>n-i)return IFS_FS_CORRUPT;
        while(need--) {
            uint32_t next=r->label[i++];
            if((next&0xc0u)!=0x80u)return IFS_FS_CORRUPT;
            cp=(cp<<6u)|(next&63u);
        }
        if(cp<min || cp>0x10ffffu || (cp>=0xd800u && cp<=0xdfffu))
            return IFS_FS_CORRUPT;
    }
    if(n>=cap)return IFS_FS_NO_SPACE;
    memcpy(out,r->label,n+1u);*bytes=n;
    return IFS_FS_OK;
}
int ifs_apfs_reader_stat(IfsApfsReader *r,const char *path,IfsFsEntryV2 *entry)
{
    if(!r)return IFS_FS_INVALID;
    uint64_t oid;const char *name;uint32_t n;Inode inode;
    int rc=locate(r,path,&oid,&name,&n,&inode);
    return rc==IFS_FS_OK?entry_fill(&inode,name,n,entry):rc;
}
int ifs_apfs_reader_list(IfsApfsReader *r,const char *path,uint32_t index,
                          IfsFsEntryV2 *entry)
{
    if(!r)return IFS_FS_INVALID;
    uint64_t oid;const char *base;uint32_t base_bytes;Inode directory;
    int rc=locate(r,path,&oid,&base,&base_bytes,&directory);
    if(rc!=IFS_FS_OK)return rc;
    if(directory.type!=IFS_FS_ENTRY_DIRECTORY)return IFS_FS_INVALID;
    for(uint32_t i=0;i<r->records;++i){
        Record record;
        rc=record_at(r->catalog,r->block_size,r->records,i,0,&record);
        if(rc!=IFS_FS_OK)return rc;
        if(record.key_len<8u||LE64(record.key)>>60u!=APFS_KEY_DREC)
            continue;
        const uint8_t *name;uint32_t bytes;uint64_t child;
        rc=decode_drec(&record,oid,&name,&bytes,&child);
        if(rc==IFS_FS_NOT_FOUND)continue;
        if(rc!=IFS_FS_OK)return rc;
        if(index--!=0u)continue;
        Inode node;
        rc=inode_for(r,child,&node);
        return rc==IFS_FS_OK?
            entry_fill(&node,(const char *)name,bytes,entry):rc;
    }
    return IFS_FS_END;
}
int ifs_apfs_reader_read(IfsApfsReader *r,const char *path,uint64_t offset,
                          void *data,uint32_t bytes)
{
    if(!r||(!data&&bytes))return IFS_FS_INVALID;
    uint64_t oid;const char *base;uint32_t base_bytes;Inode file;
    int rc=locate(r,path,&oid,&base,&base_bytes,&file);
    if(rc!=IFS_FS_OK)return rc;
    if(file.type!=IFS_FS_ENTRY_FILE)return IFS_FS_UNSUPPORTED;
    if(offset>file.size || bytes>file.size-offset)return IFS_FS_INVALID;
    if(!bytes)return IFS_FS_OK;
    uint8_t *out=data;
    uint64_t cursor=offset;
    uint32_t remaining=bytes;
    while(remaining){
        int found=0;
        for(uint32_t i=0;i<r->records;++i){
            Record rec;
            rc=record_at(r->catalog,r->block_size,r->records,i,0,&rec);
            if(rc!=IFS_FS_OK)return rc;
            if(rec.key_len!=16u||rec.value_len!=24u||
               (LE64(rec.key)&APFS_KEY_ID_MASK)!=file.private_id||
               LE64(rec.key)>>60u!=APFS_KEY_EXTENT)continue;
            uint64_t at=LE64(rec.key+8u);
            uint64_t len_flags=LE64(rec.value);
            uint64_t length=len_flags&UINT64_C(0x00ffffffffffffff);
            uint64_t physical=LE64(rec.value+8u);
            uint64_t crypto_id=LE64(rec.value+16u);
            if(!length || at%r->block_size || length%r->block_size ||
               physical>=r->blocks || length/r->block_size>r->blocks-physical)
                return IFS_FS_CORRUPT;
            if(len_flags>>56u || crypto_id)return IFS_FS_UNSUPPORTED;
            if(cursor<at || cursor-at>=length)continue;
            const uint64_t delta=cursor-at;
            uint32_t n=remaining;
            if((uint64_t)n>length-delta)n=(uint32_t)(length-delta);
            rc=ifs_fs_host_read_bytes_v2(&r->host,
                 physical*(uint64_t)r->block_size+delta,out,n);
            if(rc!=IFS_FS_OK)return rc;
            remaining-=n;out+=n;cursor+=n;found=1;break;
        }
        if(!found)return IFS_FS_UNSUPPORTED; /* Sparse/unmapped stream. */
    }
    return IFS_FS_OK;
}
int ifs_apfs_reader_open(const IfsFsHostRuntimeV2 *host,IfsApfsReader **output,
                          uint32_t *block_bytes,uint64_t *total_bytes)
{
    if(!output || !block_bytes || !total_bytes || !ifs_fs_host_v2_valid(host))
        return IFS_FS_INVALID;
    *output=NULL;*block_bytes=0u;*total_bytes=0u;
    if(!(host->capabilities&IFS_FS_HOST_READ_ONLY))return IFS_FS_ACCESS;
    uint32_t variant=0u;
    int rc=apfs_v2_probe(host,&variant);
    if(rc!=IFS_FS_OK)return rc;
    uint8_t prefix[4096];
    rc=ifs_fs_host_read_bytes_v2(host,0u,prefix,sizeof(prefix));
    if(rc!=IFS_FS_OK)return rc;
    IfsApfsReader *r=host->allocate(host->context,sizeof(*r));
    if(!r)return IFS_FS_NO_MEMORY;
    memset(r,0,sizeof(*r));
    const size_t supplied=host->structure_bytes<sizeof(r->host)?
                           host->structure_bytes:sizeof(r->host);
    memcpy(&r->host,host,supplied);
    r->host.structure_bytes=(uint32_t)supplied;
    r->block_size=LE32(prefix+36);
    r->blocks=LE64(prefix+40);
    uint8_t *nx=NULL,*volume=NULL;
    rc=active_nx(r,&nx);
    if(rc!=IFS_FS_OK)goto done;
    r->xid=LE64(nx+16u);
    uint32_t slots=LE32(nx+180u);
    if(!slots || slots>100u ||
       184u+(uint64_t)slots*8u>r->block_size){
        rc=IFS_FS_UNSUPPORTED;goto done;
    }
    uint64_t volume_oid=0u;
    for(uint32_t i=0;i<slots;++i){
        uint64_t oid=LE64(nx+184u+i*8u);
        if(!oid)continue;
        if(volume_oid){rc=IFS_FS_UNSUPPORTED;goto done;}
        volume_oid=oid;
    }
    if(!volume_oid){rc=IFS_FS_CORRUPT;goto done;}
    uint64_t volume_at=0u,root_at=0u;
    rc=object_map(r,LE64(nx+160u),volume_oid,r->xid,&volume_at);
    if(rc!=IFS_FS_OK)goto done;
    volume=allocate(r,r->block_size);
    if(!volume){rc=IFS_FS_NO_MEMORY;goto done;}
    rc=read_block(r,volume_at,volume);
    if(rc!=IFS_FS_OK)goto done;
    if(!checksum_valid(volume,r->block_size) ||
       memcmp(volume+32u,"APSB",4u) ||
       (LE32(volume+24u)&APFS_OBJ_MASK)!=APFS_OBJ_FS ||
       LE64(volume+8u)!=volume_oid || LE64(volume+16u)>r->xid){
        rc=IFS_FS_CORRUPT;goto done;
    }
    if(LE64(volume+40u) || LE64(volume+48u) ||
       LE64(volume+56u) || LE64(volume+216u) ||
       !(LE64(volume+264u)&APFS_FEATURE_UNENCRYPTED)){
        rc=IFS_FS_UNSUPPORTED;goto done;
    }
    /* The volume name belongs to the selected, checksum-validated APSB,
     * not the container superblock or an obsolete checkpoint. */
    memcpy(r->label,volume+704u,sizeof(r->label));
    rc=object_map(r,LE64(volume+128u),LE64(volume+136u),r->xid,&root_at);
    if(rc!=IFS_FS_OK)goto done;
    rc=tree_load(r,root_at,APFS_OBJ_FSTREE,0,&r->catalog,&r->records);
    if(rc!=IFS_FS_OK)goto done;
    Inode root;
    rc=inode_for(r,APFS_ROOT_INODE,&root);
    if(rc==IFS_FS_OK && root.type!=IFS_FS_ENTRY_DIRECTORY)
        rc=IFS_FS_CORRUPT;
done:
    release(r,nx);release(r,volume);
    if(rc!=IFS_FS_OK){
        ifs_apfs_reader_close(r);return rc;
    }
    *block_bytes=r->block_size;
    *total_bytes=r->blocks*(uint64_t)r->block_size;
    *output=r;return IFS_FS_OK;
}
void ifs_apfs_reader_close(IfsApfsReader *r)
{
    if(!r)return;
    release(r,r->catalog);
    release(r,r);
}
