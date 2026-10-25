// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * openmeshd - session plane networking: controller listener, agent
 * outbound connection, handshake state machine, keepalives.
 * Every peer failure only tears down that peer (workflow rule: one bad
 * node must never take the daemon down).
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <sodium.h>

#include <libubox/ulog.h>

#include "session_net.h"
#include "store.h"
#include "netconf.h"
#include "discovery.h"

#define OM_HS_TIMEOUT 10	/* seconds per handshake step */
#define OM_KA_PERIOD  15
#define OM_KA_DEAD    45

/* agent-side config transaction hook (set via om_snet_set_netconf) */
static struct om_netconf *agent_netconf;
static char agent_state_dir[OM_PATH_MAX];

static void peer_close(struct om_snet_peer *p);
static void agent_connect(struct om_snet *s);
static void peer_send_frame(struct om_snet_peer *p, const uint8_t *f,
			    size_t len);

static int write_file_atomic_0600(const char *path, const uint8_t *buf,
				  size_t len)
{
	char tmp[OM_PATH_MAX];
	int fd;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -1;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (write(fd, buf, len) != (ssize_t)len || fsync(fd) < 0) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	close(fd);
	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static struct om_adopted_node *adopted_find(struct om_snet *s,
					    const uint8_t id[OM_ID_LEN])
{
	for (unsigned i = 0; i < s->n_adopted; i++)
		if (!memcmp(s->adopted[i].id, id, OM_ID_LEN))
			return &s->adopted[i];
	return NULL;
}

static bool pending_has(struct om_snet *s, const uint8_t id[OM_ID_LEN])
{
	for (int i = 0; i < s->n_pending; i++)
		if (!memcmp(s->pending[i].id, id, OM_ID_LEN))
			return true;
	return false;
}

static const char *pending_name(struct om_snet *s, const uint8_t id[OM_ID_LEN])
{
	for (int i = 0; i < s->n_pending; i++)
		if (!memcmp(s->pending[i].id, id, OM_ID_LEN))
			return s->pending[i].name;
	return NULL;
}

static void pending_del(struct om_snet *s, const uint8_t id[OM_ID_LEN])
{
	for (int i = 0; i < s->n_pending; i++) {
		if (!memcmp(s->pending[i].id, id, OM_ID_LEN)) {
			memmove(&s->pending[i], &s->pending[i + 1],
				(s->n_pending - i - 1) * sizeof(s->pending[0]));
			s->n_pending--;
			return;
		}
	}
}

static bool read_exact(const char *path, uint8_t *buf, size_t len)
{
	FILE *f = fopen(path, "rb");
	bool ok = false;

	if (!f)
		return false;
	if (fread(buf, 1, len, f) == len)
		ok = true;
	fclose(f);
	return ok;
}

static void load_file(const char *path, uint8_t *buf, size_t len)
{
	(void)read_exact(path, buf, len);
}

/* ---------- keepalive / timeouts ---------- */

static void agent_down(struct om_snet *s, bool schedule);

static void ctrl_ka_timer_cb(struct uloop_timeout *t)
{
	struct om_snet_peer *p = container_of(t, struct om_snet_peer, timer);
	uint8_t ka[OM_SES_HDR_LEN + 64];
	size_t n;

	if (p->state != PS_ESTABLISHED ||
	    time(NULL) - p->last_rx > OM_KA_DEAD) {
		peer_close(p);
		return;
	}
	n = om_ses_seal(ka, sizeof(ka), p->k_c2a, ++p->tx_counter,
			OM_SES_KA, NULL, 0);
	if (!n) {
		peer_close(p);
		return;
	}
	peer_send_frame(p, ka, n);
	uloop_timeout_set(t, OM_KA_PERIOD * 1000);
}

static void ctrl_hs_timer_cb(struct uloop_timeout *t)
{
	struct om_snet_peer *p = container_of(t, struct om_snet_peer, timer);

	peer_close(p);		/* handshake step timed out */
}

static void agent_hs_timer_cb(struct uloop_timeout *t)
{
	struct om_snet_peer *p = container_of(t, struct om_snet_peer, timer);
	struct om_snet *s = p->s;

	if (p->fd > 0) {
		uloop_fd_delete(&p->fdw);
		close(p->fd);
		p->fd = -1;
	}
	s->backoff = s->agent_adopted ? 10 : 5;
	uloop_timeout_set(&s->retry, s->backoff * 1000);
}

/* M17: established agents must swap the handshake timeout for a
 * dead-peer check; leaving agent_hs_timer_cb armed killed every
 * session 10s after ESTABLISHED (the 20s flap: kill + 10s backoff). */
static void agent_ka_timer_cb(struct uloop_timeout *t)
{
	struct om_snet_peer *p = container_of(t, struct om_snet_peer, timer);
	struct om_snet *s = p->s;

	if (p->state != PS_ESTABLISHED ||
	    time(NULL) - p->last_rx > OM_KA_DEAD) {
		agent_down(s, true);
		return;
	}
	uloop_timeout_set(t, OM_KA_PERIOD * 1000);
}

/* ---------- controller ---------- */

static void ctrl_peer_online(struct om_snet *s, struct om_snet_peer *p,
			     struct om_adopted_node *a, bool notify)
{
	a->online = true;
	a->last_seen = time(NULL);
	p->state = PS_ESTABLISHED;
	p->last_rx = time(NULL);
	uloop_timeout_cancel(&p->timer);
	p->timer.cb = ctrl_ka_timer_cb;
	uloop_timeout_set(&p->timer, OM_KA_PERIOD * 1000);
	if (notify && s->on_adopted)
		s->on_adopted(s, a, s->adopted_priv);
}

static void ctrl_handle_frame(struct om_snet *s, struct om_snet_peer *p,
			      const uint8_t *raw, size_t rawlen)
{
	struct om_ses_frame f;
	uint8_t out[OM_SES_MAX_FRAME];
	uint8_t idh[OM_ID_LEN];
	struct om_adopted_node *a;
	size_t n;

	if (om_ses_parse(raw, rawlen, &f) < 0)
		goto bad;

	switch (p->state) {
	case PS_HELLO:
		if (f.type != OM_SES_HELLO || f.payload_len != 120)
			goto bad;
		memcpy(p->agent_id, f.payload + 32, OM_ID_LEN);
		memcpy(p->agent_pk, f.payload + 64, OM_PK_LEN);
		memcpy(p->n1, f.payload + 96, 24);
		crypto_hash_sha256(idh, p->agent_pk, OM_PK_LEN);
		if (memcmp(idh, p->agent_id, OM_ID_LEN) != 0)
			goto bad;
		a = adopted_find(s, p->agent_id);
		if (!a && !pending_has(s, p->agent_id)) {
			peer_close(p);	/* unknown node: agent retries */
			return;
		}
		om_ses_hs_init(&p->hs);
		randombytes_buf(p->n2, 24);
		if (om_ses_derive(p->k_a2c, p->k_c2a, p->hs.eph_priv,
				  f.payload, p->n1, p->n2,
				  p->agent_pk) < 0)
			goto bad;
		p->tx_counter = p->rx_counter = 0;
		n = om_ses_challenge(out, sizeof(out), &p->hs, p->n2);
		if (!n)
			goto bad;
		peer_send_frame(p, out, n);
		p->state = PS_PROOF;
		return;

	case PS_PROOF:
		if (f.type != OM_SES_PROOF || f.payload_len != 64)
			goto bad;
		if (om_ses_check_proof(p->agent_pk, p->n1, p->n2,
				       f.payload) < 0)
			goto bad;
		a = adopted_find(s, p->agent_id);
		if (a) {	/* returning node */
			uint8_t pl[OM_ID_LEN];

			memcpy(pl, s->id->id, OM_ID_LEN);
			n = om_ses_seal(out, sizeof(out), p->k_c2a,
					++p->tx_counter, OM_SES_WELCOME,
					pl, sizeof(pl));
			if (!n)
				goto bad;
			peer_send_frame(p, out, n);
			ctrl_peer_online(s, p, a, false);
			return;
		}
		/* first adoption */
		if (s->n_adopted >= OM_SNET_MAX_PEERS)
			goto bad;
		if (!s->have_network_key) {
			char path[OM_PATH_MAX];

			randombytes_buf(s->network_key, 32);
			snprintf(path, sizeof(path), "%s/network.key",
				 s->cfg.state_dir);
			if (write_file_atomic_0600(path, s->network_key,
						   32) < 0)
				goto bad;
			s->have_network_key = true;
		}
		a = &s->adopted[s->n_adopted++];
		memset(a, 0, sizeof(*a));
		memcpy(a->id, p->agent_id, OM_ID_LEN);
		memcpy(a->pk, p->agent_pk, OM_PK_LEN);
		{
			const char *pname = pending_name(s, p->agent_id);

			if (pname && *pname)
				snprintf(a->name, sizeof(a->name), "%s",
					 pname);
			else
				snprintf(a->name, sizeof(a->name), "node-%u",
					 s->n_adopted);
		}
		if (om_store_adopted(a->id, a->pk, a->name) < 0)
			ULOG_WARN("store: persisting adopted node failed "
				  "(memory-only until next adopt)\n");
		pending_del(s, p->agent_id);
		{
			uint8_t pl[64];

			memcpy(pl, s->network_key, 32);
			memcpy(pl + 32, s->id->id, OM_ID_LEN);
			n = om_ses_seal(out, sizeof(out), p->k_c2a,
					++p->tx_counter, OM_SES_GRANT,
					pl, sizeof(pl));
		}
		if (!n)
			goto bad;
		peer_send_frame(p, out, n);
		ctrl_peer_online(s, p, a, true);
		return;

	case PS_ESTABLISHED: {
		uint8_t pt[OM_SES_MAX_FRAME];
		uint8_t t;
		size_t ptlen = 0;

		if (om_ses_unseal(raw, rawlen, p->k_a2c, p->rx_counter + 1,
				  &t, pt, &ptlen) < 0)
			goto bad;
		p->rx_counter++;
		p->last_rx = time(NULL);
		return;		/* KA/ACK: nothing further in V0.1 */
	}

	default:
		goto bad;
	}

bad:
	peer_close(p);
}

static void ctrl_peer_cb(struct uloop_fd *fd, unsigned int events)
{
	struct om_snet_peer *p = container_of(fd, struct om_snet_peer, fdw);
	uint8_t frame[OM_SES_MAX_FRAME];
	ssize_t r;

	/* uloop reports no error events here: recv()<=0 covers them */
	(void)events;
	r = recv(p->fd, frame, sizeof(frame), 0);
	if (r <= 0) {
		peer_close(p);
		return;
	}
	ctrl_handle_frame(p->s, p, frame, (size_t)r);
}

static void ctrl_accept_cb(struct uloop_fd *fd, unsigned int events)
{
	struct om_snet *s = container_of(fd, struct om_snet, lfd);
	struct sockaddr_in from;
	socklen_t flen = sizeof(from);
	struct om_snet_peer *p = NULL;
	int cfd, one = 1;

	(void)events;
	cfd = accept(s->listen_fd, (struct sockaddr *)&from, &flen);
	if (cfd < 0)
		return;
	if (fcntl(cfd, F_SETFL, O_NONBLOCK) < 0) {
		close(cfd);
		return;
	}
	setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	for (unsigned i = 0; i < OM_SNET_MAX_PEERS; i++)
		if (s->peers[i].fd <= 0) {
			p = &s->peers[i];
			break;
		}
	if (!p) {
		close(cfd);	/* peer table full */
		return;
	}
	memset(p, 0, sizeof(*p));
	p->fd = cfd;
	p->s = s;
	p->state = PS_HELLO;
	p->last_rx = time(NULL);
	p->timer.cb = ctrl_hs_timer_cb;
	uloop_timeout_set(&p->timer, OM_HS_TIMEOUT * 1000);
	p->fdw.cb = ctrl_peer_cb;
	p->fdw.fd = cfd;
	uloop_fd_add(&p->fdw, ULOOP_READ);
}

/* ---------- agent ---------- */

static void agent_next_backoff(struct om_snet *s)
{
	if (!s->agent_adopted) {
		s->backoff = 5;	/* unadopted: retry fast */
		return;
	}
	if (s->backoff < 10)
		s->backoff = 10;
	else if (s->backoff < 120)
		s->backoff *= 2;
	if (s->backoff > 120)
		s->backoff = 120;
}

static void agent_down(struct om_snet *s, bool schedule)
{
	struct om_snet_peer *p = &s->agent;
	bool was_up = (p->state == PS_ESTABLISHED);

	if (p->fd > 0) {
		uloop_fd_delete(&p->fdw);
		uloop_timeout_cancel(&p->timer);
		close(p->fd);
		p->fd = -1;
	}
	p->state = PS_HELLO;
	if (was_up && s->on_agent_state)
		s->on_agent_state(s, false, s->agent_priv);
	if (schedule) {
		agent_next_backoff(s);
		uloop_timeout_set(&s->retry, s->backoff * 1000);
	}
}

static void agent_timer_cb(struct uloop_timeout *t)
{
	struct om_snet *s = container_of(t, struct om_snet, retry);

	agent_connect(s);
}

static void agent_rx_cb(struct uloop_fd *fd, unsigned int events)
{
	struct om_snet *s = container_of(fd, struct om_snet, agent.fdw);
	struct om_snet_peer *p = &s->agent;
	uint8_t frame[OM_SES_MAX_FRAME];
	struct om_ses_frame f;
	uint8_t out[OM_SES_MAX_FRAME];
	ssize_t r;
	size_t n;

	(void)events;
	r = recv(p->fd, frame, sizeof(frame), 0);
	if (r <= 0) {
		agent_down(s, true);
		return;
	}
	if (om_ses_parse(frame, (size_t)r, &f) < 0) {
		agent_down(s, true);
		return;
	}

	switch (p->state) {
	case PS_AWAIT_CHALLENGE:
		if (f.type != OM_SES_CHALLENGE || f.payload_len != 56) {
			agent_down(s, true);
			return;
		}
		memcpy(p->n2, f.payload + 32, 24);
		if (om_ses_derive(p->k_a2c, p->k_c2a, p->hs.eph_priv,
				  f.payload, p->n1, p->n2, s->id->pk) < 0) {
			agent_down(s, true);
			return;
		}
		p->tx_counter = p->rx_counter = 0;
		n = om_ses_proof(out, sizeof(out), s->id, p->n1, p->n2);
		if (!n) {
			agent_down(s, true);
			return;
		}
		peer_send_frame(p, out, n);
		p->state = PS_HELLO;	/* now awaiting enc GRANT/WELCOME */
		return;

	case PS_HELLO:
	case PS_ESTABLISHED: {
		uint8_t pt[OM_SES_MAX_FRAME];
		uint8_t t;
		size_t ptlen = 0;

		if (om_ses_unseal(frame, (size_t)r, p->k_c2a,
				  p->rx_counter + 1, &t, pt, &ptlen) < 0) {
			agent_down(s, true);
			return;
		}
		p->rx_counter++;
		p->last_rx = time(NULL);

		/* agent: received traffic proves the mesh config did not
		 * cut the controller link -> confirm pending txn */
		if (agent_netconf && agent_netconf->active &&
		    p->state == PS_ESTABLISHED)
			om_netconf_confirm(agent_netconf);

		if (p->state == PS_HELLO && t == OM_SES_GRANT) {
			char path[OM_PATH_MAX];
			char hex[OM_ID_HEX_LEN];
			uint8_t pin[OM_ID_LEN];

			if (!s->cfg.join_open || ptlen != 64) {
				ULOG_WARN("refusing GRANT "
					  "(join_open=%d len=%zu)\n",
					  s->cfg.join_open, ptlen);
				agent_down(s, false);
				return;
			}
			snprintf(path, sizeof(path), "%s/network.key",
				 s->cfg.state_dir);
			if (write_file_atomic_0600(path, pt, 32) < 0) {
				agent_down(s, true);
				return;
			}
			memcpy(pin, pt + 32, OM_ID_LEN);
			snprintf(path, sizeof(path), "%s/controller.pin",
				 s->cfg.state_dir);
			write_file_atomic_0600(path, pin, OM_ID_LEN);
			memcpy(s->ctrl_pin, pin, OM_ID_LEN);
			s->agent_adopted = true;
			n = om_ses_seal(out, sizeof(out), p->k_a2c,
					++p->tx_counter, OM_SES_ACK, NULL, 0);
			if (n)
				peer_send_frame(p, out, n);
			p->state = PS_ESTABLISHED;
			om_id_to_hex(pin, hex);
			ULOG_NOTE("session: adopted by controller %.16s..\n",
				  hex);
			s->backoff = 0;
			uloop_timeout_cancel(&p->timer);
			p->timer.cb = agent_ka_timer_cb;
			uloop_timeout_set(&p->timer, OM_KA_PERIOD * 1000);
			if (s->on_agent_state)
				s->on_agent_state(s, true, s->agent_priv);
			return;
		}
		if (p->state == PS_HELLO && t == OM_SES_WELCOME) {
			char path[OM_PATH_MAX];

			if (ptlen != OM_ID_LEN) {
				agent_down(s, true);
				return;
			}
			if (s->agent_adopted &&
			    memcmp(s->ctrl_pin, pt, OM_ID_LEN) != 0) {
				ULOG_ERR("controller pin mismatch, "
					 "refusing session\n");
				agent_down(s, false);
				return;
			}
			memcpy(s->ctrl_pin, pt, OM_ID_LEN);
			snprintf(path, sizeof(path), "%s/controller.pin",
				 s->cfg.state_dir);
			write_file_atomic_0600(path, pt, OM_ID_LEN);
			s->agent_adopted = true;
			p->state = PS_ESTABLISHED;
			s->backoff = 0;
			uloop_timeout_cancel(&p->timer);
			p->timer.cb = agent_ka_timer_cb;
			uloop_timeout_set(&p->timer, OM_KA_PERIOD * 1000);
			if (s->on_agent_state)
				s->on_agent_state(s, true, s->agent_priv);
			return;
		}
		if (t == OM_SES_KA) {	/* reply keepalive */
			n = om_ses_seal(out, sizeof(out), p->k_a2c,
					++p->tx_counter, OM_SES_KA, NULL, 0);
			if (n)
				peer_send_frame(p, out, n);
			return;
		}
		if (t == OM_SES_CONFIG && agent_netconf) {
			char mesh_id[33], band[8], key[33], mode[8];
			int bh = OM_BH_WIFI;

			if (om_json_get_str((const char *)pt, ptlen,
					    "mesh_id", mesh_id,
					    sizeof(mesh_id)) == 0 &&
			    om_json_get_str((const char *)pt, ptlen, "band",
					    band, sizeof(band)) == 0 &&
			    om_json_get_str((const char *)pt, ptlen, "key",
					    key, sizeof(key)) == 0) {
				if (om_json_get_str((const char *)pt, ptlen,
						    "mode", mode,
						    sizeof(mode)) == 0 &&
				    !strcmp(mode, "hybrid"))
					bh = OM_BH_HYBRID;
				ULOG_NOTE("session: mesh config received, "
					  "applying (mode=%s)\n",
					  bh == OM_BH_HYBRID ? "hybrid"
							     : "wifi");
				om_netconf_mesh_apply(agent_netconf,
						      agent_state_dir,
						      mesh_id, band, key,
						      bh);
			}
			return;
		}
		return;
	}

	default:
		agent_down(s, true);
	}
}

static void agent_connect_cb(struct uloop_fd *fd, unsigned int events)
{
	struct om_snet *s = container_of(fd, struct om_snet, agent.fdw);
	struct om_snet_peer *p = &s->agent;
	uint8_t out[OM_SES_MAX_FRAME];
	int err = 0;
	socklen_t elen = sizeof(err);
	size_t n;

	(void)events;
	if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 ||
	    err != 0) {
		agent_down(s, true);
		return;
	}
	om_ses_hs_init(&p->hs);
	randombytes_buf(p->n1, 24);
	n = om_ses_hello(out, sizeof(out), &p->hs, s->id, p->n1);
	if (!n) {
		agent_down(s, true);
		return;
	}
	peer_send_frame(p, out, n);
	p->state = PS_AWAIT_CHALLENGE;
	p->timer.cb = agent_hs_timer_cb;
	uloop_timeout_set(&p->timer, OM_HS_TIMEOUT * 1000);
	uloop_fd_delete(&p->fdw);
	p->fdw.cb = agent_rx_cb;
	uloop_fd_add(&p->fdw, ULOOP_READ);
}

