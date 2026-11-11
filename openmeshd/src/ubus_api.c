// SPDX-License-Identifier: GPL-3.0-or-later
/* openmeshd - "openmesh" ubus object and events. */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/ulog.h>
#include <libubox/utils.h>

#include <sodium.h>

#include "ubus_api.h"
#include "session_net.h"
#include "netconf.h"
#include "store.h"
#include "monitor.h"
#include "topology.h"
#include "lanports.h"
#include "ports.h"
#include "discovery.h"


static struct blob_buf b;

static const struct blobmsg_policy om_adopt_policy[2] = {
	{ "name", BLOBMSG_TYPE_STRING },
	{ "id", BLOBMSG_TYPE_STRING },
};

static const struct blobmsg_policy om_mesh_policy[5] = {
	{ "mesh_name", BLOBMSG_TYPE_STRING },
	{ "band", BLOBMSG_TYPE_STRING },
	{ "key", BLOBMSG_TYPE_STRING },
	{ "mode", BLOBMSG_TYPE_STRING },
	{ "lan_ports", BLOBMSG_TYPE_STRING },
};

static const struct blobmsg_policy om_roam_policy[4] = {
	{ "k", BLOBMSG_TYPE_INT8 },
	{ "v", BLOBMSG_TYPE_INT8 },
	{ "r", BLOBMSG_TYPE_INT8 },
	{ "domain", BLOBMSG_TYPE_STRING },
};

static const struct blobmsg_policy om_portplan_policy[2] = {
	{ "device", BLOBMSG_TYPE_STRING },
	{ "mode", BLOBMSG_TYPE_STRING },
};

static const struct blobmsg_policy om_portapply_policy[2] = {
	{ "device", BLOBMSG_TYPE_STRING },
	{ "ack_unknown", BLOBMSG_TYPE_INT8 },
};

/* m22-2: optional transaction identity for confirm/rollback */
static const struct blobmsg_policy om_txn_policy[1] = {
	{ "gen", BLOBMSG_TYPE_INT32 },
};

static void node_to_blob(const struct om_node_info *n)
{
	char hex[OM_ID_HEX_LEN];
	void *t = blobmsg_open_table(&b, NULL);

	om_id_to_hex(n->id, hex);
	blobmsg_add_string(&b, "id", hex);
	blobmsg_add_string(&b, "role", n->role);
	blobmsg_add_string(&b, "model", n->model);
	blobmsg_add_string(&b, "fw", n->fw);
	if (n->mac[0])
		blobmsg_add_string(&b, "mac", n->mac);
	blobmsg_add_string(&b, "addr", n->addr);
	blobmsg_add_u32(&b, "first_seen", (uint32_t)n->first_seen);
	blobmsg_add_u32(&b, "last_seen", (uint32_t)n->last_seen);
	blobmsg_close_table(&b, t);
}

#define OM_HANDLER_CTX(obj) container_of(obj, struct om_ubus, obj)

static int om_status(struct ubus_context *ctx, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method,
		     struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct timespec now;
	char hex[OM_ID_HEX_LEN];

	(void)method;
	(void)msg;

	clock_gettime(CLOCK_MONOTONIC, &now);
	om_id_to_hex(u->id->id, hex);

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "version", OM_UBUS_VERSION);
	blobmsg_add_string(&b, "role", u->disc->cfg.role);
	blobmsg_add_string(&b, "node_id", hex);
	blobmsg_add_u32(&b, "uptime",
			(uint32_t)(now.tv_sec - u->started.tv_sec));
	blobmsg_add_u32(&b, "nodes_total", u->disc->n_nodes);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_list_nodes(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	void *a;

	(void)method;
	(void)msg;

	blob_buf_init(&b, 0);
	a = blobmsg_open_array(&b, "nodes");
	for (unsigned i = 0; i < u->disc->n_nodes; i++)
		node_to_blob(&u->disc->nodes[i]);
	blobmsg_close_array(&b, a);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_health(struct ubus_context *ctx, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method,
		     struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);

	(void)method;
	(void)msg;

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", 1);
	blobmsg_add_string(&b, "identity", "ok");
	blobmsg_add_string(&b, "discovery",
			   u->disc->fd >= 0 ? "ok" : "failed");
	blobmsg_add_string(&b, "session", u->snet ? "ok" : "off");
	return ubus_send_reply(ctx, req, b.head);
}

