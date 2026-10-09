/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define MODEL_BLOCKS 64U
#define MODEL_INODES 16U
#define MODEL_INODE_SIZE 256U
#define MODEL_TABLE 5U
#define MODEL_MMP 10U
#define UNKNOWN_FEATURE 0x80000000U

enum cleanup_fault { CLEANUP_OK, CLEANUP_ALLOCATE, CLEANUP_READ, CLEANUP_FOREIGN_SEQUENCE,
	CLEANUP_FOREIGN_NODE, CLEANUP_WRITE, CLEANUP_FLUSH, ACQUIRE_FLUSH };

struct model {
	struct ext4_environment environment;
	struct ext4_write_environment writer;
	struct ext4_mmp_environment mmp;
	uint8_t *image;
	uint8_t *stable;
	size_t size;
	uint32_t block_size;
	uint32_t writes;
	uint32_t flushes;
	uint32_t sleeps;
	uint32_t live;
	int64_t clock;
	bool cleanup_test;
	bool admission_failed;
	enum cleanup_fault fault;
};

static void *
model_allocate(void *context, size_t size)
{
	struct model *model = context;
	void *buffer;

	if (model->admission_failed && model->fault == CLEANUP_ALLOCATE) {
		return NULL;
	}
	buffer = malloc(size);

	CHECK(buffer != NULL);
	model->live++;
	return buffer;
}

static void
model_release(void *context, void *buffer, size_t size)
{
	struct model *model = context;

	(void)size;
	CHECK(buffer != NULL && model->live != 0);
	model->live--;
	free(buffer);
}

static enum ext4_result
model_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct model *model = context;

	CHECK(offset <= model->size && length <= model->size - offset);
	if (model->cleanup_test && model->writes == 2U) {
		if (!model->admission_failed && offset != (uint64_t)MODEL_MMP * model->block_size) {
			model->admission_failed = true;
			return EXT4_RANGE;
		}
		if (model->admission_failed && offset == (uint64_t)MODEL_MMP * model->block_size) {
			struct ext4_mmp_disk *mmp = (struct ext4_mmp_disk *)(model->image + offset);
			struct ext4_super_disk *super =
			    (struct ext4_super_disk *)(model->image + EXT4_SUPER_OFFSET);

			if (model->fault == CLEANUP_READ) {
				return EXT4_IO;
			}
			if (model->fault == CLEANUP_FOREIGN_SEQUENCE) {
				ext4_encode32(&mmp->sequence, 0xabcdefU);
			} else if (model->fault == CLEANUP_FOREIGN_NODE) {
				mmp->node_name[0] ^= 1U;
			}
			if (model->fault == CLEANUP_FOREIGN_SEQUENCE ||
			    model->fault == CLEANUP_FOREIGN_NODE) {
				if (ext4_le32(&super->feature_ro_compat) & EXT4_FEATURE_RO_METADATA_CSUM) {
					uint32_t seed = ext4_crc32c(UINT32_MAX, super->uuid, sizeof(super->uuid));

					ext4_encode32(&mmp->checksum, ext4_crc32c(seed, mmp,
					    offsetof(struct ext4_mmp_disk, checksum)));
				}
				memcpy(model->stable + offset, mmp, sizeof(*mmp));
			}
		}
	}
	memcpy(buffer, model->image + offset, length);
	return EXT4_OK;
}

static enum ext4_result
model_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct model *model = context;

	CHECK(offset <= model->size && length <= model->size - offset);
	CHECK(offset == (uint64_t)MODEL_MMP * model->block_size);
	model->writes++;
	if (model->cleanup_test && model->writes == 3U && model->fault == CLEANUP_WRITE) {
		return EXT4_IO;
	}
	memcpy(model->image + offset, buffer, length);
	return EXT4_OK;
}

static enum ext4_result
model_flush(void *context)
{
	struct model *model = context;

	model->flushes++;
	if (model->cleanup_test &&
	    ((model->flushes == 3U && model->fault == CLEANUP_FLUSH) ||
		(model->flushes == 2U && model->fault == ACQUIRE_FLUSH))) {
		return EXT4_IO;
	}
	memcpy(model->stable, model->image, model->size);
	return EXT4_OK;
}

