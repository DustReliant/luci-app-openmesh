// SPDX-License-Identifier: GPL-3.0-or-later
/* openmeshd - UCI + system info glue (target only; not host-unit-tested). */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <uci.h>

#include "config.h"
#include "discovery.h"

static void read_file_line(const char *path, const char *prefix, char *out,
			   size_t sz, const char *def)
{
	FILE *f;
	char line[128];
	size_t pl = strlen(prefix);

	snprintf(out, sz, "%s", def);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, prefix, pl) != 0)
			continue;
		char *v = line + pl;
		size_t l = strcspn(v, "'\"\n");
		if (l >= sz)
			l = sz - 1;
		memcpy(out, v, l);
		out[l] = '\0';
		break;
	}
	fclose(f);
}

int om_main_load(struct om_main_cfg *m)
{
	struct uci_context *c;
	char str[] = "openmesh.main";
	struct uci_ptr p;

	memset(m, 0, sizeof(*m));
	m->enabled = true;
	m->interval = 10;
	m->port = OM_DISC_PORT_DEFAULT;
	snprintf(m->role, sizeof(m->role), "auto");

	read_file_line("/tmp/sysinfo/model", "", m->model, sizeof(m->model),
		       "unknown");
	read_file_line("/etc/openwrt_release", "DISTRIB_RELEASE='", m->fw,
		       sizeof(m->fw), "unknown");

	c = uci_alloc_context();
	if (!c)
		return -1;

	if (uci_lookup_ptr(c, &p, str, false) == UCI_OK && p.last &&
	    p.last->type == UCI_TYPE_SECTION) {
		struct uci_section *s = uci_to_section(p.last);
		struct uci_option *o;

		if ((o = uci_lookup_option(c, s, "enabled")) &&
		    o->type == UCI_TYPE_STRING) {
			m->enabled = !strcmp(o->v.string, "1") ||
				     !strcasecmp(o->v.string, "on") ||
				     !strcasecmp(o->v.string, "true");
		}
		if ((o = uci_lookup_option(c, s, "role")) &&
		    o->type == UCI_TYPE_STRING)
			snprintf(m->role, sizeof(m->role), "%s", o->v.string);
		if ((o = uci_lookup_option(c, s, "announce_interval")) &&
		    o->type == UCI_TYPE_STRING)
			m->interval = atoi(o->v.string);
		if ((o = uci_lookup_option(c, s, "announce_port")) &&
		    o->type == UCI_TYPE_STRING)
			m->port = atoi(o->v.string);
	}
	uci_free_context(c);

	if (m->interval < 2)
		m->interval = 2;
	if (m->interval > 60)
		m->interval = 60;
	if (m->port < 1 || m->port > 65535)
		m->port = OM_DISC_PORT_DEFAULT;
	return 0;
}
