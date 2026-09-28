/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#include <inttypes.h>

/* Casefolded directories authored by e2fsprogs: lookups by folded equivalents,
 * rejection of folded duplicates, strict-mode names, inheritance and indexed
 * growth with folded hashes. An optional export receives the changed image for
 * strict fsck, which checks the hash placement independently. */

#define CASEFOLD_SECONDS 1700007000
#define MANIFEST_LINE 1024U
#define NEW_NAMES 600U
#define NAME_BYTES 64U
#define CASEFOLD_FLAG 0x40000000U
#define INDEX_FLAG 0x00001000U

static const struct ext4_timestamp casefold_time = { CASEFOLD_SECONDS, 0 };

static size_t
decode_hex(const char *text, uint8_t *output, size_t capacity)
{
	unsigned int value;
	size_t length = strlen(text);
	size_t index;

	CHECK(length % 2U == 0 && length / 2U <= capacity);
	for (index = 0; index < length / 2U; index++) {
		CHECK(sscanf(text + index * 2U, "%2x", &value) == 1);
		output[index] = (uint8_t)value;
	}
	return length / 2U;
}

static struct ext4_inode_update
creation(void)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = 0644;
	update.access_time = casefold_time;
	update.modify_time = casefold_time;
	update.change_time = casefold_time;
	return update;
}

static struct ext4_inode
directory(struct ext4_fs *fs, const char *name)
{
	struct ext4_inode root;
	struct ext4_inode inode;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	CHECK(inode.flags & CASEFOLD_FLAG);
	return inode;
}

/* Every manifest probe must resolve exactly like its stored name in the indexed
 * and the small linear directory; the opaque name exists only in the latter. */
static void
probes(struct ext4_fs *fs, const char *path, bool *strict, uint32_t *bulk)
{
	struct ext4_inode directories[2];
	struct ext4_inode expected;
	struct ext4_inode found;
	char line[MANIFEST_LINE];
	char kind[16];
	char first[MANIFEST_LINE];
	char second[MANIFEST_LINE];
	uint8_t lookup[EXT4_NAME_MAX];
	uint8_t stored[EXT4_NAME_MAX];
	size_t lookup_length;
	size_t stored_length;
	unsigned int value;
	unsigned int index;
	unsigned int present;
	uint32_t matches = 0;
	uint32_t absent = 0;
	FILE *manifest = fopen(path, "r");
	enum ext4_result error;

	CHECK(manifest != NULL);
	directories[0] = directory(fs, "cf");
	directories[1] = directory(fs, "small");
	while (fgets(line, sizeof(line), manifest) != NULL) {
		if (sscanf(line, "strict %u", &value) == 1) {
			*strict = value != 0;
			continue;
		}
		if (sscanf(line, "bulk %u", &value) == 1) {
			*bulk = value;
			continue;
		}
		if (sscanf(line, "%15s %1023s %1023s", kind, first, second) == 3 &&
		    strcmp(kind, "match") == 0) {
			lookup_length = decode_hex(first, lookup, sizeof(lookup));
			stored_length = decode_hex(second, stored, sizeof(stored));
			present = 0;
			for (index = 0; index < 2; index++) {
				error = ext4_lookup(
				    fs, &directories[index], stored, stored_length, &expected);
				CHECK(error == EXT4_OK || error == EXT4_NOT_FOUND);
				EXPECT(ext4_lookup(
					   fs, &directories[index], lookup, lookup_length, &found),
				    error);
				if (error == EXT4_OK) {
					CHECK(found.number == expected.number);
					present++;
				}
			}
			CHECK(present != 0);
			matches++;
			continue;
		}
		CHECK(sscanf(line, "%15s %1023s", kind, first) == 2 && strcmp(kind, "absent") == 0);
		lookup_length = decode_hex(first, lookup, sizeof(lookup));
		for (index = 0; index < 2; index++) {
			EXPECT(ext4_lookup(fs, &directories[index], lookup, lookup_length, &found),
			    EXT4_NOT_FOUND);
		}
		absent++;
	}
	CHECK(fclose(manifest) == 0 && matches != 0 && absent != 0);
	printf("PASS %u folded lookups and %u absent names resolve like Linux\n", matches, absent);
}

