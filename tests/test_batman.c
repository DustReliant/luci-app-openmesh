// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Host tests for the pure batctl originators parser. Inputs replay the
 * exact rows verified on hardware (r19-r21) plus the acceptance matrix:
 * empty stream, header-only, malformed rows, '*' prefix, short fields,
 * oversized fields, many originators, unknown future header layout.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "batman_parse.h"

static struct om_link captured[OM_MAX_LINKS];
static unsigned n_captured;

static void sink(void *priv, const struct om_link *l)
{
	(void)priv;
	if (n_captured < OM_MAX_LINKS)
		captured[n_captured++] = *l;
}

static int run(const char *name, const char *buf, int want_rc,
	       unsigned want_skipped, int want_links)
{
	FILE *f = fmemopen((void *)buf, strlen(buf), "r");
	unsigned skipped = 99;
	int rc;

	n_captured = 0;
	rc = om_batman_parse(f, sink, NULL, &skipped);
	fclose(f);
	if (rc != want_rc || skipped != want_skipped ||
	    (int)n_captured != want_links) {
		printf("FAIL %s: rc=%d skipped=%u links=%u "
		       "(want rc=%d skipped=%u links=%d)\n",
		       name, rc, skipped, n_captured, want_rc, want_skipped,
		       want_links);
		return 1;
	}
	printf("ok %s\n", name);
	return 0;
}

#define HDR_STR "[B.A.T.M.A.N. adv 2024.3-openwrt-13, MainIF/MAC: x]\n" \
	   "   Originator        last-seen (#/255) Nexthop           [outgoingIF]\n" \
	   " * ec:2e:98:d2:71:28    0.900s   (231) ec:2e:98:d2:71:28 [phy0-mesh0]\n"

#define CHECK2(cond, name) do { \
	if (cond) printf("ok %s\n", name); \
	else { printf("FAIL %s\n", name); fails++; } \
} while (0)

#define HDR "[B.A.T.M.A.N. adv 2024.3-openwrt-13, MainIF/MAC: x]\n" \
	   "   Originator        last-seen (#/255) Nexthop           [outgoingIF]\n"

