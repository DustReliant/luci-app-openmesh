// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Host tests for topology assembly (m18 Phase 3). Covers the mandated
 * matrix: two-node batman, single node, offline/re-online, link gone and
 * back, multiple originators, ethernet+batman mix, invalid rows, empty
 * topology -- plus stable ids, direction normalization, dedup and
 * foreign-peer synthesis.
 */
#include <stdio.h>
#include <string.h>

#include "topology.h"

static int fails;

#define CHECK(cond, name) do { \
	if (cond) printf("ok %s\n", name); \
	else { printf("FAIL %s\n", name); fails++; } \
} while (0)

static void mk_agent(struct om_topo_in_node *n, const char *id,
		     const char *mac, bool online)
{
	memset(n, 0, sizeof(*n));
	n->id = id;
	n->hostname = id;
	n->model = "Test Mesh Node";
	n->ip = "192.168.40.232";
	n->mac = mac;
	n->role = "agent";
	n->online = online;
	n->last_seen = 1000;
}

static void mk_ctrl(struct om_topo_in_node *n)
{
	memset(n, 0, sizeof(*n));
	n->id = "cccc0000ctrl";
	n->hostname = "controller";
	n->role = "controller";
	n->online = true;
	n->client_count = 2;
}

static void mk_link(struct om_link *l, enum om_link_type type,
		    const char *peer, const char *ifname, int tq, int rssi)
{
	memset(l, 0, sizeof(*l));
	l->type = type;
	snprintf(l->peer, sizeof(l->peer), "%s", peer);
	snprintf(l->ifname, sizeof(l->ifname), "%s", ifname);
	l->tq = (uint8_t)tq;
	l->rssi = (int8_t)rssi;
	l->seen = 1000;
}

