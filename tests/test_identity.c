// SPDX-License-Identifier: GPL-3.0-or-later
/* Host unit tests for the openmeshd identity module. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "identity.h"

static int checks;

#define CHECK(x) do { \
	checks++; \
	if (!(x)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
		exit(1); \
	} \
} while (0)

static int corrupt_seed_byte(const char *path)
{
	FILE *f = fopen(path, "r+b");
	int ch;

	if (!f)
		return -1;
	if (fseek(f, 5, SEEK_SET) < 0) { fclose(f); return -1; }
	ch = fgetc(f);
	if (ch == EOF) { fclose(f); return -1; }
	if (fseek(f, 5, SEEK_SET) < 0) { fclose(f); return -1; }
	if (fputc(ch ^ 0xff, f) == EOF) { fclose(f); return -1; }
	return fclose(f);
}

int main(void)
{
	char dir1[] = "/tmp/omtest-XXXXXX";
	char dir2[] = "/tmp/omtest-XXXXXX";
	char path[OM_PATH_MAX];
	struct om_identity a, b, c;
	struct stat st;
	uint8_t msg[32] = "hello openmesh";
	uint8_t sig[OM_SIG_LEN], sig2[OM_SIG_LEN];

	CHECK(mkdtemp(dir1) != NULL);
	CHECK(mkdtemp(dir2) != NULL);

	/* 1. first boot generates, with tight permissions */
	CHECK(om_identity_init(&a, dir1) == 0);
	CHECK(a.generated == true);
	snprintf(path, sizeof(path), "%s/identity", dir1);
	CHECK(stat(path, &st) == 0);
	CHECK(st.st_size == 41);
	CHECK((st.st_mode & 0777) == 0600);
	CHECK(stat(dir1, &st) == 0);
	CHECK((st.st_mode & 0777) == 0700);

	/* 2. reload yields the exact same identity, flagged as not-new */
	CHECK(om_identity_init(&b, dir1) == 0);
	CHECK(b.generated == false);
	CHECK(memcmp(a.pk, b.pk, OM_PK_LEN) == 0);
	CHECK(memcmp(a.id, b.id, OM_ID_LEN) == 0);

	/* 3. sign/verify roundtrip */
	CHECK(om_identity_sign(&a, msg, sizeof(msg), sig) == 0);
	CHECK(om_identity_verify(a.pk, msg, sizeof(msg), sig) == 0);

	/* 4. tampered message is rejected */
	msg[0] ^= 1;
	CHECK(om_identity_verify(a.pk, msg, sizeof(msg), sig) == -1);
	msg[0] ^= 1;

	/* 5. tampered signature is rejected */
	sig[0] ^= 1;
	CHECK(om_identity_verify(a.pk, msg, sizeof(msg), sig) == -1);
	sig[0] ^= 1;
	CHECK(om_identity_verify(a.pk, msg, sizeof(msg), sig) == 0);

	/* 6. a different node's signature does not verify under a.pk */
	CHECK(om_identity_init(&c, dir2) == 0);
	CHECK(memcmp(a.pk, c.pk, OM_PK_LEN) != 0);
	CHECK(memcmp(a.id, c.id, OM_ID_LEN) != 0);
	CHECK(om_identity_sign(&c, msg, sizeof(msg), sig2) == 0);
	CHECK(om_identity_verify(c.pk, msg, sizeof(msg), sig2) == 0);
	CHECK(om_identity_verify(a.pk, msg, sizeof(msg), sig2) == -1);

	/* 7. corrupt identity file: load refuses instead of regenerating */
	CHECK(corrupt_seed_byte(path) == 0);
	{
		struct om_identity d;

		CHECK(om_identity_init(&d, dir1) == -1);
	}

	/* 8. hex helper */
	{
		char hex[OM_ID_HEX_LEN];

		om_id_to_hex(a.id, hex);
		CHECK(strlen(hex) == 64);
		CHECK(strchr(hex, '\0') == hex + 64);
	}

	printf("test_identity: %d checks passed\n", checks);
	return 0;
}