static enum ext4_result
model_sleep(void *context, uint32_t seconds)
{
	struct model *model = context;

	model->sleeps++;
	model->clock += seconds;
	return EXT4_OK;
}

static uint32_t
model_random(void *context)
{
	(void)context;
	return 0x123456U;
}

static int64_t
model_now(void *context)
{
	return ((struct model *)context)->clock;
}

static void
seal_super(struct ext4_super_disk *super)
{
	if (ext4_le32(&super->feature_ro_compat) & EXT4_FEATURE_RO_METADATA_CSUM) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
}

/* Admission-only model: checked superblock, group, allocated root and MMP records.
 * The journal body is intentionally absent: unsupported feature admission must
 * reject before either journal inspection or MMP publication. Independent-image
 * lifecycle and recovery tests remain in mmp.c. */
static void
model_open(struct model *model, uint32_t block_size, bool checksum, bool journaled)
{
	struct ext4_super_disk *super;
	struct ext4_group_disk *group;
	struct ext4_inode_disk *root;
	struct ext4_mmp_disk *mmp;
	struct ext4_fs fs = { 0 };
	uint8_t *bitmap;
	uint32_t logarithm = block_size == 1024U ? 0U : 2U;

	memset(model, 0, sizeof(*model));
	model->block_size = block_size;
	model->size = (size_t)MODEL_BLOCKS * block_size;
	model->image = calloc(1, model->size);
	model->stable = malloc(model->size);
	CHECK(model->image != NULL && model->stable != NULL);
	model->environment = (struct ext4_environment){ model, model->size, model_read,
		model_allocate, model_release };
	model->mmp = (struct ext4_mmp_environment){ model, model_sleep, model_random, model_now,
		"admission-test", "memory-image" };
	model->writer =
	    (struct ext4_write_environment){ model, model_write, model_flush, &model->mmp };
	super = (struct ext4_super_disk *)(model->image + EXT4_SUPER_OFFSET);
	ext4_encode16(&super->magic, EXT4_SUPER_MAGIC);
	ext4_encode16(&super->state, EXT4_VALID_FS);
	ext4_encode32(&super->revision, EXT4_DYNAMIC_REV);
	ext4_encode32(&super->first_inode, EXT4_FIRST_NON_RESERVED_INODE);
	ext4_encode32(&super->blocks_count_lo, MODEL_BLOCKS);
	ext4_encode32(&super->inodes_count, MODEL_INODES);
	ext4_encode32(&super->blocks_per_group, MODEL_BLOCKS);
	ext4_encode32(&super->clusters_per_group, MODEL_BLOCKS);
	ext4_encode32(&super->inodes_per_group, MODEL_INODES);
	ext4_encode32(&super->first_data_block, logarithm == 0 ? 1U : 0U);
	ext4_encode32(&super->log_block_size, logarithm);
	ext4_encode32(&super->log_cluster_size, logarithm);
	ext4_encode16(&super->inode_size, MODEL_INODE_SIZE);
	ext4_encode32(&super->feature_incompat, EXT4_FEATURE_INCOMPAT_MMP);
	ext4_encode32(&super->feature_compat, journaled ? EXT4_FEATURE_COMPAT_HAS_JOURNAL : 0U);
	ext4_encode32(&super->journal_inode, journaled ? 8U : 0U);
	ext4_encode16(&super->mmp_interval, 5U);
	ext4_encode32(&super->mmp_block_lo, MODEL_MMP);
	if (checksum) {
		ext4_encode32(&super->feature_ro_compat, EXT4_FEATURE_RO_METADATA_CSUM);
		super->checksum_type = EXT4_CHECKSUM_CRC32C;
	}
	seal_super(super);
	fs.info.block_size = block_size;
	fs.info.feature_ro_compat = ext4_le32(&super->feature_ro_compat);
	fs.inode_size = MODEL_INODE_SIZE;
	fs.descriptor_size = EXT4_GROUP_BASE_SIZE;
	fs.metadata_checksum = checksum;
	fs.checksum_seed = ext4_crc32c(UINT32_MAX, super->uuid, sizeof(super->uuid));
	group = (struct ext4_group_disk *)(model->image +
	    (EXT4_SUPER_OFFSET / block_size + 1U) * block_size);
	ext4_encode32(&group->block_bitmap_lo, 3U);
	ext4_encode32(&group->inode_bitmap_lo, 4U);
	ext4_encode32(&group->inode_table_lo, MODEL_TABLE);
	bitmap = model->image + 4U * block_size;
	memset(bitmap, 0xff, block_size);
	ext4_encode16(&group->inode_bitmap_checksum_lo,
	    (uint16_t)ext4_crc32c(fs.checksum_seed, bitmap, MODEL_INODES / EXT4_BITS_PER_BYTE));
	ext4_group_checksum_set(&fs, 0, group);
	root = (struct ext4_inode_disk *)(model->image + MODEL_TABLE * block_size +
	    MODEL_INODE_SIZE);
	ext4_encode16(&root->mode, EXT4_MODE_DIRECTORY | 0755);
	ext4_encode16(&root->links, 2U);
	ext4_encode32(&root->generation, 1U);
	ext4_inode_checksum_set(&fs, EXT4_ROOT_INODE, root);
	mmp = (struct ext4_mmp_disk *)(model->image + MODEL_MMP * block_size);
	ext4_encode32(&mmp->magic, EXT4_MMP_MAGIC);
	ext4_encode32(&mmp->sequence, EXT4_MMP_SEQ_CLEAN);
	ext4_encode16(&mmp->check_interval, 10U);
	if (checksum) {
		ext4_encode32(&mmp->checksum,
		    ext4_crc32c(fs.checksum_seed, mmp, offsetof(struct ext4_mmp_disk, checksum)));
	}
	memcpy(model->stable, model->image, model->size);
}

