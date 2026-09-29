/* SPDX-License-Identifier: BSD-3-Clause */
#include "verity.h"
#include "sha.h"

/* See the fs-verity documentation in the Linux kernel sources. Every Merkle
 * block is hashed after the salt, which is zero-padded to the hash input block;
 * the final data block is zero-padded to the Merkle block size. */

union ext4_verity_hash_context {
	struct ext4_sha256 sha256;
	struct ext4_sha512 sha512;
};

void
ext4_verity_file_digest(const struct ext4_verity *verity,
    struct ext4_verity_descriptor_disk *descriptor, uint8_t *digest)
{
	union ext4_verity_hash_context context;

	/* The file digest covers the descriptor without its signature, unsalted. */
	ext4_encode32(&descriptor->signature_size, 0);
	if (verity->algorithm == EXT4_VERITY_SHA256) {
		ext4_sha256_init(&context.sha256);
		ext4_sha256_update(&context.sha256, descriptor, sizeof(*descriptor));
		ext4_sha256_final(&context.sha256, digest);
	} else {
		ext4_sha512_init(&context.sha512);
		ext4_sha512_update(&context.sha512, descriptor, sizeof(*descriptor));
		ext4_sha512_final(&context.sha512, digest);
	}
}

void
ext4_verity_hash(const struct ext4_verity *verity, const uint8_t *block, uint8_t *digest)
{
	union ext4_verity_hash_context context;

	if (verity->algorithm == EXT4_VERITY_SHA256) {
		ext4_sha256_init(&context.sha256);
		ext4_sha256_update(&context.sha256, verity->padded_salt, verity->padded_salt_size);
		ext4_sha256_update(&context.sha256, block, verity->block_size);
		ext4_sha256_final(&context.sha256, digest);
	} else {
		ext4_sha512_init(&context.sha512);
		ext4_sha512_update(&context.sha512, verity->padded_salt, verity->padded_salt_size);
		ext4_sha512_update(&context.sha512, block, verity->block_size);
		ext4_sha512_final(&context.sha512, digest);
	}
}

static bool
ext4_verity_zero(const uint8_t *bytes, size_t length)
{
	size_t index;

	for (index = 0; index < length; index++) {
		if (bytes[index] != 0) {
			return false;
		}
	}
	return true;
}

static enum ext4_result
ext4_verity_read_exact(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset,
    void *buffer, size_t length)
{
	size_t completed;

	return ext4_read_mapped(fs, inode, offset, buffer, length, true, &completed);
}

enum ext4_result
ext4_verity_configure(struct ext4_verity *verity, uint8_t algorithm, uint8_t log_block_size,
    const uint8_t *salt, uint8_t salt_size)
{
	uint32_t hash_block;

	if ((algorithm != EXT4_VERITY_SHA256 && algorithm != EXT4_VERITY_SHA512) ||
	    log_block_size < EXT4_VERITY_MIN_LOG_BLOCK ||
	    log_block_size > EXT4_VERITY_MAX_LOG_BLOCK) {
		return EXT4_UNSUPPORTED;
	}
	if (salt_size > EXT4_VERITY_MAX_SALT) {
		return EXT4_CORRUPT;
	}
	verity->algorithm = algorithm;
	verity->block_size = 1U << log_block_size;
	verity->digest_size =
	    algorithm == EXT4_VERITY_SHA256 ? EXT4_SHA256_DIGEST_SIZE : EXT4_SHA512_DIGEST_SIZE;
	hash_block =
	    algorithm == EXT4_VERITY_SHA256 ? EXT4_SHA256_BLOCK_SIZE : EXT4_SHA512_BLOCK_SIZE;
	verity->hashes_per_block = verity->block_size / verity->digest_size;
	verity->padded_salt_size = 0;
	ext4_zero(verity->padded_salt, sizeof(verity->padded_salt));
	if (salt_size != 0) {
		verity->padded_salt_size = (salt_size + hash_block - 1U) / hash_block * hash_block;
		ext4_copy(verity->padded_salt, salt, salt_size);
	}
	return EXT4_OK;
}

enum ext4_result
ext4_verity_geometry(struct ext4_verity *verity, uint64_t *tree_blocks)
{
	uint64_t level_blocks[EXT4_VERITY_MAX_LEVELS];
	uint64_t blocks =
	    verity->data_size / verity->block_size + (verity->data_size % verity->block_size != 0);
	uint64_t offset = 0;
	unsigned int level;

	verity->levels = 0;
	while (blocks > 1U) {
		if (verity->levels == EXT4_VERITY_MAX_LEVELS) {
			return EXT4_UNSUPPORTED;
		}
		blocks = (blocks + verity->hashes_per_block - 1U) / verity->hashes_per_block;
		level_blocks[verity->levels++] = blocks;
	}
	for (level = verity->levels; level > 0; level--) {
		verity->level_start[level - 1U] = offset;
		offset += level_blocks[level - 1U];
	}
	*tree_blocks = offset;
	return EXT4_OK;
}

