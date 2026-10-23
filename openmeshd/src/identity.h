/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * openmeshd - per-node Ed25519 identity.
 */
#ifndef OPENMESH_IDENTITY_H
#define OPENMESH_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OM_PK_LEN     32
#define OM_SK_LEN     64
#define OM_SEED_LEN   32
#define OM_SIG_LEN    64
#define OM_ID_LEN     32
#define OM_ID_HEX_LEN (OM_ID_LEN * 2 + 1)
#define OM_PATH_MAX   256

struct om_identity {
	bool generated;       /* true if the keypair was created this boot */
	uint8_t pk[OM_PK_LEN]; /* Ed25519 public key (external identity) */
	uint8_t sk[OM_SK_LEN]; /* Ed25519 secret key (memory only) */
	uint8_t id[OM_ID_LEN]; /* node_id = SHA256(pk) */
};

/*
 * Load the identity from <dir>/identity, generating and persisting a new
 * keypair on first boot. Returns 0 on success; -1 on I/O error or if the
 * file exists but is corrupt (never silently regenerates an existing
 * identity).
 */
int om_identity_init(struct om_identity *id, const char *dir);

int om_identity_sign(const struct om_identity *id, const uint8_t *msg,
		     size_t len, uint8_t sig[OM_SIG_LEN]);

/* Returns 0 for a valid signature, -1 otherwise. */
int om_identity_verify(const uint8_t pk[OM_PK_LEN], const uint8_t *msg,
		       size_t len, const uint8_t sig[OM_SIG_LEN]);

void om_id_to_hex(const uint8_t id[OM_ID_LEN], char out[OM_ID_HEX_LEN]);

/* Parse a 64-char hex string; returns 0 / -1. */
int om_id_from_hex(const char *hex, uint8_t out[OM_ID_LEN]);

#endif /* OPENMESH_IDENTITY_H */
