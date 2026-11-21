/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise the installed Common V2 API on a disposable, fully constructed
 * APFS checkpoint/spaceman/object-map/catalogue image. This is a native
 * media regression, not a claim that APFS has a BOOT reader.
 */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
extern int ldtm_format_apfs_volume(const char *path);

enum { BLOCK = 4096u, VOLUME_BLOCK = 102u, TOTAL_MIB = 256u };
typedef struct {
    int fd;
    uint64_t size, fail_from;
    unsigned reads;
} TestMedia;

static int host_read(void *context, uint64_t offset, void *buffer, uint32_t bytes)
{
    TestMedia *media=(TestMedia *)context;
    if (!media || !buffer || !bytes || offset%512u || bytes%512u ||
        bytes>65536u || offset>media->size ||
        bytes>media->size-offset || offset>INT64_MAX ||
        offset>=media->fail_from)
        return -1;
    media->reads++;
    return pread(media->fd,buffer,bytes,(off_t)offset)==(ssize_t)bytes?0:-1;
}
static void *allocate(void *context,size_t bytes)
{ (void)context;return malloc(bytes); }
static void release(void *context,void *address)
{ (void)context;free(address); }
static IfsFsHostRuntimeV2 test_host(TestMedia *m)
{
    IfsFsHostRuntimeV2 host={0};
    host.abi_version=IFS_FS_HOST_ABI_V2;
    host.structure_bytes=sizeof(host);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.context=m;
    host.volume_bytes=m->size;
    host.logical_sector_bytes=512u;
    host.physical_sector_bytes=512u;
    host.maximum_transfer_bytes=65536u;
    host.read_at=host_read;
    host.allocate=allocate;
    host.release=release;
    return host;
}
static void put32(uint8_t *p,uint32_t v)
{
    for (unsigned i=0;i<4u;i++) p[i]=(uint8_t)(v>>(i*8u));
}
static void put64(uint8_t *p,uint64_t v)
{
    for (unsigned i=0;i<8u;i++) p[i]=(uint8_t)(v>>(i*8u));
}
static uint64_t le32(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1]<<8) |
           ((uint64_t)p[2]<<16) | ((uint64_t)p[3]<<24);
}
static uint64_t fletcher(const uint8_t *p)
{
    const uint64_t mod=UINT64_C(0xffffffff);
    uint64_t a=0u,b=0u;
    for (unsigned i=8u;i<BLOCK;i+=4u) {
        a=(a+le32(p+i))%mod;
        b=(b+a)%mod;
    }
    uint64_t x=mod-((a+b)%mod);
    return ((mod-((a+x)%mod))<<32)|x;
}
static int write_volume(TestMedia *m,const uint8_t *block)
{
    return pwrite(m->fd,block,BLOCK,
                  (off_t)((uint64_t)VOLUME_BLOCK*BLOCK))==BLOCK?0:-1;
}
static uint64_t disk_hash(TestMedia *m)
{
    uint8_t *buf=malloc(65536u);
    if(!buf)return 0;
    uint64_t value=UINT64_C(14695981039346656037);
    for(uint64_t at=0;at<m->size;at+=65536u) {
        uint32_t n=(uint32_t)(m->size-at<65536u?m->size-at:65536u);
        if(pread(m->fd,buf,n,(off_t)at)!=(ssize_t)n) {
            free(buf);return 0;
        }
        for(uint32_t i=0;i<n;i++) {
            value^=buf[i];
            value*=UINT64_C(1099511628211);
        }
    }
    free(buf);
    return value;
}
#define CHECK(condition) do { if(!(condition)) { \
    fprintf(stderr,"APFS REAL V2 line %d: %s\\n",__LINE__,#condition);\
    failed=1;goto finish; } } while(0)