/* Directory iteration returns stored bytes, which folded lookup cannot show. */
static bool
stored(struct ext4_fs *fs, const struct ext4_inode *directory, const char *name)
{
	struct ext4_dir_entry entry;
	uint64_t cookie = 0;
	enum ext4_result error;

	while ((error = ext4_next_dir(fs, directory, &cookie, &entry)) == EXT4_OK) {
		if (entry.name_length == strlen(name) &&
		    memcmp(entry.name, name, entry.name_length) == 0) {
			return true;
		}
	}
	EXPECT(error, EXT4_NOT_FOUND);
	return false;
}

static void
bulk_name(char *name, size_t size, const char *prefix, uint32_t index, bool upper)
{
	int length = snprintf(name, size, upper ? "%s-%04u-ÄÖÜ" : "%s-%04u-äöü", prefix, index);

	CHECK(length > 0 && (size_t)length < size);
}

static void
mutations(
    struct device *device, bool strict, uint32_t bulk, const char *exports, const char *source)
{
	static const uint8_t invalid[] = { 'b', 'a', 'd', 0xff, 'x' };
	struct ext4_inode_update update = creation();
	struct ext4_inode cf;
	struct ext4_inode sub;
	struct ext4_inode inode;
	struct ext4_inode found;
	struct ext4_inode omega;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	struct ext4_fs *fs;
	char name[NAME_BYTES];
	char upper[NAME_BYTES];
	uint32_t index;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	cf = directory(fs, "cf");
	/* Folded duplicates are the same name. */
	EXPECT(ext4_create(fs, cf.number, cf.generation, (const uint8_t *)"STRASSE", 7, &update,
		   &casefold_time, &inode),
	    EXT4_EXISTS);
	EXPECT(ext4_create(fs, cf.number, cf.generation, (const uint8_t *)"FILE", 4, &update,
		   &casefold_time, &inode),
	    EXT4_EXISTS);
	snprintf(name, sizeof(name), "bulk-%04u-été", bulk - 1U);
	EXPECT(ext4_create(fs, cf.number, cf.generation, (const uint8_t *)name, strlen(name),
		   &update, &casefold_time, &inode),
	    EXT4_EXISTS);
	EXPECT(ext4_create(fs, cf.number, cf.generation, invalid, sizeof(invalid), &update,
		   &casefold_time, &inode),
	    strict ? EXT4_INVALID_ARGUMENT : EXT4_OK);
	/* Linux never matches a malformed name under the strict encoding. */
	EXPECT(ext4_lookup(fs, &cf, invalid, sizeof(invalid), &found),
	    strict ? EXT4_NOT_FOUND : EXT4_OK);
	CHECK(strict || found.number == inode.number);
	EXPECT(ext4_lookup(fs, &cf, (const uint8_t *)"OMEGA-LINK", 10, &omega), EXT4_NOT_FOUND);
	EXPECT(ext4_lookup(fs, &cf, (const uint8_t *)"ΩMEGA", 6, &omega), EXT4_OK);
	EXPECT(ext4_link(fs, cf.number, cf.generation, (const uint8_t *)"ωmega", 6, omega.number,
		   omega.generation, &casefold_time, &found),
	    EXT4_EXISTS);
	/* A case-only rename names the same inode: a stale expected absence, EXISTS with
	 * NOREPLACE and otherwise a no-op that keeps the stored name. */
	from = (struct ext4_rename_entry){ cf.number, cf.generation, (const uint8_t *)"Ωmega", 6,
		omega.number, omega.generation };
	to = (struct ext4_rename_entry){ cf.number, cf.generation, (const uint8_t *)"ΩMEGA", 6, 0,
		0 };
	EXPECT(ext4_rename(fs, &from, &to, 0, &casefold_time, &found), EXT4_STALE);
	to.inode = omega.number;
	to.generation = omega.generation;
	EXPECT(ext4_rename(fs, &from, &to, EXT4_RENAME_NOREPLACE, &casefold_time, &found),
	    EXT4_EXISTS);
	EXPECT(ext4_rename(fs, &from, &to, 0, &casefold_time, &found), EXT4_OK);
	cf = directory(fs, "cf");
	CHECK(stored(fs, &cf, "Ωmega") && !stored(fs, &cf, "ΩMEGA"));
	/* Growth splits indexed leaves and converts nothing: every name hashes folded. */
	for (index = 0; index < NEW_NAMES; index++) {
		bulk_name(name, sizeof(name), "Neue-Datei", index, true);
		EXPECT(ext4_create(fs, cf.number, cf.generation, (const uint8_t *)name,
			   strlen(name), &update, &casefold_time, &inode),
		    EXT4_OK);
	}
	cf = directory(fs, "cf");
	for (index = 0; index < NEW_NAMES; index++) {
		bulk_name(name, sizeof(name), "neue-datei", index, false);
		EXPECT(ext4_lookup(fs, &cf, (const uint8_t *)name, strlen(name), &found), EXT4_OK);
	}
	/* New directories inherit casefolding and fold their own names. */
	EXPECT(ext4_mkdir(fs, cf.number, cf.generation, (const uint8_t *)"Unterordner", 11, &update,
		   &casefold_time, &sub),
	    EXT4_OK);
	CHECK(sub.flags & CASEFOLD_FLAG);
	EXPECT(ext4_create(fs, sub.number, sub.generation, (const uint8_t *)"Grüße", 7, &update,
		   &casefold_time, &inode),
	    EXT4_OK);
	EXPECT(ext4_lookup(fs, &sub, (const uint8_t *)"GRÜSSE", 7, &found), EXT4_OK);
	CHECK(found.number == inode.number);
	/* Removal and rename find stored names through folded equivalents. */
	found = directory(fs, "cf");
	EXPECT(ext4_lookup(fs, &found, (const uint8_t *)"FILE", 4, &inode), EXT4_OK);
	EXPECT(ext4_unlink(fs, cf.number, cf.generation, (const uint8_t *)"FILE", 4, inode.number,
		   inode.generation, &casefold_time, &found),
	    EXT4_OK);
	cf = directory(fs, "cf");
	EXPECT(ext4_lookup(fs, &cf, (const uint8_t *)"ﬁle", 5, &found), EXT4_NOT_FOUND);
	EXPECT(ext4_lookup(fs, &cf, (const uint8_t *)"CAFÉ", 5, &inode), EXT4_OK);
	from = (struct ext4_rename_entry){ cf.number, cf.generation, (const uint8_t *)"CAFÉ", 5,
		inode.number, inode.generation };
	to = (struct ext4_rename_entry){ cf.number, cf.generation, (const uint8_t *)"Café-Neu", 9,
		0, 0 };
	EXPECT(ext4_rename(fs, &from, &to, 0, &casefold_time, &found), EXT4_OK);
	cf = directory(fs, "cf");
	EXPECT(ext4_lookup(fs, &cf, (const uint8_t *)"café-neu", 9, &found), EXT4_OK);
	CHECK(found.number == inode.number);
	EXPECT(ext4_lookup(fs, &cf, (const uint8_t *)"café", 5, &found), EXT4_NOT_FOUND);
	CHECK(cf.flags & INDEX_FLAG);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	for (index = 0; index < NEW_NAMES; index += 97U) {
		bulk_name(upper, sizeof(upper), "NEUE-DATEI", index, true);
		EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
		cf = directory(fs, "cf");
		EXPECT(
		    ext4_lookup(fs, &cf, (const uint8_t *)upper, strlen(upper), &found), EXT4_OK);
		ext4_unmount(fs);
	}
	memcpy(device->stable, device->cache, device->size);
	storage_export(device, exports, source, "casefold-mutated-");
	printf("PASS writable casefolded directories: duplicates, %s names, %u indexed "
	       "creations, inheritance, unlink and rename\n",
	    strict ? "strict" : "opaque", NEW_NAMES);
}

int
main(int argc, char **argv)
{
	static struct device device;
	struct ext4_fs *fs;
	bool strict = false;
	uint32_t bulk = 0;

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: %s CASEFOLD_IMAGE MANIFEST [EXPORT_DIRECTORY]\n", argv[0]);
		return 2;
	}
	storage_open(&device, argv[1]);
	EXPECT(ext4_mount(&device.environment, &fs), EXT4_OK);
	CHECK(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_CASEFOLD);
	probes(fs, argv[2], &strict, &bulk);
	CHECK(fs->casefold_strict == strict && bulk != 0);
	ext4_unmount(fs);
	mutations(&device, strict, bulk, argc == 4 ? argv[3] : NULL, argv[1]);
	storage_close(&device);
	return 0;
}
