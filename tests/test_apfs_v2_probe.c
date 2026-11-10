/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
static uint8_t disk[16 * 4096];
static unsigned reads;
static int reader(void *ctx, uint64_t at, void *dst, uint32_t n) {
    (void)ctx;
    if (n > 512 || at > sizeof(disk) || n > sizeof(disk) - at)
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
int main(void) {
    const IfsFsModuleApiV2 *api = infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_native_valid(api, "apfs"));
    CHECK(api->base_v1 == NULL);
    const IfsFsCoreOpsV1 *core = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    CHECK(core && core->probe && !core->mount && !core->write);
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
    uint64_t cs=sum(disk,4096); put32(disk,(uint32_t)cs);
    put32(disk+4,(uint32_t)(cs>>32));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==1 && reads>=8);
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
