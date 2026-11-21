/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
static uint8_t disk[16 * 4096];
static unsigned reads;
static int fail_checkpoint_reads;
static uint32_t test_sector_bytes = 512u;
static int reader(void *ctx, uint64_t at, void *dst, uint32_t n) {
    (void)ctx;
    if ((fail_checkpoint_reads && at >= 4096u) ||
        n != test_sector_bytes || at % test_sector_bytes ||
        at > sizeof(disk) || n > sizeof(disk) - at)
        return -1;
    memcpy(dst, disk + (size_t)at, n); ++reads; return 0;
}
static void *alloc(void *ctx, size_t n) { (void)ctx; return malloc(n); }
static void release(void *ctx, void *p) { (void)ctx; free(p); }
static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8*i));
}
static uint64_t sum(const uint8_t *p, size_t n) {
    const uint64_t mod = UINT64_C(0xffffffff);
    uint64_t a = 0, b = 0;
    for (size_t i = 8; i < n; i += 4) {
        uint32_t word = (uint32_t)p[i] | ((uint32_t)p[i+1]<<8) |
            ((uint32_t)p[i+2]<<16) | ((uint32_t)p[i+3]<<24);
        a = (a + word) % mod; b = (b + a) % mod;
    }
    uint64_t x = mod - ((a+b) % mod);
    return ((mod - ((a+x)%mod)) << 32) | x;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "APFS V2: %s\n",#x); return 1; } } while(0)
static int forbidden_write(void *ctx,uint64_t at,const void *p,uint32_t n)
{ (void)ctx;(void)at;(void)p;(void)n;return -1; }
static int forbidden_flush(void *ctx)
{ (void)ctx;return -1; }
int main(void) {
    const IfsFsModuleApiV2 *api = infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api, "apfs"));
    const IfsFsCoreOpsV2 *core = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    CHECK(core && core->probe && core->mount && core->unmount &&
          !core->stat && !core->read && !core->write);
    const IfsFsVolumeOpsV2 *volume_ops = (const void *)
        ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME);
    CHECK(volume_ops && volume_ops->query &&
          volume_ops->header.capabilities == IFS_FS_VOLUME_QUERY);
    /* These are opaque tokens, not dereferenceable allocator addresses.
     * Before the registry repair this would access arbitrary memory. */
    IfsFsVolumeInfoV2 bogus_info={.structure_bytes=sizeof(bogus_info)};
    CHECK(core->unmount((void *)(uintptr_t)1u)==IFS_FS_INVALID);
    CHECK(volume_ops->query((void *)(uintptr_t)1u,&bogus_info)==IFS_FS_INVALID);
    CHECK(core->unmount((void *)(uintptr_t)UINTPTR_MAX)==IFS_FS_INVALID);
    IfsFsHostRuntimeV2 host = {0};
    host.abi_version=IFS_FS_HOST_ABI_V2; host.structure_bytes=sizeof(host);
    host.capabilities=IFS_FS_HOST_READ_ONLY; host.volume_bytes=sizeof(disk);
    host.logical_sector_bytes=512; host.physical_sector_bytes=512;
    host.maximum_transfer_bytes=512; host.context=disk;
    host.read_at=reader; host.allocate=alloc; host.release=release;
    uint32_t variant=12345;
    CHECK(core->probe(&host,&variant)!=IFS_FS_OK && variant==12345);
    memcpy(disk+32,"NXSB",4); put32(disk+36,4096); put32(disk+40,16);
    put32(disk+24, UINT32_C(0x40000001));
    put32(disk+8u,1u); /* physical NX superblock object ID */
    put32(disk+16u,1u); /* committed transaction */
    for (unsigned i=0;i<16u;++i)disk[72u+i]=(uint8_t)(i+1u);
    uint64_t cs=sum(disk,4096); put32(disk,(uint32_t)cs);
    put32(disk+4,(uint32_t)(cs>>32));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==1 && reads>=8);
    /* An 8K APFS block on 8K logical-sector media must read correctly
     * through the Common V2 byte-range bridge, not a 4K BlockIo call. */
    put32(disk+36u,8192u);put32(disk+40u,8u);
    cs=sum(disk,8192u);put32(disk,(uint32_t)cs);
    put32(disk+4u,(uint32_t)(cs>>32));
    host.logical_sector_bytes=8192u;host.physical_sector_bytes=8192u;
    host.maximum_transfer_bytes=8192u;test_sector_bytes=8192u;
    uint8_t unchanged[8192u];
    memcpy(unchanged,disk,sizeof(unchanged));
    variant=0u;
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==1u);
    CHECK(memcmp(unchanged,disk,sizeof(unchanged))==0);
    disk[6000u]^=1u;variant=0u;
    CHECK(core->probe(&host,&variant)==IFS_FS_CORRUPT && variant==0u);
    disk[6000u]^=1u;
    host.logical_sector_bytes=512u;host.physical_sector_bytes=512u;
    host.maximum_transfer_bytes=512u;test_sector_bytes=512u;
    put32(disk+36u,4096u);put32(disk+40u,16u);
    cs=sum(disk,4096u);put32(disk,(uint32_t)cs);
    put32(disk+4u,(uint32_t)(cs>>32));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==1u);
    /* A fully checksummed fake NXSB must not probe successfully when its
     * container identity is absent, independent of its block size. */
    uint8_t preserved_uuid[16];
    memcpy(preserved_uuid,disk+72u,sizeof(preserved_uuid));
    memset(disk+72u,0u,sizeof(preserved_uuid));
    cs=sum(disk,4096u);put32(disk,(uint32_t)cs);
    put32(disk+4u,(uint32_t)(cs>>32));
    variant=0u;
    CHECK(core->probe(&host,&variant)==IFS_FS_CORRUPT && variant==0u);
    memcpy(disk+72u,preserved_uuid,sizeof(preserved_uuid));
    put32(disk+16u,0u);
    cs=sum(disk,4096u);put32(disk,(uint32_t)cs);
    put32(disk+4u,(uint32_t)(cs>>32));
    CHECK(core->probe(&host,&variant)==IFS_FS_CORRUPT && variant==0u);
    put32(disk+16u,1u);
    cs=sum(disk,4096u);put32(disk,(uint32_t)cs);
    put32(disk+4u,(uint32_t)(cs>>32));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==1u);
    IfsFsVolumeInfoV2 info = {.structure_bytes=sizeof(IfsFsVolumeInfoV2)};
    void *mounted = NULL;
    host.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;
    host.write_at=forbidden_write;host.flush=forbidden_flush;
    CHECK(ifs_fs_host_v2_valid(&host));
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_ACCESS && mounted==NULL);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.write_at=NULL;host.flush=NULL;
    CHECK(ifs_fs_host_v2_valid(&host));
    /* The valid superblock reaches mount's host snapshot, then the native
     * validator must reject the incomplete filesystem. ASan checks that the
     * older V2 prefix is not read beyond its actual allocation. */
    volatile size_t short_bytes=IFS_FS_HOST_V2_BASE_BYTES;
    IfsFsHostRuntimeV2 *short_host=malloc(short_bytes);
    CHECK(short_host);
    memcpy(short_host,&host,IFS_FS_HOST_V2_BASE_BYTES);
    short_host->structure_bytes=IFS_FS_HOST_V2_BASE_BYTES;
    CHECK(core->mount(short_host,&mounted,&info)!=IFS_FS_OK && mounted==NULL);
    free(short_host);
    /* A checksummed NXSB envelope alone is not a validated filesystem. */
    CHECK(core->mount(&host, &mounted, &info) != IFS_FS_OK && mounted == NULL);
    /* Genuine host I/O failure after a successful root checksum must remain
     * I/O_ERROR, never masquerade as corrupt filesystem metadata. */
    put32(disk+104u,1u); /* one checkpoint descriptor block */
    put32(disk+112u,1u); /* first checkpoint at physical block one */
    cs=sum(disk,4096);put32(disk,(uint32_t)cs);
    put32(disk+4,(uint32_t)(cs>>32));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK);
    fail_checkpoint_reads=1;
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_IO_ERROR && !mounted);
    fail_checkpoint_reads=0;
    put32(disk+104u,0u);put32(disk+112u,0u);
    cs=sum(disk,4096);put32(disk,(uint32_t)cs);
    put32(disk+4,(uint32_t)(cs>>32));
    disk[2048]^=1; variant=12345;
    CHECK(core->probe(&host,&variant)==IFS_FS_CORRUPT && variant==12345);
    disk[2048]^=1; put32(disk+24, UINT32_C(0x40000002));
    cs=sum(disk,4096); put32(disk,(uint32_t)cs); put32(disk+4,(uint32_t)(cs>>32));
    CHECK(core->probe(&host,&variant)==IFS_FS_CORRUPT && variant==12345);
    put32(disk+24, UINT32_C(0x40000001));
    cs=sum(disk,4096); put32(disk,(uint32_t)cs); put32(disk+4,(uint32_t)(cs>>32));
    host.maximum_transfer_bytes=0;
    CHECK(core->probe(&host,&variant)==IFS_FS_INVALID);
    return 0;
}
