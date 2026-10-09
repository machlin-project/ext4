/* SPDX-License-Identifier: BSD-3-Clause */
#include "verity.h"
#include "fscrypt.h"
#include "map_read.h"
#include "sha.h"

/* See the fs-verity documentation in the Linux kernel sources. Every Merkle
 * block is hashed after the salt, which is zero-padded to the hash input block;
 * the final data block is zero-padded to the Merkle block size. */

/* Data and Merkle reads alternate between distant offsets. Keep their mapping
 * cursors separate, with buffers owned only for this verified read operation. */
struct ext4_verity_reader {
	struct ext4_map_reader data_map;
	struct ext4_map_reader tree_map;
	uint8_t *data;
	uint8_t *hashes;
	uint8_t *cached;
	uint64_t cached_index;
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
		context.sha256 = verity->hash_context.sha256;
		ext4_sha256_update(&context.sha256, block, verity->block_size);
		ext4_sha256_final(&context.sha256, digest);
	} else {
		context.sha512 = verity->hash_context.sha512;
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
	struct ext4_map_reader reader = { 0 };
	size_t completed;
	enum ext4_result error;

	error = ext4_read_plaintext(fs, inode, &reader, offset, buffer, length, true, &completed);
	ext4_map_reader_close(fs, &reader);
	return error == EXT4_OK && completed != length ? EXT4_CORRUPT : error;
}

