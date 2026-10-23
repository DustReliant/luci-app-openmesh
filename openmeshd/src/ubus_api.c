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

#include "ubus_api.h"
#include "session_net.h"

static struct blob_buf b;

static const struct blobmsg_policy om_adopt_policy[2] = {
	{ "name", BLOBMSG_TYPE_STRING },
	{ "id", BLOBMSG_TYPE_STRING },
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

static const struct ubus_method om_methods[] = {
	UBUS_METHOD_NOARG("status", om_status),
	UBUS_METHOD_NOARG("list_nodes", om_list_nodes),
	UBUS_METHOD_NOARG("health", om_health),
	UBUS_METHOD_NOARG("list_pending", om_list_pending),
	UBUS_METHOD_NOARG("list_adopted", om_list_adopted),
	UBUS_METHOD("adopt", om_adopt, om_adopt_policy),
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