static void
model_close(struct model *model)
{
	CHECK(model->live == 0);
	free(model->stable);
	free(model->image);
}

static void
check_refusal(uint32_t block_size, bool checksum, uint32_t feature, bool compatible,
    unsigned int mode)
{
	struct model model;
	struct ext4_fs *fs = NULL;
	struct ext4_super_disk *super;
	struct ext4_le32 *features;
	struct ext4_recovery_report report;
	struct ext4_write_options options = { .flags = EXT4_WRITE_UNJOURNALED };
	uint8_t *before;
	enum ext4_result result;

	model_open(&model, block_size, checksum, mode != 1U);
	super = (struct ext4_super_disk *)(model.image + EXT4_SUPER_OFFSET);
	features = compatible ? &super->feature_compat : &super->feature_ro_compat;
	ext4_encode32(features, ext4_le32(features) | feature);
	seal_super(super);
	/* Unsupported compatible bits remain readable and a clean recovery
	 * remains a no-op; neither operation may acquire MMP. */
	CHECK(ext4_mount(&model.environment, &fs) == EXT4_OK);
	ext4_unmount(fs);
	CHECK(ext4_recover(&model.environment, &model.writer, &report) == EXT4_OK);
	if (mode == 2U) {
		ext4_encode32(&super->feature_incompat,
		    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_RECOVER);
		seal_super(super);
	}
	before = malloc(model.size);
	CHECK(before != NULL);
	memcpy(before, model.image, model.size);
	memcpy(model.stable, model.image, model.size);
	if (mode == 2U) {
		result = ext4_recover(&model.environment, &model.writer, &report);
	} else {
		result = ext4_mount_writable_with_options(&model.environment, &model.writer,
		    NULL, mode == 1U ? &options : NULL, &fs);
		CHECK(fs == NULL);
	}
	CHECK(result == EXT4_UNSUPPORTED);
	printf("admission block=%u checksum=%u feature=%08x compat=%u mode=%u "
	       "writes=%u flushes=%u sleeps=%u\n",
	    block_size, checksum, feature, compatible, mode, model.writes, model.flushes,
	    model.sleeps);
	CHECK(model.writes == 0 && model.flushes == 0 && model.sleeps == 0);
	CHECK(memcmp(before, model.image, model.size) == 0);
	CHECK(memcmp(before, model.stable, model.size) == 0);
	free(before);
	model_close(&model);
}

