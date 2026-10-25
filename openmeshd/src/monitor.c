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

static void batman_link_sink(void *priv, const struct om_link *in)
{
	struct om_monitor *m = priv;
	struct om_link *l = next_slot(m);

	if (!l)
		return;
	*l = *in;
	l->seen = time(NULL);
}

static void poll_batman_links(struct om_monitor *m)
{
	/*
	 * r21: parsing lives in the pure unit batman_parse.c (host-tested
	 * against the exact rows verified on hardware); this collector only
	 * shells out, feeds the stream and translates outcomes to one-shot
	 * warnings. batctl is wrapped in `timeout` so a wedged batctl
	 * cannot stall the daemon (r21 acceptance item).
	 */
	static bool format_warned;
	char cmd[104];
	FILE *p;
	int parsed;
	unsigned skipped = 0;

	snprintf(cmd, sizeof(cmd),
		 "timeout 5 batctl meshif %s originators -n "
		 "> /tmp/.om_bat 2>/dev/null", OM_BAT_IF);
	system(cmd);
	p = fopen("/tmp/.om_bat", "r");
	if (!p)
		return;
	parsed = om_batman_parse(p, batman_link_sink, m, &skipped);
	fclose(p);
	if (parsed < 0 && !format_warned) {
		ULOG_WARN("batctl: unknown originators header layout, "
			  "poll skipped\n");
		format_warned = true;
		return;
	}
	if (parsed > 0) {
		format_warned = false;
		return;
	}
	if (skipped && !format_warned) {
		ULOG_WARN("batctl: %u originator rows failed to parse, "
			  "layout may have changed\n", skipped);
		format_warned = true;
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
	m->n_links = 0;
	m->n_stas = 0;
	m->mesh_ifname[0] = '\0';
	m->last_poll = time(NULL);
	poll_wifi_links(m);
	poll_batman_links(m);
	poll_ap_clients(m);
}
