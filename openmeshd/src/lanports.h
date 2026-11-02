/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESHD_LANPORTS_H
#define OPENMESHD_LANPORTS_H

#include <stdbool.h>
#include <stddef.h>

/* selection spec for the wired backhaul leg (m19) */
#define OM_PORTS_ALL "all"
#define OM_PORT_NAME_MAX 15	/* Linux IFNAMSIZ - 1 */
#define OM_PORTS_SPEC_MAX 63	/* bound for env transport */

/*
 * "" or NULL  = no wired leg (default, fully backward compatible)
 * "all"       = every current br-lan physical port
 * "lan1,lan2" = named ports become batman-adv hardifs
 *
 * Port names are restricted to [A-Za-z0-9_] so they are safe as
 * environment values, shell words and uci section names; the spec
 * never enters a command string directly.
 */
bool om_lan_ports_valid(const char *spec);
bool om_port_name_valid(const char *name, size_t len);

/* Iterate comma-separated segments (no validation). Returns count. */
int om_lan_ports_each(const char *spec,
		      void (*cb)(const char *port, size_t len, void *priv),
		      void *priv);

#endif
