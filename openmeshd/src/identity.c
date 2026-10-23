// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * openmeshd - per-node Ed25519 identity management.
 *
 * On-disk format (<dir>/identity, mode 0600, 41 bytes):
 *   offset 0   : magic "OMID"
 *   offset 4   : format version (1)
 *   offset 5   : 32-byte Ed25519 seed
 *   offset 37  : 4-byte integrity tag (first 4 bytes of SHA256(seed))
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sodium.h>

#include "identity.h"

#define IDENTITY_FILE   "identity"
#define IDENTITY_MAGIC  "OMID"
#define IDENTITY_VERSION 1
/* magic(4) + ver(1) + seed(32) + tag(4) */
#define IDENTITY_FILE_LEN 41

static void seed_tag(const uint8_t seed[OM_SEED_LEN], uint8_t tag[4])
{
	uint8_t h[crypto_hash_sha256_BYTES];

	crypto_hash_sha256(h, seed, OM_SEED_LEN);
	memcpy(tag, h, 4);
}

static int write_file_atomic(const char *path, const uint8_t *buf, size_t len)
{
	char tmp[OM_PATH_MAX];
	int fd;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -1;

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;

	if (write(fd, buf, len) != (ssize_t)len)
		goto fail;
	if (fsync(fd) < 0)
		goto fail;
	if (close(fd) < 0)
		goto fail_unlink;
	if (rename(tmp, path) < 0)
		goto fail_unlink;
	return 0;

fail:
	close(fd);
fail_unlink:
	unlink(tmp);
	return -1;
}

/* 0 = loaded, 1 = file absent (caller generates), -1 = corrupt or I/O error */
static int load_seed(const char *path, uint8_t seed[OM_SEED_LEN])
{
	uint8_t buf[IDENTITY_FILE_LEN];
	uint8_t tag[4];
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (errno == ENOENT) ? 1 : -1;

	if (read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf) &&
	    memcmp(buf, IDENTITY_MAGIC, 4) == 0 &&
	    buf[4] == IDENTITY_VERSION) {
		memcpy(seed, buf + 5, OM_SEED_LEN);
		seed_tag(seed, tag);
		if (memcmp(tag, buf + 37, 4) == 0) {
			close(fd);
			return 0;
		}
	}
	close(fd);
	return -1;
}

int om_identity_init(struct om_identity *id, const char *dir)
{
	uint8_t buf[IDENTITY_FILE_LEN];
	uint8_t seed[OM_SEED_LEN];
	uint8_t tag[4];
	char path[OM_PATH_MAX];
	struct stat st;
	int rc;

	if (sodium_init() < 0)
		return -1;

	memset(id, 0, sizeof(*id));

	if (snprintf(path, sizeof(path), "%s/%s", dir, IDENTITY_FILE) >= (int)sizeof(path))
		return -1;

	if (stat(dir, &st) < 0) {
		if (mkdir(dir, 0700) < 0 && errno != EEXIST)
			return -1;
	} else if (!S_ISDIR(st.st_mode)) {
		return -1;
	}

	rc = load_seed(path, seed);
	if (rc < 0)
		return -1;	/* corrupt: refuse to run, never regenerate */

	if (rc == 0) {
		crypto_sign_ed25519_seed_keypair(id->pk, id->sk, seed);
	} else {
		memcpy(buf, IDENTITY_MAGIC, 4);
		buf[4] = IDENTITY_VERSION;
		crypto_sign_ed25519_keypair(id->pk, id->sk);
		crypto_sign_ed25519_sk_to_seed(seed, id->sk);
		memcpy(buf + 5, seed, OM_SEED_LEN);
		seed_tag(seed, tag);
		memcpy(buf + 37, tag, 4);
		if (write_file_atomic(path, buf, sizeof(buf)) < 0)
			return -1;
		chmod(path, 0600);
		id->generated = true;
	}

	crypto_hash_sha256(id->id, id->pk, OM_PK_LEN);
	return 0;
}

int om_identity_sign(const struct om_identity *id, const uint8_t *msg,
		     size_t len, uint8_t sig[OM_SIG_LEN])
{
	crypto_sign_ed25519_detached(sig, NULL, msg, len, id->sk);
	return 0;
}

int om_identity_verify(const uint8_t pk[OM_PK_LEN], const uint8_t *msg,
		       size_t len, const uint8_t sig[OM_SIG_LEN])
{
	return crypto_sign_ed25519_verify_detached(sig, msg, len, pk) == 0
		? 0 : -1;
}

void om_id_to_hex(const uint8_t id[OM_ID_LEN], char out[OM_ID_HEX_LEN])
{
	static const char hexdig[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < OM_ID_LEN; i++) {
		out[i * 2] = hexdig[id[i] >> 4];
		out[i * 2 + 1] = hexdig[id[i] & 0xf];
	}
	out[OM_ID_LEN * 2] = '\0';
}
