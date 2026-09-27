/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_DISK_H
#define MACHLIN_EXT4_DISK_H

#include <ext4/ext4.h>

#define EXT4_SUPER_OFFSET 1024U
#define EXT4_SUPER_SIZE 1024U
#define EXT4_SUPER_MAGIC 0xef53U
#define EXT4_DYNAMIC_REV 1U
#define EXT4_CREATOR_LINUX 0U
#define EXT4_FIRST_NON_RESERVED_INODE 11U
#define EXT4_VALID_FS 0x0001U
#define EXT4_ERROR_FS 0x0002U
#define EXT4_CHECKSUM_CRC32C 1U
#define EXT4_MIN_BLOCK_SIZE 1024U
#define EXT4_MAX_BLOCK_SIZE 65536U
#define EXT4_INODE_BASE_SIZE 128U
#define EXT4_GROUP_BASE_SIZE 32U
#define EXT4_GROUP_64_SIZE 64U
#define EXT4_GROUP_MAX_SIZE 1024U
#define EXT4_EXTENT_MAGIC 0xf30aU
#define EXT4_EXTENT_MAX_DEPTH 5U
#define EXT4_EXTENT_UNWRITTEN_LIMIT 32768U
#define EXT4_DIRECT_BLOCKS 12U
#define EXT4_INDIRECT_LEVELS 3U
#define EXT4_DIRECTORY_TAIL_TYPE 0xdeU
#define EXT4_CRC32C_POLYNOMIAL 0x82f63b78U

#define EXT4_FEATURE_COMPAT_HAS_JOURNAL 0x0004U
#define EXT4_FEATURE_COMPAT_EXT_ATTR 0x0008U
#define EXT4_FEATURE_COMPAT_RESIZE_INODE 0x0010U
#define EXT4_FEATURE_COMPAT_DIR_INDEX 0x0020U
#define EXT4_WRITABLE_COMPAT                                                                       \
	(EXT4_FEATURE_COMPAT_HAS_JOURNAL | EXT4_FEATURE_COMPAT_EXT_ATTR |                          \
	    EXT4_FEATURE_COMPAT_RESIZE_INODE | EXT4_FEATURE_COMPAT_DIR_INDEX)

#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x0002U
#define EXT4_FEATURE_INCOMPAT_RECOVER 0x0004U
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040U
#define EXT4_FEATURE_INCOMPAT_64BIT 0x0080U
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x0200U
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED 0x2000U
#define EXT4_SUPPORTED_INCOMPAT                                                                    \
	(EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS |                          \
	    EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG |                          \
	    EXT4_FEATURE_INCOMPAT_CSUM_SEED)

#define EXT4_FEATURE_RO_HUGE_FILE 0x0008U
#define EXT4_FEATURE_RO_SPARSE_SUPER 0x0001U
#define EXT4_FEATURE_RO_LARGE_FILE 0x0002U
#define EXT4_FEATURE_RO_DIR_NLINK 0x0020U
#define EXT4_FEATURE_RO_EXTRA_ISIZE 0x0040U
#define EXT4_FEATURE_RO_GDT_CSUM 0x0010U
#define EXT4_FEATURE_RO_BIGALLOC 0x0200U
#define EXT4_FEATURE_RO_METADATA_CSUM 0x0400U
#define EXT4_WRITABLE_RO_COMPAT                                                                    \
	(EXT4_FEATURE_RO_SPARSE_SUPER | EXT4_FEATURE_RO_LARGE_FILE | EXT4_FEATURE_RO_HUGE_FILE |   \
	    EXT4_FEATURE_RO_DIR_NLINK | EXT4_FEATURE_RO_EXTRA_ISIZE |                              \
	    EXT4_FEATURE_RO_METADATA_CSUM)
