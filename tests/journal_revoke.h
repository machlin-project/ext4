/* SPDX-License-Identifier: BSD-3-Clause */
/* Fixtures for a durable revoke whose feature advertisement can lag on disk. */

enum revoke_damage {
	REVOKE_VALID,
	REVOKE_MISALIGNED_LENGTH,
	REVOKE_PROTECTED_TARGET,
	REVOKE_OUT_OF_RANGE_TARGET,
	REVOKE_BAD_CHECKSUM,
	REVOKE_DAMAGE_COUNT
};

static uint8_t *
revoke_slot(struct device *device, uint32_t logical)
{

	CHECK(logical < device->journal_blocks);
	return device->cache + device->journal_map[logical] * device->block_size;
}

static void
revoke_checksum(struct device *device, uint8_t *buffer, size_t offset)
{
	struct ext4_jbd_super *super = (struct ext4_jbd_super *)revoke_slot(device, 0);
	struct ext4_be32 *field = (struct ext4_be32 *)(buffer + offset);
	uint32_t checksum;

	if (device->profile & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) {
		ext4_encode_be32(field, 0);
		checksum = ext4_crc32c(UINT32_MAX, super->uuid, sizeof(super->uuid));
		checksum = ext4_crc32c(checksum, buffer, device->block_size);
		ext4_encode_be32(field, checksum);
	}
}

static void
revoke_fixture(struct device *device, bool advertised, bool rewrite, enum revoke_damage damage)
{
	struct ext4_jbd_super *super;
	struct ext4_jbd_header *header;
	struct ext4_jbd_revoke *revoke;
	struct ext4_jbd_tag *tag;
	struct ext4_jbd_tag3 *tag3;
	struct ext4_be32 *word;
	uint8_t *commit;
	uint8_t *descriptor;
	uint8_t *data;
	uint64_t target;
	size_t stride;
	size_t offset;
	size_t tag_size;
	uint32_t first = device->journal_first;
	uint32_t sequence;
	uint32_t checksum;
	uint32_t index;

	device_reset(device, device->pending);
	super = (struct ext4_jbd_super *)revoke_slot(device, 0);
	ext4_encode_be32(&super->feature_incompat,
	    advertised ? device->profile : device->profile & ~EXT4_JBD_REVOKE_FEATURE);
	journal_super_checksum(super);
	header = (struct ext4_jbd_header *)revoke_slot(device, first);
	sequence = ext4_be32(&header->sequence);
	stride = sizeof(*word) * ((device->profile & EXT4_JBD_64BIT) ? 2 : 1);
	revoke = (struct ext4_jbd_revoke *)revoke_slot(device, first + TEST_TARGETS + 2);
	memset(revoke, 0, device->block_size);
	ext4_encode_be32(&revoke->header.magic, EXT4_JBD_MAGIC);
	ext4_encode_be32(&revoke->header.type, EXT4_JBD_REVOKE);
	ext4_encode_be32(&revoke->header.sequence, sequence + 1);
	ext4_encode_be32(&revoke->length,
	    (uint32_t)(sizeof(*revoke) + stride + (damage == REVOKE_MISALIGNED_LENGTH)));
	word = (struct ext4_be32 *)(revoke + 1);
	target = damage == REVOKE_PROTECTED_TARGET ? device->journal_map[0]
	    : damage == REVOKE_OUT_OF_RANGE_TARGET ? device->blocks
						   : device->target[0];
	if (device->profile & EXT4_JBD_64BIT) {
		ext4_encode_be32(word++, (uint32_t)(target >> 32));
	}
	ext4_encode_be32(word, (uint32_t)target);
	revoke_checksum(device, (uint8_t *)revoke, device->block_size - sizeof(*word));
	if (damage == REVOKE_BAD_CHECKSUM) {
		((uint8_t *)revoke)[device->block_size - 1] ^= 1;
	}
	commit = revoke_slot(device, first + TEST_TARGETS + 3);
	memcpy(commit, revoke_slot(device, first + TEST_TARGETS + 1), device->block_size);
	header = (struct ext4_jbd_header *)commit;
	ext4_encode_be32(&header->sequence, sequence + 1);
	revoke_checksum(device, commit, offsetof(struct ext4_jbd_commit, checksum));
	if (device->checksum_v1) {
		/* V1 excludes revoke records: a revoke-only transaction retains its seed. */
		ext4_encode_be32(&((struct ext4_jbd_commit *)commit)->checksum[0], UINT32_MAX);
	}
	memset(revoke_slot(device, first + TEST_TARGETS + 4), 0, device->block_size);
	if (!rewrite) {
		memcpy(device->stable, device->cache, device->size);
		return;
	}
	/* A later committed descriptor supersedes the revoke across sequence wrap.
	 * Copy the existing two-target fixture, then bind every
	 * copied payload checksum to the new transaction sequence. */
	descriptor = revoke_slot(device, first + TEST_TARGETS + 4);
	memcpy(descriptor, revoke_slot(device, first), device->block_size);
	header = (struct ext4_jbd_header *)descriptor;
	ext4_encode_be32(&header->sequence, sequence + 2);
	tag_size = (device->profile & EXT4_JBD_CSUM_V3) ? sizeof(struct ext4_jbd_tag3)
							: sizeof(struct ext4_jbd_tag) +
		((device->profile & EXT4_JBD_64BIT) ? sizeof(struct ext4_be32) : 0) +
		((device->profile & EXT4_JBD_CSUM_V2) ? sizeof(struct ext4_be16) : 0);
	offset = sizeof(*header);
	for (index = 0; index < TEST_TARGETS; index++) {
		data = revoke_slot(device, first + TEST_TARGETS + 5 + index);
		memcpy(data, revoke_slot(device, first + 1 + index), device->block_size);
		if (index == 0) {
			memset(data + sizeof(struct ext4_be32), 0x3d,
			    device->block_size - sizeof(struct ext4_be32));
		}
		checksum = ext4_crc32c(UINT32_MAX, super->uuid, sizeof(super->uuid));
		checksum = ext4_crc32c(checksum, &header->sequence, sizeof(header->sequence));
		checksum = ext4_crc32c(checksum, data, device->block_size);
		if (device->profile & EXT4_JBD_CSUM_V3) {
			tag3 = (struct ext4_jbd_tag3 *)(descriptor + offset);
			ext4_encode_be32(&tag3->checksum, checksum);
		} else {
			tag = (struct ext4_jbd_tag *)(descriptor + offset);
			ext4_encode_be16(&tag->checksum, (uint16_t)checksum);
		}
		offset += tag_size + (index == 0 ? EXT4_UUID_SIZE : 0);
	}
	revoke_checksum(device, descriptor, device->block_size - sizeof(struct ext4_be32));
	commit = revoke_slot(device, first + TEST_TARGETS * 2 + 5);
	memcpy(commit, revoke_slot(device, first + TEST_TARGETS + 1), device->block_size);
	header = (struct ext4_jbd_header *)commit;
	ext4_encode_be32(&header->sequence, sequence + 2);
	revoke_checksum(device, commit, offsetof(struct ext4_jbd_commit, checksum));
	if (device->checksum_v1) {
		checksum = ext4_crc32_be(UINT32_MAX, descriptor, device->block_size);
		for (index = 0; index < TEST_TARGETS; index++) {
			checksum = ext4_crc32_be(checksum,
			    revoke_slot(device, first + TEST_TARGETS + 5 + index),
			    device->block_size);
		}
		ext4_encode_be32(&((struct ext4_jbd_commit *)commit)->checksum[0], checksum);
	}
	memset(revoke_slot(device, first + TEST_TARGETS * 2 + 6), 0, device->block_size);
	memcpy(device->stable, device->cache, device->size);
}

