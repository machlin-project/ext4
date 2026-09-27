/* SPDX-License-Identifier: BSD-3-Clause */
#include "ext4_xnu.h"

#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode_if.h>

int
ext4_xnu_error(enum ext4_result result)
{
	switch (result) {
	case EXT4_OK:
		return 0;
	case EXT4_INVALID_ARGUMENT:
	case EXT4_NOT_EXT4:
		return EINVAL;
	case EXT4_UNSUPPORTED:
		return ENOTSUP;
	case EXT4_NO_MEMORY:
		return ENOMEM;
	case EXT4_NOT_FOUND:
		return ENOENT;
	case EXT4_NOT_DIRECTORY:
		return ENOTDIR;
	case EXT4_NAME_TOO_LONG:
		return ENAMETOOLONG;
	case EXT4_READ_ONLY:
		return EROFS;
	case EXT4_IS_DIRECTORY:
		return EISDIR;
	case EXT4_RANGE:
		return EOVERFLOW;
	case EXT4_STALE:
		return ESTALE;
	case EXT4_NO_SPACE:
		return ENOSPC;
	case EXT4_EXISTS:
		return EEXIST;
	case EXT4_TOO_MANY_LINKS:
		return EMLINK;
	case EXT4_NOT_EMPTY:
		return ENOTEMPTY;
	case EXT4_PERMISSION_DENIED:
		return EPERM;
	default:
		return EIO;
	}
}

static void *
ext4_xnu_allocate(void *context, size_t size)
{
	(void)context;
	return _MALLOC(size, M_TEMP, M_WAITOK | M_NULL);
}

static void
ext4_xnu_release(void *context, void *buffer, size_t size)
{
	(void)context;
	(void)size;
	_FREE(buffer, M_TEMP);
}

static enum ext4_result
ext4_xnu_device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct ext4_xnu_mount *mount = context;
	uint8_t *destination = buffer;
	buf_t block;
	size_t within;
	size_t amount;
	int error;

	error = vnode_getwithref(mount->device);
	if (error != 0) {
		return EXT4_IO;
	}
	while (length != 0) {
		within = (size_t)(offset % mount->device_block_size);
		amount = mount->device_block_size - within;
		if (amount > length) {
			amount = length;
		}
		block = NULL;
		/* A fixed sector-sized cache key prevents overlapping metadata buffers. */
		error =
		    buf_meta_bread(mount->device, (daddr64_t)(offset / mount->device_block_size),
			(int)mount->device_block_size, NOCRED, &block);
		if (error == 0 && buf_resid(block) != 0) {
			error = EIO;
		}
		if (error == 0) {
			memcpy(destination, (const uint8_t *)buf_dataptr(block) + within, amount);
		}
		if (block != NULL) {
			buf_brelse(block);
		}
		if (error != 0) {
			break;
		}
		offset += amount;
		destination += amount;
		length -= amount;
	}
	vnode_put(mount->device);
	return error == 0 ? EXT4_OK : EXT4_IO;
}

static void
ext4_xnu_free_mount(struct ext4_xnu_mount *mount)
{
	if (mount->fs != NULL) {
		ext4_unmount(mount->fs);
	}
	if (mount->nodes_lock != NULL) {
		lck_mtx_free(mount->nodes_lock, ext4_xnu_locks);
	}
	if (mount->creation_lock != NULL) {
		lck_mtx_free(mount->creation_lock, ext4_xnu_locks);
	}
	vnode_rele(mount->device);
	_FREE(mount, M_TEMP);
}

