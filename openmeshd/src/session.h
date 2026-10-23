/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * openmeshd - session plane: framing, handshake and encryption (pure
 * logic, host-testable; sockets/uloop live in session_net).
 */
#ifndef OPENMESH_SESSION_H
#define OPENMESH_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "identity.h"

#define OM_SES_MAGIC     0x4f4d5331	/* "OMS1" */
#define OM_SES_HDR_LEN   13		/* magic(4) type(1) counter(4) len(4) */
#define OM_SES_MAX_FRAME 4096
#define OM_SES_KEEPALIVE 15

enum om_ses_type {
	OM_SES_HELLO = 1,	/* eph[32] id[32] pk[32] n1[24] = 120 */
	OM_SES_CHALLENGE = 2,	/* eph[32] n2[24] = 56 */
	OM_SES_PROOF = 3,	/* sig[64] */
	OM_SES_GRANT = 4,	/* enc: netkey[32] ctrl_id[32] */
	OM_SES_WELCOME = 5,	/* enc: empty */
	OM_SES_ACK = 6,		/* enc: empty */
	OM_SES_KA = 7,		/* enc: empty */
	OM_SES_STATS = 8,	/* enc: model/fw heartbeat report (V0.1 minimal) */
};

struct om_ses_frame {
	uint8_t type;
	uint32_t counter;
	const uint8_t *payload;
	size_t payload_len;
};

/* ---- codec ---- */

/* Returns frame length, 0 on error (buffer too small / bad args). */
size_t om_ses_frame(uint8_t *out, size_t outlen, uint8_t type,
		    uint32_t counter, const uint8_t *payload, size_t plen);

/* Returns 0 and fills f (pointing into buf), -1 on malformed input. */
int om_ses_parse(const uint8_t *buf, size_t len, struct om_ses_frame *f);

/* ---- handshake crypto ---- */

struct om_ses_handshake {
	uint8_t eph_pub[32];
	uint8_t eph_priv[32];
};

/* Generate the ephemeral X25519 keypair. */
void om_ses_hs_init(struct om_ses_handshake *hs);

size_t om_ses_hello(uint8_t *out, size_t outlen,
		    const struct om_ses_handshake *hs,
		    const struct om_identity *id, const uint8_t n1[24]);
size_t om_ses_challenge(uint8_t *out, size_t outlen,
			const struct om_ses_handshake *hs,
			const uint8_t n2[24]);
size_t om_ses_proof(uint8_t *out, size_t outlen,
		    const struct om_identity *id,
		    const uint8_t n1[24], const uint8_t n2[24]);

/* Verify the agent proof: sig must be Ed25519(n1||n2) under pk. */
int om_ses_check_proof(const uint8_t pk[32], const uint8_t n1[24],
		       const uint8_t n2[24], const uint8_t sig[64]);

/*
 * Derive the two directional keys from both ephemeral public keys, both
 * nonces and the agent long-term public key (all known to both sides at
 * handshake time; the controller long-term key is not - the agent pins
 * it from the GRANT/WELCOME payload instead). Returns 0 / -1.
 */
int om_ses_derive(uint8_t k_a2c[32], uint8_t k_c2a[32],
		  const uint8_t own_eph_priv[32],
		  const uint8_t peer_eph_pub[32],
		  const uint8_t n1[24], const uint8_t n2[24],
		  const uint8_t agent_pk[32]);

/* ---- session crypto ---- */

/*
 * Encrypt payload into out (len includes 16B tag). Returns total, 0 on
 * error. Counter must be strictly increasing per direction.
 */
size_t om_ses_seal(uint8_t *out, size_t outlen, const uint8_t key[32],
		   uint32_t counter, uint8_t type,
		   const uint8_t *pt, size_t ptlen);

/* Decrypt + verify. Returns 0 / -1 (tamper, wrong key, replay). */
int om_ses_unseal(const uint8_t *in, size_t inlen, const uint8_t key[32],
		  uint32_t counter, uint8_t *type,
		  uint8_t *pt, size_t *ptlen);

#endif /* OPENMESH_SESSION_H */
