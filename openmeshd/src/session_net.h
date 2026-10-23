/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESH_SESSION_NET_H
#define OPENMESH_SESSION_NET_H

#include <stdbool.h>
#include <time.h>

struct om_snet;
struct om_netconf;

#include <libubox/uloop.h>

#include "identity.h"
#include "session.h"

#define OM_SNET_MAX_PEERS  32
#define OM_SNET_MAX_PENDING 8

enum om_peer_state {
	PS_HELLO = 0,		/* controller: expecting HELLO */
	PS_PROOF,		/* expecting PROOF */
	PS_ESTABLISHED,
	PS_CONNECTING,		/* agent only */
	PS_AWAIT_CHALLENGE,	/* agent only */
};

struct om_adopted_node {
	uint8_t id[OM_ID_LEN];
	uint8_t pk[OM_PK_LEN];
	char name[24];
	bool online;
	time_t last_seen;
};

struct om_snet_peer {
	int fd;
	struct uloop_fd fdw;
	struct om_snet *s;
	uint8_t state;

	/* controller view of the agent */
	uint8_t agent_id[OM_ID_LEN];
	uint8_t agent_pk[OM_PK_LEN];

	struct om_ses_handshake hs;
	uint8_t n1[24], n2[24];
	uint8_t k_a2c[32], k_c2a[32];
	uint32_t tx_counter, rx_counter;

	uint8_t rbuf[OM_SES_MAX_FRAME * 2];
	size_t rlen;

	struct uloop_timeout timer;	/* handshake timeout or keepalive */
	time_t last_rx;
};

struct om_snet_cfg {
	bool is_controller;
	int listen_port;		/* controller */
	char ctrl_addr[48];		/* agent */
	int ctrl_port;			/* agent */
	bool join_open;			/* agent: accept GRANT */
	char state_dir[OM_PATH_MAX];
};

struct om_snet {
	struct om_snet_cfg cfg;
	struct om_identity *id;

	/* controller */
	int listen_fd;
	struct uloop_fd lfd;
	struct om_snet_peer peers[OM_SNET_MAX_PEERS];
	struct om_adopted_node adopted[OM_SNET_MAX_PEERS];
	unsigned n_adopted;
	struct {
		uint8_t id[OM_ID_LEN];
		char name[24];
	} pending[OM_SNET_MAX_PENDING];
	int n_pending;
	uint8_t network_key[32];
	bool have_network_key;

	/* agent */
	struct om_snet_peer agent;	/* connection to controller */
	struct uloop_timeout retry;
	unsigned backoff;		/* seconds */
	bool agent_adopted;		/* network.key present */
	uint8_t ctrl_pin[OM_ID_LEN];	/* pinned controller id */

	void (*on_adopted)(struct om_snet *s, const struct om_adopted_node *n,
			   void *priv);
	void *adopted_priv;
	void (*on_agent_state)(struct om_snet *s, bool online, void *priv);
	void *agent_priv;
};

int om_snet_init(struct om_snet *s, const struct om_snet_cfg *cfg,
		 struct om_identity *id);
void om_snet_done(struct om_snet *s);

/* Agent-side config transaction hook (mesh push target). */
void om_snet_set_netconf(struct om_snet *s, struct om_netconf *nc,
			 const char *state_dir);

/* Controller: push mesh config to all established peers. */
void om_snet_push_mesh(struct om_snet *s, const char *mesh_id,
		       const char *band, const char *key, const char *mode);

/* Controller: queue a discovered node for adoption (user action). */
int om_snet_adopt(struct om_snet *s, const uint8_t id[OM_ID_LEN],
		  const char *name);

/* Controller: drop an adopted node (removes persistence, closes link). */
int om_snet_revoke(struct om_snet *s, const uint8_t id[OM_ID_LEN]);

bool om_snet_is_adopted(struct om_snet *s, const uint8_t id[OM_ID_LEN]);

#endif /* OPENMESH_SESSION_NET_H */
