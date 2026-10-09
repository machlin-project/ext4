/* SPDX-License-Identifier: BSD-3-Clause */
#include "directory_write.h"
#include "fscrypt.h"

void
ext4_directory_request_close(struct ext4_fs *fs, struct ext4_directory_request *request)
{
	ext4_directory_name_close(fs, &request->compare);
	if (request->nokey != NULL) {
		fs->environment.release(fs->environment.context, request->nokey,
		    sizeof(*request->nokey));
		request->nokey = NULL;
	}
}

enum ext4_result
ext4_directory_request_hash(struct ext4_fs *fs, struct ext4_directory_request *request,
    uint8_t version, const uint32_t seed[4], struct ext4_name_hash *hash)
{
	struct ext4_fscrypt_key key;
	const struct ext4_casefold *fold;
	const uint8_t *name;
	size_t length;
	struct ext4_name_hash result;
	bool combined = ext4_directory_has_hashes(request->directory->flags);
	enum ext4_result error;

	if (combined != (version == EXT4_HASH_SIPHASH)) {
		return EXT4_CORRUPT;
	}
	if (combined && request->hash_ready) {
		*hash = request->hash;
		return EXT4_OK;
	}
	if (request->identity == EXT4_NAME_NOKEY_RESOLVED) {
		if (combined) {
			*hash = request->hash;
			return EXT4_OK;
		}
		return ext4_directory_hash(version, seed, request->disk_name,
		    request->disk_length, hash);
	}
	if (request->identity == EXT4_NAME_NOKEY_QUERY) {
		if (combined) {
			*hash = request->hash;
			return EXT4_OK;
		}
		name = ext4_fscrypt_nokey_cipher(request->nokey, &length);
		return name == NULL ? EXT4_INVALID_ARGUMENT :
		    ext4_directory_hash(version, seed, name, length, hash);
	}
	if (!combined) {
		return ext4_directory_name_hash(&request->compare, version, seed,
		    request->disk_name, request->disk_length, hash);
	}
	request->hash_filter = request->compare.folded &&
	    request->compare.folds[0].length != 0 &&
	    request->compare.folds[0].length < EXT4_NAME_MAX;
	fold = request->compare.folded ? &request->compare.folds[0] : NULL;
	if (request->hash_filter) {
		error = ext4_casefold_name(&request->compare.folds[1], fold->bytes, fold->length);
		if (error != EXT4_OK) {
			return error;
		}
		fold = &request->compare.folds[1];
	}
	error = ext4_fscrypt_key(fs, request->directory, &key);
	if (error == EXT4_OK) {
		error = ext4_fscrypt_name_hash(fs, &key,
		    fold == NULL ? request->compare.name : fold->bytes,
		    fold == NULL ? request->compare.length : fold->length, &result);
	}
	if (error == EXT4_OK) {
		request->hash = result;
		request->hash_ready = true;
		*hash = result;
	}
	return error;
}

bool
ext4_directory_request_probe(const struct ext4_directory_request *request)
{
	size_t length;

	return request->identity != EXT4_NAME_NOKEY_QUERY ||
	    ext4_directory_has_hashes(request->directory->flags) ||
	    ext4_fscrypt_nokey_cipher(request->nokey, &length) != NULL;
}