static void
check_cleanup(uint32_t block_size, bool checksum, enum cleanup_fault fault)
{
	struct model model;
	struct ext4_fs *fs = NULL;
	struct ext4_mmp_disk *mmp;
	struct ext4_mmp_disk *stable;
	uint8_t *before;
	size_t offset = (size_t)MODEL_MMP * block_size;
	bool attempted = fault == CLEANUP_OK || fault == CLEANUP_WRITE || fault == CLEANUP_FLUSH;

	model_open(&model, block_size, checksum, true);
	model.cleanup_test = true;
	model.fault = fault;
	before = malloc(model.size);
	CHECK(before != NULL);
	memcpy(before, model.image, model.size);
	CHECK(ext4_mount_writable(&model.environment, &model.writer, &fs) ==
	    (fault == ACQUIRE_FLUSH ? EXT4_IO : EXT4_RANGE));
	CHECK(fs == NULL && model.live == 0);
	CHECK(model.writes == (attempted ? 3U : 2U));
	CHECK(model.flushes == (fault == CLEANUP_OK || fault == CLEANUP_FLUSH ? 3U : 2U));
	mmp = (struct ext4_mmp_disk *)(model.image + offset);
	stable = (struct ext4_mmp_disk *)(model.stable + offset);
	CHECK(ext4_le32(&mmp->sequence) ==
	    (fault == CLEANUP_OK || fault == CLEANUP_FLUSH ? EXT4_MMP_SEQ_CLEAN :
		fault == CLEANUP_FOREIGN_SEQUENCE ? 0xabcdefU : 0x123457U));
	CHECK(ext4_le32(&stable->sequence) ==
	    (fault == CLEANUP_OK ? EXT4_MMP_SEQ_CLEAN :
		fault == CLEANUP_FOREIGN_SEQUENCE ? 0xabcdefU :
		fault == ACQUIRE_FLUSH ? 0x123456U : 0x123457U));
	CHECK(memcmp(before, model.image, offset) == 0);
	CHECK(memcmp(before + offset + block_size, model.image + offset + block_size,
		model.size - offset - block_size) == 0);
	CHECK(memcmp(before, model.stable, offset) == 0);
	CHECK(memcmp(before + offset + block_size, model.stable + offset + block_size,
		model.size - offset - block_size) == 0);
	free(before);
	model_close(&model);
}

int
main(void)
{
	static const uint32_t features[] = { EXT4_FEATURE_RO_READONLY,
		EXT4_FEATURE_RO_SHARED_BLOCKS, UNKNOWN_FEATURE, UNKNOWN_FEATURE };
	struct model model;
	struct ext4_fs *fs;
	uint32_t block_size;
	unsigned int checksum;
	unsigned int mode;
	enum cleanup_fault fault;
	size_t index;

	for (block_size = 1024U; block_size <= 4096U; block_size *= 4U) {
		for (checksum = 0; checksum < 2U; checksum++) {
			for (fault = CLEANUP_OK; fault <= ACQUIRE_FLUSH; fault++) {
				check_cleanup(block_size, checksum != 0, fault);
			}
			/* Prove that the model admits the actual two-write MMP protocol. */
			model_open(&model, block_size, checksum != 0, false);
			CHECK(ext4_mount(&model.environment, &fs) == EXT4_OK);
			CHECK(ext4_mmp_start(fs, &model.writer, false) == EXT4_OK);
			CHECK(model.writes == 2U && model.flushes == 2U && model.sleeps == 1U);
			CHECK(ext4_mmp_stop(fs) == EXT4_OK);
			ext4_unmount(fs);
			model_close(&model);
			for (index = 0; index < sizeof(features) / sizeof(features[0]); index++) {
				for (mode = 0; mode < 3U; mode++) {
					/* The final case uses the compatible feature field. */
					check_refusal(block_size, checksum != 0, features[index],
					    index == 3U, mode);
				}
			}
		}
	}
	puts("PASS unsupported writable features reject before any MMP side effects");
	return 0;
}
