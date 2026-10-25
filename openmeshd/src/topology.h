/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESH_TOPOLOGY_H
#define OPENMESH_TOPOLOGY_H

/*
 * Unified topology model (m18, Phase 3). Everything the LuCI topology
 * view consumes flows through these structs -- the frontend never sees
 * batctl/iwinfo output or formats.
 *
 * Layering:  batctl/iwinfo -> parser (batman_parse/monitor) -> om_link
 *            -> om_topology_build() -> Node/Link -> ubus "topology".
 *
 * Design rules (docs/design/m18-topology-model.md):
 *  - Node.id / Link.id are STABLE identities, never regenerated per call
 *    and never the bare hostname (hostnames change). Node.id = hex
 *    node_id (Ed25519-derived, r18-verified identity); Link.id derives
 *    from the normalized (source,target,type,interface) tuple so an
 *    undirected link has exactly one identity in both directions.
 *  - Network state ONLY. UI layout (x, y, zoom, ...) lives in a separate
 *    TopologyLayout persisted independently (Phase 10); these structs
 *    must never grow coordinate fields.
 *  - Metric fields use 0 as "unknown" sentinel throughout.
 */

#include <stdint.h>
#include <stdbool.h>

#include "monitor.h"

#define OM_TOPO_MAX_NODES  16
#define OM_TOPO_MAX_LINKS  32
#define OM_TOPO_ID_LEN     80	/* node id: 64-hex + slack */
/* link id = norm(src)|norm(dst)|type|iface: two 64-hex node ids plus
 * type and interface need ~150 bytes -- r21 live incident: an 80-byte
 * buffer truncated two distinct link ids into one and the dedup
 * silently dropped every link after the first */
#define OM_TOPO_LINKID_LEN 160
#define OM_TOPO_HOST_LEN   33
#define OM_TOPO_MODEL_LEN  33
#define OM_TOPO_IF_LEN     16
#define OM_TOPO_MAC_LEN    18
#define OM_TOPO_IP_LEN     16

/* --- unified entities (output) --- */

struct om_topo_node {
	char id[OM_TOPO_ID_LEN];	/* stable: hex node_id or mac-<mac> */
	char hostname[OM_TOPO_HOST_LEN];	/* adopted name */
	char model[OM_TOPO_MODEL_LEN];
	char ip[OM_TOPO_IP_LEN];
	char mac[OM_TOPO_MAC_LEN];
	char role[12];		/* controller | agent | unknown */
	char status[8];		/* online | offline */
	uint32_t uptime;	/* seconds, 0 = unknown */
	uint32_t client_count;	/* local AP clients, 0 = unknown */
	uint32_t last_seen;	/* unix time, 0 = unknown */
	/* reserved for later phases (filled by agent telemetry):
	 * cpu load, memory, temperature -- model stays additive */
};

struct om_topo_link {
	char id[OM_TOPO_LINKID_LEN];	/* norm(src)|norm(dst)|type|iface */
	char source[OM_TOPO_ID_LEN];	/* om_topo_node.id refs */
	char target[OM_TOPO_ID_LEN];	/* om_topo_node.id refs */
	char type[10];		/* wifi | batman | ethernet | 80211s | unknown */
	char status[8];		/* up | down */
	char interface[OM_TOPO_IF_LEN];
	uint8_t tq;		/* batman, 0 = n/a */
	int8_t rssi;		/* dBm, 0 = unknown */
	uint32_t tx_rate;	/* kbit/s, 0 = unknown */
	uint32_t rx_rate;
	uint32_t last_seen;
};

struct om_topology {
	struct om_topo_node nodes[OM_TOPO_MAX_NODES];
	unsigned n_nodes;
	struct om_topo_link links[OM_TOPO_MAX_LINKS];
	unsigned n_links;
	uint32_t timestamp;	/* unix time of build */
};

/* --- neutral inputs (assembly is pure; callers translate their own
 *     daemon structs so this unit stays host-testable, no libubox) --- */

struct om_topo_in_node {
	const char *id;		/* hex node_id (stable) */
	const char *hostname;	/* adopted/display name, may be "" */
	const char *model;
	const char *ip;
	const char *mac;	/* announced mac (mesh iface first) */
	const char *role;	/* controller | agent */
	bool online;
	uint32_t uptime;
	uint32_t client_count;
	uint32_t last_seen;
};

/*
 * Build a topology snapshot. Links are attached to the controller-side
 * node of each input row; peer MACs are resolved against input nodes,
 * unresolved peers are synthesized as placeholder nodes (id "mac-<mac>",
 * role unknown) so every link references a real node. Returns 0.
 */
int om_topology_build(const struct om_topo_in_node *nodes, unsigned n_nodes,
		      const struct om_link *links, unsigned n_links,
		      uint32_t now, struct om_topology *out);

#endif /* OPENMESH_TOPOLOGY_H */
