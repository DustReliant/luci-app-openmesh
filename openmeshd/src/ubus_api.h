/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESH_UBUS_API_H
#define OPENMESH_UBUS_API_H

#include <time.h>

#include <libubus.h>

#include "discovery.h"
#include "identity.h"

struct om_snet;

#define OM_UBUS_VERSION "0.0.1"

struct om_ubus {
	struct ubus_context *ctx;
	struct ubus_object obj;
	struct om_identity *id;
	struct om_disc *disc;
	struct om_snet *snet;	/* may be NULL during early startup */
	struct om_netconf *netconf;
	char state_dir[OM_PATH_MAX];
	struct timespec started;
};

struct om_adopted_node;
struct om_netconf;

/*
 * Connect to ubusd and register the "openmesh" object. Returns 0 on
 * success; -1 if ubusd is unavailable or the object name is taken (the
 * daemon keeps running without ubus either way).
 */
int om_ubus_init(struct om_ubus *u, struct om_identity *id,
		 struct om_disc *disc);
void om_ubus_done(struct om_ubus *u);

/* Attach the session layer once it is up (adds adopt/pending methods). */
void om_ubus_set_snet(struct om_ubus *u, struct om_snet *snet);

/* Attach the config transaction engine (adds mesh_create etc.). */
void om_ubus_set_netconf(struct om_ubus *u, struct om_netconf *nc,
			 const char *state_dir);

/* Emit openmesh.node.new for a freshly discovered node (best effort). */
void om_ubus_notify_node_new(struct om_ubus *u, const struct om_node_info *n);

/* Emit openmesh.node.adopted (best effort). */
void om_ubus_notify_adopted(struct om_ubus *u,
			    const struct om_adopted_node *a);

/* Emit openmesh.config.applied / openmesh.config.rolledback. */
void om_ubus_notify_txn(struct om_ubus *u, bool committed);

#endif /* OPENMESH_UBUS_API_H */