int main(void)
{
	struct om_topo_in_node nodes[3];
	struct om_link links[3];
	struct om_topology t;

	/* 1+7. two managed nodes, controller + agent, batman link (TQ) */
	{
		mk_ctrl(&nodes[0]);
		mk_agent(&nodes[1], "aaaa1111node", "ec:2e:98:d2:71:28", true);
		mk_link(&links[0], OM_LINK_BATMAN,
			"EC:2E:98:D2:71:28", "phy0-mesh0", 231, 0);
		om_topology_build(nodes, 2, links, 1, 12345, &t);
		CHECK(t.n_nodes == 2, "two-nodes");
		CHECK(t.n_links == 1, "batman-link-count");
		CHECK(!strcmp(t.links[0].source, "cccc0000ctrl") &&
		      !strcmp(t.links[0].target, "aaaa1111node"),
		      "link-endpoints-are-node-ids");
		CHECK(t.links[0].tq == 231, "tq-carried");
		CHECK(strstr(t.links[0].id, "batman") &&
		      strstr(t.links[0].id, "phy0-mesh0"), "stable-link-id");
		/* direction-normalized id: smaller id first */
		CHECK(!strncmp(t.links[0].id, "aaaa1111node", 12),
		      "id-direction-normalized");
	}

	/* 2. single node, no links */
	{
		mk_ctrl(&nodes[0]);
		om_topology_build(nodes, 1, NULL, 0, 1, &t);
		CHECK(t.n_nodes == 1 && t.n_links == 0, "single-node");
	}

	/* 3+4. node offline then online again */
	{
		mk_ctrl(&nodes[0]);
		mk_agent(&nodes[1], "aaaa1111node", "ec:2e:98:d2:71:28", false);
		om_topology_build(nodes, 2, NULL, 0, 1, &t);
		CHECK(!strcmp(t.nodes[1].status, "offline"), "node-offline");
		mk_agent(&nodes[1], "aaaa1111node", "ec:2e:98:d2:71:28", true);
		om_topology_build(nodes, 2, NULL, 0, 1, &t);
		CHECK(!strcmp(t.nodes[1].status, "online"), "node-back-online");
	}

	/* 5+6. link disappears then recovers */
	{
		mk_ctrl(&nodes[0]);
		mk_agent(&nodes[1], "aaaa1111node", "ec:2e:98:d2:71:28", true);
		mk_link(&links[0], OM_LINK_BATMAN,
			"ec:2e:98:d2:71:28", "phy0-mesh0", 100, 0);
		om_topology_build(nodes, 2, links, 1, 1, &t);
		CHECK(t.n_links == 1, "link-up");
		om_topology_build(nodes, 2, NULL, 0, 2, &t);
		CHECK(t.n_links == 0, "link-gone");
		om_topology_build(nodes, 2, links, 1, 3, &t);
		CHECK(t.n_links == 1, "link-recovered");
	}

	/* 8. ethernet + batman mix, and duplicate dedup */
	{
		mk_ctrl(&nodes[0]);
		mk_agent(&nodes[1], "aaaa1111node", "ec:2e:98:d2:71:28", true);
		mk_link(&links[0], OM_LINK_WIFI,
			"EC:2E:98:D2:71:28", "phy0-mesh0", 0, -26);
		mk_link(&links[1], OM_LINK_BATMAN,
			"ec:2e:98:d2:71:28", "phy0-mesh0", 231, 0);
		mk_link(&links[2], OM_LINK_BATMAN,
			"EC:2E:98:D2:71:28", "phy0-mesh0", 200, 0); /* dup */
		om_topology_build(nodes, 2, links, 3, 1, &t);
		CHECK(t.n_links == 2, "wifi+batman-mix-dedup");
		CHECK(!strcmp(t.links[0].type, "80211s") &&
		      t.links[0].rssi == -26, "wifi-rssi");
		CHECK(!strcmp(t.links[1].type, "batman"), "batman-second");
	}

	/* 9. invalid link rows never produce garbage nodes/links */
	{
		mk_ctrl(&nodes[0]);
		mk_link(&links[0], OM_LINK_BATMAN, "", "phy0-mesh0", 9, 0);
		om_topology_build(nodes, 1, links, 1, 1, &t);
		CHECK(t.n_nodes == 1 && t.n_links == 0, "empty-peer-skipped");
	}

	/* P0 fix: BATMAN multi-hop (via != peer) must NOT create a link */
	{
		struct om_link multi;
		mk_link(&multi, OM_LINK_BATMAN, "03:04:05:06:07:08", "phy0-mesh0", 150, 0);
		snprintf(multi.via, sizeof(multi.via), "%s", "01:02:03:04:05:06"); /* nexthop != peer */
		mk_ctrl(&nodes[0]);
		mk_agent(&nodes[1], "aaaa1111node", "ec:2e:98:d2:71:28", true);
		om_topology_build(nodes, 2, &multi, 1, 1, &t);
		CHECK(t.n_links == 0, "multi-hop-batman-filtered");
		/* direct (via == peer) still passes */
		snprintf(multi.via, sizeof(multi.via), "%s", "03:04:05:06:07:08");
		om_topology_build(nodes, 2, &multi, 1, 1, &t);
		CHECK(t.n_links == 1, "direct-batman-kept");
	}

	/* 10. empty topology */
	{
		om_topology_build(NULL, 0, NULL, 0, 7, &t);
		CHECK(t.n_nodes == 0 && t.n_links == 0 && t.timestamp == 7,
		      "empty-topology");
	}

	/* foreign peer: live but unmanaged -> synthesized node, mac case
	 * insensitivity (iwinfo uppercase vs announce lowercase) */
	{
		mk_ctrl(&nodes[0]);
		mk_link(&links[0], OM_LINK_WIFI, "AA:BB:CC:DD:EE:FF", "phy0-mesh0", 0, -60);
		om_topology_build(nodes, 1, links, 1, 1, &t);
		CHECK(t.n_nodes == 2, "foreign-synthesized");
		CHECK(!strcmp(t.nodes[1].id, "mac-aa:bb:cc:dd:ee:ff"),
		      "foreign-stable-id");
		CHECK(!strcmp(t.links[0].target, "mac-aa:bb:cc:dd:ee:ff"),
		      "foreign-link-ref");
		/* managed node found despite case difference */
		mk_agent(&nodes[1], "aaaa1111node", "aa:bb:cc:dd:ee:ff", true);
		om_topology_build(nodes, 2, links, 1, 2, &t);
		CHECK(t.n_nodes == 2 && !strcmp(t.links[0].target, "aaaa1111node"),
		      "mac-case-insensitive-bind");
	}

	/* regression (r21 live incident): full 64-hex node ids must not
	 * truncate-distinct link ids into collisions */
	{
		struct om_topo_in_node cn, an;
		struct om_link bl;
		char id64[65];

		memset(id64, 'a', 64);
		id64[64] = 0;
		memset(&cn, 0, sizeof(cn));
		memset(&an, 0, sizeof(an));
		cn.id = "c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00";
		cn.role = "controller"; cn.online = true;
		an.id = id64; an.role = "agent"; an.mac = "ec:2e:98:d2:71:28";
		an.online = true;
		mk_link(&bl, OM_LINK_WIFI, "EC:2E:98:D2:71:28", "phy0-mesh0", 0, -26);
		om_topology_build(&cn, 1, NULL, 0, 1, &t); /* need both nodes */
		{
			struct om_topo_in_node pair[2] = { cn, an };
			struct om_link two[2];

			mk_link(&two[0], OM_LINK_WIFI, "EC:2E:98:D2:71:28", "phy0-mesh0", 0, -26);
			mk_link(&two[1], OM_LINK_BATMAN, "ec:2e:98:d2:71:28", "", 231, 0);
			om_topology_build(pair, 2, two, 2, 1, &t);
			CHECK(t.n_links == 2, "full-length-id-no-collision");
			CHECK(strlen(t.links[0].id) < sizeof(t.links[0].id) &&
			      strlen(t.links[1].id) < sizeof(t.links[1].id),
			      "link-ids-not-truncated");
			CHECK(strcmp(t.links[0].id, t.links[1].id) != 0,
			      "distinct-link-ids");
		}
	}

	printf(fails ? "test_topology: %d FAILURES\n"
		     : "test_topology: all ok\n", fails);
	return fails ? 1 : 0;
}