#define EXT4_INODE_HUGE_FILE 0x00040000U
#define EXT4_INODE_INDEX 0x00001000U
#define EXT4_INODE_EXTENTS 0x00080000U
#define EXT4_INODE_INLINE_DATA 0x10000000U
#define EXT4_INODE_SYNC 0x00000008U
#define EXT4_INODE_IMMUTABLE 0x00000010U
#define EXT4_INODE_APPEND 0x00000020U
#define EXT4_INODE_NODUMP 0x00000040U
#define EXT4_INODE_NOATIME 0x00000080U
#define EXT4_INODE_JOURNAL_DATA 0x00004000U
#define EXT4_INODE_NOTAIL 0x00008000U
#define EXT4_INODE_DIRSYNC 0x00010000U
#define EXT4_INODE_TOPDIR 0x00020000U
#define EXT4_INODE_EOFBLOCKS 0x00400000U
#define EXT4_INODE_WRITABLE_FLAGS                                                                  \
	(EXT4_INODE_SYNC | EXT4_INODE_NODUMP | EXT4_INODE_NOATIME | EXT4_INODE_INDEX |             \
	    EXT4_INODE_JOURNAL_DATA | EXT4_INODE_NOTAIL | EXT4_INODE_DIRSYNC | EXT4_INODE_TOPDIR | \
	    EXT4_INODE_HUGE_FILE | EXT4_INODE_EXTENTS | EXT4_INODE_EOFBLOCKS)
#define EXT4_GROUP_INODE_UNINIT 0x0001U
#define EXT4_GROUP_BLOCK_UNINIT 0x0002U
#define EXT4_MODE_PERMISSIONS 07777U
#define EXT4_BITS_PER_BYTE 8U
#define EXT4_XATTR_MAGIC 0xea020000U
#define EXT4_TIME_EPOCH_MASK 3U
#define EXT4_TIME_NANOSECOND_SHIFT 2U
#define EXT4_NANOSECONDS_PER_SECOND 1000000000U
#define EXT4_INODE_HAS_FIELD(extra_size, field)                                                    \
	(EXT4_INODE_BASE_SIZE + (size_t)(extra_size) >= offsetof(struct ext4_inode_disk, field) +  \
		sizeof(((struct ext4_inode_disk *)0)->field))

struct ext4_le16 {
	uint8_t bytes[2];
};

struct ext4_le32 {
	uint8_t bytes[4];
};

/* Byte-array fields deliberately have alignment one. Field ordering follows the
 * ext4 on-disk specification; these are not native Linux kernel structures. */
