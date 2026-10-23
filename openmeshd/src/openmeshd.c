// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * openmeshd - OpenMesh Manager daemon
 *
 * M1: uloop event loop, periodic heartbeat and per-node Ed25519 identity.
 * Subsystems still to land: discovery (M2), ubus API (M3), adoption session
 * (M4); see docs/architecture.md section 3.
 */

/* uloop.h needs struct sigaction, which strict -std=c11 hides in musl. */
#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>

#include <libubox/uloop.h>
#include <libubox/ulog.h>

#include "identity.h"

#define OM_VERSION       "0.0.1"
#define OM_STATE_DIR     "/etc/openmesh"
#define OM_HEARTBEAT_MS  (30 * 1000)

static bool opt_debug;
static struct om_identity identity;
static struct uloop_timeout heartbeat_timer;

static void heartbeat_cb(struct uloop_timeout *t)
{
	ULOG_INFO("heartbeat\n");
	uloop_timeout_set(t, OM_HEARTBEAT_MS);
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-d]\n", prog);
}

int main(int argc, char **argv)
{
	char id_hex[OM_ID_HEX_LEN];
	int ch;

	while ((ch = getopt(argc, argv, "d")) != -1) {
		switch (ch) {
		case 'd':
			opt_debug = true;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	ulog_open(ULOG_SYSLOG | (opt_debug ? ULOG_STDIO : 0),
		  LOG_DAEMON, "openmeshd");

	if (om_identity_init(&identity, OM_STATE_DIR) < 0) {
		ULOG_ERR("identity init failed in %s "
			 "(file corrupt? remove %s/identity to reset)\n",
			 OM_STATE_DIR, OM_STATE_DIR);
		return 1;
	}
	om_id_to_hex(identity.id, id_hex);

	uloop_init();
	heartbeat_timer.cb = heartbeat_cb;
	uloop_timeout_set(&heartbeat_timer, 1000);

	ULOG_NOTE("openmeshd %s started (M1)%s\n", OM_VERSION,
		  opt_debug ? " [debug]" : "");
	ULOG_NOTE("identity: node_id=%s%s\n", id_hex,
		  identity.generated ? " (new)" : "");

	uloop_run();
	uloop_done();

	ULOG_NOTE("openmeshd %s stopped\n", OM_VERSION);
	ulog_close();
	return 0;
}
