// SPDX-License-Identifier: GPL-3.0-or-later
/* openmeshd - "openmesh" ubus object and events. */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
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


static struct blob_buf b;

static const struct blobmsg_policy om_adopt_policy[2] = {
	{ "name", BLOBMSG_TYPE_STRING },
	{ "id", BLOBMSG_TYPE_STRING },
};

static const struct blobmsg_policy om_mesh_policy[4] = {
	{ "mesh_name", BLOBMSG_TYPE_STRING },
	{ "band", BLOBMSG_TYPE_STRING },
	{ "key", BLOBMSG_TYPE_STRING },
	{ "mode", BLOBMSG_TYPE_STRING },
};

static const struct blobmsg_policy om_roam_policy[4] = {
	{ "k", BLOBMSG_TYPE_INT8 },
	{ "v", BLOBMSG_TYPE_INT8 },
	{ "r", BLOBMSG_TYPE_INT8 },
	{ "domain", BLOBMSG_TYPE_STRING },
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

static int om_mesh_create(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	struct blob_attr *tb[4] = { NULL, NULL, NULL, NULL };
	const char *name, *band, *key, *mode;
	int bh_mode = OM_BH_WIFI;
	char gen[13];

	(void)method;
	if (!u->snet || !u->netconf)
		return UBUS_STATUS_NOT_FOUND;
	blobmsg_parse(om_mesh_policy, 4, tb, blob_data(msg), blob_len(msg));
	name = tb[0] ? blobmsg_get_string(tb[0]) : NULL;
	band = tb[1] ? blobmsg_get_string(tb[1]) : "5g";
	key = tb[2] ? blobmsg_get_string(tb[2]) : "";
	mode = tb[3] ? blobmsg_get_string(tb[3]) : "wifi";
	if (!name || !*name)
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (!strcmp(mode, "hybrid"))
		bh_mode = OM_BH_HYBRID;
	if (!*key) {
		gen_key(gen);
		key = gen;
	}
	if (om_store_backhaul_set(name, key, band) < 0)
		ULOG_WARN("mesh_create: persisting backhaul failed\n");
	if (om_netconf_mesh_apply(u->netconf, u->state_dir, name, band, key,
				  bh_mode) < 0)
		return UBUS_STATUS_UNKNOWN_ERROR;
	om_snet_push_mesh(u->snet, name, band, key, mode);

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result", "applied");
	blobmsg_add_string(&b, "key", key);
	blobmsg_add_u32(&b, "confirm_window", 30);
	return ubus_send_reply(ctx, req, b.head);
}

static int om_mesh_join(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);
	char mesh_id[33], key[33], band[8];

	(void)method;
	(void)msg;
	if (!u->netconf)
		return UBUS_STATUS_NOT_FOUND;
	if (om_store_backhaul_get(mesh_id, sizeof(mesh_id), key,
				  sizeof(key), band, sizeof(band)) < 0 ||
	    !*mesh_id)
		return UBUS_STATUS_NOT_FOUND;
	if (om_netconf_mesh_apply(u->netconf, u->state_dir, mesh_id,
				  *band ? band : "5g", key,
				  OM_BH_WIFI) < 0)
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

	(void)method;
	(void)msg;
	if (!u->netconf || om_netconf_confirm(u->netconf) < 0)
		return UBUS_STATUS_NOT_FOUND;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result", "confirmed");
	return ubus_send_reply(ctx, req, b.head);
}

static int om_apply_rollback(struct ubus_context *ctx, struct ubus_object *obj,
			     struct ubus_request_data *req,
			     const char *method, struct blob_attr *msg)
{
	struct om_ubus *u = OM_HANDLER_CTX(obj);

	(void)method;
	(void)msg;
	if (!u->netconf || om_netconf_rollback(u->netconf) < 0)
		return UBUS_STATUS_NOT_FOUND;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result", "rolled-back");
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

	(void)method;
	(void)msg;
	blob_buf_init(&b, 0);
	if (om_store_backhaul_get(mesh_id, sizeof(mesh_id), mesh_key,
				  sizeof(mesh_key), band, sizeof(band)) == 0 &&
	    *mesh_id) {
		char mode[8];

		om_store_backhaul_mode(mode, sizeof(mode));
		blobmsg_add_u8(&b, "configured", 1);
		blobmsg_add_string(&b, "mesh_id", mesh_id);
		blobmsg_add_string(&b, "band", band);
		blobmsg_add_string(&b, "mode", mode);
		blobmsg_add_u8(&b, "key_set", !!*mesh_key);
		if (u->mon && u->mon->mesh_ifname[0])
			blobmsg_add_string(&b, "mesh_ifname",
					   u->mon->mesh_ifname);
	} else {
		blobmsg_add_u8(&b, "configured", 0);
	}
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
	UBUS_METHOD_NOARG("apply_confirm", om_apply_confirm),
	UBUS_METHOD_NOARG("apply_rollback", om_apply_rollback),
	UBUS_METHOD_NOARG("get_links", om_get_links),
	UBUS_METHOD_NOARG("get_clients", om_get_clients),
	UBUS_METHOD_NOARG("topology", om_topology),
	UBUS_METHOD_NOARG("get_mesh_info", om_get_mesh_info),
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