enum ext4_result
ext4_verity_configure(struct ext4_verity *verity, uint8_t algorithm, uint8_t log_block_size,
    const uint8_t *salt, uint8_t salt_size)
{
	uint8_t padded_salt[EXT4_VERITY_MAX_PADDED_SALT];
	uint32_t hash_block;
	size_t salt_length;

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
	salt_length = salt_size == 0 ? 0 : hash_block;
	ext4_zero(padded_salt, sizeof(padded_salt));
	ext4_copy(padded_salt, salt, salt_size);
	ext4_zero(&verity->hash_context, sizeof(verity->hash_context));
	if (algorithm == EXT4_VERITY_SHA256) {
		ext4_sha256_init(&verity->hash_context.sha256);
		ext4_sha256_update(&verity->hash_context.sha256, padded_salt, salt_length);
	} else {
		ext4_sha512_init(&verity->hash_context.sha512);
		ext4_sha512_update(&verity->hash_context.sha512, padded_salt, salt_length);
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
	if (descriptor_size < sizeof(descriptor) || descriptor_size > EXT4_VERITY_MAX_DESCRIPTOR ||
	    descriptor_size > size_position - metadata) {
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
	return ext4_verity_accept(fs, inode, verity, &descriptor, NULL);
}

enum ext4_result
ext4_verity_accept(struct ext4_fs *fs, const struct ext4_inode *inode,
    const struct ext4_verity *verity, const struct ext4_verity_descriptor_disk *descriptor,
    const uint8_t *signature)
{
	struct ext4_verity_descriptor_disk unsigned_descriptor;
	struct ext4_verified_signature *entry;
	uint8_t message[EXT4_VERITY_FORMATTED_HEADER + EXT4_VERITY_MAX_DIGEST];
	uint8_t *stored = NULL;
	uint32_t size = ext4_le32(&descriptor->signature_size);
	uint32_t index;
	enum ext4_result error;

	if (size == 0) {
		return fs->crypto.require_signatures ? EXT4_PERMISSION_DENIED : EXT4_OK;
	}
	if (fs->crypto.verify_signature == NULL) {
		return EXT4_OK;
	}
	/* A signature authenticates the file digest, which binds the root hash; once
	 * accepted, the same digest needs no second verification. */
	unsigned_descriptor = *descriptor;
	ext4_copy(message, EXT4_VERITY_FORMATTED_MAGIC, EXT4_VERITY_FORMATTED_MAGIC_SIZE);
	ext4_encode16(
	    (struct ext4_le16 *)(message + EXT4_VERITY_FORMATTED_MAGIC_SIZE), verity->algorithm);
	ext4_encode16((struct ext4_le16 *)(message + EXT4_VERITY_FORMATTED_MAGIC_SIZE +
			  sizeof(struct ext4_le16)),
	    (uint16_t)verity->digest_size);
	ext4_verity_file_digest(
	    verity, &unsigned_descriptor, message + EXT4_VERITY_FORMATTED_HEADER);
	for (index = 0; index < fs->verified_signature_count; index++) {
		entry = &fs->verified_signatures[index];
		if (entry->number == inode->number && entry->generation == inode->generation &&
		    entry->algorithm == verity->algorithm &&
		    ext4_equal(entry->digest, message + EXT4_VERITY_FORMATTED_HEADER,
			verity->digest_size)) {
			return EXT4_OK;
		}
	}
	if (signature == NULL) {
		stored = fs->environment.allocate(fs->environment.context, size);
		if (stored == NULL) {
			return EXT4_NO_MEMORY;
		}
		error = ext4_verity_read_exact(
		    fs, inode, verity->descriptor_offset + sizeof(*descriptor), stored, size);
		signature = stored;
	} else {
		error = EXT4_OK;
	}
	if (error == EXT4_OK) {
		error = fs->crypto.verify_signature(fs->crypto.context, message,
		    EXT4_VERITY_FORMATTED_HEADER + verity->digest_size, signature, size);
	}
	if (stored != NULL) {
		fs->environment.release(fs->environment.context, stored, size);
	}
	if (error != EXT4_OK) {
		return error;
	}
	entry = &fs->verified_signatures[fs->verified_signature_next];
	fs->verified_signature_next = (fs->verified_signature_next + 1U) % EXT4_VERIFIED_SIGNATURES;
	if (fs->verified_signature_count < EXT4_VERIFIED_SIGNATURES) {
		fs->verified_signature_count++;
	}
	entry->number = inode->number;
	entry->generation = inode->generation;
	entry->algorithm = verity->algorithm;
	ext4_zero(entry->digest, sizeof(entry->digest));
	ext4_copy(entry->digest, message + EXT4_VERITY_FORMATTED_HEADER, verity->digest_size);
	return EXT4_OK;
}

enum ext4_result
ext4_set_crypto(struct ext4_fs *fs, const struct ext4_crypto_environment *crypto)
{
	bool fscrypt;

	if (fs == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (crypto != NULL) {
		fscrypt = crypto->find_key != NULL;
		if ((crypto->require_signatures && crypto->verify_signature == NULL) ||
		    (crypto->derive_key != NULL) != fscrypt ||
		    (crypto->cipher != NULL) != fscrypt ||
		    (crypto->release_key != NULL) != fscrypt ||
		    (crypto->random_bytes != NULL) != fscrypt) {
			return EXT4_INVALID_ARGUMENT;
		}
	}
	/* Keys derived under the previous environment belong to it. */
	ext4_fscrypt_forget(fs);
	ext4_zero(&fs->crypto, sizeof(fs->crypto));
	if (crypto != NULL) {
		fs->crypto = *crypto;
	}
	/* Other trusted certificates may accept or refuse other files. */
	fs->verified_signature_count = 0;
	fs->verified_signature_next = 0;
	return EXT4_OK;
}

/* Verify one zero-padded Merkle data block. The reader caches only a level-zero
 * hash block whose path has already been verified through the root. */
static enum ext4_result
ext4_verity_verify(struct ext4_fs *fs, const struct ext4_inode *inode,
    const struct ext4_verity *verity, struct ext4_verity_reader *reader, uint64_t index)
{
	uint8_t digest[EXT4_VERITY_MAX_DIGEST];
	uint64_t hash_index;
	uint32_t within;
	unsigned int level;
	size_t completed;
	enum ext4_result error;

	ext4_verity_hash(verity, reader->data, digest);
	if (verity->levels == 0) {
		return ext4_equal(digest, verity->root_hash, verity->digest_size) ? EXT4_OK
										  : EXT4_CORRUPT;
	}
	hash_index = index / verity->hashes_per_block;
	within = (uint32_t)(index % verity->hashes_per_block) * verity->digest_size;
	if (reader->cached_index == hash_index) {
		return ext4_equal(reader->cached + within, digest, verity->digest_size)
		    ? EXT4_OK
		    : EXT4_CORRUPT;
	}
	/* The cache is valid again only after this path reaches the root. */
	reader->cached_index = UINT64_MAX;
	for (level = 0; level < verity->levels; level++) {
		error = ext4_read_plaintext(fs, inode, &reader->tree_map,
		    verity->tree_offset +
			(verity->level_start[level] + hash_index) * verity->block_size,
		    reader->hashes, verity->block_size, true, &completed);
		if (error != EXT4_OK) {
			return error;
		}
		if (!ext4_equal(reader->hashes + within, digest, verity->digest_size)) {
			return EXT4_CORRUPT;
		}
		if (level == 0) {
			ext4_copy(reader->cached, reader->hashes, verity->block_size);
		}
		ext4_verity_hash(verity, reader->hashes, digest);
		within = (uint32_t)(hash_index % verity->hashes_per_block) * verity->digest_size;
		hash_index /= verity->hashes_per_block;
	}
	if (!ext4_equal(digest, verity->root_hash, verity->digest_size)) {
		return EXT4_CORRUPT;
	}
	reader->cached_index = index / verity->hashes_per_block;
	return EXT4_OK;
}

enum ext4_result
ext4_verity_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, void *buffer,
    size_t length, size_t *completed)
{
	struct ext4_verity verity;
	struct ext4_verity_reader reader = { .cached_index = UINT64_MAX };
	uint8_t *output = buffer;
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
	reader.data = fs->environment.allocate(fs->environment.context, verity.block_size);
	reader.hashes = fs->environment.allocate(fs->environment.context, verity.block_size);
	reader.cached = fs->environment.allocate(fs->environment.context, verity.block_size);
	if (reader.data == NULL || reader.hashes == NULL || reader.cached == NULL) {
		error = EXT4_NO_MEMORY;
		goto out;
	}
	while (*completed < length) {
		index = (offset + *completed) / verity.block_size;
		start = index * verity.block_size;
		within = (size_t)(offset + *completed - start);
		valid = inode->size - start < verity.block_size ? (size_t)(inode->size - start)
								: verity.block_size;
		error = ext4_read_plaintext(
		    fs, inode, &reader.data_map, start, reader.data, valid, false, &read);
		if (error != EXT4_OK) {
			goto out;
		}
		ext4_zero(reader.data + valid, verity.block_size - valid);
		error = ext4_verity_verify(fs, inode, &verity, &reader, index);
		if (error != EXT4_OK) {
			goto out;
		}
		chunk = valid - within;
		if (chunk > length - *completed) {
			chunk = length - *completed;
		}
		ext4_copy(output + *completed, reader.data + within, chunk);
		*completed += chunk;
	}
out:
	ext4_map_reader_close(fs, &reader.data_map);
	ext4_map_reader_close(fs, &reader.tree_map);
	if (reader.data != NULL) {
		fs->environment.release(fs->environment.context, reader.data, verity.block_size);
	}
	if (reader.hashes != NULL) {
		fs->environment.release(fs->environment.context, reader.hashes, verity.block_size);
	}
	if (reader.cached != NULL) {
		fs->environment.release(fs->environment.context, reader.cached, verity.block_size);
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
