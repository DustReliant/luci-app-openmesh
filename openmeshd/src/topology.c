// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Topology assembly: neutral inputs -> unified Node/Link snapshot.
 * Pure C, no daemon dependencies -- all policy lives here so it can be
 * exhaustively host-tested (tests/test_topology.c).
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>

#include "topology.h"

static void node_set(struct om_topo_node *n, const struct om_topo_in_node *in)
{
	memset(n, 0, sizeof(*n));
	snprintf(n->id, sizeof(n->id), "%s", in->id);
	snprintf(n->hostname, sizeof(n->hostname), "%s",
		 (in->hostname && *in->hostname) ? in->hostname : in->id);
	snprintf(n->model, sizeof(n->model), "%s", in->model ?: "");
	snprintf(n->ip, sizeof(n->ip), "%s", in->ip ?: "");
	snprintf(n->mac, sizeof(n->mac), "%s", in->mac ?: "");
	snprintf(n->role, sizeof(n->role), "%s", in->role ?: "unknown");
	snprintf(n->status, sizeof(n->status), "%s", in->online ? "online" : "offline");
	n->uptime = in->uptime;
	n->client_count = in->client_count;
	n->last_seen = in->last_seen;
}

static struct om_topo_node *node_by_mac(struct om_topology *t, const char *mac)
{
	for (unsigned i = 0; i < t->n_nodes; i++)
		if (!strcasecmp(t->nodes[i].mac, mac))
			return &t->nodes[i];
	return NULL;
}

static struct om_topo_node *node_by_id(struct om_topology *t, const char *id)
{
	for (unsigned i = 0; i < t->n_nodes; i++)
		if (!strcmp(t->nodes[i].id, id))
			return &t->nodes[i];
	return NULL;
}

static struct om_topo_node *node_add(struct om_topology *t,
				     const struct om_topo_in_node *in)
{
	if (t->n_nodes >= OM_TOPO_MAX_NODES || !in->id || !*in->id)
		return NULL;
	if (node_by_id(t, in->id))
		return NULL;	/* stable ids: duplicates dropped */
	node_set(&t->nodes[t->n_nodes], in);
	return &t->nodes[t->n_nodes++];
}

/* synthesise a placeholder for a live but unmanaged peer so every link
 * references a real node (frontend never binds raw MACs). The id is
 * case-normalized: MAC case varies across sources (iwinfo uppercase,
 * batctl lowercase) and identities must not. */
static struct om_topo_node *foreign_add(struct om_topology *t, const char *mac)
{
	struct om_topo_in_node f;
	char id[OM_TOPO_ID_LEN], low[OM_TOPO_MAC_LEN];
	size_t i;

	if (t->n_nodes >= OM_TOPO_MAX_NODES)
		return NULL;
	for (i = 0; mac[i] && i < sizeof(low) - 1; i++)
		low[i] = (char)tolower((unsigned char)mac[i]);
	low[i] = '\0';
	snprintf(id, sizeof(id), "mac-%s", low);
	if (node_by_id(t, id))
		return node_by_id(t, id);
	memset(&f, 0, sizeof(f));
	f.id = id;
	f.hostname = (char *)mac;
	f.model = "";
	f.mac = (char *)mac;
	f.role = "unknown";
	f.online = true;	/* refined by caller via
				 * om_topo_in_node.collect_health */
	f.last_seen = t->timestamp;
	return node_add(t, &f);
}

static const char *link_type_str(enum om_link_type t)
{
	switch (t) {
	case OM_LINK_WIFI:
		return "80211s";
	case OM_LINK_BATMAN:
		return "batman";
	case OM_LINK_WIRED:
		return "ethernet";
	}
	return "unknown";
}

/* undirected identity: lexicographic normalization makes A->B and B->A
 * the same link (stable, direction-independent) */
static void link_id(char *out, size_t sz, const char *a, const char *b,
		    const char *type, const char *iface)
{
	int hi = strcmp(a, b) > 0;

	snprintf(out, sz, "%s|%s|%s|%s", hi ? b : a, hi ? a : b,
		 type, iface);
}

static bool link_exists(struct om_topology *t, const char *id)
{
	for (unsigned i = 0; i < t->n_links; i++)
		if (!strcmp(t->links[i].id, id))
			return true;
	return false;
}

int om_topology_build(const struct om_topo_in_node *nodes, unsigned n_nodes,
		      const struct om_link *links, unsigned n_links,
		      uint32_t now, struct om_topology *out)
{
	const struct om_topo_node *ctrl = NULL;

	memset(out, 0, sizeof(*out));
	out->timestamp = now;

	for (unsigned i = 0; i < n_nodes; i++)
		node_add(out, &nodes[i]);

	/* links are observed from the controller: source = the controller
	 * node (first input marked controller), target = resolved peer */
	for (unsigned i = 0; i < out->n_nodes; i++)
		if (!strcmp(out->nodes[i].role, "controller")) {
			ctrl = &out->nodes[i];
			break;
		}

	for (unsigned i = 0; i < n_links && out->n_links < OM_TOPO_MAX_LINKS;
	     i++) {
		const struct om_link *l = &links[i];
		struct om_topo_link *tl;
		struct om_topo_node *peer;
		char id[OM_TOPO_LINKID_LEN];
		const char *type = link_type_str(l->type);

		if (!ctrl)
			break;
		if (!l->peer[0])
			continue;	/* no peer identity: skip, never guess */
		/* P0 fix (Phase 4 audit #1): BATMAN originators with
		 * nexthop != originator are multi-hop ROUTES, not direct
		 * physical links -- must not appear as topology edges */
		if (l->type == OM_LINK_BATMAN &&
		    l->via[0] && strcmp(l->via, l->peer) != 0)
			continue;
		peer = node_by_mac(out, l->peer);
		if (!peer)
			peer = foreign_add(out, l->peer);
		if (!peer)
			continue;
		link_id(id, sizeof(id), ctrl->id, peer->id, type, l->ifname);
		if (link_exists(out, id))
			continue;

		tl = &out->links[out->n_links++];
		snprintf(tl->id, sizeof(tl->id), "%s", id);
		snprintf(tl->source, sizeof(tl->source), "%s", ctrl->id);
		snprintf(tl->target, sizeof(tl->target), "%s", peer->id);
		snprintf(tl->type, sizeof(tl->type), "%s", type);
		snprintf(tl->status, sizeof(tl->status), "up");
		snprintf(tl->interface, sizeof(tl->interface), "%s",
			 l->ifname);
		tl->tq = l->tq;
		tl->rssi = l->rssi;
		tl->tx_rate = l->tx_rate;
		tl->last_seen = l->seen;
	}
	return 0;
}
