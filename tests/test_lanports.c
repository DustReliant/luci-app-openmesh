/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host tests for the m19 wired-leg port spec (pure functions only).
 * Build & run via tests/host-build.sh.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lanports.h"

static int pass, fail;

#define CHECK(cond) do { \
	if (cond) { pass++; } \
	else { fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void collect_cb(const char *port, size_t len, void *priv)
{
	char **out = priv;

	snprintf(*out, len + 1, "%s", port);
	*out += len + 1;
}

int main(void)
{
	/* ---- om_lan_ports_valid ---- */
	CHECK(om_lan_ports_valid(NULL));	/* absent = no wired leg */
	CHECK(om_lan_ports_valid(""));		/* empty = no wired leg */
	CHECK(om_lan_ports_valid("all"));	/* whole-bridge selector */
	CHECK(om_lan_ports_valid("lan1"));
	CHECK(om_lan_ports_valid("lan1,lan2"));
	CHECK(om_lan_ports_valid("lan1,lan2,lan3"));
	CHECK(om_lan_ports_valid("eth0"));
	CHECK(om_lan_ports_valid("eth0,eth1"));
	CHECK(om_lan_ports_valid("Eth_9"));	/* case + underscore legal */
	CHECK(om_lan_ports_valid("aaaaaaaaaaaaaaa"));	/* 15 chars = IFNAMSIZ-1 */

	/* empty segments rejected */
	CHECK(!om_lan_ports_valid(",lan1"));
	CHECK(!om_lan_ports_valid("lan1,"));
	CHECK(!om_lan_ports_valid("lan1,,lan2"));
	CHECK(!om_lan_ports_valid(","));

	/* shell / uci metacharacters rejected (injection red line) */
	CHECK(!om_lan_ports_valid("lan1;reboot"));
	CHECK(!om_lan_ports_valid("lan1 rm -rf /"));
	CHECK(!om_lan_ports_valid("lan1|power"));
	CHECK(!om_lan_ports_valid("lan1$(x)"));
	CHECK(!om_lan_ports_valid("lan1`x`"));
	CHECK(!om_lan_ports_valid("lan-1"));
	CHECK(!om_lan_ports_valid("lan.1"));
	CHECK(!om_lan_ports_valid("lan'1"));
	CHECK(!om_lan_ports_valid("lan\"1"));
	CHECK(!om_lan_ports_valid("lan/1"));
	CHECK(!om_lan_ports_valid("lan\\1"));

	/* length bounds */
	CHECK(!om_lan_ports_valid("aaaaaaaaaaaaaaaa"));		/* 16-char name */
	CHECK(!om_lan_ports_valid("lan1,aaaaaaaaaaaaaaaa"));	/* bad member */
	{
		char big[80];
		memset(big, 'a', sizeof(big));
		big[79] = '\0';
		CHECK(!om_lan_ports_valid(big));	/* > spec bound */
	}
	/* "all" plus friends: only exact "all" is the selector */
	CHECK(om_lan_ports_valid("allon"));		/* plain (odd) name */
	CHECK(om_lan_ports_valid("all,lan1"));	/* no special meaning */

	/* ---- om_port_name_valid ---- */
	CHECK(om_port_name_valid("lan1", 4));
	CHECK(om_port_name_valid("eth0", 4));
	CHECK(!om_port_name_valid("lan1", 0));
	CHECK(!om_port_name_valid(NULL, 4));
	CHECK(!om_port_name_valid("lan1", 5));		/* length mismatch is caller's */
	CHECK(!om_port_name_valid("lan-1", 5));
	CHECK(!om_port_name_valid("aaaaaaaaaaaaaaaa", 16));

	/* ---- om_lan_ports_each ---- */
	{
		char buf[128] = { 0 }, *p = buf;

		CHECK(om_lan_ports_each("lan1,lan2,lan3", collect_cb, &p) == 3);
		CHECK(!strcmp(buf, "lan1"));
		CHECK(!strcmp(buf + 5, "lan2"));
		CHECK(!strcmp(buf + 10, "lan3"));
	}
	CHECK(om_lan_ports_each("", collect_cb, NULL) == 0);
	CHECK(om_lan_ports_each(NULL, collect_cb, NULL) == 0);
	CHECK(om_lan_ports_each("lan1", NULL, NULL) == 0);
	{
		char buf[32] = { 0 }, *p = buf;

		CHECK(om_lan_ports_each("lan1", collect_cb, &p) == 1);
		CHECK(!strcmp(buf, "lan1"));
	}

	printf("test_lanports: %d PASS, %d FAIL\n", pass, fail);
	return fail ? 1 : 0;
}
