/* SPDX-License-Identifier: GPL-3.0-or-later
 * Directly exercise the exact freestanding APFS Common V2 implementation
 * with a checksummed, independently assembled flat-catalogue image.
 * The test performs no writes to any real filesystem or BOOT installation.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "apfs_disk.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define BLOCK 4096u
#define BLOCK_COUNT 64u
#define XID UINT64_C(42)
#define HEADER(id,type) ((uint64_t)(id)|((uint64_t)(type)<<60u))
#define CHECK(x) do {if(!(x)){fprintf(stderr,"APFS FIRMWARE V2 line %u: %s\n",\
(unsigned)__LINE__,#x);return 1;}}while(0)
typedef struct {uint8_t key[64],value[160];uint16_t key_n,value_n;} Item;
typedef struct {unsigned reads;int fail;unsigned writes;} Context;
static uint8_t disk[BLOCK_COUNT*BLOCK];
static void put16(uint8_t *p,uint16_t v)
{p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4u;++i)p[i]=(uint8_t)(v>>(8u*i));}
static void put64(uint8_t *p,uint64_t v)
{for(unsigned i=0;i<8u;++i)p[i]=(uint8_t)(v>>(8u*i));}
static void object(uint8_t *p,uint64_t oid,uint32_t type,uint32_t subtype)
{
    memset(p,0,BLOCK);
    put64(p+8u,oid);put64(p+16u,XID);
    put32(p+24u,type);put32(p+28u,subtype);
}
static void seal(uint8_t *p){put64(p,fletcher64(p,BLOCK));}
static int node(uint8_t *p,uint64_t oid,uint32_t type,uint32_t subtype,
                const Item *items,uint32_t count,int fixed)
{
    object(p,oid,type,subtype);
    put16(p+32u,(uint16_t)(fixed?7u:3u));
    put32(p+36u,count);
    uint32_t table_bytes=count*(fixed?4u:8u);
    put16(p+42u,(uint16_t)table_bytes);
    uint32_t key_start=56u+table_bytes,key=key_start,value=BLOCK-40u;
    for(uint32_t i=0;i<count;++i){
        const Item *x=items+i;
        if(x->key_n==0u||
           x->key_n>value || x->value_n>value ||
           key+x->key_n>value-x->value_n)return -1;
        value-=x->value_n;
        memcpy(p+key,x->key,x->key_n);
        memcpy(p+value,x->value,x->value_n);
        uint32_t table=56u+i*(fixed?4u:8u);
        put16(p+table,(uint16_t)(key-key_start));
        if(fixed)put16(p+table+2u,(uint16_t)(BLOCK-40u-value));
        else{
            put16(p+table+2u,x->key_n);
            put16(p+table+4u,(uint16_t)(BLOCK-40u-value));
            put16(p+table+6u,x->value_n);
        }
        key+=x->key_n;
    }
    seal(p);return 0;
}
static int make_omap(unsigned object_block,unsigned tree_block,
                     uint64_t target_oid,unsigned target_block)
{
    uint8_t *obj=disk+object_block*BLOCK;
    object(obj,object_block,11u,0u);
    put64(obj+48u,tree_block);seal(obj);
    Item record={0};
    record.key_n=16u;record.value_n=16u;
    put64(record.key,target_oid);put64(record.key+8u,XID);
    put32(record.value+4u,BLOCK);put64(record.value+8u,target_block);
    return node(disk+tree_block*BLOCK,tree_block,0x40000002u,11u,
                &record,1u,1);
}
static void inode(Item *r,uint64_t id,uint64_t stream,uint16_t mode)
{
    memset(r,0,sizeof(*r));
    r->key_n=8u;r->value_n=92u;
    put64(r->key,HEADER(id,3u));put64(r->value+8u,stream);
    put16(r->value+80u,mode);
}
static void drec(Item *r,uint64_t parent,const char *name,uint64_t child,
                 uint16_t flags)
{
    memset(r,0,sizeof(*r));
    uint16_t n=(uint16_t)(strlen(name)+1u);
    r->key_n=(uint16_t)(10u+n);r->value_n=18u;
    put64(r->key,HEADER(parent,9u));
    put16(r->key+8u,n);
    memcpy(r->key+10u,name,n);
    put64(r->value,child);put16(r->value+16u,flags);
}
static const char payload[]="APFS Common V2 kernel image: verified";
static int make_image(int duplicate_name,int duplicate_inode)
{
    uint8_t *nx=disk;
    object(nx,1u,0x80000001u,0u);
    memcpy(nx+32u,"NXSB",4u);
    put32(nx+36u,BLOCK);put64(nx+40u,BLOCK_COUNT);
    for(unsigned i=0;i<16u;++i)nx[72u+i]=(uint8_t)(i+1u);
    put32(nx+104u,2u);put64(nx+112u,1u);
    put32(nx+136u,0u);put32(nx+140u,2u);
    put64(nx+160u,8u);put32(nx+180u,1u);put64(nx+184u,100u);
    seal(nx);
    memcpy(disk+2u*BLOCK,nx,BLOCK);
    if(make_omap(8u,9u,100u,10u) ||
       make_omap(11u,12u,300u,13u))return -1;
    uint8_t *v=disk+10u*BLOCK;
    object(v,100u,13u,0u);
    memcpy(v+32u,"APSB",4u);
    put64(v+128u,11u);put64(v+136u,300u);
    put64(v+264u,1u);seal(v);
    Item entries[8]={0};
    inode(&entries[0],2u,2u,0040755u);
    drec(&entries[1],2u,"boot",3u,4u);
    inode(&entries[2],3u,3u,0040755u);
    drec(&entries[3],3u,"kernel.bin",4u,8u);
    inode(&entries[4],4u,400u,0100644u);
    entries[4].value_n=140u;
    put16(entries[4].value+92u,1u);
    put16(entries[4].value+94u,40u);
    entries[4].value[96u]=8u;
    put16(entries[4].value+98u,40u);
    put64(entries[4].value+100u,sizeof(payload)-1u);
    entries[5].key_n=16u;entries[5].value_n=24u;
    put64(entries[5].key,HEADER(400u,8u));
    put64(entries[5].value,BLOCK);
    put64(entries[5].value+8u,20u);
    memcpy(disk+20u*BLOCK,payload,sizeof(payload)-1u);
    /* A duplicate fully checksummed directory key must be rejected during
     * the real Common V2 pathname traversal, not silently choose a child. */
    unsigned count=6u;
    if(duplicate_name)
        drec(&entries[count++],3u,"kernel.bin",4u,8u);
    if(duplicate_inode)
        entries[count++]=entries[4];
    return node(disk+13u*BLOCK,300u,2u,14u,entries,count,0);
}
static int read_at(void *context,uint64_t pos,void *dst,uint32_t n)
{
    Context *c=context;
    if(c->fail || !dst || !n || pos%512u || n%512u ||
       n>8192u || pos>sizeof(disk) || n>sizeof(disk)-pos)
        return -1;
    memcpy(dst,disk+(size_t)pos,n);++c->reads;return 0;
}
static int forbidden_write(void *context,uint64_t at,const void *src,uint32_t n)
{
    (void)at;(void)src;(void)n;
    ++((Context *)context)->writes;
    return -1;
}
static int forbidden_flush(void *context){(void)context;return -1;}
static void *alloc(void *context,size_t n){(void)context;return malloc(n);}
static void release(void *context,void *p){(void)context;free(p);}
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
int main(int argc,char **argv)
{
    CHECK(argc==1 || argc==2);
    CHECK(make_image(0,0)==0);
    /* The production firmware ELF can be supplied as argv[1]. Resolve its
     * exact Common V2 entry point rather than testing only linked source. */
    void *image=NULL;
    const IfsFsModuleApiV2 *api=NULL;
    if(argc==2) {
        image=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
        CHECK(image!=NULL);
        void *symbol=dlsym(image,IFS_FS_COMMON_EXPORT_V2);
        IfsFsModuleApiEntryV2 entry=NULL;
        CHECK(symbol && sizeof(entry)==sizeof(symbol));
        memcpy(&entry,&symbol,sizeof(entry));
        api=entry?entry():NULL;
    } else {
        api=infiltrator_filesystem_api_v2();
    }
    CHECK(ifs_fs_api_v2_valid(api,"apfs"));
    CHECK(api->runtime_profiles==(IFS_FS_PROFILE_FREESTANDING|
                                 IFS_FS_PROFILE_FIRMWARE));
    const IfsFsCoreOpsV2 *core=(const void *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    CHECK(core && core->header.capabilities==
          (IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
           IFS_FS_CORE_LIST|IFS_FS_CORE_READ) && !core->write &&
          !core->create && !core->remove && !core->rename);
    Context context={0};
    IfsFsHostRuntimeV2 host={0};
    host.abi_version=IFS_FS_HOST_ABI_V2;
    host.structure_bytes=sizeof(host);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.logical_sector_bytes=host.physical_sector_bytes=512u;
    host.volume_bytes=sizeof(disk);
    host.maximum_transfer_bytes=8192u;
    host.context=&context;
    host.read_at=read_at;host.allocate=alloc;host.release=release;
    CHECK(ifs_fs_host_v2_valid(&host));
    uint32_t variant=0u;
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==1u);
    IfsFsHostRuntimeV2 writable=host;
    writable.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;
    writable.write_at=forbidden_write;writable.flush=forbidden_flush;
    CHECK(ifs_fs_host_v2_valid(&writable));
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    void *mount=(void *)(uintptr_t)7u;
    unsigned before=context.reads;
    CHECK(core->mount(&writable,&mount,&info)==IFS_FS_ACCESS &&
          mount==NULL && context.reads==before && !context.writes);
    CHECK(core->mount(&host,&mount,&info)==IFS_FS_OK && mount &&
          info.block_bytes==BLOCK && info.total_bytes==sizeof(disk));
    char name[256]={0};
    IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,
                        .name_capacity=sizeof(name)};
    CHECK(core->stat(mount,"/",&entry)==IFS_FS_OK &&
          entry.type==IFS_FS_ENTRY_DIRECTORY);
    CHECK(core->stat(mount,"/boot",&entry)==IFS_FS_OK &&
          entry.type==IFS_FS_ENTRY_DIRECTORY);
    CHECK(core->stat(mount,"/boot/kernel.bin",&entry)==IFS_FS_OK &&
          entry.type==IFS_FS_ENTRY_FILE &&
          entry.size_bytes==sizeof(payload)-1u);
    CHECK(core->stat(mount,"/boot/missing.bin",&entry)==IFS_FS_NOT_FOUND);
    CHECK(core->directory_entry(mount,"/",0u,&entry)==IFS_FS_OK &&
          !strcmp(name,"boot"));
    CHECK(core->directory_entry(mount,"/",1u,&entry)==IFS_FS_END);
    CHECK(core->directory_entry(mount,"/boot",0u,&entry)==IFS_FS_OK &&
          !strcmp(name,"kernel.bin"));
    CHECK(core->directory_entry(mount,"/boot",1u,&entry)==IFS_FS_END);
    char small[4]={'X','X','X','X'};
    IfsFsEntryV2 short_entry={.structure_bytes=sizeof(short_entry),
                               .name=small,.name_capacity=sizeof(small)};
    CHECK(core->stat(mount,"/boot/kernel.bin",&short_entry)==IFS_FS_NO_SPACE &&
          !memcmp(small,"XXXX",4));
    char readback[sizeof(payload)]={0};
    CHECK(core->read(mount,"/boot/kernel.bin",0u,readback,
                     sizeof(payload)-1u)==IFS_FS_OK &&
          !memcmp(readback,payload,sizeof(payload)-1u));
    memset(readback,0,sizeof(readback));
    CHECK(core->read(mount,"/boot/kernel.bin",5u,readback,9u)==IFS_FS_OK &&
          !memcmp(readback,payload+5u,9u));
    CHECK(core->read(mount,"/boot/kernel.bin",sizeof(payload)-2u,
                     readback,2u)==IFS_FS_INVALID);
    CHECK(core->read(mount,"/boot/absent",0u,readback,1u)==IFS_FS_NOT_FOUND);
    context.fail=1;
    CHECK(core->read(mount,"/boot/kernel.bin",1u,readback,1u)==IFS_FS_IO_ERROR);
    context.fail=0;
    CHECK(core->unmount(mount)==IFS_FS_OK);
    CHECK(core->unmount(mount)==IFS_FS_INVALID);
    CHECK(core->read(mount,"/boot/kernel.bin",0u,readback,1u)==IFS_FS_INVALID);
    disk[13u*BLOCK+2048u]^=1u;
    info.structure_bytes=sizeof(info);mount=NULL;
    CHECK(core->mount(&host,&mount,&info)==IFS_FS_CORRUPT && !mount);
    CHECK(make_image(1,0)==0);
    info.structure_bytes=sizeof(info);mount=NULL;
    CHECK(core->mount(&host,&mount,&info)==IFS_FS_OK && mount);
    CHECK(core->stat(mount,"/boot/kernel.bin",&entry)==IFS_FS_CORRUPT);
    CHECK(core->read(mount,"/boot/kernel.bin",0u,readback,1u)==IFS_FS_CORRUPT);
    CHECK(core->unmount(mount)==IFS_FS_OK);
    /* A second checksummed inode key must fail independently of duplicate
     * directory entries, through the production Common V2 read path. */
    CHECK(make_image(0,1)==0);
    info.structure_bytes=sizeof(info);mount=NULL;
    CHECK(core->mount(&host,&mount,&info)==IFS_FS_OK && mount);
    CHECK(core->stat(mount,"/boot/kernel.bin",&entry)==IFS_FS_CORRUPT);
    CHECK(core->read(mount,"/boot/kernel.bin",0u,readback,1u)==IFS_FS_CORRUPT);
    CHECK(core->unmount(mount)==IFS_FS_OK);
    CHECK(!context.writes);
    CHECK(!image || dlclose(image)==0);
    puts("APFS-FREESTANDING-COMMON-V2-DIRECTORY-AND-KERNEL-READ-PASS");
    return 0;
}
