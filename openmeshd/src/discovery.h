/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * openmeshd - UDP discovery protocol v1 (ADR-0003).
 */
#ifndef OPENMESH_DISCOVERY_H
#define OPENMESH_DISCOVERY_H

#include <stdint.h>
#include <time.h>

#include "identity.h"

#define OM_DISC_PORT_DEFAULT 7689
#define OM_PKT_MAX     512
#define OM_DISC_MAX_NODES 32
#define OM_NONCE_RING  8
#define OM_TS_WINDOW   90	/* seconds */

/* magic(4) + ver(1) + ts(8) + nonce(24) + id(32) + pk(32) */
#define OM_HDR_LEN     101
#define OM_SIG_LEN     64
#define OM_MIN_LEN     (OM_HDR_LEN + 2 + OM_SIG_LEN) /* body >= 1 byte + NUL */

struct om_node_info {
	uint8_t id[OM_ID_LEN];
	uint8_t pk[OM_PK_LEN];
	char role[16];
	char model[48];
	char fw[32];
	char addr[48];		/* source IP of last announce */
	time_t first_seen;
	time_t last_seen;
	uint8_t nonces[OM_NONCE_RING][24];
	int n_nonces;
};

struct om_disc_cfg {
	int interval;		/* announce interval, seconds */
	int listen_port;
	int dest_port;
	char dest_addr[48];	/* default 255.255.255.255 */
	char role[16];
	char model[48];
	char fw[32];
};

struct om_disc {
	int fd;
	struct om_disc_cfg cfg;
	const struct om_identity *id;
	struct om_node_info nodes[OM_DISC_MAX_NODES];
	unsigned n_nodes;
	void (*on_new)(struct om_disc *d, const struct om_node_info *n, void *priv);
	void *priv;
};

/*
 * Create the UDP socket (non-blocking, SO_BROADCAST, bound to
 * cfg->listen_port). Returns 0 / -1.
 */
int om_disc_init(struct om_disc *d, const struct om_disc_cfg *cfg,
		 const struct om_identity *id);
void om_disc_free(struct om_disc *d);

/* Send one announce; caller drives cadence. Returns 0 / -1. */
int om_disc_send(struct om_disc *d);

/* Drain the socket; feeds om_disc_handle_packet per datagram. */
void om_disc_recv_ready(struct om_disc *d);

/* Garbage-collect nodes not seen for max(30, 3*interval) seconds. */
void om_disc_tick(struct om_disc *d, time_t now);

/*
 * Build a signed announce into out[]. ts is a parameter for deterministic
 * tests. Returns packet length, or 0 if it does not fit.
 */
size_t om_disc_build_packet(const struct om_disc *d, uint8_t *out,
			    size_t outlen, uint64_t ts);

/*
 * Validate and absorb one packet. Returns 0 if accepted (new or refresh),
 * -1 for any rejection. Exposed for unit tests.
 */
int om_disc_handle_packet(struct om_disc *d, const uint8_t *buf, size_t len,
			  const char *src, time_t now);

#endif /* OPENMESH_DISCOVERY_H */