static void agent_connect(struct om_snet *s)
{
	struct om_snet_peer *p = &s->agent;
	struct sockaddr_in dst;
	int fd, one = 1;

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons((uint16_t)s->cfg.ctrl_port);
	if (inet_pton(AF_INET, s->cfg.ctrl_addr, &dst.sin_addr) != 1) {
		uloop_timeout_set(&s->retry, 30000);
		return;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		uloop_timeout_set(&s->retry, 5000);
		return;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	memset(p, 0, sizeof(*p));
	p->fd = fd;
	p->s = s;
	p->state = PS_CONNECTING;
	if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0 &&
	    errno != EINPROGRESS) {
		close(fd);
		p->fd = -1;
		s->backoff = 5;
		uloop_timeout_set(&s->retry, 5000);
		return;
	}
	p->fdw.fd = fd;
	p->fdw.cb = agent_connect_cb;
	uloop_fd_add(&p->fdw, ULOOP_WRITE | ULOOP_EDGE_TRIGGER);
}

/* ---------- shared ---------- */

void om_snet_set_netconf(struct om_snet *s, struct om_netconf *nc,
			 const char *state_dir)
{
	agent_netconf = nc;
	snprintf(agent_state_dir, sizeof(agent_state_dir), "%s", state_dir);
	(void)s;
}

