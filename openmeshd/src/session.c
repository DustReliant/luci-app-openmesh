// SPDX-License-Identifier: GPL-3.0-or-later
/* openmeshd - session plane framing and crypto (see docs/design/m4-session.md). */

#include <string.h>

#include <sodium.h>

#include "session.h"

static void put_u32le(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static uint32_t get_u32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t om_ses_frame(uint8_t *out, size_t outlen, uint8_t type,
		    uint32_t counter, const uint8_t *payload, size_t plen)
{
	if (outlen < OM_SES_HDR_LEN + plen)
		return 0;
	put_u32le(out, OM_SES_MAGIC);
	out[4] = type;
	put_u32le(out + 5, counter);
	put_u32le(out + 9, (uint32_t)plen);
	if (plen)
		memcpy(out + OM_SES_HDR_LEN, payload, plen);
	return OM_SES_HDR_LEN + plen;
}

int om_ses_parse(const uint8_t *buf, size_t len, struct om_ses_frame *f)
{
	if (len < OM_SES_HDR_LEN || len > OM_SES_MAX_FRAME)
		return -1;
	if (get_u32le(buf) != OM_SES_MAGIC)
		return -1;
	f->type = buf[4];
	f->counter = get_u32le(buf + 5);
	f->payload_len = get_u32le(buf + 9);
	if (f->payload_len > OM_SES_MAX_FRAME - OM_SES_HDR_LEN)
		return -1;
	if ((size_t)OM_SES_HDR_LEN + f->payload_len != len)
		return -1;
	f->payload = buf + OM_SES_HDR_LEN;
	return 0;
}

void om_ses_hs_init(struct om_ses_handshake *hs)
{
	randombytes_buf(hs->eph_priv, 32);
	crypto_scalarmult_curve25519_base(hs->eph_pub, hs->eph_priv);
}

size_t om_ses_hello(uint8_t *out, size_t outlen,
		    const struct om_ses_handshake *hs,
		    const struct om_identity *id, const uint8_t n1[24])
{
	uint8_t p[120];

	memcpy(p, hs->eph_pub, 32);
	memcpy(p + 32, id->id, OM_ID_LEN);
	memcpy(p + 64, id->pk, OM_PK_LEN);
	memcpy(p + 96, n1, 24);
	return om_ses_frame(out, outlen, OM_SES_HELLO, 0, p, sizeof(p));
}

size_t om_ses_challenge(uint8_t *out, size_t outlen,
			const struct om_ses_handshake *hs,
			const uint8_t n2[24])
{
	uint8_t p[56];

	memcpy(p, hs->eph_pub, 32);
	memcpy(p + 32, n2, 24);
	return om_ses_frame(out, outlen, OM_SES_CHALLENGE, 0, p, sizeof(p));
}

size_t om_ses_proof(uint8_t *out, size_t outlen,
		    const struct om_identity *id,
		    const uint8_t n1[24], const uint8_t n2[24])
{
	uint8_t msg[48], sig[OM_SIG_LEN];

	memcpy(msg, n1, 24);
	memcpy(msg + 24, n2, 24);
	if (om_identity_sign(id, msg, sizeof(msg), sig) < 0)
		return 0;
	return om_ses_frame(out, outlen, OM_SES_PROOF, 0, sig, sizeof(sig));
}

int om_ses_check_proof(const uint8_t pk[32], const uint8_t n1[24],
		       const uint8_t n2[24], const uint8_t sig[64])
{
	uint8_t msg[48];

	memcpy(msg, n1, 24);
	memcpy(msg + 24, n2, 24);
	return om_identity_verify(pk, msg, sizeof(msg), sig);
}

int om_ses_derive(uint8_t k_a2c[32], uint8_t k_c2a[32],
		  const uint8_t own_eph_priv[32],
		  const uint8_t peer_eph_pub[32],
		  const uint8_t n1[24], const uint8_t n2[24],
		  const uint8_t agent_pk[32])
{
	uint8_t shared[32];
	uint8_t seed[32];
	crypto_generichash_state st;

	if (crypto_scalarmult_curve25519(shared, own_eph_priv,
					 peer_eph_pub) != 0)
		return -1;

	/* K_dir = BLAKE2b(shared || n1 || n2 || agent_pk || agent_eph || ctrl_eph || dir) */
	crypto_generichash_init(&st, NULL, 0, sizeof(seed));
	crypto_generichash_update(&st, shared, sizeof(shared));
	crypto_generichash_update(&st, n1, 24);
	crypto_generichash_update(&st, n2, 24);
	crypto_generichash_update(&st, agent_pk, 32);
	crypto_generichash_update(&st, (const uint8_t *)"A2C", 3);
	crypto_generichash_final(&st, k_a2c, 32);

	crypto_generichash_init(&st, NULL, 0, sizeof(seed));
	crypto_generichash_update(&st, shared, sizeof(shared));
	crypto_generichash_update(&st, n1, 24);
	crypto_generichash_update(&st, n2, 24);
	crypto_generichash_update(&st, agent_pk, 32);
	crypto_generichash_update(&st, (const uint8_t *)"C2A", 3);
	crypto_generichash_final(&st, k_c2a, 32);

	return 0;
}

size_t om_ses_seal(uint8_t *out, size_t outlen, const uint8_t key[32],
		   uint32_t counter, uint8_t type,
		   const uint8_t *pt, size_t ptlen)
{
	uint8_t aead_in[OM_SES_MAX_FRAME - OM_SES_HDR_LEN];
	uint8_t inner[OM_SES_MAX_FRAME - OM_SES_HDR_LEN];
	uint8_t nonce[12] = { 0 };
	unsigned long long ctlen = 0;

	if (ptlen > sizeof(aead_in) - 5)
		return 0;

	/* AEAD input covers type(1) + counter(4) + pt */
	aead_in[0] = type;
	put_u32le(aead_in + 1, counter);
	memcpy(aead_in + 5, pt, ptlen);

	put_u32le(nonce, counter);
	put_u32le(nonce + 8, 0xA5A5A5A5);

	if (crypto_aead_chacha20poly1305_ietf_encrypt(
			inner, &ctlen, aead_in, 5 + ptlen,
			NULL, 0, NULL, nonce, key) != 0)
		return 0;
	return om_ses_frame(out, outlen, type, counter, inner, ctlen);
}

int om_ses_unseal(const uint8_t *in, size_t inlen, const uint8_t key[32],
		  uint32_t counter, uint8_t *type,
		  uint8_t *pt, size_t *ptlen)
{
	struct om_ses_frame f;
	uint8_t nonce[12] = { 0 };
	uint8_t plain[OM_SES_MAX_FRAME];
	unsigned long long plen = 0;

	if (om_ses_parse(in, inlen, &f) < 0)
		return -1;
	if (f.counter != counter || f.payload_len < 5 + 16)
		return -1;

	put_u32le(nonce, counter);
	put_u32le(nonce + 8, 0xA5A5A5A5);

	if (crypto_aead_chacha20poly1305_ietf_decrypt(
			plain, &plen, NULL, f.payload, f.payload_len,
			NULL, 0, nonce, key) != 0)
		return -1;
	/* plain = type(1) counter(4) pt */
	if (plen < 5)
		return -1;
	*type = plain[0];
	if (get_u32le(plain + 1) != counter)
		return -1;
	memcpy(pt, plain + 5, plen - 5);
	*ptlen = plen - 5;
	return 0;
}