struct ext4_super_disk {
	struct ext4_le32 inodes_count;
	struct ext4_le32 blocks_count_lo;
	struct ext4_le32 reserved_blocks_lo;
	struct ext4_le32 free_blocks_lo;
	struct ext4_le32 free_inodes;
	struct ext4_le32 first_data_block;
	struct ext4_le32 log_block_size;
	struct ext4_le32 log_cluster_size;
	struct ext4_le32 blocks_per_group;
	struct ext4_le32 clusters_per_group;
	struct ext4_le32 inodes_per_group;
	struct ext4_le32 mount_time;
	struct ext4_le32 write_time;
	struct ext4_le16 mount_count;
	struct ext4_le16 max_mount_count;
	struct ext4_le16 magic;
	struct ext4_le16 state;
	struct ext4_le16 errors;
	struct ext4_le16 minor_revision;
	struct ext4_le32 last_check;
	struct ext4_le32 check_interval;
	struct ext4_le32 creator_os;
	struct ext4_le32 revision;
	struct ext4_le16 reserved_uid;
	struct ext4_le16 reserved_gid;
	struct ext4_le32 first_inode;
	struct ext4_le16 inode_size;
	struct ext4_le16 block_group;
	struct ext4_le32 feature_compat;
	struct ext4_le32 feature_incompat;
	struct ext4_le32 feature_ro_compat;
	uint8_t uuid[EXT4_UUID_SIZE];
	char volume_name[EXT4_VOLUME_NAME_SIZE];
	char last_mounted[64];
	struct ext4_le32 algorithm_bitmap;
	uint8_t preallocate_blocks;
	uint8_t preallocate_directory_blocks;
	struct ext4_le16 reserved_gdt_blocks;
	uint8_t journal_uuid[EXT4_UUID_SIZE];
	struct ext4_le32 journal_inode;
	struct ext4_le32 journal_device;
	struct ext4_le32 last_orphan;
	struct ext4_le32 hash_seed[4];
	uint8_t default_hash_version;
	uint8_t journal_backup_type;
	struct ext4_le16 descriptor_size;
	struct ext4_le32 default_mount_options;
	struct ext4_le32 first_meta_group;
	struct ext4_le32 creation_time;
	struct ext4_le32 journal_blocks[17];
	struct ext4_le32 blocks_count_hi;
	struct ext4_le32 reserved_blocks_hi;
	struct ext4_le32 free_blocks_hi;
	struct ext4_le16 min_extra_inode_size;
	struct ext4_le16 want_extra_inode_size;
	struct ext4_le32 flags;
	struct ext4_le16 raid_stride;
	struct ext4_le16 mmp_interval;
	struct ext4_le32 mmp_block_lo;
	struct ext4_le32 mmp_block_hi;
	struct ext4_le32 raid_stripe_width;
	uint8_t log_groups_per_flex;
	uint8_t checksum_type;
	uint8_t encryption_level;
	uint8_t reserved_padding;
	struct ext4_le32 lifetime_kib_written_lo;
	struct ext4_le32 lifetime_kib_written_hi;
	struct ext4_le32 snapshot_inode;
	struct ext4_le32 snapshot_id;
	struct ext4_le32 snapshot_reserved_lo;
	struct ext4_le32 snapshot_reserved_hi;
	struct ext4_le32 snapshot_list;
	struct ext4_le32 error_count;
	struct ext4_le32 first_error_time;
	struct ext4_le32 first_error_inode;
	struct ext4_le32 first_error_block_lo;
	struct ext4_le32 first_error_block_hi;
	char first_error_function[32];
	struct ext4_le32 first_error_line;
	struct ext4_le32 last_error_time;
	struct ext4_le32 last_error_inode;
	struct ext4_le32 last_error_line;
	struct ext4_le32 last_error_block_lo;
	struct ext4_le32 last_error_block_hi;
	char last_error_function[32];
	char mount_options[64];
	struct ext4_le32 user_quota_inode;
	struct ext4_le32 group_quota_inode;
	struct ext4_le32 overhead_clusters;
	struct ext4_le32 backup_groups[2];
	uint8_t encryption_algorithms[4];
	uint8_t encryption_salt[16];
	struct ext4_le32 lost_found_inode;
	struct ext4_le32 project_quota_inode;
	struct ext4_le32 checksum_seed;
	uint8_t write_time_hi;
	uint8_t mount_time_hi;
	uint8_t creation_time_hi;
	uint8_t last_check_hi;
	uint8_t first_error_time_hi;
	uint8_t last_error_time_hi;
	uint8_t first_error_code;
	uint8_t last_error_code;
	struct ext4_le16 encoding;
	struct ext4_le16 encoding_flags;
	struct ext4_le32 orphan_file_inode;
	struct ext4_le32 reserved[94];
	struct ext4_le32 checksum;
};

struct ext4_group_disk {
	struct ext4_le32 block_bitmap_lo;
	struct ext4_le32 inode_bitmap_lo;
	struct ext4_le32 inode_table_lo;
	struct ext4_le16 free_blocks_lo;
	struct ext4_le16 free_inodes_lo;
	struct ext4_le16 used_directories_lo;
	struct ext4_le16 flags;
	struct ext4_le32 exclude_bitmap_lo;
	struct ext4_le16 block_bitmap_checksum_lo;
	struct ext4_le16 inode_bitmap_checksum_lo;
	struct ext4_le16 unused_inodes_lo;
	struct ext4_le16 checksum;
	struct ext4_le32 block_bitmap_hi;
	struct ext4_le32 inode_bitmap_hi;
	struct ext4_le32 inode_table_hi;
	struct ext4_le16 free_blocks_hi;
	struct ext4_le16 free_inodes_hi;
	struct ext4_le16 used_directories_hi;
	struct ext4_le16 unused_inodes_hi;
	struct ext4_le32 exclude_bitmap_hi;
	struct ext4_le16 block_bitmap_checksum_hi;
	struct ext4_le16 inode_bitmap_checksum_hi;
	struct ext4_le32 reserved;
};