void om_snet_push_mesh(struct om_snet *s, const char *mesh_id,
		       const char *band, const char *key, const char *mode)
{
	char json[224];
	uint8_t out[OM_SES_MAX_FRAME];
	size_t n;
	int len = snprintf(json, sizeof(json),
			   "{\"mesh_id\":\"%s\",\"band\":\"%s\","
			   "\"key\":\"%s\",\"mode\":\"%s\"}",
			   mesh_id, band, key, mode ? mode : "wifi");

	if (len <= 0 || (size_t)len >= sizeof(json))
		return;
	for (unsigned i = 0; i < OM_SNET_MAX_PEERS; i++) {
		struct om_snet_peer *p = &s->peers[i];

		if (p->fd <= 0 || p->state != PS_ESTABLISHED)
			continue;
		n = om_ses_seal(out, sizeof(out), p->k_c2a, ++p->tx_counter,
				OM_SES_CONFIG, (const uint8_t *)json,
				(size_t)len + 1);
		if (n)
			peer_send_frame(p, out, n);
	}
}

static void peer_send_frame(struct om_snet_peer *p, const uint8_t *f,
			    size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t w = send(p->fd, f + off, len - off, MSG_NOSIGNAL);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			peer_close(p);
			return;
		}
		off += (size_t)w;
	}
}

