/* SPDX-License-Identifier: GPL-3.0-or-later
 * APFS's source-owned, byte-addressed Common V2 read-only namespace.
 * Both hosted and freestanding providers compile this same native engine.
 */
#ifndef IFS_APFS_V2_READER_H
#define IFS_APFS_V2_READER_H
#include <infiltratr/filesystem_module_v2.h>
typedef struct IfsApfsReader IfsApfsReader;
int ifs_apfs_reader_open(const IfsFsHostRuntimeV2 *host, IfsApfsReader **out,
                         uint32_t *block_bytes, uint64_t *total_bytes);
void ifs_apfs_reader_close(IfsApfsReader *reader);
int ifs_apfs_reader_metadata(IfsApfsReader *,const char *,IfsFsMetadataV2 *);
int ifs_apfs_reader_label(const IfsApfsReader *,char *,uint32_t,uint32_t *);
int ifs_apfs_reader_stat(IfsApfsReader *reader, const char *path, IfsFsEntryV2 *entry);
int ifs_apfs_reader_list(IfsApfsReader *reader, const char *path, uint32_t index,
                         IfsFsEntryV2 *entry);
int ifs_apfs_reader_read(IfsApfsReader *reader, const char *path, uint64_t offset,
                         void *data, uint32_t bytes);
#endif
