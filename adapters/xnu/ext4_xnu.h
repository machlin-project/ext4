/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_XNU_H
#define MACHLIN_EXT4_XNU_H

#include <ext4/ext4.h>
#include <kern/locks.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/vnode.h>

#define EXT4_XNU_NAME "machlin_ext4"
#define EXT4_XNU_HASH_SIZE 128U

struct ext4_xnu_node;

LIST_HEAD(ext4_xnu_node_head, ext4_xnu_node);

struct ext4_xnu_mount {
	mount_t mount;
	vnode_t device;
	struct ext4_fs *fs;
	struct ext4_info info;
	uint32_t device_block_size;
	lck_mtx_t *nodes_lock;
	lck_mtx_t *creation_lock;
	struct ext4_xnu_node_head nodes[EXT4_XNU_HASH_SIZE];
};

struct ext4_xnu_node {
	LIST_ENTRY(ext4_xnu_node) hash;
	struct ext4_xnu_mount *mount;
	struct ext4_inode inode;
	vnode_t vnode;
	uint32_t vid;
};

extern lck_grp_t *ext4_xnu_locks;
extern struct vfsops ext4_xnu_vfsops;
extern struct vnodeopv_desc ext4_xnu_vnodeops;

int ext4_xnu_error(enum ext4_result result);
int ext4_xnu_get_node(struct ext4_xnu_mount *mount, uint32_t number, vnode_t parent,
    struct componentname *name, vnode_t *result);

#endif
