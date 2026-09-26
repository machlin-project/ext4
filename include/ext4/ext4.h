/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_H
#define MACHLIN_EXT4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXT4_ROOT_INODE 2U
#define EXT4_NAME_MAX 255U
#define EXT4_LINK_MAX 65000U
#define EXT4_UUID_SIZE 16U
#define EXT4_VOLUME_NAME_SIZE 16U
#define EXT4_INODE_BLOCK_BYTES 60U

enum ext4_result {
	EXT4_OK = 0,
	EXT4_INVALID_ARGUMENT,
	EXT4_NOT_EXT4,
	EXT4_UNSUPPORTED,
	EXT4_CORRUPT,
	EXT4_IO,
	EXT4_NO_MEMORY,
	EXT4_NOT_FOUND,
	EXT4_NOT_DIRECTORY,
	EXT4_NAME_TOO_LONG,
	EXT4_READ_ONLY,
	EXT4_RECOVERY_REQUIRED,
	EXT4_IS_DIRECTORY,
	EXT4_RANGE
};

enum ext4_file_type {
	EXT4_FT_UNKNOWN = 0,
	EXT4_FT_REGULAR = 1,
	EXT4_FT_DIRECTORY = 2,
	EXT4_FT_CHARACTER = 3,
	EXT4_FT_BLOCK = 4,
	EXT4_FT_FIFO = 5,
	EXT4_FT_SOCKET = 6,
	EXT4_FT_SYMLINK = 7
};

enum ext4_mode {
	EXT4_MODE_TYPE = 0170000,
	EXT4_MODE_REGULAR = 0100000,
	EXT4_MODE_DIRECTORY = 0040000,
	EXT4_MODE_SYMLINK = 0120000
};

struct ext4_fs;

/* read must complete exactly length bytes or return an error. The resource and
 * callbacks remain valid until unmount. All offsets are resource-relative. */
struct ext4_environment {
	void *context;
	uint64_t size_bytes;
	enum ext4_result (*read)(void *context, uint64_t offset, void *buffer, size_t length);
	void *(*allocate)(void *context, size_t size);
	void (*release)(void *context, void *allocation, size_t size);
};

struct ext4_info {
	uint64_t blocks;
	uint64_t free_blocks;
	uint32_t inodes;
	uint32_t free_inodes;
	uint32_t block_size;
	uint32_t groups;
	uint32_t feature_compat;
	uint32_t feature_incompat;
	uint32_t feature_ro_compat;
	uint8_t uuid[EXT4_UUID_SIZE];
	char volume_name[EXT4_VOLUME_NAME_SIZE + 1];
};

struct ext4_timestamp {
	int64_t seconds;
	uint32_t nanoseconds;
};

struct ext4_inode {
	uint64_t size;
	uint64_t blocks_512;
	uint32_t number;
	uint32_t generation;
	uint32_t uid;
	uint32_t gid;
	uint32_t flags;
	struct ext4_timestamp access_time;
	struct ext4_timestamp change_time;
	struct ext4_timestamp modify_time;
	struct ext4_timestamp birth_time;
	uint16_t mode;
	uint16_t links;
	uint8_t block_data[EXT4_INODE_BLOCK_BYTES];
	bool fast_symlink;
	bool birth_time_valid;
};

struct ext4_dir_entry {
	uint32_t inode;
	enum ext4_file_type type;
	uint16_t name_length;
	uint8_t name[EXT4_NAME_MAX + 1];
};

enum ext4_result ext4_mount(const struct ext4_environment *environment, struct ext4_fs **result);
void ext4_unmount(struct ext4_fs *fs);
void ext4_get_info(const struct ext4_fs *fs, struct ext4_info *info);
enum ext4_result ext4_get_inode(struct ext4_fs *fs, uint32_t number, struct ext4_inode *inode);
enum ext4_result ext4_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset,
    void *buffer, size_t length, size_t *completed);
/* EXT4_NOT_FOUND means end-of-directory; cookie is an opaque resumable offset. */
enum ext4_result ext4_next_dir(struct ext4_fs *fs, const struct ext4_inode *directory,
    uint64_t *cookie, struct ext4_dir_entry *entry);
enum ext4_result ext4_lookup(struct ext4_fs *fs, const struct ext4_inode *directory,
    const uint8_t *name, size_t name_length, struct ext4_inode *inode);
const char *ext4_result_string(enum ext4_result result);

#endif
