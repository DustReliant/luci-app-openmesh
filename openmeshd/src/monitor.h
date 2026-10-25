/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESH_MONITOR_H
#define OPENMESH_MONITOR_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define OM_MAX_LINKS 16
#define OM_MAX_STAS 32
#define OM_BATMAN_RETAIN_S 90	/* keep last-known batman links this long
				 * after a failed collection */

enum om_link_type {
	OM_LINK_WIFI = 0,	/* 802.11s peer station */
	OM_LINK_BATMAN,		/* batman-adv originator route */
	OM_LINK_WIRED,		/* fdb-derived wired neighbour (V0.3) */
};

struct om_link {
	uint8_t type;
	char ifname[16];
	char peer[20];		/* MAC (colon format) */
	int8_t rssi;		/* dBm, 0 = unknown */
	uint8_t tq;		/* batman TQ 0..255, 0 = unknown */
	uint32_t tx_rate;	/* kbit/s, 0 = unknown */
	char via[20];		/* batman nexthop MAC (==peer for direct) */
	time_t seen;
};

/* associated station on a local AP interface */
struct om_sta {
	char mac[20];		/* MAC (colon format) */
	char ifname[16];
	char ssid[33];		/* may be empty */
	int8_t rssi;		/* dBm, 0 = unknown */
	uint32_t rx_rate;	/* kbit/s, 0 = unknown */
	uint32_t tx_rate;	/* kbit/s, 0 = unknown */
	time_t seen;
};

struct om_monitor {
	struct om_link links[OM_MAX_LINKS];
	unsigned n_links;
	/* batman staging + retention (r21b): a failed collection must
	 * never report "links gone" -- last-known batman links are kept
	 * for OM_BATMAN_RETAIN_S and flagged stale via batman_collect */
	struct om_link batman_stage[OM_MAX_LINKS];
	unsigned n_batman_stage;
	time_t batman_last_ok;
	bool batman_failed;	/* collection health, NOT node health */
	unsigned batctl_timeout;	/* seconds, UCI-configurable */
	struct om_sta stas[OM_MAX_STAS];
	unsigned n_stas;
	time_t last_poll;
	char mesh_ifname[16];	/* last seen 802.11s iface, "" = none */
};

/*
 * Refresh the link table from iwinfo (mesh peers) and batctl
 * (originators). Safe to call when no mesh/batman exists: yields an
 * empty table. Target-only (shells out to iwinfo/batctl).
 */
void om_monitor_poll(struct om_monitor *m);

#endif /* OPENMESH_MONITOR_H */