int main(void)
{
    int failed=0;
    char path[]="/tmp/infiltrator-apfs-v2-media-XXXXXX";
    TestMedia media={.fd=-1,.size=(uint64_t)TOTAL_MIB*1048576u,
                     .fail_from=UINT64_MAX};
    media.fd=mkstemp(path);
    if(media.fd<0)return 1;
    if(ftruncate(media.fd,(off_t)media.size)!=0 ||
       ldtm_format_apfs_volume(path)!=0) {
        fprintf(stderr,"could not construct independent APFS fixture\\n");
        failed=1;goto finish;
    }
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"apfs"));
    const IfsFsCoreOpsV2 *core=(const IfsFsCoreOpsV2 *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume=(const IfsFsVolumeOpsV2 *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(core && volume && core->probe && core->mount && core->unmount &&
          !core->stat && !core->directory_entry && !core->read &&
          !core->write && volume->query);
    IfsFsHostRuntimeV2 host=test_host(&media);
    CHECK(ifs_fs_host_v2_valid(&host));
    const uint64_t original_hash=disk_hash(&media);
    CHECK(original_hash!=0u);
    uint32_t variant=0u;
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==1u);
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    void *mounted=NULL;
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_OK && mounted);
    CHECK(info.block_bytes==BLOCK && info.total_bytes==media.size &&
          info.variant==1u && info.free_bytes<=media.size);
    IfsFsVolumeInfoV2 queried={.structure_bytes=sizeof(queried)};
    CHECK(volume->query(mounted,&queried)==IFS_FS_OK &&
          queried.total_bytes==info.total_bytes);
    CHECK(core->unmount(mounted)==IFS_FS_OK);
    CHECK(core->unmount(mounted)==IFS_FS_INVALID);
    CHECK(volume->query(mounted,&queried)==IFS_FS_INVALID);
    CHECK(disk_hash(&media)==original_hash); /* No V2 write capability. */

    /* All mutations below are to this disposable fixture only. They retain
     * a correct Fletcher checksum, so a checksum-only validator would fail. */
    uint8_t pristine[BLOCK],corrupt[BLOCK];
    CHECK(pread(media.fd,pristine,BLOCK,
                (off_t)((uint64_t)VOLUME_BLOCK*BLOCK))==BLOCK);
    for(unsigned which=0u;which<6u;which++) {
        memcpy(corrupt,pristine,sizeof(corrupt));
        if(which==0u)put64(corrupt+56u,UINT64_C(1)<<61); /* unknown incompat */
        if(which==1u)put64(corrupt+216u,1u); /* snapshots unsupported */
        if(which==2u)put64(corrupt+40u,1u); /* unsupported volume features */
        if(which==3u)put64(corrupt+264u,0u); /* encrypted/not marked plain */
        if(which==4u)put32(corrupt+24u,UINT32_C(0x40000001)); /* forged APSB type */
        if(which==5u)put64(corrupt+128u,UINT64_MAX); /* invalid object-map */
        put64(corrupt,fletcher(corrupt));
        CHECK(write_volume(&media,corrupt)==0);
        mounted=NULL;info.structure_bytes=sizeof(info);
        CHECK(core->probe(&host,&variant)==IFS_FS_OK);
        const int result=core->mount(&host,&mounted,&info);
        CHECK(result==(which<4u?IFS_FS_UNSUPPORTED:IFS_FS_CORRUPT) && !mounted);
    }
    CHECK(write_volume(&media,pristine)==0);
    CHECK(disk_hash(&media)==original_hash);
    media.fail_from=BLOCK;
    CHECK(core->probe(&host,&variant)==IFS_FS_OK);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_IO_ERROR && !mounted);
    media.fail_from=UINT64_MAX;
    CHECK(disk_hash(&media)==original_hash);
    CHECK(media.reads>0u);
    puts("APFS-REAL-MEDIA-COMMON-V2-CHECKPOINT-FEATURE-POLICY-NONMUTATION-PASS");
finish:
    (void)close(media.fd);
    (void)unlink(path);
    return failed;
}
