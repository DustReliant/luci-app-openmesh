// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * openmeshd - UDP discovery protocol v1.
 *
 * Packet layout (<= OM_PKT_MAX bytes), see docs/design/m2-discovery.md:
 *   0   "OMAN" magic(4), version u8
 *   5   timestamp u64 big-endian
 *   13  nonce(24)
 *   37  node_id(32)  == SHA256(pubkey)
 *   69  pubkey(32)
 *   101 JSON body {"role":..,"model":..,"fw":..} incl. NUL, part of signature
 *   end Ed25519 signature(64) over bytes [5 .. end of body]
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <sodium.h>

#include "discovery.h"

#define OM_MAGIC     "OMAN"
#define OM_PROTO_VER 1

static void put_u64be(uint8_t *p, uint64_t v)
{
	for (int i = 7; i >= 0; i--) {
		p[i] = (uint8_t)v;
		v >>= 8;
	}
}

static uint64_t get_u64be(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

/* minimal flat JSON scanner: "key":"value", no escapes */
int om_json_get_str(const char *body, size_t len, const char *key,
		    char *out, size_t outsz)
{
	char pat[24];
	int n = snprintf(pat, sizeof(pat), "\"%s\":\"", key);

	if (n <= 0 || (size_t)n >= sizeof(pat))
		return -1;
	for (size_t i = 0; i + (size_t)n < len; i++) {
		if (memcmp(body + i, pat, (size_t)n) != 0)
			continue;
		size_t j = i + (size_t)n, k = 0;
		while (j < len && body[j] != '"' && body[j] != '\0') {
			if (k + 1 < outsz)
				out[k++] = body[j];
			j++;
		}
		if (j >= len || body[j] != '"')
			return -1;
		out[k] = '\0';
		return 0;
	}
	return -1;
}

size_t om_disc_build_packet(const struct om_disc *d, uint8_t *out,
			    size_t outlen, uint64_t ts)
{
	uint8_t nonce[24];
	size_t total, region;
	int n;

	if (outlen < OM_MIN_LEN)
		return 0;

	memcpy(out, OM_MAGIC, 4);
	out[4] = OM_PROTO_VER;
	put_u64be(out + 5, ts);
	randombytes_buf(nonce, sizeof(nonce));
	memcpy(out + 13, nonce, sizeof(nonce));
	memcpy(out + 37, d->id->id, OM_ID_LEN);
	memcpy(out + 69, d->id->pk, OM_PK_LEN);

	n = snprintf((char *)out + OM_HDR_LEN,
		     outlen - OM_HDR_LEN - OM_SIG_LEN,
		     "{\"role\":\"%s\",\"model\":\"%s\",\"fw\":\"%s\"}",
		     d->cfg.role, d->cfg.model, d->cfg.fw);
	if (n < 0)
		return 0;

	/* body bytes incl. its NUL terminator are part of the signature */
	total = OM_HDR_LEN + (size_t)n + 1 + OM_SIG_LEN;
	if (total > outlen)
		return 0;
	region = total - OM_SIG_LEN - 5;
	crypto_sign_ed25519_detached(out + total - OM_SIG_LEN, NULL,
				     out + 5, region, d->id->sk);
	return total;
}

static struct om_node_info *find_node(struct om_disc *d, const uint8_t *id)
{
	for (unsigned i = 0; i < d->n_nodes; i++)
		if (memcmp(d->nodes[i].id, id, OM_ID_LEN) == 0)
			return &d->nodes[i];
	return NULL;
}

static struct om_node_info *alloc_node(struct om_disc *d, const uint8_t *id,
				       const uint8_t *pk, time_t now)
{
	struct om_node_info *n;

	if (d->n_nodes < OM_DISC_MAX_NODES) {
		n = &d->nodes[d->n_nodes++];
	} else {
		/* table full: evict the node seen longest ago */
		n = &d->nodes[0];
		for (unsigned i = 1; i < d->n_nodes; i++)
			if (d->nodes[i].last_seen < n->last_seen)
				n = &d->nodes[i];
	}
	memset(n, 0, sizeof(*n));
	memcpy(n->id, id, OM_ID_LEN);
	memcpy(n->pk, pk, OM_PK_LEN);
	n->first_seen = now;
	return n;
}

int om_disc_handle_packet(struct om_disc *d, const uint8_t *buf, size_t len,
			  const char *src, time_t now)
{
	struct om_node_info *n;
	uint8_t idh[OM_ID_LEN];
	uint64_t ts;
	const char *body;
	size_t body_max;
	char role[16], model[48], fw[32];
	bool is_new = false;

	/* gate 1: length, magic, version */
	if (len < OM_MIN_LEN || len > OM_PKT_MAX)
		return -1;
	if (memcmp(buf, OM_MAGIC, 4) != 0 || buf[4] != OM_PROTO_VER)
		return -1;

	/* gate 2: node_id must bind to pubkey */
	crypto_hash_sha256(idh, buf + 69, OM_PK_LEN);
	if (memcmp(idh, buf + 37, OM_ID_LEN) != 0)
		return -1;

	/* gate 3: signature over [5 .. body end] */
	if (crypto_sign_ed25519_verify_detached(buf + len - OM_SIG_LEN,
						buf + 5, len - OM_SIG_LEN - 5,
						buf + 69) != 0)
		return -1;

	/* gate 4: freshness, only when the local clock is plausible */
	ts = get_u64be(buf + 5);
	if ((uint64_t)now > 1700000000ULL) {
		uint64_t t = (uint64_t)now;
		if (ts + OM_TS_WINDOW < t || t + OM_TS_WINDOW < ts)
			return -1;
	}

	/* gate 5: replay - nonce ring per node */
	n = find_node(d, buf + 37);
	if (n) {
		for (int i = 0; i < n->n_nonces; i++)
			if (memcmp(n->nonces[i], buf + 13, 24) == 0)
				return -1;
	} else {
		n = alloc_node(d, buf + 37, buf + 69, now);
		is_new = true;
	}

	/* body must be NUL-terminated inside the signed region */
	body = (const char *)buf + OM_HDR_LEN;
	body_max = len - OM_SIG_LEN - OM_HDR_LEN;
	if (!memchr(body, '\0', body_max))
		return -1;

	/* strict: role is mandatory; model/fw optional */
	if (om_json_get_str(body, body_max, "role", role, sizeof(role)) < 0)
		return -1;
	if (om_json_get_str(body, body_max, "model", model, sizeof(model)) < 0)
		snprintf(model, sizeof(model), "unknown");
	if (om_json_get_str(body, body_max, "fw", fw, sizeof(fw)) < 0)
		snprintf(fw, sizeof(fw), "unknown");

	/* commit */
	if (n->n_nonces < OM_NONCE_RING) {
		memcpy(n->nonces[n->n_nonces], buf + 13, 24);
		n->n_nonces++;
	} else {
		memmove(n->nonces, n->nonces + 1,
			sizeof(n->nonces) - 24);
		memcpy(n->nonces[OM_NONCE_RING - 1], buf + 13, 24);
	}
	snprintf(n->role, sizeof(n->role), "%s", role);
	snprintf(n->model, sizeof(n->model), "%s", model);
	snprintf(n->fw, sizeof(n->fw), "%s", fw);
	if (src)
		snprintf(n->addr, sizeof(n->addr), "%s", src);
	n->last_seen = now;

	if (is_new && d->on_new)
		d->on_new(d, n, d->priv);

	return 0;
}

int om_disc_init(struct om_disc *d, const struct om_disc_cfg *cfg,
		 const struct om_identity *id)
{
	struct sockaddr_in a;
	int fd, on = 1;

	memset(d, 0, sizeof(*d));
	d->cfg = *cfg;
	d->id = id;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0)
		goto fail;
	if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
		goto fail;

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons((uint16_t)cfg->listen_port);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
		goto fail;

	d->fd = fd;
	return 0;

fail:
	close(fd);
	return -1;
}

