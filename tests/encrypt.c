/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#include <inttypes.h>

/* fscrypt without keys. The core must deny every operation that needs plaintext
 * names, contents or symlink targets, preserve encrypted objects, and still let
 * the unencrypted part of the volume and encrypted objects' own metadata change.
 * --synthetic builds the tree through the core and then marks it encrypted in
 * memory; otherwise the image comes from the Linux fscrypt probe. */

#define ENCRYPT_SECONDS 1700006000
#define PLAIN_FILE 24U
#define LARGE_FILE_BYTES (3U * 65536U + 777U)
#define SYNTHETIC_FILES 6U
#define PERMISSIONS 0640U
#define CHANGED_PERMISSIONS 0600U

static const struct ext4_timestamp encrypt_time = { ENCRYPT_SECONDS, 0 };

/* Matches the Linux probe so both fixture kinds share the plain file check. */
static uint8_t
pattern(unsigned int file, size_t index)
{
	return (uint8_t)(file * 131U + index * 17U + (index >> 9));
}

static size_t
file_size(unsigned int file)
{
	return file == 0 ? LARGE_FILE_BYTES : 1U + file * 97U;
}

static struct ext4_inode_update
creation(void)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = PERMISSIONS;
	update.access_time = encrypt_time;
	update.modify_time = encrypt_time;
	update.change_time = encrypt_time;
	return update;
}

static struct ext4_inode_update
data_update(const struct ext4_inode *inode)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = (uint16_t)(inode->mode & 07777U);
	update.modify_time = encrypt_time;
	update.change_time = encrypt_time;
	return update;
}

