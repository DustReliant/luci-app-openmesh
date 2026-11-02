/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure helpers for the wired-backhaul LAN port selection (m19).
 * Zero runtime dependencies: host-testable via tests/test_lanports.c.
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lanports.h"

bool om_port_name_valid(const char *name, size_t len)
{
	if (!name || len == 0 || len > OM_PORT_NAME_MAX)
		return false;
	for (size_t i = 0; i < len; i++) {
		char c = name[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	return true;
}

bool om_lan_ports_valid(const char *spec)
{
	const char *p;
	size_t seg = 0;

	if (!spec || !*spec)
		return true;		/* absent/empty = no wired leg */
	if (!strcmp(spec, OM_PORTS_ALL))
		return true;
	for (p = spec; ; p++) {
		char c = *p;

		if (c == ',' || c == '\0') {
			if (seg == 0)
				return false;	/* ",a" / "a," / "a,,b" */
			if (seg > OM_PORT_NAME_MAX)
				return false;
			if (c == '\0')
				break;
			seg = 0;
		} else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			   (c >= '0' && c <= '9') || c == '_') {
			seg++;
		} else {
			return false;	/* rejects shell/uci metacharacters */
		}
	}
	return strlen(spec) <= OM_PORTS_SPEC_MAX;
}

int om_lan_ports_each(const char *spec,
		      void (*cb)(const char *port, size_t len, void *priv),
		      void *priv)
{
	const char *p = spec;
	int n = 0;

	if (!spec || !cb)
		return 0;
	while (*p) {
		const char *seg = p;

		while (*p && *p != ',')
			p++;
		if (p > seg) {
			cb(seg, (size_t)(p - seg), priv);
			n++;
		}
		if (!*p)
			break;
		p++;			/* skip ',' */
	}
	return n;
}
