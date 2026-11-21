/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILTRATOR_APFS_DISK_H
#define INFILTRATOR_APFS_DISK_H
#include <infiltratr/endian.h>
#include <infiltratr/filesystem_module_v2.h>
#include <stdbool.h>
#include <stddef.h>

/* The block-zero copy is at a known physical address. The object itself
 * is the ephemeral NX superblock, with reserved object identifier one. */
#define APFS_NX_OBJECT_TYPE UINT32_C(0x80000001)

static inline uint64_t fletcher64(const uint8_t *raw, size_t length)
{
    uint64_t sum1 = 0U;
    uint64_t sum2 = 0U;
    const uint64_t mod = UINT64_C(0xffffffff);
    for (size_t offset = 8U; offset + 4U <= length; offset += 4U) {
        sum1 = (sum1 + infiltratr_load_le32(raw + offset)) % mod;
        sum2 = (sum2 + sum1) % mod;
    }
    const uint64_t c1 = mod - ((sum1 + sum2) % mod);
    const uint64_t c2 = mod - ((sum1 + c1) % mod);
    return (c2 << 32U) | c1;
}

static inline bool checksum_valid(const uint8_t *raw, size_t length)
{
    return length >= 8U && infiltratr_load_le64(raw) == fletcher64(raw, length);
}

int apfs_v2_probe(const IfsFsHostRuntimeV2 *host,uint32_t *variant);
#endif
