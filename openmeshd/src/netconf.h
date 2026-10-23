/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESH_NETCONF_H
#define OPENMESH_NETCONF_H

#include <stdbool.h>
#include <stdint.h>

#include <libubox/uloop.h>

#include "identity.h"

#define OM_TXN_TIMEOUT 30	/* seconds to confirm or auto-rollback */

struct om_netconf {
	bool active;
	bool committed;
	uint32_t generation;
	char bak_file[OM_PATH_MAX];
	char marker[OM_PATH_MAX];
	struct uloop_timeout timer;
	void (*on_done)(bool committed, uint32_t generation, void *priv);
	void *priv;
};

/*
 * One-shot transaction: snapshot /etc/config/wireless, add an
 * om-managed 802.11s interface (mode mesh, sae) on the chosen band,
 * commit and reload. If om_netconf_confirm() is not called within
 * OM_TXN_TIMEOUT seconds, everything is restored.
 * band: "5g" | "2g" | "auto". Returns 0 / -1.
 */
int om_netconf_mesh_apply(struct om_netconf *nc, const char *state_dir,
			  const char *mesh_id, const char *band,
			  const char *sae_key);

/* Keep the just-applied configuration. Returns 0 / -1. */
int om_netconf_confirm(struct om_netconf *nc);

/* Restore the snapshot immediately (also used by the deadline timer). */
int om_netconf_rollback(struct om_netconf *nc);

/*
 * Boot-time recovery: if a previous transaction never got confirmed
 * (power loss), restore its snapshot. Call before uloop_run.
 */
void om_netconf_recover(struct om_netconf *nc, const char *state_dir);

#endif /* OPENMESH_NETCONF_H */
