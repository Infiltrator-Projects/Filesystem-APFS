# APFS: technical reference

## Scope and architecture

Apple File System replaced HFS Plus as the default on modern Apple platforms. It separates physical stores, a container sharing capacity among volumes, volume-specific namespace trees, and transaction-scoped object mappings. This reference describes the public on-disk model and unencrypted single-store reading. Fusion, encryption, sealed system volumes, snapshots, compression and write/recovery support require their own qualified profiles. Do not treat a successful NXSB probe as evidence of all these capabilities.

## Object headers and integrity

Ordinary objects begin with a 32-byte little-endian header: checksum at 0 (64 bits), object identifier at 8 (64), transaction ID at 16 (64), type at 24 (32), subtype at 28 (32). Storage class is encoded in the high type bits: virtual 0, physical 0x40000000, ephemeral 0x80000000. Physical identifiers name blocks directly; virtual identifiers require object-map lookup; ephemeral objects are resolved through checkpoint mappings. Validate Fletcher-64 over the required object bytes, excluding the checksum field, with the APFS checkword convention (not merely the raw running sums). Reject mismatched type, identity, size or checksum; some special objects omit ordinary headers.

## Container superblock geometry

The NX superblock starts with the object header; magic NXSB is at 32, block size 36, block count 40, compatible features 48, read-only-compatible 56, incompatible 64, UUID 72, next oid 88, next xid 96. Multi-byte fields are little endian. Check block size, block count, multiplication overflow and device length. The initial superblock is at container block zero; current committed state is selected from checkpoints, not automatically from this initial block. All physical block offsets are relative to the appropriate store.

## Checkpoint descriptor and data areas

NX fields: descriptor blocks 104, data blocks 108, descriptor base 112, data base 120, next descriptor/data indices 128/132, descriptor index/length 136/140, data index/length 144/148. Spaceman, container object map and reaper identifiers are at 152/160/168; maximum filesystem count at 180; volume oid array begins at 184. Descriptor area can be noncontiguous under a flagged representation; reject unsupported geometry instead of masking and assuming a contiguous range. A checkpoint map has flags at 32, count at 36, 40-byte entries from 40: type 0, subtype 4, size 8, pad 12, filesystem oid 16, object oid 24, physical address 32. LAST flag is bit 0.

## Checkpoint selection

Enumerate candidate descriptor records within validated bounds. Associate maps and NXSB objects by transaction identity and checkpoint sequence. Require complete mappings for needed ephemeral objects, consistent geometry and checksums. Select the newest internally coherent committed checkpoint that the implementation can read, with explicit diagnostics for rejected candidates. Do not assemble a synthetic checkpoint by mixing the newest individually valid objects from different transactions. Recovery and fallback to an older checkpoint must not modify media during inspection.

## Object maps and version lookup

OMAP physical object: flags 32, snapshot count 36, tree type 40, snapshot tree type 44, tree oid 48, snapshot tree oid 56, most recent snapshot xid 64, pending revert minimum/maximum 72/80. Map key is oid at 0 and xid at 8, both 64-bit. Value is flags 0, size 4 and physical block 8. Lookup the requested oid at the greatest xid not exceeding the selected transaction. Reject a deleted winning record (flag 1); do not fall back to an older live record. Other flags include saved 2, encrypted 4, no-header 8 and crypto-generation 16. Container and volume object maps have different scopes.

## Volume superblocks

Resolve a nonzero container volume oid through the container object map. A volume object begins with APSB magic at 32, filesystem index 36, feature masks 40/48/56, unmount time 64, reserve 72, quota 80 and allocated-block count 88. Wrapped metadata crypto state follows, before root, extent-reference and snapshot tree types and identifiers. Use the exact revision-specific packed layout for those fields. The volume has its own UUID, name, role, namespace root and object map. Container free capacity is shared; do not equate the volume allocation count or quota with independent partition geometry.

## B-tree node layout

The standard node has flags at 32 (16 bits), level 34, key count 36, table-space offset/length 40/42, free-space 44/46, key-free-list 48/50 and value-free-list 52/54; node data begins 56. Root flag 1, leaf 2, fixed KV 4, hashed 8 and no-header 16. Fixed records use two 16-bit offsets (4 bytes); variable records use key and value location pairs (8 bytes). Root footer is 40 bytes, including tree flags, node size, fixed key and value lengths, maximum key/value lengths and 64-bit key/node counts. For the ordinary zero table-offset profile, key base is 56+table length; value base is node end minus root footer if root. Key offsets add to their base; value offsets subtract from theirs. Invalid value offset is 0xffff. Validate nonoverlap, ranges and record order.