static void peer_close(struct om_snet_peer *p)
{
	if (p == &p->s->agent) {
		agent_down(p->s, true);
		return;
	}
	/* controller peer: mark only this peer's adopted node offline */
	if (p->fd > 0) {
		struct om_adopted_node *a;

		uloop_fd_delete(&p->fdw);
		uloop_timeout_cancel(&p->timer);
		close(p->fd);
		a = adopted_find(p->s, p->agent_id);
		if (a && a->online) {
			a->online = false;
			a->last_seen = time(NULL);
		}
		memset(p, 0, sizeof(*p));
		p->fd = -1;
	}
}

/* ---------- api ---------- */

static int load_adopted_cb(const struct om_stored_node *n, void *priv)
{
	struct om_snet *s = priv;
	struct om_adopted_node *a;

	if (s->n_adopted >= OM_SNET_MAX_PEERS)
		return 1;
	a = &s->adopted[s->n_adopted++];
	memset(a, 0, sizeof(*a));
	memcpy(a->id, n->id, OM_ID_LEN);
	memcpy(a->pk, n->pk, OM_PK_LEN);
	snprintf(a->name, sizeof(a->name), "%s", n->name);
	return 0;
}

int om_snet_init(struct om_snet *s, const struct om_snet_cfg *cfg,
		 struct om_identity *id)
{
	struct sockaddr_in a;
	int one = 1;
	char path[OM_PATH_MAX];

