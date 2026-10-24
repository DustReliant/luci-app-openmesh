// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * openmeshd - OpenMesh Manager daemon
 *
 * M2: uloop event loop, per-node Ed25519 identity (M1) and the UDP
 * discovery protocol (M2). Still to land: ubus API (M3), adoption
 * session (M4); see docs/architecture.md section 3.
 */

/* uloop.h needs struct sigaction, which strict -std=c11 hides in musl. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>

#include <sodium.h>

#include <libubox/uloop.h>
#include <libubox/ulog.h>

#include "identity.h"
#include "discovery.h"
#include "config.h"
#include "ubus_api.h"
#include "session_net.h"
#include "netconf.h"
#include "monitor.h"

#define OM_VERSION       "0.0.1"
#define OM_STATE_DIR     "/etc/openmesh"
#define OM_HEARTBEAT_MS  (30 * 1000)
#define OM_TICK_MS       500

static bool opt_debug;
static const char *opt_statedir = OM_STATE_DIR;
static const char *opt_dest_addr = "255.255.255.255";
static const char *opt_ctrl_addr;
static bool opt_join_open;
static int opt_listen_port;
static int opt_dest_port;

static struct om_identity identity;
static struct om_disc disc;
static struct om_ubus ubus_ctx;
static struct om_snet snet;
static struct om_netconf netconf;
static struct om_monitor mon;
static struct uloop_timeout heartbeat_timer;
static struct uloop_timeout tick_timer;
static struct uloop_fd disc_fd;
static time_t next_announce;

static void on_txn_done(bool committed, uint32_t generation, void *priv)
{
	(void)priv;
	ULOG_NOTE("netconf: transaction %u %s\n", generation,
		  committed ? "committed" : "rolled back");
	om_ubus_notify_txn(&ubus_ctx, committed);
}

static void on_adopted(struct om_snet *s, const struct om_adopted_node *n,
		       void *priv)
{
	char hex[OM_ID_HEX_LEN];

	(void)s;
	(void)priv;
	om_id_to_hex(n->id, hex);
	ULOG_NOTE("session: node adopted %.16s.. name=%s\n", hex, n->name);
	om_ubus_notify_adopted(&ubus_ctx, n);
}

static void on_agent_state(struct om_snet *s, bool online, void *priv)
{
	(void)s;
	(void)priv;
	ULOG_NOTE("session: controller link %s\n",
		  online ? "established" : "lost");
}

static void log_new_node(struct om_disc *d, const struct om_node_info *n,
			 void *priv)
{
	char hex[OM_ID_HEX_LEN];

	(void)d;
	(void)priv;
	om_id_to_hex(n->id, hex);
	ULOG_NOTE("discovery: new node %.16s.. role=%s model=%s fw=%s from %s\n",
		  hex, n->role, n->model, n->fw, n->addr);
	om_ubus_notify_node_new(&ubus_ctx, n);
}

static void heartbeat_cb(struct uloop_timeout *t)
{
	ULOG_INFO("heartbeat\n");
	uloop_timeout_set(t, OM_HEARTBEAT_MS);
}

static void tick_cb(struct uloop_timeout *t)
{
	time_t now = time(NULL);

	if (now >= next_announce) {
		uint32_t base = (uint32_t)disc.cfg.interval;

		if (om_disc_send(&disc) < 0)
			ULOG_WARN("announce send failed: %s\n",
				  strerror(errno));
		next_announce = now + base * 4 / 5 +
			randombytes_uniform(base * 2 / 5 + 1);
		om_monitor_poll(&mon);	/* same cadence as announces */
	}
	om_disc_tick(&disc, now);
	uloop_timeout_set(t, OM_TICK_MS);
}

static void disc_fd_cb(struct uloop_fd *fd, unsigned int events)
{
	(void)fd;
	if (events & ULOOP_READ)
		om_disc_recv_ready(&disc);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-d] [-s state_dir] [-P listen_port] [-D dest_port]\n"
		"          [-A dest_addr] [-c controller_ip[:port]] [-j]\n",
		prog);
}