## Tree navigation and key order

An internal entry identifies a child; resolve according to the tree's physical, virtual or ephemeral storage class and the selected transaction. Hashed nodes may carry child hashes, not just a plain oid. Validate level descent and prevent cycles, bound key comparisons by record length, and use the tree-specific comparator. APFS filesystem keys encode object ID in the low 60 bits and record type in the high four bits of a little-endian 64-bit word. Types include inode 3, xattr 4, sibling link 5, data-stream ID 6, file extent 8, directory record 9 and directory statistics 10. Sorting the raw bytes or treating all keys as one unsigned integer is not a substitute for the defined comparator.

## Inodes and extended fields

The ordinary inode value begins with parent ID 0, private ID 8, create/mod/change/access timestamps 16/24/32/40, internal flags 48, child or link count 56, protection class 60, write generation 64, BSD flags 68, owner 72, group 76, mode 80, padding 82 and uncompressed size 84. Extended fields begin at 92. Xfield header is count (16 bits), used-data (16); each descriptor has type, flags and 16-bit size, followed by aligned data. Data-stream xfield type 8 contains size, allocated size, default crypto ID, bytes written and bytes read, each 64 bits. The private ID can identify a shared data stream and is not necessarily the inode object ID. Preserve unknown fields when mutating.

## Directories and path lookup

Directory record key combines parent ID and type with either 16-bit name length or a 32-bit length-and-hash field, depending on the volume profile. The latter uses low 10 bits for length and upper 22 for hash. Validate termination and bounds; hashes are not unique names. Compare using the applicable normalization and case-sensitivity rules, and verify the directory value's target inode. Value contains file ID 0, date added 8, flags 16, then optional xfields. Follow the namespace from root inode 2. Sibling-link and sibling-map records carry hard-link metadata; deleting one name does not automatically retire shared file data.

## File extents and worked translation

File extent key adds a 64-bit logical byte offset to the record header. Its 24-byte value contains length-and-flags at 0, physical block number 8, crypto ID 16; low 56 bits of length-and-flags are length in bytes and high 8 bits flags. Locate the containing logical extent for the data-stream ID, not necessarily the inode number. With 4096-byte blocks, extent logical start 8192, physical start 100 and requested offset 9000, the relative physical byte is 410408. A gap or supported hole reads as zero; never turn a hole into a read from container block zero. Clip to stream size. Compression, encrypted streams and sealed-volume extent trees require distinct decoding paths.

## Space manager and sharing

Spaceman tracks allocation in devices, chunks and allocation bitmaps; checkpoint data maps locate ephemeral structures. Extent-reference records track physical storage ownership, while file extent records map logical stream offsets. Clones and snapshots can share blocks. Copy-on-write relocation must update mappings and reference accounting without freeing data still reachable by another stream or snapshot. Allocation reserves and deferred free queues are not equivalent to immediately reusable capacity. Derive free space through the appropriate committed allocator structures; a scan of file extents is insufficient to establish safe allocation.

## Transactions, snapshots and repair

APFS publishes changes using copy-on-write metadata and checkpoint transactions. A writer needs allocation reservation, ordered writes of new data and metadata, object-map updates, checkpoint mapping and final publication with required flushes. Old objects must remain reachable until all retained snapshots and transactions no longer need them. Snapshot metadata connects transaction, snapshot superblock and extent references. Verify integrity hashes on sealed volumes under the correct trust model; an ordinary object checksum is not a seal. Interrupted transactions must leave at least one recoverable state. The full allocator, snapshot retirement, encryption and transaction-recovery implementation is not specified by these introductory algorithms.

## Primary sources and validation

Apple File System Reference: https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf . Independent readable layout cross-check: https://github.com/sgan81/apfs-fuse/blob/master/ApfsLib/DiskStruct.h (blob aac7020ca409aae090989cc42502cdccff4e1c36); BTree.cpp provides a constrained reader example. Validate native-created images including multiple volumes, object-map deletion, checkpoint wrap, fixed/variable nodes, sparse files, normalization collisions and clones. Exercise corruption of checksums and bounds. The historical manual follows; provider status and format feature support are separate. This guide does not claim a complete independently qualified APFS writer.
