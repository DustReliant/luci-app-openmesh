// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * openmeshd - configuration transactions with auto-rollback.
 *
 * Mode A (wifi): native 802.11s iface bridged into lan (ADR-0006).
 * Mode B (hybrid): 802.11s hardif + batman-adv softif "om_bat" bridged
 * into lan (ADR-0002). Snapshot -> apply -> confirm-window -> restore.
 *
 * wireless edits go through libuci; network edits (mode B, named
 * sections only) go through idempotent uci-CLI shell calls - a V0.1
 * simplification, one transaction at a time, no concurrent writer.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <uci.h>

#include <libubox/ulog.h>

#include "netconf.h"

#define TXN_MARKER "txn.pending"

static int copy_file(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb");
	FILE *out;
	char buf[4096];
	size_t r;

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

static void reload_network(void)
{
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

/* delete anonymous sections carrying option om_managed='1' (wireless) */
static void drop_om_sections(struct uci_context *c, struct uci_package *p,
			     const char *pkg)
{
	struct uci_ptr ptr;
	struct uci_element *e, *tmp;

	uci_foreach_element_safe(&p->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);
		struct uci_option *o = uci_lookup_option(c, s, "om_managed");

		if (!o || o->type != UCI_TYPE_STRING ||
		    strcmp(o->v.string, "1"))
			continue;
		memset(&ptr, 0, sizeof(ptr));
		ptr.package = pkg;
		ptr.p = p;
		ptr.section = s->e.name;
		uci_delete(c, &ptr);
	}
}

/* idempotent mode B network config via uci CLI */
static int apply_network_hybrid(void)
{	const char *cmds[] = {
		"uci -q set network." OM_BAT_IF "=interface",
		"uci -q set network." OM_BAT_IF ".proto='batadv'",
		"uci -q set network." OM_BAT_IF ".om_managed='1'",
		"uci -q set network." OM_MESH_IF "=interface",
		"uci -q set network." OM_MESH_IF ".proto='batadv_hardif'",
		"uci -q set network." OM_MESH_IF ".master='" OM_BAT_IF "'",
		"uci -q set network." OM_MESH_IF ".om_managed='1'",
		/* add om_bat to br-lan ports once */
		"BR=$(uci -q show network | sed -n "
		"\"s/network\\.\\([^.]*\\)\\.name='br-lan'/\\1/p\"); "
		"[ -n \"$BR\" ] && ! uci -q get network.$BR.ports | "
		"grep -q '" OM_BAT_IF "' && "
		"uci -q add_list network.$BR.ports='" OM_BAT_IF "'",
		"uci -q commit network",
		NULL
	};

	for (int i = 0; cmds[i]; i++)
		if (system(cmds[i]) != 0) {
			ULOG_WARN("netconf: cmd failed: %s\n", cmds[i]);
			return -1;
		}
	return 0;
}

int om_netconf_mesh_apply(struct om_netconf *nc, const char *state_dir,
			  const char *mesh_id, const char *band,
			  const char *sae_key, int mode)
{
	struct uci_context *cw = NULL;
	struct uci_package *pw = NULL;
	struct uci_section *ns;
	struct uci_ptr ptr;
	char radio[32] = "";
	char marker[OM_PATH_MAX];
	static uint32_t gen = 0;
	FILE *f;

	if (nc->active)
		return -1;	/* one transaction at a time */
	if (!mesh_id || !*mesh_id || !sae_key || !*sae_key)
		return -1;

	pick_radio(band, radio, sizeof(radio));
	if (!radio[0]) {
		ULOG_ERR("netconf: no radio for band '%s'\n", band);
		return -1;
	}

	/* snapshots + on-flash pending marker (power-loss safety) */
	nc->n_files = 0;
	snprintf(nc->dest_files[nc->n_files], OM_PATH_MAX, "/etc/config/wireless");
	snprintf(nc->bak_files[nc->n_files], OM_PATH_MAX,
		 "%s/wireless.om-bak", state_dir);
	nc->n_files++;
	if (mode == OM_BH_HYBRID) {
		snprintf(nc->dest_files[nc->n_files], OM_PATH_MAX,
			 "/etc/config/network");
		snprintf(nc->bak_files[nc->n_files], OM_PATH_MAX,
			 "%s/network.om-bak", state_dir);
		nc->n_files++;
	}
	for (int i = 0; i < nc->n_files; i++)
		if (copy_file(nc->dest_files[i], nc->bak_files[i]) < 0) {
			ULOG_ERR("netconf: snapshot of %s failed\n",
				 nc->dest_files[i]);
			nc->n_files = 0;
			return -1;
		}

	gen++;
	snprintf(marker, sizeof(marker), "%s/%s", state_dir, TXN_MARKER);
	f = fopen(marker, "w");
	if (!f)
		goto fail;
	fprintf(f, "gen=%u\nfiles=%d\n", gen, nc->n_files);
	fclose(f);
	snprintf(nc->marker, sizeof(nc->marker), "%s", marker);

	/* ---- wireless (libuci) ---- */
	cw = uci_alloc_context();
	if (!cw || uci_load(cw, "wireless", &pw) != UCI_OK)
		goto fail;
	drop_om_sections(cw, pw, "wireless");
	uci_add_section(cw, pw, "wifi-iface", &ns);
	if (!ns)
		goto fail;
	memset(&ptr, 0, sizeof(ptr));
	ptr.package = "wireless";
	ptr.p = pw;
	ptr.s = ns;
	ptr.section = ns->e.name;
	ptr.option = "device";		ptr.value = radio;	uci_set(cw, &ptr);
	ptr.option = "mode";		ptr.value = "mesh";	uci_set(cw, &ptr);
	ptr.option = "mesh_id";		ptr.value = mesh_id;	uci_set(cw, &ptr);
	ptr.option = "encryption";	ptr.value = "sae";	uci_set(cw, &ptr);
	ptr.option = "key";		ptr.value = sae_key;	uci_set(cw, &ptr);
	ptr.option = "om_managed";	ptr.value = "1";	uci_set(cw, &ptr);
	ptr.option = "network";
	ptr.value = (mode == OM_BH_HYBRID) ? OM_MESH_IF : "lan";
	uci_set(cw, &ptr);
	uci_save(cw, pw);
	if (uci_commit(cw, &pw, false) != UCI_OK)
		goto fail;
	uci_free_context(cw);
	cw = NULL;

	/* ---- network (mode B, uci CLI) ---- */
	if (mode == OM_BH_HYBRID && apply_network_hybrid() < 0)
		goto fail;

	reload_network();

	nc->active = true;
	nc->committed = false;
	nc->generation = gen;
	nc->timer.cb = txn_timer_cb;
	uloop_timeout_set(&nc->timer, OM_TXN_TIMEOUT * 1000);
	ULOG_NOTE("netconf: applied mesh '%s' on %s mode=%s (gen %u, confirm "
		  "within %ds)\n", mesh_id, radio,
		  mode == OM_BH_HYBRID ? "hybrid" : "wifi", gen,
		  OM_TXN_TIMEOUT);
	return 0;

fail:
	if (cw)
		uci_free_context(cw);
	for (int i = 0; i < nc->n_files; i++)
		unlink(nc->bak_files[i]);
	unlink(marker);
	nc->n_files = 0;
	return -1;
}

static void roam_emit_options(FILE *out, bool k, bool v, bool r,
			      const char *domain)
{
	fprintf(out, "	option ieee80211k '%d'\n", k ? 1 : 0);
	fprintf(out, "	option bss_transition '%d'\n", v ? 1 : 0);
	fprintf(out, "	option ieee80211r '%d'\n", r ? 1 : 0);
	if (r) {
		fprintf(out, "	option mobility_domain '%s'\n", domain);
		fprintf(out, "	option ft_psk_generate_local '1'\n");
	}
}

/*
 * Direct file surgery for the roaming options: replace the option lines
 * inside each target wifi-iface block. Bypasses uci set/commit because
 * the netifd wireless reload pipeline rewrites /etc/config/wireless and
 * was observed leaving some options only in the volatile uci delta
 * (never in the file). The transaction snapshot/rollback covers us.
 */
static int roam_rewrite_file(const char *path, const char secs[][32],
			     int nsecs, bool k, bool v, bool r,
			     const char *domain)
{
	FILE *in = fopen(path, "r");
	FILE *out;
	char tmp[OM_PATH_MAX];
	char line[256];
	char cur_sec[32] = "";
	bool in_target = false;

	if (!in)
		return -1;
	snprintf(tmp, sizeof(tmp), "%s.roamtmp", path);
	out = fopen(tmp, "w");
	if (!out) {
		fclose(in);
		return -1;
	}

	while (fgets(line, sizeof(line), in)) {
		char name[32];

		if (sscanf(line, "config %*s '%31s'", name) == 1 ||
		    sscanf(line, "config %*s %31s", name) == 1) {
			if (in_target)
				roam_emit_options(out, k, v, r, domain);
			in_target = false;
			snprintf(cur_sec, sizeof(cur_sec), "%s", name);
			for (int i = 0; i < nsecs; i++)
				if (!strcmp(cur_sec, secs[i])) {
					in_target = true;
					break;
				}
			fputs(line, out);
			continue;
		}
		if (in_target) {
			static const char *ours[] = {
				"ieee80211k", "bss_transition", "ieee80211r",
				"mobility_domain", "ft_psk_generate_local"
			};
			char opt[32];
			bool skip = false;

			if (sscanf(line, " option %31s", opt) == 1)
				for (size_t i = 0; i < 5; i++)
					if (!strcmp(opt, ours[i]))
						skip = true;
			if (skip)
				continue;
		}
		fputs(line, out);
	}
	if (in_target)
		roam_emit_options(out, k, v, r, domain);
	fclose(in);
	if (fclose(out) != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int om_netconf_roaming_apply(struct om_netconf *nc, const char *state_dir,
			     bool k, bool v, bool r, const char *domain)
{
	struct uci_context *c;
	struct uci_package *p = NULL;
	struct uci_element *e;
	char marker[OM_PATH_MAX];
	static uint32_t gen = 0;
	int touched = 0;
	FILE *f;

	if (nc->active)
		return -1;
	if (!domain || strlen(domain) != 4)
		return -1;

	nc->n_files = 1;
	snprintf(nc->dest_files[0], OM_PATH_MAX, "/etc/config/wireless");
	snprintf(nc->bak_files[0], OM_PATH_MAX, "%s/wireless.om-bak",
		 state_dir);
	if (copy_file(nc->dest_files[0], nc->bak_files[0]) < 0) {
		nc->n_files = 0;
		return -1;
	}
	gen++;
	snprintf(marker, sizeof(marker), "%s/%s", state_dir, TXN_MARKER);
	f = fopen(marker, "w");
	if (!f)
		goto fail;
	fprintf(f, "gen=%u\nfiles=1\n", gen);
	fclose(f);
	snprintf(nc->marker, sizeof(nc->marker), "%s", marker);

	c = uci_alloc_context();
	if (!c || uci_load(c, "wireless", &p) != UCI_OK)
		goto fail;

	{
		char secs[8][32];
		int nsecs = 0;

		uci_foreach_element(&p->sections, e) {
			struct uci_section *s = uci_to_section(e);
			struct uci_option *o;
			const char *mode;

			if (strcmp(s->type, "wifi-iface"))
				continue;
			o = uci_lookup_option(c, s, "mode");
			mode = (o && o->type == UCI_TYPE_STRING) ?
				o->v.string : "";
			if (strcmp(mode, "ap"))
				continue;
			if (nsecs < 8)
				snprintf(secs[nsecs++], 32, "%s", s->e.name);
		}
		uci_free_context(c);
		c = NULL;

		if (nsecs == 0 ||
		    roam_rewrite_file("/etc/config/wireless", secs, nsecs,
				      k, v, r, domain) < 0)
			goto fail;
		touched = nsecs;
	}
	if (!touched)
		goto fail;

	reload_network();
	nc->active = true;
	nc->committed = false;
	nc->generation = gen;
	nc->timer.cb = txn_timer_cb;
	uloop_timeout_set(&nc->timer, OM_TXN_TIMEOUT * 1000);
	ULOG_NOTE("netconf: roaming k=%d v=%d r=%d domain=%s on %d AP ifaces "
		  "(gen %u, confirm within %ds)\n", k, v, r, domain, touched,
		  gen, OM_TXN_TIMEOUT);
	return 0;

fail:
	for (int i = 0; i < nc->n_files; i++)
		unlink(nc->bak_files[i]);
	unlink(marker);
	nc->n_files = 0;
	return -1;
}

int om_netconf_confirm(struct om_netconf *nc)
{
	if (!nc->active)
		return -1;
	uloop_timeout_cancel(&nc->timer);
	for (int i = 0; i < nc->n_files; i++)
		unlink(nc->bak_files[i]);
	unlink(nc->marker);
	nc->active = false;
	nc->committed = true;
	ULOG_NOTE("netconf: transaction %u confirmed\n", nc->generation);
	if (nc->on_done)
		nc->on_done(true, nc->generation, nc->priv);
	return 0;
}

/*
 * Belt-and-braces: after restoring snapshots, force-drop any om-managed
 * sections that some racing writer may have re-added to the files. These
 * sections are always ours, so deleting them is unconditionally safe.
 */
static void purge_om_sections(void)
{
	system("while uci -q show wireless 2>/dev/null | grep -q om_managed; do "
		"uci -q delete wireless.$(uci -q show wireless | "
		"grep om_managed | head -1 | cut -d. -f2 | cut -d= -f1); "
		"uci -q commit wireless; done");
	system("uci -q delete network.om_bat; uci -q delete network.om_mesh; "
		"uci -q commit network 2>/dev/null");
}

int om_netconf_rollback(struct om_netconf *nc)
{
	uint32_t gen = nc->generation;
	int rc = 0;

	if (!nc->active)
		return -1;
	uloop_timeout_cancel(&nc->timer);
	for (int i = 0; i < nc->n_files; i++) {
		if (copy_file(nc->bak_files[i], nc->dest_files[i]) < 0) {
			ULOG_ERR("netconf: rollback of %s FAILED, manual "
				 "check needed\n", nc->dest_files[i]);
			rc = -1;
		}
		unlink(nc->bak_files[i]);
	}
	purge_om_sections();
	unlink(nc->marker);
	nc->active = false;
	nc->committed = false;
	reload_network();
	ULOG_NOTE("netconf: transaction %u rolled back%s\n", gen,
		  rc ? " (with errors)" : "");
	if (nc->on_done)
		nc->on_done(false, gen, nc->priv);
	return rc;
}

void om_netconf_recover(struct om_netconf *nc, const char *state_dir)
{
	char marker[OM_PATH_MAX], bak[OM_PATH_MAX];
	struct stat st;
	int nfiles = 1;
	FILE *f;

	snprintf(marker, sizeof(marker), "%s/%s", state_dir, TXN_MARKER);
	if (stat(marker, &st) < 0)
		return;		/* no pending transaction */
	f = fopen(marker, "r");
	if (f) {
		char line[64];

		while (fgets(line, sizeof(line), f))
			if (sscanf(line, "files=%d", &nfiles) == 1)
				break;
		fclose(f);
	}
	if (nfiles < 1 || nfiles > 2)
		nfiles = 1;

	ULOG_WARN("netconf: unconfirmed transaction at boot, restoring\n");
	snprintf(bak, sizeof(bak), "%s/wireless.om-bak", state_dir);
	if (stat(bak, &st) == 0) {
		copy_file(bak, "/etc/config/wireless");
		unlink(bak);
	}
	if (nfiles == 2) {
		snprintf(bak, sizeof(bak), "%s/network.om-bak", state_dir);
		if (stat(bak, &st) == 0) {
			copy_file(bak, "/etc/config/network");
			unlink(bak);
		}
	}
	unlink(marker);
	nc->active = false;
	reload_network();
}
