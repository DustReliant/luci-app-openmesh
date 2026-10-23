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

/* mesh interface names look like "phy1-mesh0" (iw) */
static void poll_wifi_links(struct om_monitor *m)
{
	FILE *p = popen("iw dev 2>/dev/null | awk '/Interface/{i=$2} "
			"/type mesh/{print i}'", "r");
	char ifname[32];
	char cmd[128];

	while (p && fgets(ifname, sizeof(ifname), p)) {
		trim(ifname);
		if (!*ifname)
			continue;
		FILE *a;
		char line[256], prev_mac[20] = "";

		snprintf(cmd, sizeof(cmd),
			 "iwinfo %s assoclist 2>/dev/null", ifname);
		a = popen(cmd, "r");
		if (!a)
			continue;
		while (fgets(line, sizeof(line), a)) {
			struct om_link *l;
			char mac[20];
			int rssi = 0;
			unsigned rate = 0;

			trim(line);
			if (mac_ok(line)) {
				snprintf(mac, sizeof(mac), "%s", line);
				snprintf(prev_mac, sizeof(prev_mac), "%s",
					 mac);
				continue;
			}
			if (!prev_mac[0])
				continue;
			if (sscanf(line, " signal: %d dBm", &rssi) == 1 ||
			    sscanf(line, " signal: -%d dBm", &rssi) == 1) {
				l = next_slot(m);
				if (!l)
					break;
				memset(l, 0, sizeof(*l));
				l->type = OM_LINK_WIFI;
				snprintf(l->ifname, sizeof(l->ifname), "%s",
					 ifname);
				snprintf(l->peer, sizeof(l->peer), "%s",
					 prev_mac);
				l->rssi = (int8_t)rssi;
				l->seen = time(NULL);
			}
			if (sscanf(line, " tx bitrate: %u", &rate) == 1 &&
			    m->n_links > 0) {
				m->links[m->n_links - 1].tx_rate = rate;
			}
		}
		pclose(a);
	}
	if (p)
		pclose(p);
}

static void poll_batman_links(struct om_monitor *m)
{
	char cmd[96];
	FILE *p;
	char line[256];
	bool header = true;

	snprintf(cmd, sizeof(cmd), "batctl meshif %s originators -n 2>/dev/null",
		 OM_BAT_IF);
	p = popen(cmd, "r");
	if (!p)
		return;
	while (fgets(line, sizeof(line), p)) {
		char mac[20], last[24], via[20], q[8];
		double quality;
		unsigned rate_kbps = 0;
		struct om_link *l;

		trim(line);
		if (header) {	/* skip legend lines */
			if (strstr(line, "Originator"))
				header = false;
			continue;
		}
		if (sscanf(line, "%19s %23s %19s %7s %lf",
			   mac, last, via, q, &quality) < 5)
			continue;
		if (!mac_ok(mac))
			continue;
		/* possible trailing "( x Mbit/s)" candidate qualifier */
		{
			char *sp = strchr(line, '(');
			double mb = 0;

			if (sp && sscanf(sp, "( %lf Mbit/s", &mb) == 1)
				rate_kbps = (unsigned)(mb * 1000);
		}
		if (!strcmp(via, "0.0.0.0"))
			continue;	/* self entry */
		l = next_slot(m);
		if (!l)
			break;
		memset(l, 0, sizeof(*l));
		l->type = OM_LINK_BATMAN;
		snprintf(l->peer, sizeof(l->peer), "%s", mac);
		snprintf(l->via, sizeof(l->via), "%s", via);
		l->tx_rate = rate_kbps;
		l->seen = time(NULL);
	}
	pclose(p);
}

void om_monitor_poll(struct om_monitor *m)
{
	m->n_links = 0;
	m->last_poll = time(NULL);
	poll_wifi_links(m);
	poll_batman_links(m);
}
