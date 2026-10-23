// SPDX-License-Identifier: GPL-3.0-or-later
/* Host unit tests for the session plane codec/handshake/crypto. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sodium.h>

#include "identity.h"
#include "session.h"

static int checks;

#define CHECK(x) do { \
	checks++; \
	if (!(x)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
		exit(1); \
	} \
} while (0)

static void mkident(struct om_identity *id, const char *tag)
{
	char tmpl[64];

	snprintf(tmpl, sizeof(tmpl), "/tmp/omses-%s-XXXXXX", tag);
	CHECK(mkdtemp(tmpl) != NULL);
	CHECK(om_identity_init(id, tmpl) == 0);
}

int main(void)
{
	struct om_identity agent, ctrl;
	struct om_ses_handshake ha, hc;
	uint8_t n1[24], n2[24];
	uint8_t buf[OM_SES_MAX_FRAME];
	uint8_t ka1[32], ka2[32], kc1[32], kc2[32];
	struct om_ses_frame f;
	size_t n;

	mkident(&agent, "agent");
	mkident(&ctrl, "ctrl");
	randombytes_buf(n1, sizeof(n1));
	randombytes_buf(n2, sizeof(n2));
	om_ses_hs_init(&ha);
	om_ses_hs_init(&hc);

	/* 1. HELLO build/parse roundtrip */
	n = om_ses_hello(buf, sizeof(buf), &ha, &agent, n1);
	CHECK(n == OM_SES_HDR_LEN + 120);
	CHECK(om_ses_parse(buf, n, &f) == 0);
	CHECK(f.type == OM_SES_HELLO && f.counter == 0 && f.payload_len == 120);
	CHECK(memcmp(f.payload, ha.eph_pub, 32) == 0);
	CHECK(memcmp(f.payload + 32, agent.id, 32) == 0);
	CHECK(memcmp(f.payload + 64, agent.pk, 32) == 0);
	CHECK(memcmp(f.payload + 96, n1, 24) == 0);

	/* 2. malformed frames rejected */
	CHECK(om_ses_parse(buf, n - 1, &f) == -1);	/* truncated */
	CHECK(om_ses_parse(buf, 4, &f) == -1);
	buf[0] ^= 1;
	CHECK(om_ses_parse(buf, n, &f) == -1);	/* bad magic */
	buf[0] ^= 1;
	buf[9] ^= 1;	/* corrupt length field */
	CHECK(om_ses_parse(buf, n, &f) == -1);
	buf[9] ^= 1;

	/* 3. PROOF verifies under agent pk; forged nonce fails */
	n = om_ses_proof(buf, sizeof(buf), &agent, n1, n2);
	CHECK(n == OM_SES_HDR_LEN + 64);
	CHECK(om_ses_parse(buf, n, &f) == 0);
	CHECK(om_ses_check_proof(agent.pk, n1, n2, f.payload) == 0);
	n2[0] ^= 1;
	CHECK(om_ses_check_proof(agent.pk, n1, n2, f.payload) == -1);
	n2[0] ^= 1;
	/* wrong key */
	CHECK(om_ses_check_proof(ctrl.pk, n1, n2, f.payload) == -1);

	/* 4. both sides derive identical directional keys */
	CHECK(om_ses_derive(ka1, kc1, ha.eph_priv, hc.eph_pub,
			    n1, n2, agent.pk) == 0);
	CHECK(om_ses_derive(ka2, kc2, hc.eph_priv, ha.eph_pub,
			    n1, n2, agent.pk) == 0);
	CHECK(memcmp(ka1, ka2, 32) == 0);
	CHECK(memcmp(kc1, kc2, 32) == 0);
	CHECK(memcmp(ka1, kc1, 32) != 0);

	/* 5. seal/unseal roundtrip (A2C direction) */
	uint8_t pt[64], rt[128], t;
	size_t rtlen;

	randombytes_buf(pt, sizeof(pt));
	n = om_ses_seal(buf, sizeof(buf), ka1, 7, OM_SES_GRANT, pt, sizeof(pt));
	CHECK(n > 0 && n <= OM_SES_MAX_FRAME);
	CHECK(om_ses_unseal(buf, n, ka2, 7, &t, rt, &rtlen) == 0);
	CHECK(t == OM_SES_GRANT && rtlen == sizeof(pt) && !memcmp(pt, rt, rtlen));

	/* 6. tampered ciphertext rejected */
	buf[n - 1] ^= 1;
	CHECK(om_ses_unseal(buf, n, ka2, 7, &t, rt, &rtlen) == -1);
	buf[n - 1] ^= 1;

	/* 7. wrong directional key rejected */
	CHECK(om_ses_unseal(buf, n, kc2, 7, &t, rt, &rtlen) == -1);

	/* 8. counter mismatch rejected (replay) */
	CHECK(om_ses_unseal(buf, n, ka2, 6, &t, rt, &rtlen) == -1);
	CHECK(om_ses_unseal(buf, n, ka2, 8, &t, rt, &rtlen) == -1);

	/* 9. GRANT payload shape (netkey+ctrl_id) roundtrips intact */
	uint8_t grant[64], grt[64];

	randombytes_buf(grant, sizeof(grant));
	n = om_ses_seal(buf, sizeof(buf), kc1, 1, OM_SES_GRANT,
			grant, sizeof(grant));
	CHECK(n > 0);
	/* controller sends with k_c2a; agent decrypts with its k_c2a (= kc2) */
	CHECK(om_ses_unseal(buf, n, kc2, 1, &t, grt, &rtlen) == 0);
	CHECK(rtlen == 64 && !memcmp(grant, grt, 64));

	printf("test_session: %d checks passed\n", checks);
	return 0;
}