static struct ext4_inode
find(struct ext4_fs *fs, uint32_t parent, const char *name)
{
	struct ext4_inode directory;
	struct ext4_inode inode;

	EXPECT(ext4_get_inode(fs, parent, &directory), EXT4_OK);
	EXPECT(ext4_lookup(fs, &directory, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static void
write_pattern(struct ext4_fs *fs, const struct ext4_inode *inode, unsigned int file)
{
	struct ext4_inode_update update = data_update(inode);
	uint8_t *bytes = malloc(file_size(file));
	size_t completed;
	size_t index;

	CHECK(bytes != NULL);
	for (index = 0; index < file_size(file); index++) {
		bytes[index] = pattern(file, index);
	}
	EXPECT(ext4_write_partial(fs, inode->number, inode->generation, 0, bytes, file_size(file),
		   &update, &completed),
	    EXT4_OK);
	free(bytes);
}

static void
verify_pattern(struct ext4_fs *fs, const struct ext4_inode *inode, unsigned int file)
{
	uint8_t *bytes = malloc(file_size(file));
	size_t completed;
	size_t index;

	CHECK(bytes != NULL && inode->size == file_size(file));
	EXPECT(ext4_read(fs, inode, 0, bytes, file_size(file), &completed), EXT4_OK);
	CHECK(completed == file_size(file));
	for (index = 0; index < completed; index++) {
		CHECK(bytes[index] == pattern(file, index));
	}
	free(bytes);
}

static void
mark_encrypted(struct device *device, struct ext4_fs *fs, uint32_t number)
{
	struct ext4_inode_disk *disk;
	uint64_t offset;

	EXPECT(ext4_inode_location(fs, number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	ext4_encode32(&disk->flags, ext4_le32(&disk->flags) | EXT4_INODE_ENCRYPT);
	ext4_inode_checksum_set(fs, number, disk);
}

/* Build an unencrypted tree, then mark part of it encrypted as Linux would. */
static void
synthesize(struct device *device)
{
	struct ext4_inode_update update = creation();
	struct ext4_super_disk *super;
	struct ext4_inode secret;
	struct ext4_inode inner;
	struct ext4_inode empty;
	struct ext4_inode plain;
	struct ext4_inode locked;
	struct ext4_inode inode;
	struct ext4_inode files[SYNTHETIC_FILES];
	struct ext4_inode link;
	struct ext4_fs *fs;
	char name[32];
	unsigned int index;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_mkdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"secret", 6, &update,
		   &encrypt_time, &secret),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, secret.number, secret.generation, (const uint8_t *)"inner", 5,
		   &update, &encrypt_time, &inner),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, secret.number, secret.generation, (const uint8_t *)"empty", 5,
		   &update, &encrypt_time, &empty),
	    EXT4_OK);
	for (index = 0; index < SYNTHETIC_FILES; index++) {
		snprintf(name, sizeof(name), "file-%02u", index);
		EXPECT(ext4_create(fs, index % 2U ? secret.number : inner.number,
			   index % 2U ? secret.generation : inner.generation, (const uint8_t *)name,
			   strlen(name), &update, &encrypt_time, &files[index]),
		    EXT4_OK);
		write_pattern(fs, &files[index], index);
	}
	EXPECT(ext4_symlink(fs, secret.number, secret.generation, (const uint8_t *)"link", 4,
		   (const uint8_t *)"inner/file-01-target-x", 22, &update, &encrypt_time, &link),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"plain", 5, &update,
		   &encrypt_time, &plain),
	    EXT4_OK);
	EXPECT(ext4_create(fs, plain.number, plain.generation, (const uint8_t *)"visible", 7,
		   &update, &encrypt_time, &inode),
	    EXT4_OK);
	write_pattern(fs, &inode, PLAIN_FILE);
	EXPECT(ext4_mkdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"locked-empty", 12, &update,
		   &encrypt_time, &locked),
	    EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	mark_encrypted(device, fs, secret.number);
	mark_encrypted(device, fs, inner.number);
	mark_encrypted(device, fs, empty.number);
	mark_encrypted(device, fs, link.number);
	mark_encrypted(device, fs, locked.number);
	for (index = 0; index < SYNTHETIC_FILES; index++) {
		mark_encrypted(device, fs, files[index].number);
	}
	super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	ext4_encode32(&super->feature_incompat,
	    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_ENCRYPT);
	if (fs->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	ext4_unmount(fs);
	memcpy(device->base, device->cache, device->size);
	device_reset(device, device->base);
	puts("PASS synthesized an encrypted tree on an ENCRYPT volume");
}

/* Encrypted regular files and symlinks, found without reading encrypted names. */
static void
encrypted_objects(struct ext4_fs *fs, struct ext4_inode *file, struct ext4_inode *link)
{
	struct ext4_inode inode;
	uint32_t number;
	uint32_t files = 0;
	uint32_t links = 0;

	for (number = fs->first_inode; number <= fs->info.inodes; number++) {
		if (ext4_get_inode(fs, number, &inode) != EXT4_OK ||
		    !(inode.flags & EXT4_INODE_ENCRYPT)) {
			continue;
		}
		if ((inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_REGULAR && inode.size != 0) {
			*file = inode;
			files++;
		} else if ((inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_SYMLINK) {
			*link = inode;
			links++;
		}
	}
	CHECK(files != 0 && links != 0);
}

static enum ext4_dir_action
visit(void *context, const struct ext4_dir_entry *entry, uint64_t next_cookie)
{
	(void)context;
	(void)entry;
	(void)next_cookie;
	CHECK(false);
	return EXT4_DIR_STOP;
}

static void
read_only(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode secret;
	struct ext4_inode file;
	struct ext4_inode link;
	struct ext4_inode inode;
	struct ext4_mapping mapping;
	struct ext4_dir_entry entry;
	uint8_t byte;
	uint64_t cookie = 0;
	size_t completed;

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	CHECK(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_ENCRYPT);
	secret = find(fs, EXT4_ROOT_INODE, "secret");
	CHECK(secret.flags & EXT4_INODE_ENCRYPT);
	EXPECT(ext4_iterate_dir(fs, &secret, &cookie, visit, NULL), EXT4_ENCRYPTED);
	EXPECT(ext4_next_dir(fs, &secret, &cookie, &entry), EXT4_ENCRYPTED);
	EXPECT(ext4_lookup(fs, &secret, (const uint8_t *)"inner", 5, &inode), EXT4_ENCRYPTED);
	EXPECT(ext4_read(fs, &secret, 0, &byte, 1, &completed), EXT4_ENCRYPTED);
	encrypted_objects(fs, &file, &link);
	EXPECT(ext4_read(fs, &file, 0, &byte, 1, &completed), EXT4_ENCRYPTED);
	CHECK(completed == 0);
	EXPECT(ext4_map_read(fs, &file, 0, 1, &mapping), EXT4_ENCRYPTED);
	EXPECT(ext4_read(fs, &link, 0, &byte, 1, &completed), EXT4_ENCRYPTED);
	inode = find(fs, find(fs, EXT4_ROOT_INODE, "plain").number, "visible");
	verify_pattern(fs, &inode, PLAIN_FILE);
	ext4_unmount(fs);
	CHECK(device->live == 0 && device->writes == 0);
	puts("PASS read-only access denies encrypted names, contents and targets");
}

/* known supplies the source identity when its encrypted name cannot be looked up. */
static enum ext4_result
rename_names(struct ext4_fs *fs, uint32_t from_directory, const char *from_name,
    uint32_t to_directory, const char *to_name, const struct ext4_inode *known)
{
	struct ext4_inode source_parent;
	struct ext4_inode destination_parent;
	struct ext4_inode object;
	struct ext4_inode result;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;

	EXPECT(ext4_get_inode(fs, from_directory, &source_parent), EXT4_OK);
	EXPECT(ext4_get_inode(fs, to_directory, &destination_parent), EXT4_OK);
	object = known != NULL ? *known : find(fs, from_directory, from_name);
	from = (struct ext4_rename_entry){ source_parent.number, source_parent.generation,
		(const uint8_t *)from_name, strlen(from_name), object.number, object.generation };
	to = (struct ext4_rename_entry){ destination_parent.number, destination_parent.generation,
		(const uint8_t *)to_name, strlen(to_name), 0, 0 };
	return ext4_rename(fs, &from, &to, 0, &encrypt_time, &result);
}

static void
writable(struct device *device, const char *exports, const char *source)
{
	static const uint8_t byte = 'x';
	struct ext4_inode_update update = creation();
	struct ext4_inode_update attributes = { 0 };
	struct ext4_xattr_change context = { EXT4_XATTR_SET, EXT4_XATTR_INDEX_ENCRYPTION,
		(const uint8_t *)"c", 1, "forged", 6 };
	struct ext4_special_file fifo = { EXT4_FT_FIFO, 0, 0 };
	struct ext4_fs *fs;
	struct ext4_inode secret;
	struct ext4_inode plain;
	struct ext4_inode visible;
	struct ext4_inode file;
	struct ext4_inode link;
	struct ext4_inode result;
	uint64_t allocated;
	uint32_t writes;
	size_t completed;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	secret = find(fs, EXT4_ROOT_INODE, "secret");
	plain = find(fs, EXT4_ROOT_INODE, "plain");
	visible = find(fs, plain.number, "visible");
	encrypted_objects(fs, &file, &link);
	writes = device->writes;
	/* Names inside an encrypted directory would have to be encrypted. */
	EXPECT(ext4_create(fs, secret.number, secret.generation, (const uint8_t *)"new", 3, &update,
		   &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_mkdir(fs, secret.number, secret.generation, (const uint8_t *)"new", 3, &update,
		   &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_mknod(fs, secret.number, secret.generation, (const uint8_t *)"new", 3, &fifo,
		   &update, &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_symlink(fs, secret.number, secret.generation, (const uint8_t *)"new", 3,
		   (const uint8_t *)"target", 6, &update, &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_link(fs, secret.number, secret.generation, (const uint8_t *)"new", 3,
		   visible.number, visible.generation, &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_unlink(fs, secret.number, secret.generation, (const uint8_t *)"any", 3,
		   file.number, file.generation, &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_rmdir(fs, secret.number, secret.generation, (const uint8_t *)"any", 3,
		   file.number, file.generation, &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(
	    rename_names(fs, secret.number, "any", EXT4_ROOT_INODE, "out", &file), EXT4_ENCRYPTED);
	EXPECT(
	    rename_names(fs, plain.number, "visible", secret.number, "in", NULL), EXT4_ENCRYPTED);
	/* Plaintext data cannot be written into, or cut out of, ciphertext. */
	update = data_update(&file);
	EXPECT(ext4_write(fs, file.number, file.generation, 0, &byte, 1, &update, &completed),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_write_partial(
		   fs, file.number, file.generation, file.size, &byte, 1, &update, &completed),
	    EXT4_ENCRYPTED);
	EXPECT(
	    ext4_truncate(fs, file.number, file.generation, 1, &update, &result), EXT4_ENCRYPTED);
	EXPECT(ext4_truncate_atomic(
		   fs, file.number, file.generation, file.size + 1U, &update, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_fallocate(fs, file.number, file.generation, 0, 1, EXT4_FALLOC_KEEP_SIZE,
		   &update, &allocated),
	    EXT4_ENCRYPTED);
	attributes.fields = EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS;
	attributes.change_time = encrypt_time;
	attributes.xattrs = &context;
	attributes.xattr_count = 1;
	EXPECT(ext4_set_attributes(fs, file.number, file.generation, &attributes, &result),
	    EXT4_PERMISSION_DENIED);
	CHECK(device->writes == writes && !fs->aborted);

	/* Metadata of encrypted objects and the unencrypted namespace still change. */
	attributes.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS;
	attributes.permissions = CHANGED_PERMISSIONS;
	attributes.xattrs = NULL;
	attributes.xattr_count = 0;
	EXPECT(
	    ext4_set_attributes(fs, file.number, file.generation, &attributes, &result), EXT4_OK);
	CHECK((result.mode & 07777U) == CHANGED_PERMISSIONS);
	EXPECT(ext4_rmdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"secret", 6, secret.number,
		   secret.generation, &encrypt_time, &result),
	    EXT4_NOT_EMPTY);
	result = find(fs, EXT4_ROOT_INODE, "locked-empty");
	EXPECT(ext4_rmdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"locked-empty", 12,
		   result.number, result.generation, &encrypt_time, &result),
	    EXT4_OK);
	EXPECT(rename_names(fs, EXT4_ROOT_INODE, "plain", EXT4_ROOT_INODE, "renamed-plain", NULL),
	    EXT4_OK);
	EXPECT(rename_names(fs, plain.number, "visible", plain.number, "visible-moved", NULL),
	    EXT4_OK);
	/* Moving the encrypted directory object rewrites its dotdot entry. */
	EXPECT(rename_names(fs, EXT4_ROOT_INODE, "secret", plain.number, "secret", NULL), EXT4_OK);
	EXPECT(rename_names(fs, plain.number, "secret", EXT4_ROOT_INODE, "secret", NULL), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	secret = find(fs, EXT4_ROOT_INODE, "secret");
	CHECK((secret.flags & EXT4_INODE_ENCRYPT) && secret.links >= 2);
	visible = find(fs, find(fs, EXT4_ROOT_INODE, "renamed-plain").number, "visible-moved");
	verify_pattern(fs, &visible, PLAIN_FILE);
	EXPECT(ext4_get_inode(fs, file.number, &result), EXT4_OK);
	CHECK(result.size == file.size && (result.mode & 07777U) == CHANGED_PERMISSIONS);
	ext4_unmount(fs);
	memcpy(device->stable, device->cache, device->size);
	storage_export(device, exports, source, "encrypt-mutated-");
	puts("PASS writable owner preserves ciphertext and changes unencrypted names");
}

int
main(int argc, char **argv)
{
	static struct device device;
	const char *image = NULL;
	const char *exports = NULL;
	bool synthetic = false;
	int argument;

	for (argument = 1; argument < argc; argument++) {
		if (strcmp(argv[argument], "--synthetic") == 0) {
			synthetic = true;
		} else if (image == NULL) {
			image = argv[argument];
		} else if (exports == NULL) {
			exports = argv[argument];
		} else {
			image = NULL;
			break;
		}
	}
	if (image == NULL) {
		fprintf(stderr, "usage: %s [--synthetic] IMAGE [EXPORT_DIRECTORY]\n", argv[0]);
		return 2;
	}
	storage_open(&device, image);
	if (synthetic) {
		synthesize(&device);
	}
	read_only(&device);
	writable(&device, exports, image);
	storage_close(&device);
	return 0;
}