static int
ext4_xnu_mount_volume(mount_t mp, vnode_t device, user_addr_t data, vfs_context_t context)
{
	struct ext4_xnu_mount *mount;
	struct ext4_environment environment;
	struct vfsioattr io;
	uint64_t blocks;
	uint32_t block_size;
	int error;

	(void)data;
	if (vfs_flags(mp) & MNT_UPDATE) {
		return ENOTSUP;
	}
	if (!(vfs_flags(mp) & MNT_RDONLY)) {
		return EROFS;
	}
	if (device == NULL || vnode_vtype(device) != VBLK) {
		return ENOTBLK;
	}
	error = VNOP_IOCTL(device, DKIOCGETBLOCKSIZE, (caddr_t)&block_size, 0, context);
	if (error != 0) {
		return error;
	}
	error = VNOP_IOCTL(device, DKIOCGETBLOCKCOUNT, (caddr_t)&blocks, 0, context);
	if (error != 0) {
		return error;
	}
	if (block_size < DEV_BSIZE || block_size > PAGE_SIZE ||
	    (block_size & (block_size - 1)) != 0 || blocks > INT64_MAX / block_size) {
		return ENOTSUP;
	}
	mount = _MALLOC(sizeof(*mount), M_TEMP, M_WAITOK | M_ZERO | M_NULL);
	if (mount == NULL) {
		return ENOMEM;
	}
	error = vnode_ref(device);
	if (error != 0) {
		_FREE(mount, M_TEMP);
		return error;
	}
	mount->mount = mp;
	mount->device = device;
	mount->device_block_size = block_size;
	mount->nodes_lock = lck_mtx_alloc_init(ext4_xnu_locks, LCK_ATTR_NULL);
	mount->creation_lock = lck_mtx_alloc_init(ext4_xnu_locks, LCK_ATTR_NULL);
	if (mount->nodes_lock == NULL || mount->creation_lock == NULL) {
		ext4_xnu_free_mount(mount);
		return ENOMEM;
	}
	environment.context = mount;
	environment.size_bytes = blocks * block_size;
	environment.read = ext4_xnu_device_read;
	environment.allocate = ext4_xnu_allocate;
	environment.release = ext4_xnu_release;
	error = ext4_xnu_error(ext4_mount(&environment, &mount->fs));
	if (error != 0) {
		ext4_xnu_free_mount(mount);
		return error;
	}
	ext4_get_info(mount->fs, &mount->info);
	if (mount->info.block_size < block_size) {
		ext4_xnu_free_mount(mount);
		return ENOTSUP;
	}
	vfs_setfsprivate(mp, mount);
	vfs_setflags(mp, MNT_LOCAL | MNT_RDONLY);
	vfs_getnewfsid(mp);
	vfs_setlocklocal(mp);
	vfs_ioattr(mp, &io);
	io.io_devblocksize = block_size;
	vfs_setioattr(mp, &io);
	return 0;
}

static int
ext4_xnu_unmount_volume(mount_t mp, int flags, vfs_context_t context)
{
	struct ext4_xnu_mount *mount = vfs_fsprivate(mp);
	int error;

	(void)context;
	error = vflush(mp, NULL, (flags & MNT_FORCE) ? FORCECLOSE : 0);
	if (error != 0) {
		return error;
	}
	vfs_setfsprivate(mp, NULL);
	ext4_xnu_free_mount(mount);
	return 0;
}

static int
ext4_xnu_root(mount_t mp, vnode_t *result, vfs_context_t context)
{
	(void)context;
	return ext4_xnu_get_node(vfs_fsprivate(mp), EXT4_ROOT_INODE, NULL, NULL, result);
}

static int
ext4_xnu_vget(mount_t mp, ino64_t number, vnode_t *result, vfs_context_t context)
{
	(void)context;
	if (number > UINT32_MAX) {
		return ENOENT;
	}
	return ext4_xnu_get_node(vfs_fsprivate(mp), (uint32_t)number, NULL, NULL, result);
}

static int
ext4_xnu_volume_attributes(mount_t mp, struct vfs_attr *attributes, vfs_context_t context)
{
	struct ext4_xnu_mount *mount = vfs_fsprivate(mp);
	struct ext4_info *info = &mount->info;

	(void)context;
	VFSATTR_RETURN(attributes, f_bsize, info->block_size);
	VFSATTR_RETURN(attributes, f_iosize, info->block_size);
	VFSATTR_RETURN(attributes, f_blocks, info->blocks);
	VFSATTR_RETURN(attributes, f_bfree, info->free_blocks);
	VFSATTR_RETURN(attributes, f_bavail, 0);
	VFSATTR_RETURN(attributes, f_bused, info->blocks - info->free_blocks);
	VFSATTR_RETURN(attributes, f_files, info->inodes);
	VFSATTR_RETURN(attributes, f_ffree, info->free_inodes);
	VFSATTR_RETURN(attributes, f_fssubtype, 0);
	return 0;
}

static int
ext4_xnu_sync(mount_t mp, int flags, vfs_context_t context)
{
	(void)mp;
	(void)flags;
	(void)context;
	return 0;
}

struct vfsops ext4_xnu_vfsops = { .vfs_mount = ext4_xnu_mount_volume,
	.vfs_unmount = ext4_xnu_unmount_volume,
	.vfs_root = ext4_xnu_root,
	.vfs_getattr = ext4_xnu_volume_attributes,
	.vfs_sync = ext4_xnu_sync,
	.vfs_vget = ext4_xnu_vget };
