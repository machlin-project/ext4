/* SPDX-License-Identifier: BSD-3-Clause */
/* Distinguish interrupted async tails from a gap followed by a later commit. */

static void
test_async_tail(struct device *device)
{
	struct ext4_recovery_report report;
	struct ext4_jbd_commit *commit;
	struct ext4_jbd_header *header;
	uint32_t variant;
	uint32_t first;
	uint32_t commit_slot;
	uint32_t sequence;
	bool prefix;
	bool later;
	bool reject;

	if (!(device->profile & EXT4_JBD_ASYNC_COMMIT)) {
		return;
	}
	for (variant = 0; variant < 10; variant++) {
		prefix = variant >= 4;
		later = (variant & 1U) != 0 || variant >= 8;
		reject = later && variant != 8;
		if (prefix) {
			revoke_fixture(device, true, false, REVOKE_VALID);
		} else {
			device_reset(device, device->pending);
		}
		if (variant >= 8) {
			/* A validated 64-bit timestamp distinguishes an older ring record. */
			commit = (struct ext4_jbd_commit *)revoke_slot(
			    device, device->journal_first + TEST_TARGETS + 1);
			ext4_encode_be32(&commit->seconds_hi, 1);
			ext4_encode_be32(&commit->seconds_lo, 5);
			revoke_checksum(
			    device, (uint8_t *)commit, offsetof(struct ext4_jbd_commit, checksum));
		}
		first = device->journal_first + (prefix ? TEST_TARGETS + 2 : 0);
		commit_slot = first + (prefix ? 1 : TEST_TARGETS + 1);
		header = (struct ext4_jbd_header *)revoke_slot(device, first);
		sequence = ext4_be32(&header->sequence);
		commit = (struct ext4_jbd_commit *)revoke_slot(device, commit_slot);
		commit->checksum[0].bytes[0] ^= 1;
		if (variant & 2U) {
			revoke_slot(device, first)[device->block_size - 1U] ^= 1;
		}
		if (later) {
			commit = (struct ext4_jbd_commit *)revoke_slot(device, commit_slot + 1);
			memset(commit, 0, device->block_size);
			ext4_encode_be32(&commit->header.magic, EXT4_JBD_MAGIC);
			ext4_encode_be32(&commit->header.type, EXT4_JBD_COMMIT);
			ext4_encode_be32(&commit->header.sequence, sequence + 1U);
			if (variant >= 8) {
				ext4_encode_be32(&commit->seconds_hi, variant == 8 ? 0 : 1);
				ext4_encode_be32(
				    &commit->seconds_lo, variant == 8 ? UINT32_MAX : 6);
			}
			if (device->checksum_v1) {
				commit->checksum_type = EXT4_JBD_CRC32;
				commit->checksum_size = sizeof(commit->checksum[0]);
				ext4_encode_be32(&commit->checksum[0], UINT32_MAX);
			} else {
				revoke_checksum(device, (uint8_t *)commit,
				    offsetof(struct ext4_jbd_commit, checksum));
			}
		}
		memcpy(device->stable, device->cache, device->size);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report),
		    reject ? EXT4_CORRUPT : EXT4_OK);
		CHECK(device->live == 0);
		if (reject) {
			CHECK(device->writes == 0);
		} else {
			CHECK(report.transactions == (uint32_t)prefix && report.discarded_tail);
			CHECK(check_outcome(device) == prefix);
			check_clean(device);
		}
	}
	puts("PASS async commit: interrupted tail, valid prefix and later-commit corruption");
}
