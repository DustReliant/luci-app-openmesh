// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * openmeshd - backhaul link monitoring (V0.2).
 *
 * Sources: iwinfo assoclist on 802.11s interfaces (peer RSSI/rate) and
 * batctl originators on om_bat (path selection). Shells out at a low
 * cadence (once per announce interval); parsing is defensive throughout.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libubox/ulog.h>

#include "monitor.h"
#include "batman_parse.h"
#include "batman_collect.h"
#include "netconf.h"	/* OM_BAT_IF */

static void trim(char *s)
{
	size_t n = strlen(s);

	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
		     s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
}

static bool mac_ok(const char *s)
{
	if (strlen(s) != 17)
		return false;
	for (int i = 0; i < 17; i++) {
		if (i % 3 == 2) {
			if (s[i] != ':')
				return false;
		} else if (!isxdigit((unsigned char)s[i])) {
			return false;
		}
	}
	return true;
}

static struct om_link *next_slot(struct om_monitor *m)
{
	if (m->n_links >= OM_MAX_LINKS)
		return NULL;
	return &m->links[m->n_links++];
}

static struct om_sta *next_sta_slot(struct om_monitor *m)
{
	if (m->n_stas >= OM_MAX_STAS)
		return NULL;
	return &m->stas[m->n_stas++];
}

/*
 * 802.11s peers. r19 finding: 24.10 iwinfo emits the same compact
 * one-line-per-station format for mesh peers as for AP clients
 * ("<MAC>  -27 dBm / -92 dBm (SNR 65)  50 ms ago" + RX:/TX: lines),
 * not the old per-field "signal:" layout M11 was written against
 * (that path never saw real peer data before dual-node bring-up).
 */
static void poll_wifi_links(struct om_monitor *m)
{
	FILE *p = popen("iw dev 2>/dev/null | awk '/Interface/{i=$2} "
			"/type mesh/{print i}'", "r");
	char ifname[32];
	char cmd[128];

	while (p && fgets(ifname, sizeof(ifname), p)) {
		char line[256];
		FILE *a;
		int cur = -1;

		trim(ifname);
		if (!*ifname)
			continue;
		snprintf(m->mesh_ifname, sizeof(m->mesh_ifname), "%s",
			 ifname);
		snprintf(cmd, sizeof(cmd),
			 "iwinfo %s assoclist > /tmp/.om_mwl 2>/dev/null", ifname);
		system(cmd);
		a = fopen("/tmp/.om_mwl", "r");
		if (!a)
			continue;
		while (fgets(line, sizeof(line), a)) {
			char mac[20];
			int rssi = 0;
			double mb;

			trim(line);
			if (sscanf(line, "%19s %d dBm", mac, &rssi) >= 2 &&
			    mac_ok(mac)) {
				struct om_link *l = next_slot(m);

				if (!l)
					break;
				memset(l, 0, sizeof(*l));
				l->type = OM_LINK_WIFI;
				snprintf(l->ifname, sizeof(l->ifname), "%s",
					 ifname);
				snprintf(l->peer, sizeof(l->peer), "%s", mac);
				l->rssi = (int8_t)rssi;
				l->seen = time(NULL);
				cur = m->n_links - 1;
			} else if (cur >= 0) {
				if (sscanf(line, "TX: %lf MBit/s", &mb) == 1)
					m->links[cur].tx_rate =
						(uint32_t)(mb * 1000);
			}
		}
		pclose(a);
	}
	if (p)
		fclose(p);
}

/* staging sink: parsed rows land in the batman staging table; they are
 * merged into the live link table only after a healthy collection */
static void batman_stage_sink(void *priv, const struct om_link *in)
{
	struct om_monitor *m = priv;

	if (m->n_batman_stage >= OM_MAX_LINKS)
		return;
	m->batman_stage[m->n_batman_stage] = *in;
	m->batman_stage[m->n_batman_stage++].seen = time(NULL);
}

/*
 * r21b acceptance: execute layer (om_batman_collect, bounded by the
 * UCI-configurable timeout) is separated from the pure parser. A failed
 * collection is COLLECT_FAILED, never "link gone": last-known batman
 * links are retained OM_BATMAN_RETAIN_S seconds and the snapshot flags
 * batman_collect=stale|failed so consumers can tell collection trouble
 * from real network change. Node online/offline is session-derived and
 * untouched by any of this. Warnings fire on state transitions only
 * (fail -> one WARN, recovery -> one NOTE), no log spam.
 */
