// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * openmeshd - OpenMesh Manager daemon
 *
 * M0 skeleton: uloop event loop plus a periodic heartbeat log line, so the
 * toolchain, libubox linkage, procd supervision and packaging are all proven
 * before real subsystems (discovery/session/netconf/monitor/ubus_api, see
 * docs/architecture.md section 3) land from M2 onwards.
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

#define OM_VERSION       "0.0.1"
#define OM_HEARTBEAT_MS  (30 * 1000)

static bool opt_debug;
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

	uloop_init();
	heartbeat_timer.cb = heartbeat_cb;
	uloop_timeout_set(&heartbeat_timer, 1000);

	ULOG_NOTE("openmeshd %s started (M0 skeleton)%s\n",
		  OM_VERSION, opt_debug ? " [debug]" : "");

	uloop_run();
	uloop_done();

	ULOG_NOTE("openmeshd %s stopped\n", OM_VERSION);
	ulog_close();
	return 0;
}