enum ext4_result
ext4_verity_open(struct ext4_fs *fs, const struct ext4_inode *inode, struct ext4_verity *verity)
{
	struct ext4_verity_descriptor_disk descriptor;
	struct ext4_le32 size_disk;
	uint64_t end;
	uint64_t metadata;
	uint64_t size_position;
	uint64_t descriptor_position;
	uint64_t tree_blocks;
	uint32_t descriptor_size;
	enum ext4_result error;

	ext4_zero(verity, sizeof(*verity));
	if (!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_VERITY) ||
	    (inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR ||
	    (inode->flags & (EXT4_INODE_EXTENTS | EXT4_INODE_INLINE_DATA)) != EXT4_INODE_EXTENTS) {
		return EXT4_CORRUPT;
	}
	if (inode->size > UINT64_MAX - EXT4_VERITY_METADATA_ALIGNMENT) {
		return EXT4_CORRUPT;
	}
	metadata = (inode->size + EXT4_VERITY_METADATA_ALIGNMENT - 1U) &
	    ~(uint64_t)(EXT4_VERITY_METADATA_ALIGNMENT - 1U);
	error = ext4_extent_last_end(fs, inode, &end);
	if (error != EXT4_OK) {
		return error;
	}
	size_position = end * fs->info.block_size;
	if (size_position < metadata + sizeof(descriptor) + sizeof(size_disk)) {
		return EXT4_CORRUPT;
	}
	size_position -= sizeof(size_disk);
	error = ext4_verity_read_exact(fs, inode, size_position, &size_disk, sizeof(size_disk));
	if (error != EXT4_OK) {
		return error;
	}
	descriptor_size = ext4_le32(&size_disk);
	if (descriptor_size < sizeof(descriptor) || descriptor_size > size_position - metadata) {
		return EXT4_CORRUPT;
	}
	descriptor_position =
	    (size_position - descriptor_size) & ~(uint64_t)(fs->info.block_size - 1U);
	if (descriptor_position < metadata) {
		return EXT4_CORRUPT;
	}
	error =
	    ext4_verity_read_exact(fs, inode, descriptor_position, &descriptor, sizeof(descriptor));
	if (error != EXT4_OK) {
		return error;
	}
	verity->data_size = (uint64_t)ext4_le32(&descriptor.data_size_lo) |
	    (uint64_t)ext4_le32(&descriptor.data_size_hi) << 32;
	if (descriptor.version != EXT4_VERITY_VERSION ||
	    !ext4_verity_zero(descriptor.reserved, sizeof(descriptor.reserved)) ||
	    ext4_le32(&descriptor.signature_size) > descriptor_size - sizeof(descriptor) ||
	    verity->data_size != inode->size || descriptor.salt_size > EXT4_VERITY_MAX_SALT) {
		return EXT4_CORRUPT;
	}
	error = ext4_verity_configure(verity, descriptor.hash_algorithm, descriptor.log_block_size,
	    descriptor.salt, descriptor.salt_size);
	if (error != EXT4_OK) {
		return error;
	}
	ext4_copy(verity->root_hash, descriptor.root_hash, verity->digest_size);
	error = ext4_verity_geometry(verity, &tree_blocks);
	if (error != EXT4_OK) {
		return error;
	}
	/* The whole tree lies between the metadata start and the descriptor. */
	if (tree_blocks > (descriptor_position - metadata) / verity->block_size) {
		return EXT4_CORRUPT;
	}
	verity->tree_offset = metadata;
	verity->descriptor_offset = descriptor_position;
	return EXT4_OK;
}

/* Verify one zero-padded Merkle data block. cached holds the level-zero hash
 * block last verified through the root, identified by cached_index. */
