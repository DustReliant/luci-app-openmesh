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

/* ---- wired-peer ingress hints (m23 auto-detection) ---- */
#define OM_HINT_MAX    8	/* one row per physical port, evict oldest */
#define OM_HINT_STABLE 2	/* announcements before a hint counts */

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
	char mac[18];		/* primary MAC, "" if not announced */
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
	char mac[18];		/* primary MAC, "" to omit */
};

/* last verified announce per PHYSICAL ingress port (m23): a peer whose
 * signed announce enters via a br-lan member port is sitting on the
 * other end of that cable */
struct om_disc_hint {
	char ifname[16];
	uint8_t node[OM_ID_LEN];
	time_t first_seen;
	time_t last_seen;
	uint32_t count;
};

struct om_disc {
	int fd;
	struct om_disc_cfg cfg;
	const struct om_identity *id;
	struct om_node_info nodes[OM_DISC_MAX_NODES];
	unsigned n_nodes;
	struct om_disc_hint hints[OM_HINT_MAX];
	unsigned n_hints;
	void (*on_new)(struct om_disc *d, const struct om_node_info *n, void *priv);
	void (*hint_cb)(const struct om_disc_hint *h, void *priv);
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

struct om_disc;

/* minimal flat JSON scanner: "key":"value" (no escapes); 0 / -1 */
int om_json_get_str(const char *body, size_t len, const char *key,
		    char *out, size_t outsz);

/*
 * Validate and absorb one packet. Returns 0 if accepted (new or refresh),
 * -1 for any rejection. Exposed for unit tests.
 */
int om_disc_handle_packet(struct om_disc *d, const uint8_t *buf, size_t len,
			  const char *src, time_t now);

/* ---- ingress hints (m23) ----
 * Record a verified announce seen on a physical ingress port; fires
 * hint_cb exactly when the hint first reaches OM_HINT_STABLE count. */
void om_disc_note_ingress(struct om_disc *d, const char *ifname,
			  const uint8_t node[OM_ID_LEN], time_t now);

/* copy hints out (max entries); returns number stored */
int om_disc_get_hints(const struct om_disc *d, struct om_disc_hint out[],
		      int max);

void om_disc_set_hint_cb(struct om_disc *d,
			 void (*cb)(const struct om_disc_hint *h,
				    void *priv),
			 void *priv);

#endif /* OPENMESH_DISCOVERY_H */
