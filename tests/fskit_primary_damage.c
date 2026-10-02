/* SPDX-License-Identifier: BSD-3-Clause */
#include "../core/internal.h"
#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

/* Damage only the primary checksum, retaining all geometry and backup copies. */
int
main(int argc, char **argv)
{
	struct ext4_super_disk super;
	struct stat status;
	off_t offset = EXT4_SUPER_OFFSET + offsetof(struct ext4_super_disk, checksum);
	int descriptor;

	_Static_assert(sizeof(super) == EXT4_SUPER_SIZE, "Superblock fixture layout");
	assert(argc == 2);
	descriptor = open(argv[1], O_RDWR | O_NOFOLLOW);
	assert(descriptor >= 0);
	assert(fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode));
	assert(status.st_size >= EXT4_SUPER_OFFSET + EXT4_SUPER_SIZE);
	assert(
	    pread(descriptor, &super, sizeof(super), EXT4_SUPER_OFFSET) == (ssize_t)sizeof(super));
	assert(ext4_le16(&super.magic) == EXT4_SUPER_MAGIC);
	assert(ext4_le32(&super.feature_ro_compat) & EXT4_FEATURE_RO_METADATA_CSUM);
	/* One changed bit makes a valid primary checksum invalid. */
	super.checksum.bytes[0] ^= 1U;
	assert(pwrite(descriptor, &super.checksum, sizeof(super.checksum), offset) ==
	    (ssize_t)sizeof(super.checksum));
	assert(fsync(descriptor) == 0);
	assert(close(descriptor) == 0);
	return 0;
}
