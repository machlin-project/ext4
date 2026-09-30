/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_INTERNAL_H
#define MACHLIN_EXT4_INTERNAL_H

#include "disk.h"

struct ext4_journal;
struct ext4_transaction;
struct ext4_read_state;

struct ext4_block_range {
	uint64_t first;
	uint64_t length;
};

struct ext4_block_path {
	uint64_t blocks[EXT4_EXTENT_MAX_DEPTH];
	uint16_t count;
};

/* Longest hash input: the bounded casefolded form of a 255-byte name. */
#define EXT4_DIRECTORY_HASH_MAX 4096U

struct ext4_name_hash {
	uint32_t major;
	uint32_t minor;
};

#define EXT4_ORPHAN_FILE_MAX_BLOCKS 512U
#define EXT4_ORPHAN_FILE_MAX_ENTRIES (1U << 20)

struct ext4_orphan_file {
	struct ext4_inode inode;
	/* Data block addresses followed by unique mapping-node addresses. */
	uint64_t *blocks;
	size_t capacity;
	uint32_t block_count;
	uint32_t mapping_count;
	uint32_t pending;
};

#define EXT4_QUOTA_TYPES 3U
#define EXT4_VALIDATED_MAPS 64U
#define EXT4_VALIDATED_INDEXES 16U

/* The raw inode fields that define and account for an inode's block map. */
struct ext4_map_record {
	uint8_t block_data[EXT4_INODE_BLOCK_BYTES];
	struct ext4_le32 size_lo;
	struct ext4_le32 size_hi;
	struct ext4_le32 blocks_lo;
	struct ext4_le32 flags;
	struct ext4_le32 xattr_block_lo;
	struct ext4_le16 xattr_block_hi;
	struct ext4_le16 blocks_hi;
	struct ext4_le16 mode;
	struct ext4_le16 padding;
};

/* An inode generation whose complete allocation map passed validation while its
 * record held these map fields. */
struct ext4_validated_map {
	uint32_t number;
	uint32_t generation;
	struct ext4_map_record record;
};

/* A verity file whose built-in signature the adapter accepted, by file digest. */
#define EXT4_VERIFIED_SIGNATURES 16U
#define EXT4_VERITY_DIGEST_BYTES 64U

struct ext4_verified_signature {
	uint32_t number;
	uint32_t generation;
	uint8_t algorithm;
	uint8_t digest[EXT4_VERITY_DIGEST_BYTES];
};

/* An encrypted inode's key, derived by the adapter and held by handle. */
#define EXT4_FSCRYPT_KEYS 16U

struct ext4_fscrypt_cached_key {
	uint32_t number;
	uint32_t generation;
	void *handle;
	uint8_t mode;
	uint8_t flags;
};

/* Consecutive inode tables: groups first_group through first_group + groups - 1
 * each occupy the filesystem's inode_table_blocks, starting at block. */
struct ext4_inode_table_run {
	uint64_t block;
	uint32_t first_group;
	uint32_t groups;
};

struct ext4_inode_hold {
	struct ext4_fs *fs;
	struct ext4_inode_hold *next;
	struct ext4_read_state *reader;
	uint32_t number;
	uint32_t generation;
	uint32_t references;
	bool unlinked;
};

