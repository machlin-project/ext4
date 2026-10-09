/* SPDX-License-Identifier: BSD-3-Clause */
/* Real disposable filesystem coverage. These calls use the installed core API;
 * the independent filename wire vectors remain a separate oracle. */
static void
casefold_namespace(struct device *device, struct ext4_fs *fs, struct ext4_inode *directory)
{
	struct ext4_inode_update update = creation();
	struct ext4_inode alpha;
	struct ext4_inode beta;
	struct ext4_inode peer;
	struct ext4_inode nested;
	struct ext4_inode result;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	uint8_t name[201];
	uint8_t query[201];
	uint32_t count = device->block_size == 1024 ? 400U : 96U;
	uint32_t index;
	uint32_t byte;
	uint32_t events;
	int length;

	EXPECT(ext4_create(fs, directory->number, directory->generation,
	    (const uint8_t *)"Alpha", 5, &update, &encrypt_time, &alpha), EXT4_OK);
	EXPECT(ext4_create(fs, directory->number, directory->generation,
	    (const uint8_t *)"ALPHA", 5, &update, &encrypt_time, &result), EXT4_EXISTS);
	result = find(fs, directory->number, "aLpHa");
	CHECK(result.number == alpha.number);
	events = device->events;
	EXPECT(ext4_set_inode_flags(fs, directory->number, directory->generation,
	    EXT4_INODE_CASEFOLD, 0, &encrypt_time, &result), EXT4_NOT_EMPTY);
	CHECK(device->events == events);
	EXPECT(ext4_create(fs, directory->number, directory->generation,
	    (const uint8_t *)"Beta", 4, &update, &encrypt_time, &beta), EXT4_OK);
	from = entry_of(directory, "ALPHA", &alpha);
	to = entry_of(directory, "beta", &beta);
	EXPECT(ext4_rename(fs, &from, &to, EXT4_RENAME_EXCHANGE, &encrypt_time, &result), EXT4_OK);
	result = find(fs, directory->number, "alpha");
	CHECK(result.number == beta.number);
	result = find(fs, directory->number, "BETA");
	CHECK(result.number == alpha.number);
	from = entry_of(directory, "alpha", &beta);
	to = entry_of(directory, "BETA", &alpha);
	EXPECT(ext4_rename(fs, &from, &to, EXT4_RENAME_EXCHANGE, &encrypt_time, &result), EXT4_OK);
	update.permissions = DIRECTORY_PERMISSIONS;
	EXPECT(ext4_mkdir(fs, directory->number, directory->generation,
	    (const uint8_t *)"Peer", 4, &update, &encrypt_time, &peer), EXT4_OK);
	CHECK((peer.flags & (EXT4_INODE_ENCRYPT | EXT4_INODE_CASEFOLD)) ==
	    (EXT4_INODE_ENCRYPT | EXT4_INODE_CASEFOLD));
	update.permissions = PERMISSIONS;
	EXPECT(ext4_create(fs, peer.number, peer.generation, (const uint8_t *)"Nested", 6,
	    &update, &encrypt_time, &nested), EXT4_OK);
	from = entry_of(directory, "alpha", &alpha);
	to = entry_of(&peer, "Moved", NULL);
	EXPECT(ext4_rename(fs, &from, &to, EXT4_RENAME_NOREPLACE, &encrypt_time, &result), EXT4_OK);
	result = find(fs, peer.number, "MOVED");
	CHECK(result.number == alpha.number);
	from = entry_of(&peer, "moved", &alpha);
	to = entry_of(directory, "Alpha", NULL);
	EXPECT(ext4_rename(fs, &from, &to, EXT4_RENAME_NOREPLACE, &encrypt_time, &result), EXT4_OK);
	for (index = 0; index < count; index++) {
		length = snprintf((char *)name, sizeof(name), "Bulk-%04u-", index);
		CHECK(length > 0 && (size_t)length < sizeof(name));
		memset(name + length, 'A', 200U - (size_t)length);
		name[200] = 0;
		EXPECT(ext4_link(fs, directory->number, directory->generation, name, 200,
		    alpha.number, alpha.generation, &encrypt_time, &result), EXT4_OK);
	}
	EXPECT(ext4_get_inode(fs, directory->number, directory), EXT4_OK);
	if (device->block_size <= 4096) {
		CHECK(directory->flags & EXT4_INODE_INDEX);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	for (index = 0; index < count; index++) {
		length = snprintf((char *)query, sizeof(query), "bulk-%04u-", index);
		CHECK(length > 0 && (size_t)length < sizeof(query));
		for (byte = (uint32_t)length; byte < 200; byte++) {
			query[byte] = 'a';
		}
		query[200] = 0;
		EXPECT(ext4_get_inode(fs, directory->number, directory), EXT4_OK);
		EXPECT(ext4_lookup(fs, directory, query, 200, &result), EXT4_OK);
		CHECK(result.number == alpha.number);
		EXPECT(ext4_unlink(fs, directory->number, directory->generation, query, 200,
		    alpha.number, alpha.generation, &encrypt_time, &result), EXT4_OK);
	}
	EXPECT(ext4_unlink(fs, peer.number, peer.generation, (const uint8_t *)"NESTED", 6,
	    nested.number, nested.generation, &encrypt_time, &result), EXT4_OK);
	EXPECT(ext4_rmdir(fs, directory->number, directory->generation, (const uint8_t *)"PEER", 4,
	    peer.number, peer.generation, &encrypt_time, &result), EXT4_OK);
	EXPECT(ext4_unlink(fs, directory->number, directory->generation, (const uint8_t *)"ALPHA", 5,
	    alpha.number, alpha.generation, &encrypt_time, &result), EXT4_OK);
	EXPECT(ext4_unlink(fs, directory->number, directory->generation, (const uint8_t *)"BETA", 4,
	    beta.number, beta.generation, &encrypt_time, &result), EXT4_OK);
	/* Empty formerly populated indexes retain topology while switching format. */
	EXPECT(ext4_set_inode_flags(fs, directory->number, directory->generation,
	    EXT4_INODE_CASEFOLD, 0, &encrypt_time, directory), EXT4_OK);
	EXPECT(ext4_create(fs, directory->number, directory->generation,
	    (const uint8_t *)"Exact", 5, &update, &encrypt_time, &alpha), EXT4_OK);
	EXPECT(ext4_create(fs, directory->number, directory->generation,
	    (const uint8_t *)"EXACT", 5, &update, &encrypt_time, &beta), EXT4_OK);
	CHECK(alpha.number != beta.number);
	result = find(fs, directory->number, "Exact");
	CHECK(result.number == alpha.number);
	result = find(fs, directory->number, "EXACT");
	CHECK(result.number == beta.number);
	EXPECT(ext4_unlink(fs, directory->number, directory->generation,
	    (const uint8_t *)"Exact", 5, alpha.number, alpha.generation, &encrypt_time, &result), EXT4_OK);
	EXPECT(ext4_unlink(fs, directory->number, directory->generation,
	    (const uint8_t *)"EXACT", 5, beta.number, beta.generation, &encrypt_time, &result), EXT4_OK);
	EXPECT(ext4_set_inode_flags(fs, directory->number, directory->generation,
	    EXT4_INODE_CASEFOLD, EXT4_INODE_CASEFOLD, &encrypt_time, directory), EXT4_OK);
	EXPECT(ext4_create(fs, directory->number, directory->generation,
	    (const uint8_t *)"After", 5, &update, &encrypt_time, &alpha), EXT4_OK);
	result = find(fs, directory->number, "AFTER");
	CHECK(result.number == alpha.number);
	EXPECT(ext4_unlink(fs, directory->number, directory->generation, (const uint8_t *)"after", 5,
	    alpha.number, alpha.generation, &encrypt_time, &result), EXT4_OK);
}

/* Enumerate actual write/barrier cuts of each empty-index format transition.
 * The restored owner must either reject a torn checksummed state without writing,
 * or expose one complete old/new format that admits another namespace change. */
static void
casefold_transition_cuts(struct device *device, uint32_t number, uint32_t generation,
    struct keyring *keyring, bool enable)
{
	struct ext4_crypto_environment crypto = keyring_environment(keyring);
	struct ext4_inode_update update = creation();
	struct ext4_recovery_report report;
	struct ext4_fs *fs;
	struct ext4_inode directory;
	struct ext4_inode file;
	struct ext4_inode result;
	uint8_t *before = malloc(device->size);
	uint8_t *after = malloc(device->size);
	uint32_t events;
	uint32_t before_refusal;
	uint32_t cut;
	uint32_t old = 0;
	uint32_t changed = 0;
	uint32_t rejected = 0;
	bool combined;
	enum ext4_result error;

	CHECK(before != NULL && after != NULL && device->live == 0);
	memcpy(before, device->stable, device->size);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	events = device->events;
	EXPECT(ext4_set_inode_flags(fs, number, generation, EXT4_INODE_CASEFOLD,
	    enable ? EXT4_INODE_CASEFOLD : 0, &encrypt_time, &directory), EXT4_OK);
	events = device->events - events;
	/* Keep cleanup outside the measured mutation cuts and save a mountable endpoint. */
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(events != 0 && keyring->handles == 0);
	memcpy(after, device->stable, device->size);
	for (cut = 1; cut <= events; cut++) {
		device_reset(device, before);
		EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
		EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
		device->stop_at = device->events + cut;
		device->survival = cut % 3U;
		device->partial = cut % 2U != 0;
		error = ext4_set_inode_flags(fs, number, generation, EXT4_INODE_CASEFOLD,
		    enable ? EXT4_INODE_CASEFOLD : 0, &encrypt_time, &directory);
		CHECK(error != EXT4_OK && device->off);
		ext4_unmount(fs);
		CHECK(device->live == 0 && keyring->handles == 0);
		device_reset(device, device->stable);
		error = ext4_recover(&device->environment, &device->writer, &report);
		if (error == EXT4_CORRUPT) {
			CHECK(device->metadata_checksum && device->writes == 0);
			rejected++;
			continue;
		}
		EXPECT(error, EXT4_OK);
		EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
		EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
		EXPECT(ext4_get_inode(fs, number, &directory), EXT4_OK);
		combined = (directory.flags & EXT4_INODE_CASEFOLD) != 0;
		CHECK(directory.flags & EXT4_INODE_ENCRYPT);
		if (combined == enable) {
			changed++;
		} else {
			old++;
		}
		EXPECT(ext4_create(fs, number, generation, (const uint8_t *)"Probe", 5,
		    &update, &encrypt_time, &file), EXT4_OK);
		EXPECT(ext4_get_inode(fs, number, &directory), EXT4_OK);
		EXPECT(ext4_lookup(fs, &directory, (const uint8_t *)"PROBE", 5, &result),
		    combined ? EXT4_OK : EXT4_NOT_FOUND);
		if (combined) {
			CHECK(result.number == file.number);
		}
		/* Unlike one lookup path, this validates every leaf before refusing. */
		before_refusal = device->events;
		EXPECT(ext4_set_inode_flags(fs, number, generation, EXT4_INODE_CASEFOLD,
		    combined ? 0 : EXT4_INODE_CASEFOLD, &encrypt_time, &result), EXT4_NOT_EMPTY);
		CHECK(device->events == before_refusal);
		ext4_unmount(fs);
		CHECK(device->live == 0 && keyring->handles == 0);
	}
	CHECK(old != 0 && changed != 0 && old + changed + rejected == events);
	device_reset(device, after);
	printf("PASS casefold %s transition cuts: %u events, %u old, %u new, %u rejected cuts\n",
	    enable ? "set" : "clear", events, old, changed, rejected);
	free(after);
	free(before);
}