	memset(s, 0, sizeof(*s));
	s->cfg = *cfg;
	s->id = id;
	s->agent.fd = -1;

	snprintf(path, sizeof(path), "%s/network.key", cfg->state_dir);
	if (cfg->is_controller) {
		s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (s->listen_fd < 0)
			return -1;
		setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one,
			   sizeof(one));
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		a.sin_port = htons((uint16_t)cfg->listen_port);
		if (bind(s->listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
		    listen(s->listen_fd, 4) < 0) {
			close(s->listen_fd);
			s->listen_fd = -1;
			return -1;
		}
		fcntl(s->listen_fd, F_SETFL, O_NONBLOCK);
		s->lfd.fd = s->listen_fd;
		s->lfd.cb = ctrl_accept_cb;
		uloop_fd_add(&s->lfd, ULOOP_READ);
		s->have_network_key =
			read_exact(path, s->network_key, 32);
		om_store_foreach(load_adopted_cb, s);
		return 0;
	}

	/* agent */
	s->agent_adopted = read_exact(path, s->network_key, 32);
	snprintf(path, sizeof(path), "%s/controller.pin", cfg->state_dir);
	load_file(path, s->ctrl_pin, OM_ID_LEN);
	s->retry.cb = agent_timer_cb;
	uloop_timeout_set(&s->retry, 1000);
	return 0;
}