struct ext4_fs {
	struct ext4_environment environment;
	struct ext4_info info;
	uint32_t first_data_block;
	uint32_t blocks_per_group;
	uint32_t cluster_blocks;
	uint32_t clusters_per_group;
	uint32_t inodes_per_group;
	uint32_t checksum_seed;
	uint32_t directory_hash_seed[4];
	uint32_t directory_hash_flags;
	uint8_t directory_default_hash_version;
	uint32_t first_inode;
	uint32_t journal_inode;
	uint32_t journal_device;
	uint8_t journal_uuid[EXT4_UUID_SIZE];
	uint32_t last_orphan;
	uint32_t orphan_file_inode;
	uint32_t first_meta_group;
	uint32_t backup_groups[2];
	uint16_t reserved_gdt_blocks;
	uint16_t inode_size;
	uint16_t descriptor_size;
	bool metadata_checksum;
	bool writer_attached;
	bool aborted;
	/* Multi-mount protection owned by this writable instance. */
	struct ext4_mmp_environment mmp_environment;
	struct ext4_write_environment mmp_writer;
	uint64_t mmp_block;
	int64_t mmp_written;
	uint32_t mmp_sequence;
	uint16_t mmp_interval;
	uint8_t mmp_node_name[EXT4_MMP_NODE_NAME_SIZE];
	bool mmp_active;
	bool mmp_released;
	/* The utf8-12.1 encoding rejects names that are not well-formed UTF-8. */
	bool casefold_strict;
	/* User, group and project quota inodes; zero when a type is not tracked.
	 * Writable owners account usage while quota_active is set. */
	uint32_t quota_inodes[EXT4_QUOTA_TYPES];
	bool quota_active;
	/* The adapter's enforcement policy, when enforcing, and its exemption. */
	struct ext4_quota_policy quota_policy;
	bool quota_exempt;
	struct ext4_inode_table_run *inode_table_runs;
	size_t inode_table_run_count;
	uint32_t inode_table_blocks;
	/* A writable mount owns the device exclusively and changes maps only through
	 * validated operations, so an unchanged map needs one complete validation.
	 * Any change to the record's map fields requires another. Recovery rebuilds
	 * maps from logs and never uses this cache. */
	struct ext4_validated_map validated_maps[EXT4_VALIDATED_MAPS];
	uint32_t validated_map_next;
	/* Indexed directories whose every block the mount classified, or whose index
	 * its own validated operations produced, while their map records held these
	 * fields. The mount's changes preserve the classification. */
	struct ext4_validated_map validated_indexes[EXT4_VALIDATED_INDEXES];
	uint32_t validated_index_next;
	bool validated_maps_enabled;
	/* The adapter's cryptography, and verity files whose signatures it accepted.
	 * A changed descriptor has another digest and is verified again. */
	struct ext4_crypto_environment crypto;
	struct ext4_verified_signature verified_signatures[EXT4_VERIFIED_SIGNATURES];
	uint32_t verified_signature_count;
	uint32_t verified_signature_next;
	struct ext4_fscrypt_cached_key fscrypt_keys[EXT4_FSCRYPT_KEYS];
	uint32_t fscrypt_key_count;
	uint32_t fscrypt_key_next;
	struct ext4_journal *journal;
	struct ext4_orphan_file *orphan_file;
	struct ext4_inode_hold *holds;
	uint32_t hold_count;
	/* Advance before a transaction can change the visible inode or block map. */
	uint64_t read_revision;
	struct ext4_block_range *system_ranges;
	size_t system_range_capacity;
	size_t system_range_count;
};

struct ext4_group {
	uint64_t block_bitmap;
	uint64_t inode_bitmap;
	uint64_t inode_table;
	uint64_t table_blocks;
	uint32_t block_bitmap_checksum;
	uint32_t inode_bitmap_checksum;
	uint32_t free_blocks;
	uint32_t free_inodes;
	uint16_t flags;
};

/* Sort, reject overlaps and merge adjacent already-bounded physical ranges. */
enum ext4_result ext4_ranges_sort(struct ext4_block_range *ranges, size_t *count);
/* Sort and merge already-bounded ranges, allowing repeated or overlapping input. */
void ext4_ranges_union(struct ext4_block_range *ranges, size_t *count);
/* Search a sorted, disjoint range set without visiting each member. */
bool ext4_ranges_overlap(
    const struct ext4_block_range *ranges, size_t count, uint64_t block, uint64_t length);
/* Geometry is shared by reads, transactional mutation and metadata exclusion.
 * A group's fixed prefix can be empty; bitmap/table locations remain in its
 * descriptor and can belong to another group when FLEX_BG is enabled. */
bool ext4_group_has_super(const struct ext4_fs *fs, uint32_t group);
enum ext4_result ext4_group_reserved(
    const struct ext4_fs *fs, uint32_t group, struct ext4_block_range *range);
enum ext4_result ext4_group_descriptor_offset(
    const struct ext4_fs *fs, uint32_t group, uint64_t *offset);
