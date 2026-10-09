/* SPDX-License-Identifier: BSD-3-Clause */
#include "fscrypt.h"
#include "map_read.h"
#include "journal.h"
#include "keyring.h"

#include <inttypes.h>
#include <stdio.h>
#ifdef EXT4_TEST_OPENSSL
#include <openssl/evp.h>
#endif

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)
#define EXPECT(expression, wanted)                                                                 \
	do {                                                                                       \
		enum ext4_result actual = (expression);                                            \
		if (actual != (wanted)) {                                                          \
			fprintf(stderr, "%s:%d: %s returned %s, expected %s\n", __FILE__,           \
			    __LINE__, #expression, ext4_result_string(actual),                      \
			    ext4_result_string(wanted));                                           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

/* File-fork unit fixtures are authored by Python/OpenSSL, not by the core.
 * The synthetic filesystem deliberately bypasses mount/inode/xattr admission:
 * it exercises contents, descriptor, signature and tree I/O with real extents
 * and an already-derived cached key. Full-volume Linux acceptance is separate. */
struct fixture {
	/* Keep first: the reference keyring's callbacks use this same context. */
	struct keyring keyring;
	struct ext4_fs fs;
	struct ext4_inode inode;
	uint8_t *device;
	uint8_t *cipher;
	uint8_t *stored;
	uint8_t *plain;
	uint8_t digest[64];
	size_t digest_size;
	size_t device_size;
	size_t stored_size;
	uint64_t tree;
	uint64_t descriptor;
	uint32_t merkle;
	uint32_t algorithm;
	uint32_t version;
	uint32_t sparse;
	uint32_t signature_size;
	uint32_t live;
	uint32_t allocations;
	uint32_t reads;
	uint32_t ciphers;
	uint32_t fail_allocation;
	uint32_t fail_read;
	uint32_t fail_cipher;
};

static void *
allocate(void *context, size_t size)
{
	struct fixture *f = context;
	void *result;

	if (++f->allocations == f->fail_allocation) {
		return NULL;
	}
	result = malloc(size);
	CHECK(result != NULL);
	f->live++;
	return result;
}

static void
release(void *context, void *allocation, size_t size)
{
	struct fixture *f = context;

	(void)size;
	CHECK(f->live != 0);
	f->live--;
	free(allocation);
}

static enum ext4_result
read_device(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct fixture *f = context;

	CHECK(offset <= f->device_size && length <= f->device_size - offset);
	if (++f->reads == f->fail_read) {
		/* Exact-I/O callbacks can dirty their destination before reporting failure. */
		memcpy(buffer, f->device + offset, length / 2U);
		return EXT4_IO;
	}
	memcpy(buffer, f->device + offset, length);
	return EXT4_OK;
}

static enum ext4_result
cipher(void *context, void *handle, uint8_t mode, bool encrypt, const uint8_t *iv,
    const void *input, void *output, size_t length)
{
	struct fixture *f = context;
#ifdef EXT4_TEST_OPENSSL
	struct key *key = handle;
	EVP_CIPHER_CTX *cipher_context;
	int written = 0;
	int final = 0;
	int success;
#endif

	if (++f->ciphers == f->fail_cipher) {
		memset(output, 0xe1, length);
		return EXT4_IO;
	}
#ifdef EXT4_TEST_OPENSSL
	/* Keep small-block cases on the independent in-tree reference cipher.
	 * Large blocks otherwise multiply its deliberately simple inverse-S-box
	 * search by every small Merkle read. OpenSSL is optional and test-only. */
	if (mode == EXT4_FSCRYPT_MODE_AES_256_XTS && length > 4096U) {
		CHECK(key->size == 64U && length <= 65536U && input != output);
		cipher_context = EVP_CIPHER_CTX_new();
		CHECK(cipher_context != NULL);
		success = EVP_CipherInit_ex(cipher_context, EVP_aes_256_xts(), NULL,
		    key->bytes, iv, encrypt);
		if (success == 1) {
			success = EVP_CIPHER_CTX_set_padding(cipher_context, 0);
		}
		if (success == 1) {
			success = EVP_CipherUpdate(cipher_context, output, &written, input, (int)length);
		}
		if (success == 1) {
			success = EVP_CipherFinal_ex(cipher_context, (uint8_t *)output + written, &final);
		}
		CHECK(success != 1 || (size_t)(written + final) == length);
		EVP_CIPHER_CTX_free(cipher_context);
		return success == 1 ? EXT4_OK : EXT4_IO;
	}
#endif
	return keyring_cipher(context, handle, mode, encrypt, iv, input, output, length);
}

static enum ext4_result
signature(void *context, const uint8_t *message, size_t message_size,
    const uint8_t *bytes, size_t size)
{
	struct fixture *f = context;
	size_t index;

	if (size != f->signature_size || message_size != 12U + f->digest_size ||
	    memcmp(message, "FSVerity", 8) != 0 ||
	    memcmp(message + 12U, f->digest, f->digest_size) != 0) {
		return EXT4_PERMISSION_DENIED;
	}
	for (index = 0; index < size; index++) {
		if (bytes[index] != (uint8_t)(index * 11U + 5U)) {
			return EXT4_PERMISSION_DENIED;
		}
	}
	return EXT4_OK;
}

static uint8_t *
load(const char *stem, const char *suffix, size_t length)
{
	char path[1024];
	uint8_t *bytes = malloc(length == 0 ? 1U : length);
	FILE *stream;

	CHECK(bytes != NULL);
	CHECK(snprintf(path, sizeof(path), "%s.%s", stem, suffix) < (int)sizeof(path));
	stream = fopen(path, "rb");
	CHECK(stream != NULL);
	CHECK(fread(bytes, 1, length, stream) == length);
	CHECK(fgetc(stream) == EOF && fclose(stream) == 0);
	return bytes;
}

static void
add_extent(struct ext4_inode *inode, uint32_t logical, uint32_t count, bool unwritten)
{
	struct ext4_extent_header_disk *header = (void *)inode->block_data;
	uint16_t index = ext4_le16(&header->entries);
	struct ext4_extent_disk *extent = (void *)(inode->block_data + sizeof(*header) +
	    index * sizeof(*extent));

	CHECK(index < 4U && count > 0 && count < EXT4_EXTENT_UNWRITTEN_LIMIT);
	ext4_encode32(&extent->logical, logical);
	ext4_encode16(&extent->length,
	    (uint16_t)(count + (unwritten ? EXT4_EXTENT_UNWRITTEN_LIMIT : 0)));
	ext4_encode32(&extent->physical_lo, logical + 1U);
	ext4_encode16(&header->entries, (uint16_t)(index + 1U));
}

static void
mapping(struct fixture *f, uint32_t omitted, bool unwritten)
{
	struct ext4_extent_header_disk *header = (void *)f->inode.block_data;
	uint32_t count = (uint32_t)(f->stored_size / f->fs.info.block_size);

	memset(f->inode.block_data, 0, sizeof(f->inode.block_data));
	ext4_encode16(&header->magic, EXT4_EXTENT_MAGIC);
	ext4_encode16(&header->maximum, 4);
	if (omitted >= count) {
		add_extent(&f->inode, 0, count, false);
		return;
	}
	if (omitted != 0) {
		add_extent(&f->inode, 0, omitted, false);
	}
	if (unwritten) {
		add_extent(&f->inode, omitted, 1, true);
	}
	if (omitted + 1U < count) {
		add_extent(&f->inode, omitted + 1U, count - omitted - 1U, false);
	}
}

static void
install_key(struct fixture *f, bool wrong)
{
	struct ext4_crypto_environment crypto = keyring_environment(&f->keyring);
	struct ext4_fscrypt_policy policy = { 0 };
	struct ext4_fscrypt_key key;
	uint32_t index;

	crypto.cipher = cipher;
	crypto.verify_signature = signature;
	EXPECT(ext4_set_crypto(&f->fs, &crypto), EXT4_OK);
	policy.version = (uint8_t)f->version;
	policy.contents_mode = EXT4_FSCRYPT_MODE_AES_256_XTS;
	policy.filenames_mode = EXT4_FSCRYPT_MODE_AES_256_CTS;
	policy.identifier_size = f->version == 1 ? 8U : 16U;
	memcpy(policy.identifier,
	    f->version == 1 ? f->keyring.descriptor : f->keyring.identifier,
	    policy.identifier_size);
	for (index = 0; index < sizeof(policy.nonce); index++) {
		policy.nonce[index] = (uint8_t)index;
	}
	EXPECT(ext4_fscrypt_derive(&f->fs, &policy, EXT4_MODE_REGULAR, &key), EXT4_OK);
	if (wrong) {
		((struct key *)key.handle)->bytes[0] ^= 1;
	}
	f->fs.fscrypt_keys[0] = (struct ext4_fscrypt_cached_key){ f->inode.number,
		f->inode.generation, key.handle, key.mode, key.flags };
	f->fs.fscrypt_key_count = 1;
	f->fs.fscrypt_key_next = 1;
}

static void
reset_faults(struct fixture *f)
{
	f->reads = f->allocations = f->ciphers = 0;
	f->fail_read = f->fail_allocation = f->fail_cipher = 0;
	f->fs.verified_signature_count = f->fs.verified_signature_next = 0;
}

/* Compare the writer's ciphertext to the independently generated complete fork,
 * including metadata beyond EOF. Repeated partial changes intentionally reuse
 * the latest snapshot, as small Merkle blocks sharing one filesystem block do. */
static void
check_data_change(struct fixture *f)
{
	uint32_t block = f->fs.info.block_size;
	uint8_t *snapshot = malloc(block);
	uint8_t *plain = malloc(block);
	struct ext4_fscrypt_key key;
	size_t offset;
	size_t within;
	uint32_t logical;
	uint32_t failure;

	CHECK(snapshot != NULL && plain != NULL);
	reset_faults(f);
	for (offset = 0; offset < f->stored_size; offset += block) {
		logical = (uint32_t)(offset / block);
		memset(snapshot, 0xa5, block);
		EXPECT(ext4_data_change(&f->fs, &f->inode, logical, snapshot, true,
		    0, f->stored + offset, block), EXT4_OK);
		CHECK(memcmp(snapshot, f->cipher + offset, block) == 0);
		if (block > 1024U) {
			memset(snapshot, 0xa5, block);
			for (within = 0; within < block; within += 1024U) {
				EXPECT(ext4_data_change(&f->fs, &f->inode, logical, snapshot,
				    within == 0, within, f->stored + offset + within, 1024U),
				    EXT4_OK);
			}
			CHECK(memcmp(snapshot, f->cipher + offset, block) == 0);
		}
	}
	EXPECT(ext4_fscrypt_key(&f->fs, &f->inode, &key), EXT4_OK);
	memcpy(snapshot, f->cipher, block);
	memcpy(plain, f->stored, block);
	memset(plain + 13, 0, 19);
	EXPECT(ext4_data_change(&f->fs, &f->inode, 0, snapshot, false, 13, NULL, 19), EXT4_OK);
	/* The callback never permits input/output aliasing. */
	EXPECT(ext4_fscrypt_block(&f->fs, &key, 0, false, snapshot, plain), EXT4_OK);
	CHECK(memcmp(plain, f->stored, 13) == 0);
	for (within = 13; within < 32; within++) {
		CHECK(plain[within] == 0);
	}
	CHECK(memcmp(plain + 32, f->stored + 32, block - 32U) == 0);
	for (failure = 1; failure <= 2; failure++) {
		memcpy(snapshot, f->cipher, block);
		reset_faults(f);
		f->fail_cipher = failure;
		EXPECT(ext4_data_change(&f->fs, &f->inode, 0, snapshot, false,
		    13, f->stored + 13, 19), EXT4_IO);
		CHECK(f->live == 0);
		/* A failed encrypt may damage only the private snapshot; the owner
		 * must cancel it. A failed decrypt cannot touch that snapshot. */
		if (failure == 1) {
			CHECK(memcmp(snapshot, f->cipher, block) == 0);
		}
	}
	reset_faults(f);
	f->fail_allocation = 1;
	memcpy(snapshot, f->cipher, block);
	EXPECT(ext4_data_change(&f->fs, &f->inode, 0, snapshot, false,
	    13, f->stored + 13, 19), EXT4_NO_MEMORY);
	CHECK(memcmp(snapshot, f->cipher, block) == 0 && f->live == 0);
	reset_faults(f);
	free(plain);
	free(snapshot);
}

/* A checked synthetic held inode exercises the public preflight boundary. The
 * inert journal is never entered: no key and stale generation must return first.
 * This does not stand in for actual enable transactions on a mounted volume. */
static void
check_preflight(struct fixture *f)
{
	struct ext4_inode_disk *disk = (struct ext4_inode_disk *)f->device;
	struct ext4_inode_hold hold = { 0 };
	struct ext4_journal *journal = calloc(1, sizeof(*journal));
	struct ext4_inode result;
	struct ext4_verity_parameters parameters = { EXT4_VERITY_HASH_SHA256, 1024,
		NULL, 0, NULL, 0 };

	CHECK(journal != NULL);
	EXPECT(ext4_set_crypto(&f->fs, NULL), EXT4_OK);
	memset(disk, 0, EXT4_INODE_BASE_SIZE);
	ext4_encode16(&disk->mode, EXT4_MODE_REGULAR | 0600);
	ext4_encode16(&disk->links, 1);
	ext4_encode32(&disk->flags, EXT4_INODE_EXTENTS | EXT4_INODE_ENCRYPT);
	ext4_encode32(&disk->generation, f->inode.generation);
	hold.fs = &f->fs;
	hold.number = f->inode.number;
	hold.generation = f->inode.generation;
	hold.references = 1;
	hold.location_valid = true;
	f->fs.holds = &hold;
	f->fs.journal = journal;
	f->fs.inode_size = EXT4_INODE_BASE_SIZE;
	f->fs.info.inodes = 100;
	f->fs.first_inode = 11;
	journal->fs = &f->fs;
	reset_faults(f);
	EXPECT(ext4_enable_verity(&f->fs, hold.number, hold.generation, &parameters, &result),
	    EXT4_ENCRYPTED);
	CHECK(f->live == 0 && f->ciphers == 0 && journal->compound == NULL);
	hold.unlinked = true;
	ext4_encode16(&disk->links, 0);
	EXPECT(ext4_enable_verity(&f->fs, hold.number, hold.generation + 1U,
	    &parameters, &result), EXT4_STALE);
	/* Plain held-unlinked records must use the same live decoder. */
	ext4_encode32(&disk->flags, EXT4_INODE_EXTENTS);
	EXPECT(ext4_enable_verity(&f->fs, hold.number, hold.generation + 1U,
	    &parameters, &result), EXT4_STALE);
	CHECK(f->live == 0 && journal->compound == NULL);
	f->fs.holds = NULL;
	f->fs.journal = NULL;
	f->fs.inode_size = 0;
	f->fs.info.inodes = 0;
	f->fs.first_inode = 0;
	memset(disk, 0, EXT4_INODE_BASE_SIZE);
	free(journal);
	install_key(f, false);
	reset_faults(f);
}

static void
check_read(struct fixture *f)
{
	uint8_t *output = malloc((size_t)f->inode.size + 1U);
	uint8_t digest[64];
	uint32_t algorithm = 0;
	size_t completed;
	size_t size;
	size_t offset;
	size_t amount;

	CHECK(output != NULL);
	EXPECT(ext4_read(&f->fs, &f->inode, 0, output, (size_t)f->inode.size + 1U,
		   &completed), EXT4_OK);
	CHECK(completed == f->inode.size && memcmp(output, f->plain, completed) == 0);
	/* Unaligned short requests exercise filesystem/Merkle boundary combinations. */
	for (offset = 0; offset < f->inode.size; offset += f->merkle + 13U) {
		amount = f->inode.size - offset < 31U ? (size_t)f->inode.size - offset : 31U;
		EXPECT(ext4_read(&f->fs, &f->inode, offset, output, amount, &completed), EXT4_OK);
		CHECK(completed == amount && memcmp(output, f->plain + offset, amount) == 0);
	}
	EXPECT(ext4_read(&f->fs, &f->inode, f->inode.size, output, 1, &completed), EXT4_OK);
	CHECK(completed == 0);
	EXPECT(ext4_read(&f->fs, &f->inode, f->tree, output, 1, &completed), EXT4_OK);
	CHECK(completed == 0);
	EXPECT(ext4_measure_verity(&f->fs, &f->inode, &algorithm, digest, sizeof(digest),
		   &size), EXT4_OK);
	CHECK(algorithm == f->algorithm && size == f->digest_size &&
	    memcmp(digest, f->digest, size) == 0);
	EXPECT(ext4_measure_verity(&f->fs, &f->inode, &algorithm, digest, 1, &size), EXT4_RANGE);
	CHECK(size == f->digest_size);
	CHECK(f->live == 0);
	free(output);
}

static void
fault_reads(struct fixture *f)
{
	uint8_t *output;
	uint32_t counts[3];
	uint32_t kind;
	uint32_t fault;
	size_t completed;
	size_t index;
	size_t amount = 2U * f->fs.info.block_size + 17U;

	if (amount > f->inode.size) {
		amount = (size_t)f->inode.size;
	}
	if (amount == 0) {
		return;
	}
	output = malloc(amount);
	CHECK(output != NULL);
	reset_faults(f);
	EXPECT(ext4_read(&f->fs, &f->inode, 0, output, amount, &completed), EXT4_OK);
	counts[0] = f->allocations;
	counts[1] = f->reads;
	counts[2] = f->ciphers;
	for (kind = 0; kind < 3U; kind++) {
		for (fault = 1; fault <= counts[kind]; fault++) {
			reset_faults(f);
			f->fail_allocation = kind == 0 ? fault : 0;
			f->fail_read = kind == 1 ? fault : 0;
			f->fail_cipher = kind == 2 ? fault : 0;
			memset(output, 0x91, amount);
			EXPECT(ext4_read(&f->fs, &f->inode, 0, output, amount, &completed),
			    kind == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(completed < amount && completed % f->merkle == 0 && f->live == 0);
			CHECK(memcmp(output, f->plain, completed) == 0);
			for (index = completed; index < amount; index++) {
				CHECK(output[index] == 0x91);
			}
		}
	}
	reset_faults(f);
	free(output);
}

static void
check_damage(struct fixture *f)
{
	uint64_t positions[5] = { 0, f->tree, f->descriptor, f->stored_size - 4U,
		f->descriptor + 256U };
	uint8_t output[31];
	uint32_t index;
	size_t completed;
	uint8_t *whole;
	size_t position;
	enum ext4_result error;

	if (f->inode.size == 0) {
		return;
	}
	for (index = 0; index < (f->signature_size == 0 ? 4U : 5U); index++) {
		reset_faults(f);
		f->device[f->fs.info.block_size + positions[index]] ^= 0x40;
		error = ext4_read(&f->fs, &f->inode, 0, output, 1, &completed);
		CHECK(error != EXT4_OK && completed == 0 && f->live == 0);
		f->device[f->fs.info.block_size + positions[index]] ^= 0x40;
	}
	/* A later failed data block must leave only the verified prefix published. */
	if (f->inode.size > f->fs.info.block_size) {
		whole = malloc((size_t)f->inode.size);
		CHECK(whole != NULL);
		memset(whole, 0x91, (size_t)f->inode.size);
		f->device[2U * f->fs.info.block_size] ^= 0x40;
		EXPECT(ext4_read(&f->fs, &f->inode, 0, whole, (size_t)f->inode.size,
			   &completed), EXT4_CORRUPT);
		CHECK(completed == f->fs.info.block_size);
		CHECK(memcmp(whole, f->plain, completed) == 0);
		for (position = completed; position < f->inode.size; position++) {
			CHECK(whole[position] == 0x91);
		}
		f->device[2U * f->fs.info.block_size] ^= 0x40;
		free(whole);
	}
	reset_faults(f);
}

static void
fault_measure(struct fixture *f)
{
	uint8_t digest[64];
	uint32_t counts[3];
	uint32_t algorithm;
	uint32_t kind;
	uint32_t fault;
	size_t size;

	reset_faults(f);
	EXPECT(ext4_measure_verity(&f->fs, &f->inode, &algorithm, digest, sizeof(digest),
		   &size), EXT4_OK);
	counts[0] = f->allocations;
	counts[1] = f->reads;
	counts[2] = f->ciphers;
	for (kind = 0; kind < 3U; kind++) {
		for (fault = 1; fault <= counts[kind]; fault++) {
			reset_faults(f);
			f->fail_allocation = kind == 0 ? fault : 0;
			f->fail_read = kind == 1 ? fault : 0;
			f->fail_cipher = kind == 2 ? fault : 0;
			EXPECT(ext4_measure_verity(&f->fs, &f->inode, &algorithm, digest,
				   sizeof(digest), &size), kind == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(f->live == 0);
		}
	}
	reset_faults(f);
}

static void
check_sparse(struct fixture *f)
{
	struct ext4_map_reader reader = { 0 };
	uint8_t output[31];
	uint32_t mode;
	uint32_t missing;
	uint32_t metadata;
	size_t completed;

	if (!f->sparse) {
		return;
	}
	for (mode = 0; mode < 2U; mode++) {
		mapping(f, 1, mode != 0);
		check_read(f);
		EXPECT(ext4_read_plaintext(&f->fs, &f->inode, &reader, f->fs.info.block_size,
			   output, sizeof(output), true, &completed), EXT4_CORRUPT);
		CHECK(completed == 0);
		ext4_map_reader_close(&f->fs, &reader);
		memset(&reader, 0, sizeof(reader));
		for (metadata = 0; metadata < 2U; metadata++) {
			missing = (uint32_t)((metadata == 0 ? f->tree : f->descriptor) /
			    f->fs.info.block_size);
			mapping(f, missing, mode != 0);
			EXPECT(ext4_read_plaintext(&f->fs, &f->inode, &reader,
				   (uint64_t)missing * f->fs.info.block_size, output,
				   sizeof(output), true, &completed), EXT4_CORRUPT);
			CHECK(completed == 0);
			ext4_map_reader_close(&f->fs, &reader);
			memset(&reader, 0, sizeof(reader));
			EXPECT(ext4_read(&f->fs, &f->inode, 0, output, 1, &completed), EXT4_CORRUPT);
			CHECK(completed == 0);
		}
	}
	mapping(f, UINT32_MAX, false);
	EXPECT(ext4_read_plaintext(&f->fs, &f->inode, &reader, UINT64_MAX - 1U,
		   output, sizeof(output), false, &completed), EXT4_RANGE);
	CHECK(completed == 0);
	ext4_map_reader_close(&f->fs, &reader);
}

static void
run(const char *stem)
{
	struct fixture *f = calloc(1, sizeof(*f));
	char path[1024];
	char hex[129];
	FILE *stream;
	uint32_t block;
	uint32_t index;
	unsigned int byte;
	uint8_t output[64];
	size_t completed;
	size_t size;
	enum ext4_result error;

	CHECK(f != NULL);
	CHECK(snprintf(path, sizeof(path), "%s.meta", stem) < (int)sizeof(path));
	stream = fopen(path, "r");
	CHECK(stream != NULL);
	CHECK(fscanf(stream, "%" SCNu32 " %" SCNu32 " %" SCNu64 " %zu %" SCNu64
			     " %" SCNu64 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32,
		  &block, &f->merkle, &f->inode.size, &f->stored_size, &f->tree,
		  &f->descriptor, &f->algorithm, &f->version, &f->sparse, &f->signature_size) == 10);
	CHECK(fscanf(stream, "%128s", hex) == 1 && fclose(stream) == 0);
	f->digest_size = f->algorithm == 1 ? 32U : 64U;
	CHECK(strlen(hex) == 2U * f->digest_size);
	for (index = 0; index < f->digest_size; index++) {
		CHECK(sscanf(hex + 2U * index, "%2x", &byte) == 1);
		f->digest[index] = (uint8_t)byte;
	}
	f->plain = load(stem, "plain", (size_t)f->inode.size);
	f->stored = load(stem, "stored", f->stored_size);
	f->cipher = load(stem, "cipher", f->stored_size);
	f->device_size = f->stored_size + block;
	f->device = calloc(1, f->device_size);
	CHECK(f->device != NULL);
	memcpy(f->device + block, f->cipher, f->stored_size);
	f->fs.environment = (struct ext4_environment){ f, f->device_size,
		read_device, allocate, release };
	f->fs.info.block_size = block;
	f->fs.info.blocks = f->device_size / block;
	f->fs.info.feature_ro_compat = EXT4_FEATURE_RO_VERITY;
	f->fs.cluster_blocks = 1;
	f->inode.number = 17;
	f->inode.generation = 123;
	f->inode.mode = EXT4_MODE_REGULAR;
	f->inode.flags = EXT4_INODE_EXTENTS | EXT4_INODE_VERITY | EXT4_INODE_ENCRYPT;
	mapping(f, UINT32_MAX, false);
	keyring_init(&f->keyring, 3);
	install_key(f, false);
	check_data_change(f);
	check_preflight(f);
	check_read(f);
	fault_reads(f);
	fault_measure(f);
	check_damage(f);
	check_sparse(f);
	install_key(f, true);
	error = ext4_measure_verity(&f->fs, &f->inode, &index, output, sizeof(output), &size);
	CHECK(error != EXT4_OK && f->live == 0);
	if (f->inode.size != 0) {
		error = ext4_read(&f->fs, &f->inode, 0, output, 1, &completed);
		CHECK(error != EXT4_OK && completed == 0);
	}
	EXPECT(ext4_set_crypto(&f->fs, NULL), EXT4_OK);
	EXPECT(ext4_read(&f->fs, &f->inode, 0, output, 1, &completed), EXT4_ENCRYPTED);
	CHECK(completed == 0);
	EXPECT(ext4_measure_verity(&f->fs, &f->inode, &index, output, sizeof(output), &size),
	    EXT4_ENCRYPTED);
	install_key(f, false);
	check_read(f);
	/* Identical independently built metadata also verifies without encryption. */
	f->inode.flags &= ~(uint32_t)EXT4_INODE_ENCRYPT;
	memcpy(f->device + block, f->stored, f->stored_size);
	reset_faults(f);
	check_read(f);
	EXPECT(ext4_set_crypto(&f->fs, NULL), EXT4_OK);
	CHECK(f->keyring.handles == 0 && f->live == 0);
	free(f->device);
	free(f->cipher);
	free(f->stored);
	free(f->plain);
	free(f);
	printf("PASS encrypted verity %s\n", stem);
}

int
main(int argc, char **argv)
{
	int index;

	CHECK(argc > 1);
	CHECK(setvbuf(stdout, NULL, _IOLBF, 0) == 0);
	for (index = 1; index < argc; index++) {
		run(argv[index]);
	}
	return 0;
}