void om_snet_done(struct om_snet *s)
{
	if (s->listen_fd > 0) {
		uloop_fd_delete(&s->lfd);
		close(s->listen_fd);
		s->listen_fd = -1;
	}
	for (unsigned i = 0; i < OM_SNET_MAX_PEERS; i++)
		if (s->peers[i].fd > 0)
			peer_close(&s->peers[i]);
	if (s->agent.fd > 0)
		agent_down(s, false);
}

int om_snet_adopt(struct om_snet *s, const uint8_t id[OM_ID_LEN],
		  const char *name)
{
	struct om_adopted_node *a = adopted_find(s, id);

	if (a) {
		if (name && *name) {
			snprintf(a->name, sizeof(a->name), "%s", name);
			om_store_rename(id, name);
		}
		return 0;
	}
	if (s->n_pending >= OM_SNET_MAX_PENDING)
		return -1;
	if (pending_has(s, id))
		return 0;
	memcpy(s->pending[s->n_pending].id, id, OM_ID_LEN);
	snprintf(s->pending[s->n_pending].name,
		 sizeof(s->pending[s->n_pending].name), "%s",
		 (name && *name) ? name : "");
	s->n_pending++;
	return 0;
}

int om_snet_revoke(struct om_snet *s, const uint8_t id[OM_ID_LEN])
{
	struct om_adopted_node *a = adopted_find(s, id);
	unsigned idx;

	if (!a)
		return -1;
	for (unsigned i = 0; i < OM_SNET_MAX_PEERS; i++)
		if (s->peers[i].fd > 0 &&
		    !memcmp(s->peers[i].agent_id, id, OM_ID_LEN))
			peer_close(&s->peers[i]);
	idx = (unsigned)(a - s->adopted);
	memmove(a, a + 1, (s->n_adopted - idx - 1) * sizeof(*a));
	s->n_adopted--;
	om_store_remove(id);
	return 0;
}

bool om_snet_is_adopted(struct om_snet *s, const uint8_t id[OM_ID_LEN])
{
	return adopted_find(s, id) != NULL;
}