enum ext4_result ext4_group_get(struct ext4_fs *fs, uint32_t group, struct ext4_group *result);
enum ext4_result ext4_group_decode(
    struct ext4_fs *fs, uint32_t group, struct ext4_group_disk *disk, struct ext4_group *result);
void ext4_group_checksum_set(struct ext4_fs *fs, uint32_t group, struct ext4_group_disk *disk);
enum ext4_result ext4_system_ranges_build(struct ext4_fs *fs);
/* Acquire multi-mount protection before the first write. Offline recovery uses
 * the checker sequence; stop publishes CLEAN. guard refreshes a stale sequence
 * and rejects mutation after release or when another host owns the volume. */
enum ext4_result ext4_mmp_start(
    struct ext4_fs *fs, const struct ext4_write_environment *writer, bool checker);
enum ext4_result ext4_mmp_stop(struct ext4_fs *fs);
enum ext4_result ext4_mmp_guard(struct ext4_fs *fs);
bool ext4_system_block(const struct ext4_fs *fs, uint64_t block);
bool ext4_system_overlaps(const struct ext4_fs *fs, uint64_t block, uint64_t length);
enum ext4_result ext4_inode_location(struct ext4_fs *fs, uint32_t number, uint64_t *offset);
/* Locate an allocated inode with one descriptor read and a checked bitmap.
 * The exclusive owner keeps both observations stable; errors preserve offset. */
enum ext4_result ext4_inode_resolve(struct ext4_fs *fs, uint32_t number, uint64_t *offset);
enum ext4_result ext4_inode_decode(
    struct ext4_fs *fs, uint32_t number, void *buffer, struct ext4_inode *inode);
/* Offline orphan ownership may inspect allocated inodes with no links. */
enum ext4_result ext4_inode_decode_orphan(
    struct ext4_fs *fs, uint32_t number, void *buffer, struct ext4_inode *inode);
enum ext4_result ext4_inode_decode_live(
    struct ext4_fs *fs, uint32_t number, void *buffer, struct ext4_inode *inode);
struct ext4_inode_hold *ext4_inode_find_hold(struct ext4_fs *fs, uint32_t number);
void ext4_inode_holds_destroy(struct ext4_fs *fs);
void ext4_read_cache_invalidate(struct ext4_fs *fs);
/* Validate mutation-compatible formats. Public operation boundaries separately
 * enforce immutable/append policy; orphan cleanup must finish accepted deletion. */
enum ext4_result ext4_inode_writable(
    struct ext4_fs *fs, const struct ext4_inode_disk *disk, const struct ext4_inode *inode);
enum ext4_result ext4_inode_flags_writable(struct ext4_fs *fs, const struct ext4_inode *inode);
bool ext4_inode_has_xattrs(const struct ext4_fs *fs, const struct ext4_inode_disk *disk);
void ext4_inode_checksum_set(struct ext4_fs *fs, uint32_t number, struct ext4_inode_disk *disk);
enum ext4_result ext4_inode_apply(
    struct ext4_fs *fs, struct ext4_inode_disk *disk, const struct ext4_inode_update *update);
enum ext4_result ext4_inode_allocated(struct ext4_fs *fs, uint32_t number);
enum ext4_result ext4_edit_inode(struct ext4_fs *fs, struct ext4_transaction *transaction,
    uint32_t number, uint32_t generation, struct ext4_inode_disk **disk, struct ext4_inode *inode);
uint32_t ext4_directory_record_length(
    struct ext4_fs *fs, const struct ext4_dir_header_disk *header);
/* Names in an encrypted directory are ciphertext and may hold any byte; other names
 * may hold neither NUL nor '/'. */
enum ext4_result ext4_directory_entry_decode(struct ext4_fs *fs, const uint8_t *buffer,
    uint32_t offset, bool ciphertext, struct ext4_dir_entry *entry, uint32_t *record_length);
/* The caller resolves the on-disk signedness policy into a hash version.
 * Seed words are host-order; NULL/all-zero uses the specified default seed.
 * Failure leaves result unchanged. This hashes bytes without name normalization. */
enum ext4_result ext4_directory_hash(uint8_t version, const uint32_t seed[4], const uint8_t *name,
    size_t length, struct ext4_name_hash *result);