struct ext4_inode_disk {
	struct ext4_le16 mode;
	struct ext4_le16 uid_lo;
	struct ext4_le32 size_lo;
	struct ext4_le32 access_time;
	struct ext4_le32 change_time;
	struct ext4_le32 modify_time;
	struct ext4_le32 deletion_time;
	struct ext4_le16 gid_lo;
	struct ext4_le16 links;
	struct ext4_le32 blocks_lo;
	struct ext4_le32 flags;
	struct ext4_le32 version_lo;
	uint8_t block_data[EXT4_INODE_BLOCK_BYTES];
	struct ext4_le32 generation;
	struct ext4_le32 xattr_block_lo;
	struct ext4_le32 size_hi;
	struct ext4_le32 obsolete_fragment;
	struct ext4_le16 blocks_hi;
	struct ext4_le16 xattr_block_hi;
	struct ext4_le16 uid_hi;
	struct ext4_le16 gid_hi;
	struct ext4_le16 checksum_lo;
	struct ext4_le16 reserved;
	struct ext4_le16 extra_size;
	struct ext4_le16 checksum_hi;
	struct ext4_le32 change_time_extra;
	struct ext4_le32 modify_time_extra;
	struct ext4_le32 access_time_extra;
	struct ext4_le32 birth_time;
	struct ext4_le32 birth_time_extra;
	struct ext4_le32 version_hi;
	struct ext4_le32 project_id;
};

struct ext4_extent_header_disk {
	struct ext4_le16 magic;
	struct ext4_le16 entries;
	struct ext4_le16 maximum;
	struct ext4_le16 depth;
	struct ext4_le32 generation;
};

struct ext4_extent_disk {
	struct ext4_le32 logical;
	struct ext4_le16 length;
	struct ext4_le16 physical_hi;
	struct ext4_le32 physical_lo;
};

struct ext4_extent_index_disk {
	struct ext4_le32 logical;
	struct ext4_le32 child_lo;
	struct ext4_le16 child_hi;
	struct ext4_le16 unused;
};

struct ext4_dir_header_disk {
	struct ext4_le32 inode;
	struct ext4_le16 record_length;
	uint8_t name_length;
	uint8_t type;
};

struct ext4_dir_tail_disk {
	struct ext4_le32 reserved;
	struct ext4_le16 record_length;
	uint8_t zero;
	uint8_t type;
	struct ext4_le32 checksum;
};

struct ext4_dx_root_prefix_disk {
	struct ext4_dir_header_disk dot;
	uint8_t dot_name[4];
	struct ext4_dir_header_disk dotdot;
	uint8_t dotdot_name[4];
	struct ext4_le32 reserved;
	uint8_t hash_version;
	uint8_t info_length;
	uint8_t indirect_levels;
	uint8_t flags;
};

struct ext4_dx_count_disk {
	struct ext4_le16 limit;
	struct ext4_le16 count;
	struct ext4_le32 block;
};

struct ext4_dx_tail_disk {
	struct ext4_le32 reserved;
	struct ext4_le32 checksum;
};

_Static_assert(sizeof(struct ext4_super_disk) == EXT4_SUPER_SIZE, "superblock wire size");
_Static_assert(sizeof(struct ext4_group_disk) == EXT4_GROUP_64_SIZE, "group descriptor wire size");
_Static_assert(sizeof(struct ext4_inode_disk) == EXT4_INODE_BASE_SIZE + 32, "inode wire prefix");
_Static_assert(sizeof(struct ext4_extent_disk) == 12, "extent wire size");
_Static_assert(sizeof(struct ext4_dir_tail_disk) == 12, "directory tail wire size");

#endif
