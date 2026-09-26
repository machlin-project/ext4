/* SPDX-License-Identifier: BSD-3-Clause */
#include "ext4_xnu.h"
#include <mach/kmod.h>

lck_grp_t *ext4_xnu_locks;
static vfstable_t ext4_xnu_registration;
static struct vnodeopv_desc *ext4_xnu_vectors[] = { &ext4_xnu_vnodeops, NULL };
static struct vfs_fsentry ext4_xnu_entry = { .vfe_vfsops = &ext4_xnu_vfsops,
	.vfe_vopcnt = 1,
	.vfe_opvdescs = ext4_xnu_vectors,
	.vfe_fsname = EXT4_XNU_NAME,
	.vfe_flags = VFS_TBLTHREADSAFE | VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBL64BITREADY |
	    VFS_TBLREADDIR_EXTENDED };

static kern_return_t
ext4_xnu_start(kmod_info_t *module, void *data)
{
	int error;

	(void)module;
	(void)data;
	ext4_xnu_locks = lck_grp_alloc_init(EXT4_XNU_NAME, LCK_GRP_ATTR_NULL);
	if (ext4_xnu_locks == NULL) {
		return KERN_RESOURCE_SHORTAGE;
	}
	error = vfs_fsadd(&ext4_xnu_entry, &ext4_xnu_registration);
	if (error != 0) {
		lck_grp_free(ext4_xnu_locks);
		ext4_xnu_locks = NULL;
		return KERN_FAILURE;
	}
	return KERN_SUCCESS;
}

static kern_return_t
ext4_xnu_stop(kmod_info_t *module, void *data)
{
	(void)module;
	(void)data;
	if (vfs_fsremove(ext4_xnu_registration) != 0) {
		return KERN_FAILURE;
	}
	lck_grp_free(ext4_xnu_locks);
	ext4_xnu_locks = NULL;
	return KERN_SUCCESS;
}

KMOD_EXPLICIT_DECL(org.machlin.ext4.kext, "0.1.0", ext4_xnu_start, ext4_xnu_stop)