static void poll_batman_links(struct om_monitor *m)
{
	static bool format_warned;
	FILE *p;
	int parsed;
	unsigned skipped = 0;

	if (om_batman_collect(OM_BAT_IF,
			      m->batctl_timeout ?: OM_BATCTL_TIMEOUT_DEFAULT,
			      "/tmp/.om_bat") < 0) {
		if (!m->batman_failed) {
			ULOG_WARN("batctl: collection failed/timeout "
				  "(timeout=%us), retaining last-known "
				  "links\n",
				  m->batctl_timeout ?:
				  OM_BATCTL_TIMEOUT_DEFAULT);
			m->batman_failed = true;
		}
		return;
	}
	m->n_batman_stage = 0;	/* collect ok: stage is rebuilt fresh */
	p = fopen("/tmp/.om_bat", "r");
	if (!p)
		return;
	parsed = om_batman_parse(p, batman_stage_sink, m, &skipped);
	fclose(p);
	if (parsed < 0) {
		if (!format_warned) {
			ULOG_WARN("batctl: unknown originators header "
				  "layout, poll skipped\n");
			format_warned = true;
		}
		return;		/* format change == collection failure too */
	}
	if (skipped && !parsed && !format_warned) {
		ULOG_WARN("batctl: %u originator rows failed to parse, "
			  "layout may have changed\n", skipped);
		format_warned = true;
		return;
	}
	format_warned = false;
	m->batman_last_ok = time(NULL);
	if (m->batman_failed) {
		m->batman_failed = false;
		ULOG_NOTE("batctl: collection recovered\n");
	}
}

/*
 * Associated stations on local AP interfaces. 24.10 iwinfo layout
 * (verified on device, see docs/design/m16-clients.md):
 *   <MAC>  -49 dBm / -92 dBm (SNR 43)  3630 ms ago
 *     RX: 6.0 MBit/s ...
 *     TX: 780.0 MBit/s ...
 * AP ifaces show as "type AP" in iw dev (mesh peers are "type mesh"
 * and use a different, per-field-line assoclist layout).
 */
static void poll_ap_clients(struct om_monitor *m)
{
	FILE *p = popen("iw dev 2>/dev/null | awk '/Interface/{i=$2} "
			"/type AP/{print i}'", "r");
	char ifname[32];
	char cmd[128];

	while (p && fgets(ifname, sizeof(ifname), p)) {
		char ssid[33] = "";
		char line[256];
		FILE *a;
		int cur = -1;

		trim(ifname);
		if (!*ifname)
			continue;

		snprintf(cmd, sizeof(cmd), "iwinfo %s info 2>/dev/null",
			 ifname);
		FILE *f = popen(cmd, "r");

		while (f && fgets(line, sizeof(line), f)) {
			char *q = strstr(line, "ESSID: \"");
			char *end;

			if (!q)
				continue;
			snprintf(ssid, sizeof(ssid), "%s", q + 8);
			end = strchr(ssid, '"');
			if (end)
				*end = '\0';
			break;
		}
		if (f)
			pclose(f);

		snprintf(cmd, sizeof(cmd), "iwinfo %s assoclist 2>/dev/null",
			 ifname);
		a = popen(cmd, "r");
		if (!a)
			continue;
		while (fgets(line, sizeof(line), a)) {
			char mac[20];
			int rssi = 0;
			double mb;

			trim(line);
			if (sscanf(line, "%19s %d dBm", mac, &rssi) >= 2 &&
			    mac_ok(mac)) {
				struct om_sta *s = next_sta_slot(m);

				if (!s)
					break;
				memset(s, 0, sizeof(*s));
				snprintf(s->mac, sizeof(s->mac), "%s", mac);
				snprintf(s->ifname, sizeof(s->ifname), "%s",
					 ifname);
				snprintf(s->ssid, sizeof(s->ssid), "%s",
					 ssid);
				s->rssi = (int8_t)rssi;
				s->seen = time(NULL);
				cur = s - m->stas;
			} else if (cur >= 0) {
				if (sscanf(line, " RX: %lf MBit/s", &mb) == 1)
					m->stas[cur].rx_rate =
						(uint32_t)(mb * 1000);
				else if (sscanf(line, " TX: %lf MBit/s",
						&mb) == 1)
					m->stas[cur].tx_rate =
						(uint32_t)(mb * 1000);
			}
		}
		pclose(a);
	}
	if (p)
		pclose(p);
}

void om_monitor_poll(struct om_monitor *m)
{
	time_t now = time(NULL);

	m->n_links = 0;
	m->n_stas = 0;
	m->mesh_ifname[0] = '\0';
	m->last_poll = now;
	poll_wifi_links(m);
	poll_batman_links(m);
	/* merge batman stage: fresh rows on success; on COLLECT_FAILED keep
	 * last-known rows for OM_BATMAN_RETAIN_S (flagged via
	 * om_monitor_batman_health()), then drop -- never a silent wipe */
	if (!m->batman_failed ||
	    now - m->batman_last_ok <= OM_BATMAN_RETAIN_S)
		for (unsigned i = 0; i < m->n_batman_stage &&
			     m->n_links < OM_MAX_LINKS; i++)
			m->links[m->n_links++] = m->batman_stage[i];
	poll_ap_clients(m);
}