static void
test_revoke_advertisement(struct device *device)
{
	struct ext4_recovery_report report;
	uint8_t *before = malloc(device->size);
	uint8_t *expected = malloc(device->block_size);
	uint8_t *target;
	unsigned int advertised;
	unsigned int rewrite;
	enum revoke_damage damage;
	unsigned int passed = 0;
	unsigned int skipped = 0;

	CHECK(before != NULL && expected != NULL);
	for (advertised = 0; advertised < 2; advertised++) {
		for (rewrite = 0; rewrite < 2; rewrite++) {
			for (damage = REVOKE_VALID; damage < REVOKE_DAMAGE_COUNT; damage++) {
				if (damage == REVOKE_BAD_CHECKSUM &&
				    !(device->profile & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3))) {
					skipped++;
					continue;
				}
				revoke_fixture(device, advertised != 0, rewrite != 0, damage);
				memcpy(before, device->cache, device->size);
				EXPECT(ext4_recover(&device->environment, &device->writer, &report),
				    damage == REVOKE_VALID ? EXT4_OK : EXT4_CORRUPT);
				CHECK(device->live == 0);
				if (damage != REVOKE_VALID) {
					CHECK(device->writes == 0 &&
					    memcmp(before, device->cache, device->size) == 0);
				} else {
					CHECK(report.transactions == 2 + rewrite &&
					    report.replayed_blocks == (rewrite ? 2U : 1U) &&
					    report.revoked_blocks == (rewrite ? 0U : 1U));
					target =
					    device->cache + device->target[0] * device->block_size;
					if (rewrite) {
						memset(expected, 0x3d, device->block_size);
						ext4_encode_be32(
						    (struct ext4_be32 *)expected, EXT4_JBD_MAGIC);
					} else {
						memcpy(expected,
						    device->base +
							device->target[0] * device->block_size,
						    device->block_size);
					}
					CHECK(memcmp(target, expected, device->block_size) == 0);
					fill_target(expected, device->block_size, 1);
					target =
					    device->cache + device->target[1] * device->block_size;
					CHECK(memcmp(target, expected, device->block_size) == 0);
					check_clean(device);
				}
				passed++;
			}
		}
	}
	free(expected);
	free(before);
	printf("PASS revoke advertisement: features=%u cases=%u; SKIP checksum-absent=%u\n",
	    device->profile, passed, skipped);
}
