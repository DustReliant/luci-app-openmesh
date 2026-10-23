// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * openmeshd - configuration transactions with auto-rollback.
 *
 * V0.1 scope (ADR-0006): native UCI 802.11s interface management on
 * /etc/config/wireless. Snapshot -> apply -> confirm-window -> restore.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sodium.h>
#include <uci.h>

#include <libubox/ulog.h>

#include "netconf.h"

#define WIRELESS_CONF "/etc/config/wireless"
#define TXN_MARKER    "txn.pending"

static int copy_file(const char *from, const char *to)
{
	FILE *in, *out;
	char buf[4096];
	size_t r;

	in = fopen(from, "rb");
	if (!in)
		return -1;
	out = fopen(to, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	while ((r = fread(buf, 1, sizeof(buf), in)) > 0)
		if (fwrite(buf, 1, r, out) != r) {
			fclose(in);
			fclose(out);
			unlink(to);
			return -1;
		}
	fclose(in);
	fclose(out);
	return 0;
}

static void marker_path(const char *state_dir, char *out, size_t outsz)
{
	snprintf(out, outsz, "%s/%s", state_dir, TXN_MARKER);
}

static void bak_path(const char *state_dir, char *out, size_t outsz)
{
	snprintf(out, outsz, "%s/wireless.om-bak", state_dir);
}

static void reload_network(void)
{
	/* V0.1: shell out once per transaction; fine at this cadence */
	if (system("ubus call network reload >/dev/null 2>&1") != 0)
		system("/sbin/wifi up >/dev/null 2>&1");
}

static void txn_timer_cb(struct uloop_timeout *t)
{
	struct om_netconf *nc = container_of(t, struct om_netconf, timer);

	ULOG_ERR("netconf: confirm deadline hit, rolling back\n");
	om_netconf_rollback(nc);
}

/* Pick a radio device name for the requested band; "" = error. */
static void pick_radio(const char *band, char *out, size_t outsz)
{
	struct uci_context *c = uci_alloc_context();
	struct uci_package *p = NULL;
	struct uci_element *e;
	const char *want = NULL;

	out[0] = '\0';
	if (!c)
		return;
	if (!strcmp(band, "2g"))
		want = "2g";
	else if (!strcmp(band, "5g"))
		want = "5g";
	/* auto: prefer 5g below */

	if (uci_load(c, "wireless", &p) == UCI_OK) {
		uci_foreach_element(&p->sections, e) {
			struct uci_section *s = uci_to_section(e);
			struct uci_option *o;
			const char *b;

			if (strcmp(s->type, "wifi-device"))
				continue;
			o = uci_lookup_option(c, s, "band");
			b = (o && o->type == UCI_TYPE_STRING) ?
			    o->v.string : NULL;
			if (!want || (b && !strcmp(b, want))) {
				snprintf(out, outsz, "%s", s->e.name);
				if (!want || !strcmp(band, "5g"))
					break;	/* match or 5g preference */
			}
		}
	}
	uci_free_context(c);
}

int om_netconf_mesh_apply(struct om_netconf *nc, const char *state_dir,
			  const char *mesh_id, const char *band,
			  const char *sae_key)
{
	struct uci_context *c;
	struct uci_package *p = NULL;
	struct uci_section *ns = NULL;
	struct uci_ptr ptr;
	char radio[32] = "";
	char bak[OM_PATH_MAX], marker[OM_PATH_MAX];
	struct stat st;
	static uint32_t gen = 0;

	if (nc->active)
		return -1;	/* one transaction at a time */
	if (!mesh_id || !*mesh_id || !sae_key || !*sae_key)
		return -1;

	pick_radio(band, radio, sizeof(radio));
	if (!radio[0]) {
		ULOG_ERR("netconf: no radio for band '%s'\n", band);
		return -1;
	}

	/* snapshot + on-flash pending marker (power-loss safety) */
	bak_path(state_dir, bak, sizeof(bak));
	marker_path(state_dir, marker, sizeof(marker));
	if (stat(WIRELESS_CONF, &st) == 0 && copy_file(WIRELESS_CONF, bak) < 0) {
		ULOG_ERR("netconf: snapshot failed\n");
		return -1;
	}
	gen++;
	FILE *f = fopen(marker, "w");

	if (!f) {
		unlink(bak);
		return -1;
	}
	fprintf(f, "gen=%u\nfiles=wireless\n", gen);
	fclose(f);

	c = uci_alloc_context();
	if (!c)
		goto fail;
	if (uci_load(c, "wireless", &p) != UCI_OK)
		goto fail;

	/* drop previous om-managed mesh ifaces (idempotent re-create) */
	{
		struct uci_element *e, *tmp;

		uci_foreach_element_safe(&p->sections, tmp, e) {
			struct uci_section *s = uci_to_section(e);
			struct uci_option *o = uci_lookup_option(c, s,
								 "om_managed");

			if (o && o->type == UCI_TYPE_STRING &&
			    !strcmp(o->v.string, "1")) {
				memset(&ptr, 0, sizeof(ptr));
				ptr.package = "wireless";
				ptr.p = p;
				ptr.section = s->e.name;
				uci_delete(c, &ptr);
			}
		}
	}

	uci_add_section(c, p, "wifi-iface", &ns);
	if (!ns)
		goto fail;
	memset(&ptr, 0, sizeof(ptr));
	ptr.package = "wireless";
	ptr.p = p;
	ptr.s = ns;
	ptr.section = ns->e.name;

	ptr.option = "device";		ptr.value = radio;	uci_set(c, &ptr);
	ptr.option = "mode";		ptr.value = "mesh";	uci_set(c, &ptr);
	ptr.option = "mesh_id";		ptr.value = mesh_id;	uci_set(c, &ptr);
	ptr.option = "network";		ptr.value = "lan";	uci_set(c, &ptr);
	ptr.option = "encryption";	ptr.value = "sae";	uci_set(c, &ptr);
	ptr.option = "key";		ptr.value = sae_key;	uci_set(c, &ptr);
	ptr.option = "om_managed";	ptr.value = "1";	uci_set(c, &ptr);

	uci_save(c, p);
	if (uci_commit(c, &p, false) != UCI_OK)
		goto fail;
	uci_free_context(c);

	reload_network();

	nc->active = true;
	nc->committed = false;
	nc->generation = gen;
	snprintf(nc->bak_file, sizeof(nc->bak_file), "%s", bak);
	snprintf(nc->marker, sizeof(nc->marker), "%s", marker);
	nc->timer.cb = txn_timer_cb;
	uloop_timeout_set(&nc->timer, OM_TXN_TIMEOUT * 1000);
	ULOG_NOTE("netconf: applied mesh '%s' on %s (gen %u, confirm within "
		  "%ds)\n", mesh_id, radio, gen, OM_TXN_TIMEOUT);
	return 0;

fail:
	if (c)
		uci_free_context(c);
	unlink(bak);
	unlink(marker);
	return -1;
}

static int restore_snapshot(struct om_netconf *nc)
{
	if (copy_file(nc->bak_file, WIRELESS_CONF) < 0)
		return -1;
	unlink(nc->bak_file);
	reload_network();
	return 0;
}

int om_netconf_confirm(struct om_netconf *nc)
{
	if (!nc->active)
		return -1;
	uloop_timeout_cancel(&nc->timer);
	unlink(nc->marker);
	unlink(nc->bak_file);
	nc->active = false;
	nc->committed = true;
	ULOG_NOTE("netconf: transaction %u confirmed\n", nc->generation);
	if (nc->on_done)
		nc->on_done(true, nc->generation, nc->priv);
	return 0;
}

int om_netconf_rollback(struct om_netconf *nc)
{
	uint32_t gen = nc->generation;

	if (!nc->active)
		return -1;
	uloop_timeout_cancel(&nc->timer);
	if (restore_snapshot(nc) < 0)
		ULOG_ERR("netconf: rollback restore FAILED, manual check "
			 "needed (%s)\n", WIRELESS_CONF);
	unlink(nc->marker);
	nc->active = false;
	nc->committed = false;
	ULOG_NOTE("netconf: transaction %u rolled back\n", gen);
	if (nc->on_done)
		nc->on_done(false, gen, nc->priv);
	return 0;
}

void om_netconf_recover(struct om_netconf *nc, const char *state_dir)
{
	char marker[OM_PATH_MAX], bak[OM_PATH_MAX];
	struct stat st;

	marker_path(state_dir, marker, sizeof(marker));
	bak_path(state_dir, bak, sizeof(bak));
	if (stat(marker, &st) < 0)
		return;		/* no pending transaction */
	ULOG_WARN("netconf: unconfirmed transaction found at boot, "
		  "restoring snapshot\n");
	if (stat(bak, &st) == 0) {
		if (copy_file(bak, WIRELESS_CONF) < 0)
			ULOG_ERR("netconf: recovery restore FAILED\n");
		unlink(bak);
	}
	unlink(marker);
	nc->active = false;
	/* reload happens when the loop starts via first announce/tick */
	reload_network();
}