static int om_list_pending(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	void *a;

	(void)method;
	(void)msg;
	blob_buf_init(&b, 0);
	a = blobmsg_open_array(&b, "nodes");
	if (u->snet)
		for (unsigned i = 0; i < u->disc->n_nodes; i++)
			if (!om_snet_is_adopted(u->snet, u->disc->nodes[i].id))
				node_to_blob(&u->disc->nodes[i]);
	blobmsg_close_array(&b, a);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_list_adopted(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	void *a;

	(void)method;
	(void)msg;
	blob_buf_init(&b, 0);
	a = blobmsg_open_array(&b, "nodes");
	if (u->snet)
		for (unsigned i = 0; i < u->snet->n_adopted; i++) {
			const struct om_adopted_node *n = &u->snet->adopted[i];
			char hex[OM_ID_HEX_LEN];
			void *t = blobmsg_open_table(&b, NULL);

			om_id_to_hex(n->id, hex);
			blobmsg_add_string(&b, "id", hex);
			blobmsg_add_string(&b, "name", n->name);
			blobmsg_add_u8(&b, "online", n->online);
			blobmsg_add_u32(&b, "last_seen", (uint32_t)n->last_seen);
			blobmsg_close_table(&b, t);
		}
	blobmsg_close_array(&b, a);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_adopt(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct blob_attr *tb[2] = { NULL, NULL };
	uint8_t id[OM_ID_LEN];
	const char *idstr, *name = NULL;
	bool seen = false;

	(void)method;
	if (!u->snet)
		return UBUS_STATUS_NOT_FOUND;
	blobmsg_parse(om_adopt_policy, 2, tb, blob_data(msg), blob_len(msg));
	if (!tb[1] || !(idstr = blobmsg_get_string(tb[1])) ||
	    om_id_from_hex(idstr, id) < 0)
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (tb[0])
		name = blobmsg_get_string(tb[0]);

	for (unsigned i = 0; i < u->disc->n_nodes; i++)
		if (!memcmp(u->disc->nodes[i].id, id, OM_ID_LEN))
			seen = true;
	if (!seen)
		return UBUS_STATUS_NOT_FOUND;

	if (om_snet_adopt(u->snet, id, name) < 0)
		return UBUS_STATUS_UNKNOWN_ERROR;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result", "queued");
	return ubus_send_reply(ctx, req, b.head);
}

static int om_revoke(struct ubus_context *ctx, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method,
		     struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct blob_attr *tb[2] = { NULL, NULL };
	uint8_t id[OM_ID_LEN];
	const char *idstr;

	(void)method;
	if (!u->snet)
		return UBUS_STATUS_NOT_FOUND;
	blobmsg_parse(om_adopt_policy, 2, tb, blob_data(msg), blob_len(msg));
	if (!tb[1] || !(idstr = blobmsg_get_string(tb[1])) ||
	    om_id_from_hex(idstr, id) < 0)
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (om_snet_revoke(u->snet, id) < 0)
		return UBUS_STATUS_NOT_FOUND;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result", "removed");
	return ubus_send_reply(ctx, req, b.head);
}

static void gen_key(char out[13])
{
	static const char alphabet[] =
		"abcdefghjkmnpqrstuvwxyz23456789";
	uint8_t rnd[12];

	randombytes_buf(rnd, sizeof(rnd));
	for (size_t i = 0; i < sizeof(rnd); i++)
		out[i] = alphabet[rnd[i] % (sizeof(alphabet) - 1)];
	out[sizeof(rnd)] = '\0';
}

/* m19: named lan ports must exist here (typo guard); "all"/"" skip this */
struct om_port_chk {
	bool ok;
};

static void port_exists_cb(const char *port, size_t len, void *priv)
{
	struct om_port_chk *c = priv;
	char name[OM_PORT_NAME_MAX + 1];
	char path[48];
	struct stat st;

	if (len == 0 || len >= sizeof(name)) {
		c->ok = false;
		return;
	}
	memcpy(name, port, len);
	name[len] = '\0';
	snprintf(path, sizeof(path), "/sys/class/net/%s", name);
	if (stat(path, &st) < 0) {
		ULOG_WARN("mesh_create: lan port '%s' not present\n", name);
		c->ok = false;
	}
}

static int om_mesh_create(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct blob_attr *tb[5] = { NULL, NULL, NULL, NULL, NULL };
	struct om_port_chk chk = { .ok = true };
	const char *name, *band, *key, *mode, *lan_ports;
	int bh_mode = OM_BH_WIFI;
	char gen[13];

	(void)method;
	if (!u->snet || !u->netconf)
		return UBUS_STATUS_NOT_FOUND;
	blobmsg_parse(om_mesh_policy, 5, tb, blob_data(msg), blob_len(msg));
	name = tb[0] ? blobmsg_get_string(tb[0]) : NULL;
	band = tb[1] ? blobmsg_get_string(tb[1]) : "5g";
	key = tb[2] ? blobmsg_get_string(tb[2]) : "";
	mode = tb[3] ? blobmsg_get_string(tb[3]) : "wifi";
	lan_ports = tb[4] ? blobmsg_get_string(tb[4]) : "";
	if (!name || !*name)
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (!strcmp(mode, "hybrid"))
		bh_mode = OM_BH_HYBRID;
	if (!om_lan_ports_valid(lan_ports))
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (bh_mode == OM_BH_HYBRID && *lan_ports &&
	    strcmp(lan_ports, OM_PORTS_ALL)) {
		om_lan_ports_each(lan_ports, port_exists_cb, &chk);
		if (!chk.ok)
			return UBUS_STATUS_INVALID_ARGUMENT;
	}
	if (!*key) {
		gen_key(gen);
		key = gen;
	}
	if (om_store_backhaul_set(name, key, band, lan_ports) < 0)
		ULOG_WARN("mesh_create: persisting backhaul failed\n");
	if (om_netconf_mesh_apply(u->netconf, u->state_dir, name, band, key,
				  bh_mode, lan_ports) < 0)
		return UBUS_STATUS_UNKNOWN_ERROR;
	om_snet_push_mesh(u->snet, name, band, key, mode, lan_ports);

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result", "applied");
	blobmsg_add_string(&b, "key", key);
	blobmsg_add_u32(&b, "confirm_window", 30);
	blobmsg_add_u32(&b, "generation", u->netconf->generation);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_mesh_join(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	char mesh_id[33], key[33], band[8], lan_ports[OM_PORTS_SPEC_MAX + 1];

	(void)method;
	(void)msg;
	if (!u->netconf)
		return UBUS_STATUS_NOT_FOUND;
	if (om_store_backhaul_get(mesh_id, sizeof(mesh_id), key,
				  sizeof(key), band, sizeof(band),
				  lan_ports, sizeof(lan_ports)) < 0 ||
	    !*mesh_id)
		return UBUS_STATUS_NOT_FOUND;
	if (om_netconf_mesh_apply(u->netconf, u->state_dir, mesh_id,
				  *band ? band : "5g", key,
				  OM_BH_WIFI, lan_ports) < 0)
		return UBUS_STATUS_UNKNOWN_ERROR;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result", "applied");
	return ubus_send_reply(ctx, req, b.head);
}

static int om_apply_confirm(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req, const char *method,
			    struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct blob_attr *tb[1] = { NULL };
	bool has_gen = false;
	uint32_t gen = 0;

	(void)method;
	blobmsg_parse(om_txn_policy, 1, tb, blob_data(msg), blob_len(msg));
	if (tb[0]) {
		has_gen = true;
		gen = blobmsg_get_u32(tb[0]);
	}
	if (!u->netconf ||
	    om_netconf_confirm(u->netconf, gen, has_gen) < 0)
		return UBUS_STATUS_NOT_FOUND;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result", "confirmed");
	blobmsg_add_u32(&b, "generation", u->netconf->generation);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_apply_rollback(struct ubus_context *ctx, struct ubus_object *obj,
			     struct ubus_request_data *req,
			     const char *method, struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct blob_attr *tb[1] = { NULL };
	bool has_gen = false;
	uint32_t gen = 0;

	(void)method;
	blobmsg_parse(om_txn_policy, 1, tb, blob_data(msg), blob_len(msg));
	if (tb[0]) {
		has_gen = true;
		gen = blobmsg_get_u32(tb[0]);
	}
	if (!u->netconf ||
	    om_netconf_rollback(u->netconf, gen, has_gen) < 0)
		return UBUS_STATUS_NOT_FOUND;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result", "rolled-back");
	blobmsg_add_u32(&b, "generation", u->netconf->generation);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_get_links(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	void *a;

	(void)method;
	(void)msg;
	blob_buf_init(&b, 0);
	/* r21b: collection health -- distinguishes COLLECT_FAILED from real
	 * link loss. ok = fresh batman data; stale = collect failed, rows
	 * retained (<90s); failed = collect failed, retention expired.
	 * Absent when no monitor is attached. */
	if (u->mon) {
		const char *h = "ok";

		if (u->mon->batman_failed)
			h = (time(NULL) - u->mon->batman_last_ok <=
			     OM_BATMAN_RETAIN_S) ? "stale" : "failed";
		blobmsg_add_string(&b, "batman_collect", h);
	}
	a = blobmsg_open_array(&b, "links");
	if (u->mon)
		for (unsigned i = 0; i < u->mon->n_links; i++) {
			const struct om_link *l = &u->mon->links[i];
			void *t = blobmsg_open_table(&b, NULL);

			blobmsg_add_string(&b, "type",
				l->type == OM_LINK_WIFI ? "wifi" :
				l->type == OM_LINK_BATMAN ? "batman" :
				"wired");
			if (l->ifname[0])
				blobmsg_add_string(&b, "ifname", l->ifname);
			blobmsg_add_string(&b, "peer", l->peer);
			if (l->rssi)
				blobmsg_add_u32(&b, "rssi",
						(uint32_t)l->rssi);
			if (l->tx_rate)
				blobmsg_add_u32(&b, "rate_kbps",
						l->tx_rate);
			if (l->via[0])
				blobmsg_add_string(&b, "via", l->via);
			blobmsg_close_table(&b, t);
		}
	blobmsg_close_array(&b, a);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_get_clients(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	void *a;

	(void)method;
	(void)msg;
	blob_buf_init(&b, 0);
	a = blobmsg_open_array(&b, "clients");
	if (u->mon)
		for (unsigned i = 0; i < u->mon->n_stas; i++) {
			const struct om_sta *s = &u->mon->stas[i];
			void *t = blobmsg_open_table(&b, NULL);

			blobmsg_add_string(&b, "mac", s->mac);
			blobmsg_add_string(&b, "ifname", s->ifname);
			if (s->ssid[0])
				blobmsg_add_string(&b, "ssid", s->ssid);
			if (s->rssi)
				blobmsg_add_u32(&b, "rssi",
						(uint32_t)s->rssi);
			if (s->rx_rate)
				blobmsg_add_u32(&b, "rx_kbps", s->rx_rate);
			if (s->tx_rate)
				blobmsg_add_u32(&b, "tx_kbps", s->tx_rate);
			blobmsg_close_table(&b, t);
		}
	blobmsg_close_array(&b, a);
	return ubus_send_reply(ctx, req, b.head);
}

/*
 * m18 Phase 3: unified topology snapshot. Assembles neutral inputs from
 * identity (controller), adopted store + discovery (agents) and the
 * monitor link table, delegates to the pure om_topology_build() and
 * serializes. Frontends consume this -- never batctl/iwinfo output.
 */
static int om_topology(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct om_topo_in_node in[OM_TOPO_MAX_NODES];
	struct om_topology topo;
	char ctrl_hex[OM_ID_HEX_LEN];
	struct timespec now;
	unsigned n_in = 0;
	void *an, *al;

	(void)method;
	(void)msg;
	memset(in, 0, sizeof(in));
	memset(&topo, 0, sizeof(topo));
	clock_gettime(CLOCK_MONOTONIC, &now);
	om_id_to_hex(u->id->id, ctrl_hex);

	/* controller self */
	in[n_in].id = ctrl_hex;
	in[n_in].hostname = "controller";
	in[n_in].role = "controller";
	in[n_in].online = true;
	in[n_in].uptime = (uint32_t)(now.tv_sec - u->started.tv_sec);
	in[n_in].client_count = u->mon ? u->mon->n_stas : 0;
	n_in++;

	/* adopted agents, enriched from the discovery table (model/ip/mac) */
	if (u->snet)
		for (unsigned i = 0; i < u->snet->n_adopted &&
				    n_in < OM_TOPO_MAX_NODES; i++) {
			const struct om_adopted_node *a = &u->snet->adopted[i];
			char hex[OM_ID_HEX_LEN];

			om_id_to_hex(a->id, hex);
			in[n_in].id = strdup(hex);
			in[n_in].hostname = a->name;
			in[n_in].role = "agent";
			in[n_in].online = a->online;
			in[n_in].last_seen = (uint32_t)a->last_seen;
			for (unsigned d = 0; d < u->disc->n_nodes; d++)
				if (!memcmp(u->disc->nodes[d].id, a->id,
					    OM_ID_LEN)) {
					const struct om_node_info *nd =
						&u->disc->nodes[d];

					in[n_in].model = nd->model;
					in[n_in].ip = nd->addr;
					in[n_in].mac = nd->mac;
					break;
				}
			n_in++;
		}

	om_topology_build(in, n_in, u->mon ? u->mon->links : NULL,
			  u->mon ? u->mon->n_links : 0, time(NULL), &topo);
	for (unsigned i = 1; i < n_in; i++)
		free((void *)in[i].id);

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "timestamp", topo.timestamp);
	/* Phase 4 audit #3: propagate collection health so the frontend
	 * can distinguish fresh data from stale retention */
	if (u->mon) {
		const char *h = "ok";

		if (u->mon->batman_failed)
			h = (time(NULL) - u->mon->batman_last_ok <=
			     OM_BATMAN_RETAIN_S) ? "stale" : "failed";
		blobmsg_add_string(&b, "batman_collect", h);
	}
	an = blobmsg_open_array(&b, "nodes");
	for (unsigned i = 0; i < topo.n_nodes; i++) {
		const struct om_topo_node *n = &topo.nodes[i];
		void *t = blobmsg_open_table(&b, NULL);

		blobmsg_add_string(&b, "id", n->id);
		blobmsg_add_string(&b, "hostname", n->hostname);
		if (n->model[0])
			blobmsg_add_string(&b, "model", n->model);
		if (n->ip[0])
			blobmsg_add_string(&b, "ip", n->ip);
		if (n->mac[0])
			blobmsg_add_string(&b, "mac", n->mac);
		blobmsg_add_string(&b, "role", n->role);
		blobmsg_add_string(&b, "status", n->status);
		if (n->uptime)
			blobmsg_add_u32(&b, "uptime", n->uptime);
		if (n->client_count)
			blobmsg_add_u32(&b, "client_count", n->client_count);
		if (n->last_seen)
			blobmsg_add_u32(&b, "last_seen", n->last_seen);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, an);
	al = blobmsg_open_array(&b, "links");
	for (unsigned i = 0; i < topo.n_links; i++) {
		const struct om_topo_link *l = &topo.links[i];
		void *t = blobmsg_open_table(&b, NULL);

		blobmsg_add_string(&b, "id", l->id);
		blobmsg_add_string(&b, "source", l->source);
		blobmsg_add_string(&b, "target", l->target);
		blobmsg_add_string(&b, "type", l->type);
		blobmsg_add_string(&b, "status", l->status);
		if (l->interface[0])
			blobmsg_add_string(&b, "interface", l->interface);
		if (l->tq)
			blobmsg_add_u32(&b, "tq", l->tq);
		if (l->rssi)
			blobmsg_add_u32(&b, "rssi", (uint32_t)l->rssi);
		if (l->tx_rate)
			blobmsg_add_u32(&b, "tx_rate", l->tx_rate);
		if (l->last_seen)
			blobmsg_add_u32(&b, "last_seen", l->last_seen);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, al);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_get_mesh_info(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req, const char *method,
			    struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	char mesh_id[33], mesh_key[33], band[8];
	char lan_ports[OM_PORTS_SPEC_MAX + 1];

	(void)method;
	(void)msg;
	blob_buf_init(&b, 0);
	if (om_store_backhaul_get(mesh_id, sizeof(mesh_id), mesh_key,
				  sizeof(mesh_key), band, sizeof(band),
				  lan_ports, sizeof(lan_ports)) == 0 &&
	    *mesh_id) {
		char mode[8];

		om_store_backhaul_mode(mode, sizeof(mode));
		blobmsg_add_u8(&b, "configured", 1);
		blobmsg_add_string(&b, "mesh_id", mesh_id);
		blobmsg_add_string(&b, "band", band);
		blobmsg_add_string(&b, "mode", mode);
		blobmsg_add_u8(&b, "key_set", !!*mesh_key);
		blobmsg_add_string(&b, "lan_ports", lan_ports);
		if (u->mon && u->mon->mesh_ifname[0])
			blobmsg_add_string(&b, "mesh_ifname",
					   u->mon->mesh_ifname);
	} else {
		blobmsg_add_u8(&b, "configured", 0);
	}
	return ubus_send_reply(ctx, req, b.head);
}

/* m19: current br-lan bridge members for the wizard dropdown */
static int om_get_lan_ports(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req,
			    const char *method, struct blob_attr *msg)
{
	char ports[8][OM_PORT_LEN];
	void *a;
	int n, i;

	(void)obj;
	(void)method;
	(void)msg;
	n = om_store_lan_ports(ports, 8);
	blob_buf_init(&b, 0);
	a = blobmsg_open_array(&b, "ports");
	for (i = 0; i < n && i < 8; i++)
		blobmsg_add_string(&b, NULL, ports[i]);
	blobmsg_close_array(&b, a);
	return ubus_send_reply(ctx, req, b.head);
}

/* ==================== m21: port discovery + plan (read-only) ============ */

static const char *port_net_type(const struct om_port_facts *f)
{
	if (f->wireless) return "wireless";
	if (f->is_bridge) return "bridge";
	if (f->is_batman_soft) return "batman";
	if (f->is_vlan_subif) return "vlan";
	return "ethernet";
}

/* ---- m23: wired-peer hints + auto-join ---- */

#define OM_HINT_FRESH_S 35	/* ~3 announce periods = still there */

struct om_hint_view {
	char ifname[16];
	char label[40];		/* adopted name or short id */
	bool recent;
	bool stable;
};

/* peer label: adopted nodes carry their user-given name */
static void hint_peer_label(const uint8_t node[OM_ID_LEN], char out[],
			    size_t outsz)
{
	struct om_stored_node sn;

	if (om_store_adopted_find(node, &sn) == 0 && sn.name[0]) {
		snprintf(out, outsz, "%s", sn.name);
		return;
	}
	char hex[OM_ID_HEX_LEN];

	om_id_to_hex(node, hex);
	snprintf(out, outsz, "%.8s…", hex);
}

static int hint_views(struct om_ubus *u, struct om_hint_view out[], int max)
{
	struct om_disc_hint raw[OM_HINT_MAX];
	time_t now = time(NULL);
	int n, i, m = 0;

	if (!u->disc)
		return 0;
	n = om_disc_get_hints(u->disc, raw, OM_HINT_MAX);
	for (i = 0; i < n && m < max; i++) {
		struct om_hint_view *v = &out[m++];

		snprintf(v->ifname, sizeof(v->ifname), "%s", raw[i].ifname);
		hint_peer_label(raw[i].node, v->label, sizeof(v->label));
		v->recent = (now - raw[i].last_seen) <= OM_HINT_FRESH_S;
		v->stable = raw[i].count >= OM_HINT_STABLE;
	}
	return m;
}

static const struct om_hint_view *hint_for(struct om_hint_view hv[], int n,
					   const char *ifname)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp(hv[i].ifname, ifname))
			return &hv[i];
	return NULL;
}

/*
 * Auto-join engine (m23): fires from the discovery receive path when a
 * port hint first reaches stability AND the operator opted in. Every
 * safety layer still applies: fresh server-side classification (the
 * ONLY overridable manual risk, mgmt_unknown, is NOT auto-joinable),
 * protected/WAN/VLAN/DSA-CPU ports are ineligible, one transaction at
 * a time, and the 30s rollback covers the whole change.
 */
static void om_autojoin_cb(const struct om_disc_hint *h, void *priv)
{
	struct om_ubus *u = priv;
	struct om_port_entry pe[OM_PORTS_MAX];
	struct om_port_entry *hit = NULL;
	char mesh_id[33], key[33], band[8];
	char lan_ports[OM_PORTS_SPEC_MAX + 1];
	char label[40];
	int n, i;

	if (!u->snet || !u->netconf || !om_store_wired_auto())
		return;

	n = om_ports_discover(pe, OM_PORTS_MAX);
	for (i = 0; i < n; i++)
		if (!strcmp(pe[i].f.device, h->ifname)) {
			hit = &pe[i];
			break;
		}
	if (!hit)
		return;
	if (!om_autojoin_should(&hit->v, true, u->netconf->active,
				true, h->count >= OM_HINT_STABLE)) {
		ULOG_INFO("autojoin: %s not joinable (%s)\n", h->ifname,
			  hit->v.reason[0] ? hit->v.reason : "busy");
		return;
	}
	if (om_store_backhaul_get(mesh_id, sizeof(mesh_id), key,
				  sizeof(key), band, sizeof(band),
				  lan_ports, sizeof(lan_ports)) < 0 ||
	    !*mesh_id || !*key)
		return;

	hint_peer_label(h->node, label, sizeof(label));
	ULOG_NOTE("autojoin: mesh peer '%s' detected on %s, attaching "
		  "wired backhaul (30s rollback)\n", label, h->ifname);
	if (om_netconf_mesh_apply(u->netconf, u->state_dir, mesh_id,
				  *band ? band : "5g", key,
				  OM_BH_HYBRID, h->ifname) < 0)
		return;
	om_store_backhaul_set(mesh_id, key, band, h->ifname);
	om_snet_push_mesh(u->snet, mesh_id, *band ? band : "5g", key,
			  "hybrid", h->ifname);
}

static void port_entry_blob(const struct om_port_entry *pe,
			    const struct om_hint_view *hv)
{
	void *t = blobmsg_open_table(&b, NULL);

	blobmsg_add_string(&b, "device", pe->f.device);
	blobmsg_add_string(&b, "label", pe->f.device);	/* board labels TBD */
	blobmsg_add_string(&b, "type", port_net_type(&pe->f));
	blobmsg_add_u8(&b, "carrier", pe->f.carrier);
	if (pe->f.speed > 0)
		blobmsg_add_u32(&b, "speed_mbps", (uint32_t)pe->f.speed);
	blobmsg_add_string(&b, "bridge",
			   pe->f.in_lan_bridge ? "br-lan" : "");
	blobmsg_add_string(&b, "role", pe->v.role);
	blobmsg_add_string(&b, "management",
			   om_port_mgmt_str(pe->v.mgmt));
	blobmsg_add_u8(&b, "eligible", pe->v.eligible);
	if (pe->v.reason[0])
		blobmsg_add_string(&b, "reason", pe->v.reason);
	/* m23: a verified mesh peer's announce is arriving on this port */
	if (hv && hv->recent) {
		blobmsg_add_string(&b, "mesh_peer", hv->label);
		blobmsg_add_u8(&b, "mesh_peer_stable", hv->stable);
	}
	blobmsg_close_table(&b, t);
}

static int om_ports(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct om_port_entry pe[OM_PORTS_MAX];
	struct om_hint_view hv[OM_HINT_MAX];
	void *a;
	int n, i, nh;

	(void)obj;
	(void)method;
	(void)msg;
	n = om_ports_discover(pe, OM_PORTS_MAX);
	if (n < 0)
		return UBUS_STATUS_UNKNOWN_ERROR;
	nh = hint_views(u, hv, OM_HINT_MAX);
	blob_buf_init(&b, 0);
	a = blobmsg_open_array(&b, "ports");
	for (i = 0; i < n; i++)
		port_entry_blob(&pe[i], hint_for(hv, nh, pe[i].f.device));
	blobmsg_close_array(&b, a);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_portplan(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct blob_attr *tb[2] = { NULL, NULL };
	struct om_port_plan plan;
	const char *dev, *mode;
	void *a, *c, *pv;
	int i, rc;

	(void)method;
	blobmsg_parse(om_portplan_policy, 2, tb, blob_data(msg), blob_len(msg));
	dev = tb[0] ? blobmsg_get_string(tb[0]) : NULL;
	mode = tb[1] ? blobmsg_get_string(tb[1]) : "wireless_only";
	if (!dev || !*dev)
		return UBUS_STATUS_INVALID_ARGUMENT;
	/* backend revalidation is mandatory: a stale frontend list is
	 * never trusted (task §五: 写操作必须重新验证) */
	rc = om_port_plan(dev, mode, &plan);
	if (rc < 0)
		return UBUS_STATUS_INVALID_ARGUMENT;

	{
		struct om_hint_view hv[OM_HINT_MAX];
		int nh = hint_views(u, hv, OM_HINT_MAX);

		blob_buf_init(&b, 0);
		blobmsg_add_string(&b, "device", plan.entry.f.device);
		blobmsg_add_string(&b, "mode", plan.mode);
		blobmsg_add_u8(&b, "ok", plan.ok);
		blobmsg_add_u8(&b, "preview_only", true);

		a = blobmsg_open_table(&b, "port");
		port_entry_blob(&plan.entry,
				hint_for(hv, nh, plan.entry.f.device));
		blobmsg_close_table(&b, a);
	}

	a = blobmsg_open_array(&b, "checks");
	for (i = 0; i < plan.n_checks; i++) {
		c = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "name", plan.checks[i].name);
		blobmsg_add_string(&b, "state", plan.checks[i].state);
		blobmsg_add_string(&b, "detail", plan.checks[i].detail);
		blobmsg_close_table(&b, c);
	}
	blobmsg_close_array(&b, a);

	pv = blobmsg_open_table(&b, "preview");
	a = blobmsg_open_array(&b, "uci_changes");
	for (i = 0; i < plan.n_uci; i++)
		blobmsg_add_string(&b, NULL, plan.uci_changes[i]);
	blobmsg_close_array(&b, a);
	a = blobmsg_open_array(&b, "ops");
	for (i = 0; i < plan.n_ops; i++)
		blobmsg_add_string(&b, NULL, plan.ops[i]);
	blobmsg_close_array(&b, a);
	a = blobmsg_open_array(&b, "interruptions");
	for (i = 0; i < plan.n_interruptions; i++)
		blobmsg_add_string(&b, NULL, plan.interruptions[i]);
	blobmsg_close_array(&b, a);
	blobmsg_add_string(&b, "mgmt_risk", plan.mgmt_risk);
	blobmsg_add_string(&b, "rollback", plan.rollback);
	blobmsg_close_table(&b, pv);

	return ubus_send_reply(ctx, req, b.head);
}

/*
 * m22 (Phase 7.2): attach one physical port to the wired backhaul leg.
 * The 7.1 plan gate runs FRESH here (never trusts the frontend), the
 * actual UCI work reuses the m19 hybrid transaction (30s auto-rollback
 * + snapshot restore), and agents receive the selection via the m19
 * session push (they re-validate and skip unknown port names).
 */
static int om_port_apply(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct blob_attr *tb[2] = { NULL, NULL };
	struct om_port_plan plan;
	char mesh_id[33], key[33], band[8];
	char lan_ports[OM_PORTS_SPEC_MAX + 1];
	const char *dev;
	bool ack = false;
	int gate;

	(void)method;
	if (!u->netconf)
		return UBUS_STATUS_NOT_FOUND;
	blobmsg_parse(om_portapply_policy, 2, tb, blob_data(msg), blob_len(msg));
	dev = tb[0] ? blobmsg_get_string(tb[0]) : NULL;
	if (tb[1])
		ack = blobmsg_get_u8(tb[1]);
	if (!dev || !*dev || !om_lan_ports_valid(dev))
		return UBUS_STATUS_INVALID_ARGUMENT;

	/* fresh server-side validation (stale UI lists are not authority) */
	if (om_port_plan(dev, "wired_preferred", &plan) < 0)
		return UBUS_STATUS_INVALID_ARGUMENT;
	gate = om_port_apply_gate(&plan.entry.v, ack);
	if (gate != OM_APPLY_ALLOW) {
		blob_buf_init(&b, 0);
		blobmsg_add_string(&b, "result",
			gate == OM_APPLY_NEED_ACK ? "need_ack" : "denied");
		blobmsg_add_string(&b, "device", dev);
		blobmsg_add_string(&b, "reason",
			plan.entry.v.reason[0] ? plan.entry.v.reason : "blocked");
		return ubus_send_reply(ctx, req, b.head);
	}

	/* the wired leg rides on an existing mesh: no mesh, no leg */
	if (om_store_backhaul_get(mesh_id, sizeof(mesh_id), key,
				  sizeof(key), band, sizeof(band),
				  lan_ports, sizeof(lan_ports)) < 0 ||
	    !*mesh_id || !*key) {
		blob_buf_init(&b, 0);
		blobmsg_add_string(&b, "result", "no_mesh");
		return ubus_send_reply(ctx, req, b.head);
	}

	/* m19 transaction: re-home old om_eth, move the port out of
	 * br-lan, create om_eth_<port>; 30s confirm or full rollback */
	if (om_netconf_mesh_apply(u->netconf, u->state_dir, mesh_id,
				  *band ? band : "5g", key,
				  OM_BH_HYBRID, dev) < 0)
		return UBUS_STATUS_UNKNOWN_ERROR;
	om_store_backhaul_set(mesh_id, key, band, dev);
	if (u->snet)
		om_snet_push_mesh(u->snet, mesh_id, *band ? band : "5g",
				  key, "hybrid", dev);

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result", "applied");
	blobmsg_add_string(&b, "device", dev);
	blobmsg_add_u32(&b, "confirm_window", OM_TXN_TIMEOUT);
	blobmsg_add_u32(&b, "generation", u->netconf->generation);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_roaming_apply(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req, const char *method,
			    struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct blob_attr *tb[4] = { NULL, NULL, NULL, NULL };
	bool k = true, v = true, r = true;
	const char *domain = NULL;
	char gen_domain[5];
	uint32_t rnd;

	(void)method;
	if (!u->netconf)
		return UBUS_STATUS_NOT_FOUND;
	blobmsg_parse(om_roam_policy, 4, tb, blob_data(msg), blob_len(msg));
	if (tb[0]) k = blobmsg_get_u8(tb[0]);
	if (tb[1]) v = blobmsg_get_u8(tb[1]);
	if (tb[2]) r = blobmsg_get_u8(tb[2]);
	if (tb[3]) domain = blobmsg_get_string(tb[3]);
	if (!domain || strlen(domain) != 4) {
		randombytes_buf(&rnd, sizeof(rnd));
		snprintf(gen_domain, sizeof(gen_domain), "%04x",
			 rnd & 0xffff);
		domain = gen_domain;
	}
	if (om_netconf_roaming_apply(u->netconf, u->state_dir, k, v, r,
				     domain) < 0)
		return UBUS_STATUS_UNKNOWN_ERROR;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result", "applied");
	blobmsg_add_string(&b, "mobility_domain", domain);
	blobmsg_add_u32(&b, "confirm_window", 30);
	blobmsg_add_u32(&b, "generation", u->netconf->generation);
	return ubus_send_reply(ctx, req, b.head);
}

static const struct ubus_method om_methods[] = {
	UBUS_METHOD_NOARG("status", om_status),
	UBUS_METHOD_NOARG("list_nodes", om_list_nodes),
	UBUS_METHOD_NOARG("health", om_health),
	UBUS_METHOD_NOARG("list_pending", om_list_pending),
	UBUS_METHOD_NOARG("list_adopted", om_list_adopted),
	UBUS_METHOD("adopt", om_adopt, om_adopt_policy),
	UBUS_METHOD("revoke", om_revoke, om_adopt_policy),
	UBUS_METHOD("mesh_create", om_mesh_create, om_mesh_policy),
	UBUS_METHOD_NOARG("mesh_join", om_mesh_join),
	UBUS_METHOD("apply_confirm", om_apply_confirm, om_txn_policy),
	UBUS_METHOD("apply_rollback", om_apply_rollback, om_txn_policy),
	UBUS_METHOD_NOARG("get_links", om_get_links),
	UBUS_METHOD_NOARG("get_clients", om_get_clients),
	UBUS_METHOD_NOARG("topology", om_topology),
	UBUS_METHOD_NOARG("get_mesh_info", om_get_mesh_info),
	UBUS_METHOD_NOARG("get_lan_ports", om_get_lan_ports),
	UBUS_METHOD_NOARG("ports", om_ports),
	UBUS_METHOD("port_plan", om_portplan, om_portplan_policy),
	UBUS_METHOD("port_apply", om_port_apply, om_portapply_policy),
	UBUS_METHOD("roaming_apply", om_roaming_apply, om_roam_policy),
};

static struct ubus_object_type om_obj_type =
	UBUS_OBJECT_TYPE("openmesh", om_methods);

int om_ubus_init(struct om_ubus *u, struct om_identity *id,
		 struct om_disc *disc)
{
	memset(u, 0, sizeof(*u));
	u->id = id;
	u->disc = disc;
	clock_gettime(CLOCK_MONOTONIC, &u->started);
	/* m23: stable wired-peer hints may trigger the opt-in auto-join */
	if (disc)
		om_disc_set_hint_cb(disc, om_autojoin_cb, u);

	u->ctx = ubus_connect(NULL);
	if (!u->ctx)
		return -1;

	u->obj.name = "openmesh";
	u->obj.type = &om_obj_type;
	u->obj.methods = om_methods;
	u->obj.n_methods = ARRAY_SIZE(om_methods);

	ubus_add_uloop(u->ctx);
	if (ubus_add_object(u->ctx, &u->obj) != 0) {
		ubus_free(u->ctx);
		u->ctx = NULL;
		return -1;	/* e.g. object name taken by another instance */
	}
	return 0;
}

void om_ubus_done(struct om_ubus *u)
{
	if (u->ctx) {
		ubus_free(u->ctx);
		u->ctx = NULL;
	}
}

void om_ubus_set_snet(struct om_ubus *u, struct om_snet *snet)
{
	u->snet = snet;
}

void om_ubus_set_netconf(struct om_ubus *u, struct om_netconf *nc,
			 const char *state_dir)
{
	u->netconf = nc;
	snprintf(u->state_dir, sizeof(u->state_dir), "%s", state_dir);
}

void om_ubus_set_monitor(struct om_ubus *u, struct om_monitor *m)
{
	u->mon = m;
}

void om_ubus_notify_node_new(struct om_ubus *u, const struct om_node_info *n)
{
	char hex[OM_ID_HEX_LEN];

	if (!u->ctx)
		return;
	blob_buf_init(&b, 0);
	om_id_to_hex(n->id, hex);
	blobmsg_add_string(&b, "id", hex);
	blobmsg_add_string(&b, "role", n->role);
	blobmsg_add_string(&b, "model", n->model);
	blobmsg_add_string(&b, "addr", n->addr);
	ubus_send_event(u->ctx, "openmesh.node.new", b.head);
}

void om_ubus_notify_adopted(struct om_ubus *u,
			    const struct om_adopted_node *a)
{
	char hex[OM_ID_HEX_LEN];

	if (!u->ctx)
		return;
	blob_buf_init(&b, 0);
	om_id_to_hex(a->id, hex);
	blobmsg_add_string(&b, "id", hex);
	blobmsg_add_string(&b, "name", a->name);
	ubus_send_event(u->ctx, "openmesh.node.adopted", b.head);
}

void om_ubus_notify_txn(struct om_ubus *u, bool committed)
{
	if (!u->ctx)
		return;
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "committed", committed);
	ubus_send_event(u->ctx,
			committed ? "openmesh.config.applied"
				  : "openmesh.config.rolledback",
			b.head);
}
