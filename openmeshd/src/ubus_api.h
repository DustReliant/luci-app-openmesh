/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESH_UBUS_API_H
#define OPENMESH_UBUS_API_H

#include <time.h>

#include <libubus.h>

#include "discovery.h"
#include "identity.h"

#define OM_UBUS_VERSION "0.0.1"

struct om_ubus {
	struct ubus_context *ctx;
	struct ubus_object obj;
	struct om_identity *id;
	struct om_disc *disc;
	struct timespec started;
};

/*
 * Connect to ubusd and register the "openmesh" object. Returns 0 on
 * success; -1 if ubusd is unavailable or the object name is taken (the
 * daemon keeps running without ubus either way).
 */
int om_ubus_init(struct om_ubus *u, struct om_identity *id,
		 struct om_disc *disc);
void om_ubus_done(struct om_ubus *u);

/* Emit openmesh.node.new for a freshly discovered node (best effort). */
void om_ubus_notify_node_new(struct om_ubus *u, const struct om_node_info *n);

#endif /* OPENMESH_UBUS_API_H */
