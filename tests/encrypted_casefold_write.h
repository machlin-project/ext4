/* SPDX-License-Identifier: BSD-3-Clause */
/* Transaction-owned directory mutation around independent ciphertext/hash bytes.
 * This deliberately cancels every transaction; durability needs the image suite. */
static enum ext4_result
mutation_unexpected_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct model *model = context;

	(void)offset;
	(void)buffer;
	(void)length;
	model->writes++;
	return EXT4_IO;
}

static enum ext4_result
mutation_unexpected_flush(void *context)
{
	struct model *model = context;

	model->flushes++;
	return EXT4_IO;
}

struct mutation_counts {
	uint32_t allocations;
	uint32_t reads;
};

#define MUTATE(call) do { error = (call); if (error != EXT4_OK) { goto out; } } while (0)

static void
mutation_run(const struct fixture *fixture, uint32_t block_size, bool indexed,
    bool checksum, bool strict, uint32_t fault, bool read_fault, struct mutation_counts *counts)
{
	struct model *model = calloc(1, sizeof(*model));
	struct ext4_journal journal = { .first = 1, .last = MODEL_BLOCKS };
	struct ext4_transaction *transaction = NULL;
	struct ext4_allocation allocation = { 0 };
	struct ext4_directory_request request = { 0 };
	struct ext4_crypto_environment crypto;
	struct ext4_directory_slot slot;
	struct ext4_inode_disk *disk;
	struct ext4_group_disk *group;
	struct ext4_super_disk *super;
	struct ext4_dir_header_disk *entry;
	const struct ext4_dir_hash_disk *hash;
	const uint8_t *snapshot;
	uint8_t cipher[EXT4_NAME_MAX];
	uint8_t *saved;
	void *buffer;
	uint32_t physical = indexed ? MODEL_LEAF : MODEL_DIRECTORY;
	uint32_t offset = indexed ? 0U : 24U;
	uint32_t prepared_hashes;
	uint32_t allocations;
	uint32_t reads;
	bool allocation_ready = false;
	enum ext4_result error = EXT4_OK;
	uint32_t hash_offset = 8U + ((fixture->fields[CIPHER_LENGTH] + 3U) & ~3U);

	CHECK(model != NULL);
	model_init(model, fixture, block_size, indexed, checksum, strict);
	journal.fs = &model->fs;
	journal.writer.context = model;
	journal.writer.write = mutation_unexpected_write;
	journal.writer.flush = mutation_unexpected_flush;
	model->fs.journal = &journal;
	/* A complete allocated bitmap is enough for these no-growth mutations. */
	memset(model->device + 3U * block_size, 0xff, block_size);
	group = (void *)(model->device + (EXT4_SUPER_OFFSET / block_size + 1U) * block_size);
	if (checksum) {
		ext4_encode16(&group->block_bitmap_checksum_lo, (uint16_t)ext4_crc32c(
		    model->fs.checksum_seed, model->device + 3U * block_size, MODEL_BLOCKS / 8U));
		ext4_group_checksum_set(&model->fs, 0, group);
	}
	super = (void *)(model->device + EXT4_SUPER_OFFSET);
	ext4_encode16(&super->magic, EXT4_SUPER_MAGIC);
	ext4_encode16(&super->state, EXT4_VALID_FS);
	ext4_encode32(&super->feature_compat, model->fs.info.feature_compat);
	ext4_encode32(&super->feature_incompat, model->fs.info.feature_incompat);
	ext4_encode32(&super->feature_ro_compat, model->fs.info.feature_ro_compat);
	super->default_hash_version = EXT4_HASH_HALF_MD4_UNSIGNED;
	if (checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	saved = malloc(model->size);
	CHECK(saved != NULL);
	memcpy(saved, model->device, model->size);
	allocations = model->allocations;
	reads = model->reads;
	if (fault != 0) {
		if (read_fault) {
			model->fail_read = reads + fault;
		} else {
			model->fail_allocation = allocations + fault;
		}
	}
	MUTATE(ext4_transaction_begin(&journal, 8, &transaction));
	MUTATE(ext4_transaction_buffer(transaction, 5U, &buffer));
	disk = (void *)((uint8_t *)buffer + MODEL_INODE_SIZE);
	allocation_ready = true;
	MUTATE(ext4_allocation_init(&allocation, &model->fs, transaction, &model->directory));
	MUTATE(ext4_write_map_validate(&allocation, &model->directory, disk));
	model->hash_original = true;
	model->allow_encrypt = true;
	MUTATE(ext4_directory_request_open(&model->fs, &model->directory, fixture->name,
	    fixture->fields[NAME_LENGTH], EXT4_NAME_REQUIRE_KEY, cipher, &request));
	model->allow_encrypt = false;
	prepared_hashes = model->hash_calls;
	if (strict && fixture->fields[NAME_VALID] == 0) {
		/* Linux strict mode does not match malformed plaintext, even exactly.
		 * The independently encoded no-key identity still permits removal. */
		EXPECT(ext4_directory_scan(&allocation, &model->directory, disk, &request,
		    EXT4_DIRECTORY_FIND, 2, &slot), EXT4_NOT_FOUND);
		ext4_directory_request_close(&model->fs, &request);
		model->missing_key = true;
		crypto = model_crypto(model);
		MUTATE(ext4_set_crypto(&model->fs, &crypto));
		MUTATE(ext4_directory_request_open(&model->fs, &model->directory, fixture->nokey,
		    fixture->nokey_length, EXT4_NAME_ALLOW_NOKEY_REMOVAL, cipher, &request));
	}
	MUTATE(ext4_directory_scan(&allocation, &model->directory, disk, &request,
	    EXT4_DIRECTORY_FIND, 2, &slot));
	CHECK(slot.number == 3 && slot.physical == physical && slot.offset == offset);
	MUTATE(ext4_directory_replace(&allocation, &model->directory, &slot, 4,
	    EXT4_FT_REGULAR));
	snapshot = ext4_transaction_peek(transaction, physical);
	CHECK(snapshot != NULL && memcmp(snapshot + offset + 8U, fixture->cipher,
	    fixture->fields[CIPHER_LENGTH]) == 0);
	hash = (const void *)(snapshot + offset + hash_offset);
	CHECK(ext4_le32(&hash->major) == fixture->fields[STORED_MAJOR]);
	CHECK(ext4_le32(&hash->minor) == fixture->fields[STORED_MINOR]);
	slot.number = 4;
	MUTATE(ext4_directory_remove(&allocation, &model->directory, &slot));
	MUTATE(ext4_directory_scan(&allocation, &model->directory, disk, NULL,
	    EXT4_DIRECTORY_EMPTY, 2, &slot));
	if (model->missing_key) {
		CHECK(model->hash_calls == prepared_hashes);
		ext4_directory_request_close(&model->fs, &request);
		model->missing_key = false;
		crypto = model_crypto(model);
		MUTATE(ext4_set_crypto(&model->fs, &crypto));
		model->allow_encrypt = true;
		MUTATE(ext4_directory_request_open(&model->fs, &model->directory, fixture->name,
		    fixture->fields[NAME_LENGTH], EXT4_NAME_REQUIRE_KEY, cipher, &request));
		model->allow_encrypt = false;
		prepared_hashes = model->hash_calls;
	}
	/* Exercise both directions on an empty tree before reinsertion. The stored
	 * context stays v2; only the directory record/hash format changes. */
	MUTATE(ext4_directory_change_format(&allocation, &model->directory, disk,
	    model->directory.flags & ~(uint32_t)EXT4_INODE_CASEFOLD));
	model->directory.flags &= ~(uint32_t)EXT4_INODE_CASEFOLD;
	ext4_encode32(&disk->flags, model->directory.flags);
	ext4_fscrypt_forget(&model->fs);
	if (indexed) {
		snapshot = ext4_transaction_peek(transaction, MODEL_DIRECTORY);
		CHECK(((const struct ext4_dx_root_prefix_disk *)snapshot)->hash_version ==
		    EXT4_HASH_HALF_MD4_UNSIGNED);
	}
	MUTATE(ext4_directory_change_format(&allocation, &model->directory, disk,
	    model->directory.flags | EXT4_INODE_CASEFOLD));
	model->directory.flags |= EXT4_INODE_CASEFOLD;
	ext4_encode32(&disk->flags, model->directory.flags);
	ext4_fscrypt_forget(&model->fs);
	if (indexed) {
		snapshot = ext4_transaction_peek(transaction, MODEL_DIRECTORY);
		CHECK(((const struct ext4_dx_root_prefix_disk *)snapshot)->hash_version ==
		    EXT4_HASH_SIPHASH);
	}
	if (strict && fixture->fields[NAME_VALID] == 0) {
		EXPECT(ext4_directory_scan(&allocation, &model->directory, disk, &request,
		    EXT4_DIRECTORY_INSERT, 2, &slot), EXT4_INVALID_ARGUMENT);
	} else {
		MUTATE(ext4_directory_scan(&allocation, &model->directory, disk, &request,
		    EXT4_DIRECTORY_INSERT, 2, &slot));
		MUTATE(ext4_directory_insert(&allocation, &model->directory, disk, &slot,
		    3, EXT4_FT_REGULAR, &request));
		snapshot = ext4_transaction_peek(transaction, physical);
		entry = (void *)(snapshot + offset);
		CHECK(ext4_le32(&entry->inode) == 3);
		CHECK(entry->name_length == fixture->fields[CIPHER_LENGTH]);
		CHECK(memcmp(snapshot + offset + 8U, fixture->cipher,
		    fixture->fields[CIPHER_LENGTH]) == 0);
		hash = (const void *)(snapshot + offset + hash_offset);
		CHECK(ext4_le32(&hash->major) == fixture->fields[STORED_MAJOR]);
		CHECK(ext4_le32(&hash->minor) == fixture->fields[STORED_MINOR]);
		memcpy(allocation.scratch, snapshot, block_size);
		MUTATE(ext4_directory_checksum(&model->fs, &model->directory,
		    indexed ? 1U : 0U, allocation.scratch));
	}
	CHECK(model->hash_calls == prepared_hashes);
out:
	if (counts != NULL) {
		counts->allocations = model->allocations - allocations;
		counts->reads = model->reads - reads;
	}
	if (fault == 0) {
		EXPECT(error, EXT4_OK);
	} else {
		EXPECT(error, read_fault ? EXT4_IO : EXT4_NO_MEMORY);
	}
	ext4_directory_request_close(&model->fs, &request);
	if (allocation_ready) {
		ext4_allocation_destroy(&allocation);
	}
	if (transaction != NULL) {
		ext4_transaction_cancel(transaction);
	}
	CHECK(memcmp(saved, model->device, model->size) == 0);
	CHECK(model->writes == 0 && model->flushes == 0);
	model->fs.journal = NULL;
	EXPECT(ext4_set_crypto(&model->fs, NULL), EXT4_OK);
	CHECK(model->live == 0 && model->keyring.handles == 0);
	free(saved);
	free(model->device);
	free(model);
}

#undef MUTATE

static void
mutation_snapshots(const struct fixture *fixture, uint32_t block_size, bool indexed,
    bool checksum, bool strict)
{
	struct mutation_counts counts;
	uint32_t fault;

	mutation_run(fixture, block_size, indexed, checksum, strict, 0, false, &counts);
	if (fixture->fields[NAME_LENGTH] != 1 || strict) {
		return;
	}
	/* Enumerate every observed allocation/read boundary of this bounded path. */
	CHECK(counts.allocations < 512 && counts.reads < 512);
	for (fault = 1; fault <= counts.allocations; fault++) {
		mutation_run(fixture, block_size, indexed, checksum, strict, fault, false, NULL);
	}
	for (fault = 1; fault <= counts.reads; fault++) {
		mutation_run(fixture, block_size, indexed, checksum, strict, fault, true, NULL);
	}
	printf("mutation faults: block=%u indexed=%u checksum=%u allocations=%u reads=%u\n",
	    block_size, indexed, checksum, counts.allocations, counts.reads);
}