enum ext4_result
ext4_directory_request_open(struct ext4_fs *fs, const struct ext4_inode *directory,
    const uint8_t *name, size_t length, enum ext4_directory_prepare prepare,
    uint8_t *cipher, struct ext4_directory_request *request)
{
	struct ext4_fscrypt_key key;
	struct ext4_name_hash hash;
	uint8_t padded[EXT4_NAME_MAX];
	bool combined = ext4_directory_has_hashes(directory->flags);
	enum ext4_result error;

	ext4_zero(request, sizeof(*request));
	request->directory = directory;
	request->cipher = cipher;
	request->compare.name = name;
	request->compare.length = length;
	request->disk_name = name;
	request->disk_length = length;
	if (name == NULL || length == 0 || length > EXT4_NAME_MAX) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (prepare == EXT4_NAME_LOGGED && (directory->flags & EXT4_INODE_ENCRYPT)) {
		return EXT4_UNSUPPORTED;
	}
	error = ext4_fscrypt_directory_policy(fs, directory);
	if (error != EXT4_OK) {
		return error;
	}
	if (ext4_fscrypt_dot(name, length)) {
		return EXT4_OK;
	}
	if (!(directory->flags & EXT4_INODE_ENCRYPT)) {
		request->identity = prepare == EXT4_NAME_LOGGED ? EXT4_NAME_REPLAY : EXT4_NAME_PLAIN;
		return ext4_directory_name_open(fs, directory, name, length, &request->compare);
	}
	if (cipher == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_fscrypt_key(fs, directory, &key);
	if (error == EXT4_ENCRYPTED && prepare == EXT4_NAME_ALLOW_NOKEY_REMOVAL) {
		request->nokey = fs->environment.allocate(fs->environment.context,
		    sizeof(*request->nokey));
		if (request->nokey == NULL) {
			return EXT4_NO_MEMORY;
		}
		if (!ext4_fscrypt_nokey_decode(name, length, request->nokey)) {
			ext4_directory_request_close(fs, request);
			return EXT4_NOT_FOUND;
		}
		request->identity = EXT4_NAME_NOKEY_QUERY;
		request->disk_name = NULL;
		request->disk_length = 0;
		request->hash.major = ext4_le32((const struct ext4_le32 *)request->nokey->bytes);
		request->hash.minor = ext4_le32((const struct ext4_le32 *)
		    (request->nokey->bytes + sizeof(struct ext4_le32)));
		return EXT4_OK;
	}
	if (error != EXT4_OK) {
		return error;
	}
	request->identity = EXT4_NAME_KEYED;
	if (combined) {
		error = ext4_directory_name_open(fs, directory, name, length, &request->compare);
		if (error == EXT4_OK) {
			error = ext4_fscrypt_key(fs, directory, &key);
		}
	}
	if (error == EXT4_OK) {
		error = ext4_fscrypt_name_encrypt(fs, &key, name, length, EXT4_NAME_MAX,
		    padded, cipher, &request->disk_length);
	}
	if (error == EXT4_OK) {
		request->disk_name = cipher;
		if (combined) {
			error = ext4_directory_request_hash(fs, request, EXT4_HASH_SIPHASH, NULL, &hash);
		}
	}
	if (error != EXT4_OK) {
		ext4_directory_request_close(fs, request);
	}
	return error;
}

enum ext4_result
ext4_directory_request_match(struct ext4_fs *fs, struct ext4_directory_request *request,
    const uint8_t *name, size_t length, const struct ext4_name_hash *stored, bool *match)
{
	struct ext4_fscrypt_key key;
	uint8_t plain[EXT4_NAME_MAX];
	size_t plain_length;
	enum ext4_result error;

	*match = false;
	if (request->identity == EXT4_NAME_NOKEY_RESOLVED) {
		*match = length == request->disk_length && ext4_equal(name, request->disk_name, length);
		return EXT4_OK;
	}
	if (request->identity == EXT4_NAME_NOKEY_QUERY) {
		*match = ext4_fscrypt_nokey_match(request->nokey, name, length);
		return EXT4_OK;
	}
	if (request->identity != EXT4_NAME_KEYED) {
		*match = ext4_directory_name_match(&request->compare, name, length);
		return EXT4_OK;
	}
	if (ext4_fscrypt_dot(name, length)) {
		return EXT4_OK;
	}
	if (!ext4_directory_has_hashes(request->directory->flags)) {
		*match = length == request->disk_length && ext4_equal(name, request->disk_name, length);
		return EXT4_OK;
	}
	if (request->hash_filter &&
	    (stored->major != request->hash.major || stored->minor != request->hash.minor)) {
		return EXT4_OK;
	}
	error = ext4_fscrypt_key(fs, request->directory, &key);
	if (error == EXT4_OK) {
		error = ext4_fscrypt_name_decrypt(fs, &key, name, length, sizeof(plain), plain,
		    &plain_length);
	}
	if (error == EXT4_OK) {
		*match = ext4_directory_name_match(&request->compare, plain, plain_length);
	}
	return error;
}
