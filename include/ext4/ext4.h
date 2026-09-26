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

/* A separate capability: supplying read callbacks never authorizes writes.
 * write completes exactly length bytes; errors may have changed any part of the
 * requested range. flush must persist every preceding successful write through
 * all volatile caches before returning success. Read-after-write must be coherent.
 * The caller exclusively owns the resource throughout recovery or mutation.
 * A failed write/flush makes the outcome uncertain: stop mutations and reopen
 * through recovery. Checksummed control-block damage requires offline repair. */
struct ext4_write_environment {
	void *context;
	enum ext4_result (*write)(
	    void *context, uint64_t offset, const void *buffer, size_t length);
	enum ext4_result (*flush)(void *context);
};

struct ext4_recovery_report {
	uint32_t transactions;
	uint32_t replayed_blocks;
	uint32_t revoked_blocks;
	bool discarded_tail;
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

/* Immutable read mapping; an adapter must not reuse it across future mutations. */
struct ext4_mapping {
	uint64_t device_offset;
	size_t length;
	bool hole;
};

enum ext4_result ext4_mount(const struct ext4_environment *environment, struct ext4_fs **result);
/* Offline recovery only. A read-only mount never invokes this operation.
 * On error the resource remains unmounted and must not be used for mutations. */
enum ext4_result ext4_recover(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, struct ext4_recovery_report *report);
void ext4_unmount(struct ext4_fs *fs);
void ext4_get_info(const struct ext4_fs *fs, struct ext4_info *info);
enum ext4_result ext4_get_inode(struct ext4_fs *fs, uint32_t number, struct ext4_inode *inode);
enum ext4_result ext4_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset,
    void *buffer, size_t length, size_t *completed);
enum ext4_result ext4_map_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset,
    size_t length, struct ext4_mapping *mapping);
/* EXT4_NOT_FOUND means end-of-directory; cookie is an opaque resumable offset. */
enum ext4_result ext4_next_dir(struct ext4_fs *fs, const struct ext4_inode *directory,
    uint64_t *cookie, struct ext4_dir_entry *entry);
enum ext4_result ext4_lookup(struct ext4_fs *fs, const struct ext4_inode *directory,
    const uint8_t *name, size_t name_length, struct ext4_inode *inode);
const char *ext4_result_string(enum ext4_result result);

#endif