static enum ext4_result
ext4_verity_verify(struct ext4_fs *fs, const struct ext4_inode *inode,
    const struct ext4_verity *verity, uint64_t index, const uint8_t *data, uint8_t *hashes,
    uint8_t *cached, uint64_t *cached_index)
{
	uint8_t digest[EXT4_VERITY_MAX_DIGEST];
	uint64_t hash_index;
	uint32_t within;
	unsigned int level;
	enum ext4_result error;

	ext4_verity_hash(verity, data, digest);
	if (verity->levels == 0) {
		return ext4_equal(digest, verity->root_hash, verity->digest_size) ? EXT4_OK
										  : EXT4_CORRUPT;
	}
	hash_index = index / verity->hashes_per_block;
	within = (uint32_t)(index % verity->hashes_per_block) * verity->digest_size;
	if (*cached_index == hash_index) {
		return ext4_equal(cached + within, digest, verity->digest_size) ? EXT4_OK
										: EXT4_CORRUPT;
	}
	/* The cache is valid again only after this path reaches the root. */
	*cached_index = UINT64_MAX;
	for (level = 0; level < verity->levels; level++) {
		error = ext4_verity_read_exact(fs, inode,
		    verity->tree_offset +
			(verity->level_start[level] + hash_index) * verity->block_size,
		    hashes, verity->block_size);
		if (error != EXT4_OK) {
			return error;
		}
		if (!ext4_equal(hashes + within, digest, verity->digest_size)) {
			return EXT4_CORRUPT;
		}
		if (level == 0) {
			ext4_copy(cached, hashes, verity->block_size);
		}
		ext4_verity_hash(verity, hashes, digest);
		within = (uint32_t)(hash_index % verity->hashes_per_block) * verity->digest_size;
		hash_index /= verity->hashes_per_block;
	}
	if (!ext4_equal(digest, verity->root_hash, verity->digest_size)) {
		return EXT4_CORRUPT;
	}
	*cached_index = index / verity->hashes_per_block;
	return EXT4_OK;
}

enum ext4_result
ext4_verity_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, void *buffer,
    size_t length, size_t *completed)
{
	struct ext4_verity verity;
	uint8_t *output = buffer;
	uint8_t *data = NULL;
	uint8_t *hashes = NULL;
	uint8_t *cached = NULL;
	uint64_t cached_index = UINT64_MAX;
	uint64_t index;
	uint64_t start;
	size_t valid;
	size_t within;
	size_t chunk;
	size_t read;
	enum ext4_result error;

	*completed = 0;
	if (offset >= inode->size || length == 0) {
		return EXT4_OK;
	}
	if (length > inode->size - offset) {
		length = (size_t)(inode->size - offset);
	}
	error = ext4_verity_open(fs, inode, &verity);
	if (error != EXT4_OK) {
		return error;
	}
	data = fs->environment.allocate(fs->environment.context, verity.block_size);
	hashes = fs->environment.allocate(fs->environment.context, verity.block_size);
	cached = fs->environment.allocate(fs->environment.context, verity.block_size);
	if (data == NULL || hashes == NULL || cached == NULL) {
		error = EXT4_NO_MEMORY;
		goto out;
	}
	while (*completed < length) {
		index = (offset + *completed) / verity.block_size;
		start = index * verity.block_size;
		within = (size_t)(offset + *completed - start);
		valid = inode->size - start < verity.block_size ? (size_t)(inode->size - start)
								: verity.block_size;
		error = ext4_read_mapped(fs, inode, start, data, valid, false, &read);
		if (error != EXT4_OK) {
			goto out;
		}
		ext4_zero(data + valid, verity.block_size - valid);
		error = ext4_verity_verify(
		    fs, inode, &verity, index, data, hashes, cached, &cached_index);
		if (error != EXT4_OK) {
			goto out;
		}
		chunk = valid - within;
		if (chunk > length - *completed) {
			chunk = length - *completed;
		}
		ext4_copy(output + *completed, data + within, chunk);
		*completed += chunk;
	}
out:
	if (data != NULL) {
		fs->environment.release(fs->environment.context, data, verity.block_size);
	}
	if (hashes != NULL) {
		fs->environment.release(fs->environment.context, hashes, verity.block_size);
	}
	if (cached != NULL) {
		fs->environment.release(fs->environment.context, cached, verity.block_size);
	}
	return error;
}

enum ext4_result
ext4_measure_verity(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t *hash_algorithm,
    uint8_t *digest, size_t capacity, size_t *size)
{
	struct ext4_verity verity;
	struct ext4_verity_descriptor_disk descriptor;
	enum ext4_result error;

	if (fs == NULL || inode == NULL || hash_algorithm == NULL || size == NULL ||
	    (digest == NULL && capacity != 0)) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (!(inode->flags & EXT4_INODE_VERITY)) {
		return EXT4_NOT_FOUND;
	}
	/* The descriptor of an encrypted file is ciphertext. */
	if (inode->flags & EXT4_INODE_ENCRYPT) {
		return EXT4_ENCRYPTED;
	}
	error = ext4_verity_open(fs, inode, &verity);
	if (error == EXT4_OK) {
		error = ext4_verity_read_exact(
		    fs, inode, verity.descriptor_offset, &descriptor, sizeof(descriptor));
	}
	if (error != EXT4_OK) {
		return error;
	}
	*hash_algorithm = verity.algorithm;
	*size = verity.digest_size;
	if (capacity < verity.digest_size) {
		return EXT4_RANGE;
	}
	ext4_verity_file_digest(&verity, &descriptor, digest);
	return EXT4_OK;
}