int main(int argc, char **argv)
{
	struct om_main_cfg mc;
	struct om_disc_cfg dc;
	struct om_snet_cfg sc;
	char id_hex[OM_ID_HEX_LEN];
	int ch;

	while ((ch = getopt(argc, argv, "ds:P:D:A:c:j")) != -1) {
		switch (ch) {
		case 'd':
			opt_debug = true;
			break;
		case 's':
			opt_statedir = optarg;
			break;
		case 'P':
			opt_listen_port = atoi(optarg);
			break;
		case 'D':
			opt_dest_port = atoi(optarg);
			break;
		case 'A':
			opt_dest_addr = optarg;
			break;
		case 'c':
			opt_ctrl_addr = optarg;
			break;
		case 'j':
			opt_join_open = true;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	ulog_open(ULOG_SYSLOG | (opt_debug ? ULOG_STDIO : 0),
		  LOG_DAEMON, "openmeshd");

	if (om_main_load(&mc) < 0) {
		ULOG_ERR("config load failed\n");
		return 1;
	}
	if (!mc.enabled) {
		ULOG_WARN("disabled in UCI, exiting\n");
		return 0;
	}

	if (om_identity_init(&identity, opt_statedir) < 0) {
		ULOG_ERR("identity init failed in %s "
			 "(file corrupt? remove %s/identity to reset)\n",
			 opt_statedir, opt_statedir);
		return 1;
	}
	om_id_to_hex(identity.id, id_hex);

	memset(&dc, 0, sizeof(dc));
	dc.interval = mc.interval;
	dc.listen_port = opt_listen_port ? opt_listen_port : mc.port;
	dc.dest_port = opt_dest_port ? opt_dest_port : dc.listen_port;
	snprintf(dc.dest_addr, sizeof(dc.dest_addr), "%s", opt_dest_addr);
	snprintf(dc.role, sizeof(dc.role), "%s", mc.role);
	snprintf(dc.model, sizeof(dc.model), "%s", mc.model);
	snprintf(dc.fw, sizeof(dc.fw), "%s", mc.fw);
	snprintf(dc.mac, sizeof(dc.mac), "%s", mc.mac);

	if (om_disc_init(&disc, &dc, &identity) < 0) {
		ULOG_ERR("discovery init failed on port %d: %s\n",
			 dc.listen_port, strerror(errno));
		return 1;
	}
	disc.on_new = log_new_node;

	uloop_init();

	/* role: explicit uci role wins; auto = controller unless a
	 * controller address is configured */
	memset(&sc, 0, sizeof(sc));
	{
		const char *ctrl = opt_ctrl_addr ? opt_ctrl_addr
						 : mc.controller_addr;

		if (!strcmp(mc.role, "controller"))
			sc.is_controller = true;
		else if (!strcmp(mc.role, "agent"))
			sc.is_controller = false;
		else
			sc.is_controller = (ctrl[0] == '\0');

		if (!sc.is_controller) {
			char addr[64];

			snprintf(addr, sizeof(addr), "%s", ctrl);
			char *colon = strrchr(addr, ':');

			if (colon) {
				*colon = '\0';
				sc.ctrl_port = atoi(colon + 1);
			} else {
				sc.ctrl_port = mc.session_port;
			}
			snprintf(sc.ctrl_addr, sizeof(sc.ctrl_addr), "%s",
				 addr);
		}
		sc.listen_port = mc.session_port;
		sc.join_open = opt_join_open || mc.join_open;
		snprintf(sc.state_dir, sizeof(sc.state_dir), "%s",
			 opt_statedir);
	}

	if (om_snet_init(&snet, &sc, &identity) < 0) {
		ULOG_ERR("session init failed (port %d in use?): %s\n",
			 sc.listen_port, strerror(errno));
		return 1;
	}
	snet.on_adopted = on_adopted;
	snet.on_agent_state = on_agent_state;

	/* ubus RPC belongs to the controller role only (LuCI lives there);
	 * agent instances run headless and log instead */
	if (sc.is_controller) {
		if (om_ubus_init(&ubus_ctx, &identity, &disc) < 0)
			ULOG_WARN("ubus unavailable, running without RPC\n");
		om_ubus_set_snet(&ubus_ctx, &snet);
		om_ubus_set_netconf(&ubus_ctx, &netconf, opt_statedir);
		om_ubus_set_monitor(&ubus_ctx, &mon);
	}

	om_netconf_recover(&netconf, opt_statedir);
	netconf.on_done = on_txn_done;
	om_snet_set_netconf(&snet, &netconf, opt_statedir);

	heartbeat_timer.cb = heartbeat_cb;
	uloop_timeout_set(&heartbeat_timer, 1000);
	tick_timer.cb = tick_cb;
	uloop_timeout_set(&tick_timer, 10);
	disc_fd.fd = disc.fd;
	disc_fd.cb = disc_fd_cb;
	uloop_fd_add(&disc_fd, ULOOP_READ);

	ULOG_NOTE("openmeshd %s started (M2)%s\n", OM_VERSION,
		  opt_debug ? " [debug]" : "");
	ULOG_NOTE("identity: node_id=%s%s\n", id_hex,
		  identity.generated ? " (new)" : "");
	ULOG_NOTE("discovery: announcing role=%s model=%s fw=%s "
		  "listen=%d dest=%d every %ds\n",
		  dc.role, dc.model, dc.fw, dc.listen_port, dc.dest_port,
		  dc.interval);

	uloop_run();

	uloop_done();
	om_snet_done(&snet);
	om_ubus_done(&ubus_ctx);
	om_disc_free(&disc);

	ULOG_NOTE("openmeshd %s stopped\n", OM_VERSION);
	ulog_close();
	return 0;
}