int main(void)
{
	int fails = 0;

	/* 1. real row from hardware (r21 evidence), with '*' prefix */
	fails += run("real-star-row",
		     HDR
		     " * ec:2e:98:d2:71:28    0.900s   (231) ec:2e:98:d2:71:28 [phy0-mesh0]\n",
		     1, 0, 1);
	if (strcmp(captured[0].peer, "ec:2e:98:d2:71:28") ||
	    captured[0].tq != 231 || strcmp(captured[0].via, "ec:2e:98:d2:71:28") ||
	    strcmp(captured[0].ifname, "phy0-mesh0") ||
	    captured[0].type != OM_LINK_BATMAN) {
		printf("FAIL field-check: peer=%s tq=%u via=%s\n",
		       captured[0].peer, captured[0].tq, captured[0].via);
		fails++;
	} else
		printf("ok field-check\n");

	/* 2. row without '*' */
	fails += run("no-star-row",
		     HDR " aa:bb:cc:dd:ee:ff 1.0s (100) aa:bb:cc:dd:ee:ff [wlan0]\n",
		     1, 0, 1);

	/* 3. empty stream */
	fails += run("empty", "", 0, 0, 0);

	/* 4. banner + header only */
	fails += run("header-only", HDR, 0, 0, 0);

	/* 5. malformed rows are skipped, good rows survive */
	fails += run("garbage-mixed",
		     HDR
		     "not a mac line at all\n"
		     " 11:22:33:44:55:66  0.5s (200) 11:22:33:44:55:66 [mesh0]\n"
		     "zz:zz:zz:zz:zz:zz  0.5s (9) zz:zz:zz:zz:zz:zz [mesh0]\n",
		     1, 2, 1);

	/* 6. too few fields */
	fails += run("short-fields",
		     HDR " aa:bb:cc:dd:ee:ff 0.5s\n", 0, 1, 0);

	/* 7. oversized MAC and oversized iface (truncated, no overflow) */
	fails += run("oversized-mac",
		     HDR " aa:bb:cc:dd:ee:ff:00 0.5s (9) x [if0]\n",
		     0, 1, 0);
	fails += run("oversized-iface",
		     HDR " aa:bb:cc:dd:ee:ff 0.5s (9) aa:bb:cc:dd:ee:ff [ifname_way_over_15]\n",
		     1, 0, 1);

	/* 8. many originators */
	fails += run("multi-originator",
		     HDR
		     " * 01:02:03:04:05:06 0.1s (250) 01:02:03:04:05:06 [m0]\n"
		     " * 02:03:04:05:06:07 0.2s (200) 02:03:04:05:06:07 [m0]\n"
		     " * 03:04:05:06:07:08 0.3s (150) 03:04:05:06:07:08 [m1]\n",
		     3, 0, 3);

	/* 9. unknown future header layout -> refuse, no links */
	fails += run("future-header",
		     "[B.A.T.M.A.N. adv 2099.0]\n"
		     "   Originator   Quality   Neighbor\n"
		     " * aa:bb:cc:dd:ee:ff 99 aa:bb:cc:dd:ee:ff\n",
		     -1, 0, 0);

	/* 10. missing [IF] column entirely still yields the link */
	fails += run("no-iface-col",
		     HDR " aa:bb:cc:dd:ee:ff 0.5s (9) aa:bb:cc:dd:ee:ff\n",
		     1, 0, 1);

	/* 09. abnormal whitespace: tabs and multi-space runs */
	fails += run("odd-whitespace",
		     HDR
		     "\t\t *\t01:02:03:04:05:06\t  0.1s\t(250)\t01:02:03:04:05:06 [m0]\n",
		     1, 0, 1);

	/* 10. CRLF line endings */
	fails += run("crlf",
		     "[B.A.T.M.A.N. adv 2024.3]\r\n"
		     "   Originator        last-seen (#/255) Nexthop           [outgoingIF]\r\n"
		     " * 01:02:03:04:05:06    0.1s   (250) 01:02:03:04:05:06 [m0]\r\n",
		     1, 0, 1);

	/* 12+13. originator disappears, then recovers (same stream minus
	 * and plus the row -- parser level; retention is monitor policy) */
	{
		const char *with_row =
			HDR " * ec:2e:98:d2:71:28 0.9s (231) ec:2e:98:d2:71:28 [phy0-mesh0]\n";
		const char *without = HDR;

		fails += run("originator-gone", without, 0, 0, 0);
		fails += run("originator-back", with_row, 1, 0, 1);
	}

	/* 15. large volume: 12 originators (OM_MAX_LINKS=16 bounds the
	 * monitor table; parser itself is unbounded, the sink caps) */
	{
		char big[4096];
		int off;

		off = snprintf(big, sizeof(big), "%s", HDR);
		for (int i = 0; i < 12; i++)
			off += snprintf(big + off, sizeof(big) - off,
					" * %02x:02:03:04:05:%02x 0.1s (%d) %02x:02:03:04:05:%02x [m0]\n",
					i, i, 250 - i, i, i);
		fails += run("large-volume", big, 12, 0, 12);
	}

	/* string-input API parity: om_batman_parse_text == om_batman_parse */
	{
		FILE *f = fmemopen((void *)HDR_STR, strlen(HDR_STR), "r");
		unsigned sk1 = 0, sk2 = 0;
		struct om_link a[2], b[2];
		int rc1, rc2;

		n_captured = 0;
		rc1 = om_batman_parse(f, sink, NULL, &sk1);
		memcpy(a, captured, sizeof(a));
		n_captured = 0;
		rc2 = om_batman_parse_text(HDR_STR, sink, NULL, &sk2);
		memcpy(b, captured, sizeof(b));
		fclose(f);
		CHECK2(rc1 == rc2 && sk1 == sk2 && n_captured == 1 &&
		       !memcmp(a, b, sizeof(a)), "string-api-parity");
	}

	printf(fails ? "test_batman: %d FAILURES\n" : "test_batman: all ok\n",
	       fails);
	return fails ? 1 : 0;
}
