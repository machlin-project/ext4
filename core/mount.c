/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

static enum ext4_result
ext4_super_validate(struct ext4_fs *fs, const struct ext4_super_disk *super, bool recovery)
{
	struct ext4_block_range primary;
	uint32_t logarithm;
	uint32_t cluster_logarithm;
	uint32_t revision;
	uint32_t incompat;
	uint32_t checksum;
	uint32_t bitmap_capacity;
	uint32_t expected_first_block;
	uint64_t device_blocks;
	uint64_t group_blocks;
	uint64_t group_count;
	uint64_t inode_groups;
	uint16_t minimum_descriptor_size;
	uint16_t state;
	unsigned int word;

	if (ext4_le16(&super->magic) != EXT4_SUPER_MAGIC) {
		return EXT4_NOT_EXT4;
	}
	revision = ext4_le32(&super->revision);
	if (revision > EXT4_DYNAMIC_REV || ext4_le32(&super->creator_os) != EXT4_CREATOR_LINUX) {
		return EXT4_UNSUPPORTED;
	}
	fs->info.feature_compat = ext4_le32(&super->feature_compat);
	fs->info.feature_incompat = incompat = ext4_le32(&super->feature_incompat);
	fs->info.feature_ro_compat = ext4_le32(&super->feature_ro_compat);
	fs->metadata_checksum = (fs->info.feature_ro_compat & EXT4_FEATURE_RO_METADATA_CSUM) != 0;
	if (fs->metadata_checksum) {
		if (super->checksum_type != EXT4_CHECKSUM_CRC32C) {
			return EXT4_UNSUPPORTED;
		}
		checksum =
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum));
		if (checksum != ext4_le32(&super->checksum)) {
			return EXT4_CORRUPT;
		}
	}
	if ((fs->info.feature_ro_compat & EXT4_FEATURE_RO_ORPHAN_PRESENT) &&
	    !(fs->info.feature_compat & EXT4_FEATURE_COMPAT_ORPHAN_FILE)) {
		return EXT4_CORRUPT;
	}
	if (!recovery &&
	    ((incompat & EXT4_FEATURE_INCOMPAT_RECOVER) ||
		(fs->info.feature_ro_compat & EXT4_FEATURE_RO_ORPHAN_PRESENT))) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (incompat & ~(EXT4_SUPPORTED_INCOMPAT | EXT4_FEATURE_INCOMPAT_RECOVER)) {
		return EXT4_UNSUPPORTED;
	}
	/* Distributed descriptors replace the reserved-GDT resize inode layout. */
	if ((incompat & EXT4_FEATURE_INCOMPAT_META_BG) &&
	    (fs->info.feature_compat & EXT4_FEATURE_COMPAT_RESIZE_INODE)) {
		return EXT4_CORRUPT;
	}
	state = ext4_le16(&super->state);
	if ((state & EXT4_ERROR_FS) ||
	    (!recovery && (!(state & EXT4_VALID_FS) || ext4_le32(&super->last_orphan) != 0))) {
		return EXT4_RECOVERY_REQUIRED;
	}
	logarithm = ext4_le32(&super->log_block_size);
	cluster_logarithm = ext4_le32(&super->log_cluster_size);
	if (logarithm > 6 || cluster_logarithm < logarithm || cluster_logarithm - logarithm > 15) {
		return EXT4_UNSUPPORTED;
	}
	if (!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_BIGALLOC) &&
	    cluster_logarithm != logarithm) {
		return EXT4_CORRUPT;
	}
	if ((fs->info.feature_ro_compat & EXT4_FEATURE_RO_BIGALLOC) &&
	    !(incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) {
		return EXT4_CORRUPT;
	}
	fs->cluster_blocks = 1U << (cluster_logarithm - logarithm);
	fs->clusters_per_group = ext4_le32(&super->clusters_per_group);
	fs->info.block_size = EXT4_MIN_BLOCK_SIZE << logarithm;
	fs->info.blocks = ext4_le32(&super->blocks_count_lo);
	fs->info.free_blocks = ext4_le32(&super->free_blocks_lo);
	if (incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		fs->info.blocks |= (uint64_t)ext4_le32(&super->blocks_count_hi) << 32;
		fs->info.free_blocks |= (uint64_t)ext4_le32(&super->free_blocks_hi) << 32;
	}
	fs->info.inodes = ext4_le32(&super->inodes_count);
	fs->info.free_inodes = ext4_le32(&super->free_inodes);
	fs->first_data_block = ext4_le32(&super->first_data_block);
	fs->blocks_per_group = ext4_le32(&super->blocks_per_group);
	fs->inodes_per_group = ext4_le32(&super->inodes_per_group);
	fs->first_inode =
	    revision == 0 ? EXT4_FIRST_NON_RESERVED_INODE : ext4_le32(&super->first_inode);
	fs->journal_inode = ext4_le32(&super->journal_inode);
	fs->journal_device = ext4_le32(&super->journal_device);
	ext4_copy(fs->journal_uuid, super->journal_uuid, sizeof(fs->journal_uuid));
	fs->last_orphan = ext4_le32(&super->last_orphan);
	fs->orphan_file_inode = (fs->info.feature_compat & EXT4_FEATURE_COMPAT_ORPHAN_FILE)
	    ? ext4_le32(&super->orphan_file_inode)
	    : 0;
	if ((fs->info.feature_compat & EXT4_FEATURE_COMPAT_ORPHAN_FILE) &&
	    (fs->orphan_file_inode == 0 || fs->orphan_file_inode < fs->first_inode ||
		fs->orphan_file_inode > fs->info.inodes ||
		fs->orphan_file_inode == fs->journal_inode)) {
		return EXT4_CORRUPT;
	}
	fs->reserved_gdt_blocks = ext4_le16(&super->reserved_gdt_blocks);
	fs->inode_size = revision == 0 ? EXT4_INODE_BASE_SIZE : ext4_le16(&super->inode_size);
	fs->descriptor_size = (incompat & EXT4_FEATURE_INCOMPAT_64BIT)
	    ? ext4_le16(&super->descriptor_size)
	    : EXT4_GROUP_BASE_SIZE;

	/* Device bounds and cluster geometry. */
	device_blocks = fs->environment.size_bytes / fs->info.block_size;
	bitmap_capacity = fs->info.block_size * EXT4_BITS_PER_BYTE;
	expected_first_block =
	    (fs->info.block_size == EXT4_MIN_BLOCK_SIZE && fs->cluster_blocks == 1) ? 1U : 0U;
	if (fs->first_data_block != expected_first_block) {
		return EXT4_CORRUPT;
	}
	if (fs->info.blocks <= fs->first_data_block || fs->info.blocks > device_blocks) {
		return EXT4_CORRUPT;
	}
	if (fs->clusters_per_group == 0 || fs->clusters_per_group > bitmap_capacity) {
		return EXT4_CORRUPT;
	}
	group_blocks = (uint64_t)fs->clusters_per_group * fs->cluster_blocks;
	if (group_blocks != fs->blocks_per_group) {
		return EXT4_CORRUPT;
	}
	if (fs->info.free_blocks > fs->info.blocks ||
	    fs->info.free_blocks % fs->cluster_blocks != 0) {
		return EXT4_CORRUPT;
	}

	/* Inode counts and on-disk record size. */
	if (fs->inodes_per_group == 0 || fs->inodes_per_group > bitmap_capacity) {
		return EXT4_CORRUPT;
	}
	if (fs->info.inodes < EXT4_ROOT_INODE || fs->info.free_inodes > fs->info.inodes) {
		return EXT4_CORRUPT;
	}
	if (fs->inode_size < EXT4_INODE_BASE_SIZE || fs->inode_size > fs->info.block_size) {
		return EXT4_CORRUPT;
	}
	if ((fs->inode_size & (fs->inode_size - 1)) != 0) {
		return EXT4_CORRUPT;
	}

	/* Group descriptors must accommodate the enabled address width. */
	minimum_descriptor_size =
	    (incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? EXT4_GROUP_64_SIZE : EXT4_GROUP_BASE_SIZE;
	if (fs->descriptor_size < minimum_descriptor_size ||
	    fs->descriptor_size > EXT4_GROUP_MAX_SIZE ||
	    fs->descriptor_size > fs->info.block_size) {
		return EXT4_CORRUPT;
	}
	if ((fs->descriptor_size & (fs->descriptor_size - 1)) != 0) {
		return EXT4_CORRUPT;
	}

	group_count = (fs->info.blocks - fs->first_data_block - 1) / fs->blocks_per_group + 1;
	inode_groups = ((uint64_t)fs->info.inodes - 1) / fs->inodes_per_group + 1;
	if (group_count > UINT32_MAX || group_count != inode_groups ||
	    group_count * fs->descriptor_size >
		(fs->info.blocks - fs->first_data_block - 1) * fs->info.block_size) {
		return EXT4_CORRUPT;
	}
	fs->info.groups = (uint32_t)group_count;
	if (incompat & EXT4_FEATURE_INCOMPAT_META_BG) {
		fs->first_meta_group = ext4_le32(&super->first_meta_group);
		if (fs->first_meta_group >
		    (group_count * fs->descriptor_size + fs->info.block_size - 1U) /
			fs->info.block_size) {
			return EXT4_CORRUPT;
		}
	}
	if (fs->info.feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2) {
		for (word = 0; word < 2; word++) {
			fs->backup_groups[word] = ext4_le32(&super->backup_groups[word]);
			if (fs->backup_groups[word] >= fs->info.groups) {
				return EXT4_CORRUPT;
			}
		}
	}
	ext4_copy(fs->info.uuid, super->uuid, sizeof(fs->info.uuid));
	ext4_copy(fs->info.volume_name, super->volume_name, EXT4_VOLUME_NAME_SIZE);
	fs->info.volume_name[EXT4_VOLUME_NAME_SIZE] = '\0';
	fs->checksum_seed = (incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
	    ? ext4_le32(&super->checksum_seed)
	    : ext4_crc32c(UINT32_MAX, super->uuid, sizeof(super->uuid));
	fs->directory_hash_flags =
	    ext4_le32(&super->flags) & (EXT4_SIGNED_DIRECTORY_HASH | EXT4_UNSIGNED_DIRECTORY_HASH);
	for (word = 0; word < 4; word++) {
		fs->directory_hash_seed[word] = ext4_le32(&super->hash_seed[word]);
	}
	return ext4_group_reserved(fs, 0, &primary);
}

enum ext4_result
ext4_load(const struct ext4_environment *environment, bool recovery, struct ext4_fs **result)
{
	struct ext4_super_disk super;
	struct ext4_inode root;
	struct ext4_fs *fs;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (environment == NULL || environment->read == NULL || environment->allocate == NULL ||
	    environment->release == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (environment->size_bytes < EXT4_SUPER_OFFSET + EXT4_SUPER_SIZE) {
		return EXT4_NOT_EXT4;
	}
	fs = environment->allocate(environment->context, sizeof(*fs));
	if (fs == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(fs, sizeof(*fs));
	fs->environment = *environment;
	error = ext4_device_read(fs, EXT4_SUPER_OFFSET, &super, sizeof(super));
	if (error == EXT4_OK) {
		error = ext4_super_validate(fs, &super, recovery);
	}
	/* During recovery the home copy of the root may be partially checkpointed. */
	if (error == EXT4_OK && !recovery) {
		error = ext4_get_inode(fs, EXT4_ROOT_INODE, &root);
		if (error == EXT4_OK && (root.mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY) {
			error = EXT4_CORRUPT;
		}
	}
	if (error != EXT4_OK) {
		ext4_unmount(fs);
		return error;
	}
	*result = fs;
	return EXT4_OK;
}

enum ext4_result
ext4_mount(const struct ext4_environment *environment, struct ext4_fs **result)
{
	return ext4_load(environment, false, result);
}

void
ext4_unmount(struct ext4_fs *fs)
{
	struct ext4_environment environment;

	if (fs == NULL) {
		return;
	}
	environment = fs->environment;
	ext4_inode_holds_destroy(fs);
	ext4_journal_close(fs->journal);
	ext4_orphan_file_close(fs);
	if (fs->system_ranges != NULL) {
		environment.release(environment.context, fs->system_ranges,
		    fs->system_range_capacity * sizeof(*fs->system_ranges));
	}
	environment.release(environment.context, fs, sizeof(*fs));
}

void
ext4_get_info(const struct ext4_fs *fs, struct ext4_info *info)
{
	*info = fs->info;
}