void om_disc_free(struct om_disc *d)
{
	if (d->fd >= 0)
		close(d->fd);
	d->fd = -1;
}

int om_disc_send(struct om_disc *d)
{
	struct sockaddr_in dst;
	uint8_t pkt[OM_PKT_MAX];
	size_t n = om_disc_build_packet(d, pkt, sizeof(pkt),
					(uint64_t)time(NULL));

	if (n == 0)
		return -1;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons((uint16_t)d->cfg.dest_port);
	if (inet_pton(AF_INET, d->cfg.dest_addr, &dst.sin_addr) != 1)
		return -1;
	return sendto(d->fd, pkt, n, 0, (struct sockaddr *)&dst,
		      sizeof(dst)) == (ssize_t)n ? 0 : -1;
}

void om_disc_recv_ready(struct om_disc *d)
{
	uint8_t buf[OM_PKT_MAX];
	struct sockaddr_in from;
	socklen_t flen;
	char addr[48];

	for (;;) {
		ssize_t r;

		flen = sizeof(from);
		r = recvfrom(d->fd, buf, sizeof(buf), 0,
			     (struct sockaddr *)&from, &flen);
		if (r < 0)
			break;	/* EAGAIN / EINTR: nothing more */
		if (!inet_ntop(AF_INET, &from.sin_addr, addr, sizeof(addr)))
			continue;
		om_disc_handle_packet(d, buf, (size_t)r, addr, time(NULL));
	}
}

void om_disc_tick(struct om_disc *d, time_t now)
{
	time_t timeout = d->cfg.interval * 3;
	unsigned i = 0;

	if (timeout < 30)
		timeout = 30;
	while (i < d->n_nodes) {
		if (now - d->nodes[i].last_seen > timeout) {
			memmove(&d->nodes[i], &d->nodes[i + 1],
				(d->n_nodes - i - 1) * sizeof(d->nodes[i]));
			d->n_nodes--;
		} else {
			i++;
		}
	}
}