enum ext4_result ext4_directory_checksum(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint8_t *buffer);
enum ext4_result ext4_orphan_cleanup(struct ext4_fs *fs, struct ext4_recovery_report *report);
enum ext4_result ext4_orphan_finish_inode(
    struct ext4_fs *fs, uint32_t number, uint32_t generation, bool retained);
enum ext4_result ext4_orphan_validate_live(struct ext4_fs *fs);
enum ext4_result ext4_orphan_validate(struct ext4_fs *fs);
enum ext4_result ext4_orphan_file_prepare(struct ext4_fs *fs);
void ext4_orphan_file_close(struct ext4_fs *fs);
enum ext4_result ext4_data_block_valid(struct ext4_fs *fs, uint64_t block);
enum ext4_result ext4_block_allocated(struct ext4_fs *fs, uint64_t block);

enum ext4_result ext4_load(
    const struct ext4_environment *environment, bool recovery, struct ext4_fs **result);

/* Keep wire decoding visible to the compiler in metadata scans. Byte accesses
 * preserve the unaligned and host-endianness contract without libc or intrinsics. */
static inline uint16_t
ext4_le16(const struct ext4_le16 *value)
{
	return (uint16_t)value->bytes[0] | (uint16_t)((uint16_t)value->bytes[1] << 8);
}

static inline uint32_t
ext4_le32(const struct ext4_le32 *value)
{
	return (uint32_t)value->bytes[0] | ((uint32_t)value->bytes[1] << 8) |
	    ((uint32_t)value->bytes[2] << 16) | ((uint32_t)value->bytes[3] << 24);
}

static inline void
ext4_encode16(struct ext4_le16 *output, uint16_t value)
{
	output->bytes[0] = (uint8_t)value;
	output->bytes[1] = (uint8_t)(value >> 8);
}

static inline void
ext4_encode32(struct ext4_le32 *output, uint32_t value)
{
	unsigned int index;

	for (index = 0; index < sizeof(output->bytes); index++) {
		output->bytes[index] = (uint8_t)(value >> (index * 8));
	}
}

void ext4_copy(void *destination, const void *source, size_t length);
void ext4_zero(void *destination, size_t length);
bool ext4_equal(const void *left, const void *right, size_t length);
uint32_t ext4_crc32c(uint32_t checksum, const void *buffer, size_t length);
uint32_t ext4_crc32_be(uint32_t checksum, const void *buffer, size_t length);
uint16_t ext4_crc16(uint16_t checksum, const void *buffer, size_t length);
uint32_t ext4_inode_seed(const struct ext4_fs *fs, const struct ext4_inode *inode);
enum ext4_result ext4_device_read(struct ext4_fs *fs, uint64_t offset, void *buffer, size_t length);
enum ext4_result ext4_block_read(struct ext4_fs *fs, uint64_t block, void *buffer);
/* Read a block's home contents on the device, bypassing the journal's committed and
 * pending blocks. Only the recovery marker update needs this view, and only while
 * the log holds no committed transaction. */
enum ext4_result ext4_block_read_committed(struct ext4_fs *fs, uint64_t block, void *buffer);
enum ext4_result ext4_map_block(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint64_t *physical);
/* Read mapped bytes regardless of EOF. Holes read as zero unless data is
 * required; completed reports the prefix read before any error. */
enum ext4_result ext4_read_mapped(struct ext4_fs *fs, const struct ext4_inode *inode,
    uint64_t offset, void *buffer, size_t length, bool require_data, size_t *completed);
/* End of the last extent in an extent-mapped inode, in filesystem blocks. */
enum ext4_result ext4_extent_last_end(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t *end);
enum ext4_result ext4_map_block_path(struct ext4_fs *fs, const struct ext4_inode *inode,
    uint32_t logical, uint64_t *physical, struct ext4_block_path *path);
/* Return a contiguous data or hole run confined to the reported mapping path. */
enum ext4_result ext4_map_blocks_path(struct ext4_fs *fs, const struct ext4_inode *inode,
    uint32_t logical, uint64_t *physical, uint64_t *blocks, struct ext4_block_path *path);

#endif
