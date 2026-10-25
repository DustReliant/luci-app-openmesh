// SPDX-License-Identifier: GPL-3.0-or-later
/* Pure batctl originators parser -- see batman_parse.h for the layering
 * rationale and the format contract. Extracted verbatim from monitor.c
 * in r21 (behaviour-preserving; guarded by tests/test_batman.c which
 * replays the exact rows verified on hardware). */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "batman_parse.h"

static void trim(char *s)
{
	size_t n = strlen(s);

	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
		     s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
}

static int mac_ok(const char *s)
{
	if (strlen(s) != 17)
		return 0;
	for (int i = 0; i < 17; i++) {
		if (i % 3 == 2) {
			if (s[i] != ':')
				return 0;
		} else if (!isxdigit((unsigned char)s[i])) {
			return 0;
		}
	}
	return 1;
}

int om_batman_parse(FILE *p, om_link_sink sink, void *priv,
		    unsigned *skipped)
{
	char line[256];
	bool header = true;
	unsigned parsed = 0, bad = 0;

	if (skipped)
		*skipped = 0;
	while (fgets(line, sizeof(line), p)) {
		char mac[20], last[24], via[20], q[16];
		double quality;
		struct om_link l;

		trim(line);
		if (header) {	/* skip banner until the column header */
			if (!strstr(line, "Originator"))
				continue;
			header = false;
			/* guard against future layout changes */
			if (!strstr(line, "(#/255)") ||
			    !strstr(line, "Nexthop"))
				return -1;
			continue;
		}
		{
			char *s = line;

			while (*s == ' ' || *s == '\t')
				s++;
			if (*s == '*')
				s++;
			if (sscanf(s, "%19s %23s (%lf) %19s [%15s]",
				   mac, last, &quality, via, q) < 3) {
				bad++;
				continue;
			}
			{
				size_t ql = strlen(q);

				if (ql && q[ql - 1] == ']')
					q[ql - 1] = '\0';
			}
		}
		if (!mac_ok(mac)) {
			bad++;
			continue;
		}
		/* direct (single-hop) originators legitimately have
		 * nexthop == originator; own entries are not listed */
		memset(&l, 0, sizeof(l));
		l.type = OM_LINK_BATMAN;
		snprintf(l.peer, sizeof(l.peer), "%s", mac);
		snprintf(l.via, sizeof(l.via), "%s", q[0] ? q : via);
		l.tq = quality > 255 ? 255 : (uint8_t)quality;
		sink(priv, &l);
		parsed++;
	}
	if (skipped)
		*skipped = bad;
	return (int)parsed;
}

int om_batman_parse_text(const char *text, om_link_sink sink, void *priv,
			 unsigned *skipped)
{
	FILE *f = fmemopen((void *)text, strlen(text), "r");
	int rc;

	if (!f) {
		if (skipped)
			*skipped = 0;
		return 0;
	}
	rc = om_batman_parse(f, sink, priv, skipped);
	fclose(f);
	return rc;
}
