// SPDX-License-Identifier: GPL-3.0-or-later
/* openmeshd - adopted-node persistence via UCI (config node sections). */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sodium.h>
#include <uci.h>

#include <libubox/ulog.h>

#include "identity.h"
#include "lanports.h"
#include "store.h"

#define OM_UCI_PKG "openmesh"

static struct uci_context *ctx_open(void)
{
	return uci_alloc_context();
}

static void ctx_close(struct uci_context *c)
{
	uci_free_context(c);
}

static int commit_pkg(struct uci_context *c, struct uci_package *p)
{
	uci_save(c, p);
	return uci_commit(c, &p, false);
}

int om_store_adopted(const uint8_t id[32], const uint8_t pk[32],
		     const char *name)
{
	struct uci_context *c = ctx_open();
	struct uci_package *p = NULL;
	struct uci_section *s = NULL;
	struct uci_ptr ptr;
	char idhex[OM_ID_HEX_LEN];
	char pkb64[64];

	if (!c)
		return -1;
	if (uci_load(c, OM_UCI_PKG, &p) != UCI_OK) {
		ctx_close(c);
		return -1;
	}
	uci_add_section(c, p, "node", &s);
	if (!s) {
		ctx_close(c);
		return -1;
	}

	om_id_to_hex(id, idhex);
	sodium_bin2base64(pkb64, sizeof(pkb64), pk, 32,
			  sodium_base64_VARIANT_ORIGINAL);

	memset(&ptr, 0, sizeof(ptr));
	ptr.package = OM_UCI_PKG;
	ptr.p = p;
	ptr.s = s;
	ptr.section = s->e.name;

	ptr.option = "id";
	ptr.value = idhex;
	uci_set(c, &ptr);
	ptr.option = "pubkey";
	ptr.value = pkb64;
	uci_set(c, &ptr);
	ptr.option = "name";
	ptr.value = name && *name ? name : "node";
	uci_set(c, &ptr);

	commit_pkg(c, p);
	ctx_close(c);
	return 0;
}

static struct uci_section *find_by_id(struct uci_context *c,
				      struct uci_package *p,
				      const uint8_t id[32], char *name_out,
				      size_t name_sz);

static struct om_stored_node *fill_stored(struct uci_context *c,
					  struct uci_section *s,
					  struct om_stored_node *out)
{
	struct uci_option *o;

	memset(out, 0, sizeof(*out));
	o = uci_lookup_option(c, s, "id");
	if (o && o->type == UCI_TYPE_STRING && o->v.string) {
		uint8_t idbin[OM_ID_LEN];

		if (om_id_from_hex(o->v.string, idbin) == 0)
			memcpy(out->id, idbin, OM_ID_LEN);
	}
	o = uci_lookup_option(c, s, "key");
	if (o && o->type == UCI_TYPE_STRING && o->v.string) {
		uint8_t pkbin[OM_PK_LEN];

		if (om_id_from_hex(o->v.string, pkbin) == 0)
			memcpy(out->pk, pkbin, OM_PK_LEN);
	}
	o = uci_lookup_option(c, s, "name");
	if (o && o->type == UCI_TYPE_STRING && o->v.string)
		snprintf(out->name, sizeof(out->name), "%s", o->v.string);
	return out;
}

int om_store_adopted_find(const uint8_t id[32], struct om_stored_node *out)
{
	struct uci_context *c = ctx_open();
	struct uci_package *p = NULL;
	struct uci_section *s;
	char secname[128];
	int rc = -1;

	if (!c)
		return -1;
	if (uci_load(c, OM_UCI_PKG, &p) == UCI_OK) {
		s = find_by_id(c, p, id, secname, sizeof(secname));
		if (s) {
			fill_stored(c, s, out);
			rc = 0;
		}
	}
	ctx_close(c);
	return rc;
}

static struct uci_section *find_by_id(struct uci_context *c,
				      struct uci_package *p,
				      const uint8_t id[32], char *name_out,
				      size_t name_sz)
{
	struct uci_element *e;
	char idhex[OM_ID_HEX_LEN];

	om_id_to_hex(id, idhex);
	uci_foreach_element(&p->sections, e) {
		struct uci_section *s = uci_to_section(e);
		struct uci_option *o;
		const char *v;

		if (strcmp(s->type, "node") != 0)
			continue;
		o = uci_lookup_option(c, s, "id");
		if (!o || o->type != UCI_TYPE_STRING)
			continue;
		v = o->v.string;
		if (!strcmp(v, idhex)) {
			if (name_out)
				snprintf(name_out, name_sz, "%s", s->e.name);
			return s;
		}
	}
	return NULL;
}

int om_store_rename(const uint8_t id[32], const char *name)
{
	struct uci_context *c = ctx_open();
	struct uci_package *p = NULL;
	struct uci_section *s;
	struct uci_ptr ptr;
	char secname[128];
	int rc = -1;

	if (!c)
		return -1;
	if (uci_load(c, OM_UCI_PKG, &p) != UCI_OK)
		goto out;
	s = find_by_id(c, p, id, secname, sizeof(secname));
	if (!s)
		goto out;

	memset(&ptr, 0, sizeof(ptr));
	ptr.package = OM_UCI_PKG;
	ptr.p = p;
	ptr.section = secname;
	ptr.option = "name";
	ptr.value = name && *name ? name : "node";
	if (uci_set(c, &ptr) != UCI_OK)
		goto out;
	commit_pkg(c, p);
	rc = 0;
out:
	ctx_close(c);
	return rc;
}

int om_store_remove(const uint8_t id[32])
{
	struct uci_context *c = ctx_open();
	struct uci_package *p = NULL;
	struct uci_section *s;
	struct uci_ptr ptr;
	char secname[128];
	int rc = -1;

	if (!c)
		return -1;
	if (uci_load(c, OM_UCI_PKG, &p) != UCI_OK)
		goto out;
	s = find_by_id(c, p, id, secname, sizeof(secname));
	if (!s)
		goto out;

	memset(&ptr, 0, sizeof(ptr));
	ptr.package = OM_UCI_PKG;
	ptr.p = p;
	ptr.section = secname;
	if (uci_delete(c, &ptr) != UCI_OK)
		goto out;
	commit_pkg(c, p);
	rc = 0;
out:
	ctx_close(c);
	return rc;
}

int om_store_foreach(int (*cb)(const struct om_stored_node *n, void *priv),
		     void *priv)
{
	struct uci_context *c = ctx_open();
	struct uci_package *p = NULL;
	struct uci_element *e;
	int count = 0;

	if (!c)
		return -1;
	if (uci_load(c, OM_UCI_PKG, &p) != UCI_OK) {
		ctx_close(c);
		return 0;	/* no config yet: nothing stored */
	}

	uci_foreach_element(&p->sections, e) {
		struct uci_section *s = uci_to_section(e);
		struct om_stored_node n;
		struct uci_option *o;
		const char *idhex, *pkb64, *name;
		size_t b64len;

		if (strcmp(s->type, "node") != 0)
			continue;
		o = uci_lookup_option(c, s, "id");
		if (!o || o->type != UCI_TYPE_STRING)
			continue;
		idhex = o->v.string;
		o = uci_lookup_option(c, s, "pubkey");
		if (!o || o->type != UCI_TYPE_STRING)
			continue;
		pkb64 = o->v.string;
		o = uci_lookup_option(c, s, "name");
		name = (o && o->type == UCI_TYPE_STRING) ? o->v.string : "";

		if (om_id_from_hex(idhex, n.id) < 0) {
			ULOG_WARN("store: bad id in %s.%s, skipping\n",
				  OM_UCI_PKG, s->e.name);
			continue;
		}
		b64len = strlen(pkb64);
		{
			size_t binlen = 0;

			if (sodium_base642bin(n.pk, 32, pkb64, b64len, NULL,
					      &binlen, NULL,
					      sodium_base64_VARIANT_ORIGINAL) != 0 ||
			    binlen != 32) {
				ULOG_WARN("store: bad pubkey in %s.%s, "
					  "skipping\n", OM_UCI_PKG,
					  s->e.name);
				continue;
			}
		}
		snprintf(n.name, sizeof(n.name), "%s", name);
		count++;
		if (cb && cb(&n, priv) != 0)
			break;
	}

	ctx_close(c);
	return count;
}

int om_store_backhaul_set(const char *mesh_id, const char *mesh_key,
			  const char *band, const char *lan_ports)
{
	struct uci_context *c = uci_alloc_context();
	struct uci_package *p = NULL;
	struct uci_section *s = NULL;
	struct uci_element *e;
	struct uci_ptr ptr;
	int rc = -1;

	if (!c)
		return -1;
	if (uci_load(c, OM_UCI_PKG, &p) != UCI_OK)
		goto out;
	uci_foreach_element(&p->sections, e) {
		struct uci_section *t = uci_to_section(e);

		if (!strcmp(t->type, "backhaul")) {
			s = t;
			break;
		}
	}
	if (!s) {
		uci_add_section(c, p, "backhaul", &s);
		if (!s)
			goto out;
	}
	memset(&ptr, 0, sizeof(ptr));
	ptr.package = OM_UCI_PKG;
	ptr.p = p;
	ptr.s = s;
	ptr.section = s->e.name;

	ptr.option = "mesh_id";	ptr.value = mesh_id;	uci_set(c, &ptr);
	ptr.option = "mesh_key";	ptr.value = mesh_key;	uci_set(c, &ptr);
	ptr.option = "band";		ptr.value = band;	uci_set(c, &ptr);
	ptr.option = "lan_ports";
	ptr.value = (lan_ports && om_lan_ports_valid(lan_ports)) ?
		    lan_ports : "";
	uci_set(c, &ptr);

	uci_save(c, p);
	if (uci_commit(c, &p, false) == UCI_OK)
		rc = 0;
out:
	uci_free_context(c);
	return rc;
}

int om_store_backhaul_get(char *mesh_id, size_t id_sz, char *mesh_key,
			  size_t key_sz, char *band, size_t band_sz,
			  char *lan_ports, size_t ports_sz)
{
	struct uci_context *c = uci_alloc_context();
	struct uci_package *p = NULL;
	struct uci_element *e;
	int found = -1;

	mesh_id[0] = mesh_key[0] = band[0] = '\0';
	if (lan_ports && ports_sz)
		lan_ports[0] = '\0';
	if (!c)
		return -1;
	if (uci_load(c, OM_UCI_PKG, &p) == UCI_OK) {
		uci_foreach_element(&p->sections, e) {
			struct uci_section *t = uci_to_section(e);
			struct uci_option *o;

			if (strcmp(t->type, "backhaul"))
				continue;
			o = uci_lookup_option(c, t, "mesh_id");
			if (o && o->type == UCI_TYPE_STRING)
				snprintf(mesh_id, id_sz, "%s", o->v.string);
			o = uci_lookup_option(c, t, "mesh_key");
			if (o && o->type == UCI_TYPE_STRING)
				snprintf(mesh_key, key_sz, "%s", o->v.string);
			o = uci_lookup_option(c, t, "band");
			if (o && o->type == UCI_TYPE_STRING)
				snprintf(band, band_sz, "%s", o->v.string);
			o = uci_lookup_option(c, t, "lan_ports");
			if (o && o->type == UCI_TYPE_STRING)
				snprintf(lan_ports, ports_sz, "%s",
					 o->v.string);
			found = 0;
			break;
		}
	}
	uci_free_context(c);
	return found;
}

int om_store_lan_ports(char out[][OM_PORT_LEN], int max)
{
	struct uci_context *c = uci_alloc_context();
	struct uci_package *p = NULL;
	struct uci_element *e;
	int n = 0;

	if (!c)
		return 0;
	if (uci_load(c, "network", &p) == UCI_OK) {
		uci_foreach_element(&p->sections, e) {
			struct uci_section *t = uci_to_section(e);
			struct uci_option *nm, *o;

			nm = uci_lookup_option(c, t, "name");
			if (!nm || nm->type != UCI_TYPE_STRING ||
			    strcmp(nm->v.string, "br-lan"))
				continue;
			o = uci_lookup_option(c, t, "ports");
			if (o && o->type == UCI_TYPE_LIST) {
				struct uci_element *pe;

				uci_foreach_element(&o->v.list, pe) {
					if (!strncmp(pe->name, "om_", 3))
						continue;
					if (n < max)
						snprintf(out[n], OM_PORT_LEN,
							 "%s", pe->name);
					n++;
				}
			} else if (o && o->type == UCI_TYPE_STRING &&
				   strcmp(o->v.string, "om_bat")) {
				if (n < max)
					snprintf(out[n], OM_PORT_LEN, "%s",
						 o->v.string);
				n++;
			}
			break;		/* only one br-lan device section */
		}
	}
	uci_free_context(c);
	return n;
}

void om_store_backhaul_mode(char *mode, size_t mode_sz)
{
	struct uci_context *c = uci_alloc_context();
	struct uci_package *p = NULL;
	struct uci_element *e;

	snprintf(mode, mode_sz, "wifi");
	if (!c)
		return;
	if (uci_load(c, "network", &p) == UCI_OK) {
		uci_foreach_element(&p->sections, e) {
			struct uci_section *t = uci_to_section(e);
			struct uci_option *o = uci_lookup_option(c, t, "proto");

			if (o && o->type == UCI_TYPE_STRING &&
			    !strcmp(o->v.string, "batadv")) {
				snprintf(mode, mode_sz, "hybrid");
				break;
			}
		}
	}
	uci_free_context(c);
}

bool om_store_wired_auto(void)
{
	struct uci_context *c = uci_alloc_context();
	struct uci_package *p = NULL;
	struct uci_section *s = NULL;
	struct uci_element *e;
	struct uci_option *o;
	bool on = false;

	if (!c)
		return false;
	if (uci_load(c, OM_UCI_PKG, &p) == UCI_OK) {
		uci_foreach_element(&p->sections, e) {
			if (!strcmp(uci_to_section(e)->type, "openmesh")) {
				s = uci_to_section(e);
				break;
			}
		}
		if (s) {
			o = uci_lookup_option(c, s, "wired_auto");
			if (o && o->type == UCI_TYPE_STRING &&
			    !strcmp(o->v.string, "1"))
				on = true;
		}
	}
	uci_free_context(c);
	return on;
}

int om_store_protected_ports(char out[][OM_PORT_LEN], int max)
{
	struct uci_context *c = uci_alloc_context();
	struct uci_package *p = NULL;
	struct uci_section *s = NULL;
	struct uci_element *e, *pe;
	struct uci_option *o;
	int n = 0;

	if (!c)
		return 0;
	if (uci_load(c, OM_UCI_PKG, &p) == UCI_OK) {
		uci_foreach_element(&p->sections, e) {
			if (!strcmp(uci_to_section(e)->type, "openmesh")) {
				s = uci_to_section(e);
				break;
			}
		}
		if (s) {
			o = uci_lookup_option(c, s, "protected_ports");
			if (o && o->type == UCI_TYPE_LIST) {
				uci_foreach_element(&o->v.list, pe) {
					size_t l = strlen(pe->name);
					bool ok = l > 0 && l < OM_PORT_LEN;
					/* display-only list (string compare,
					 * never reaches a shell): interface
					 * names incl. vlan dots are fine */
					for (size_t k = 0; ok && k < l; k++) {
						char ch = pe->name[k];

						if (!((ch >= 'a' && ch <= 'z') ||
						      (ch >= 'A' && ch <= 'Z') ||
						      (ch >= '0' && ch <= '9') ||
						      ch == '_' || ch == '.' ||
						      ch == '-'))
							ok = false;
					}
					if (!ok)
						continue;
					if (n < max)
						snprintf(out[n], OM_PORT_LEN,
							 "%s", pe->name);
					n++;
				}
			}
		}
	}
	uci_free_context(c);
	return n;
}

void om_store_sync_backhaul_ports(void)
{
	struct uci_context *c = uci_alloc_context();
	struct uci_package *pn = NULL, *po = NULL;
	struct uci_section *s = NULL;
	struct uci_element *e;
	struct uci_ptr ptr;
	char spec[OM_PORTS_SPEC_MAX + 1] = "";
	size_t len = 0;

	if (!c)
		return;
	/* collect surviving om_eth hardif devices */
	if (uci_load(c, "network", &pn) == UCI_OK) {
		uci_foreach_element(&pn->sections, e) {
			struct uci_section *t = uci_to_section(e);
			struct uci_option *o;
			size_t dl;

			if (!t->e.name ||
			    strncmp(t->e.name, "om_eth_", 7))
				continue;
			o = uci_lookup_option(c, t, "device");
			if (!o || o->type != UCI_TYPE_STRING || !o->v.string)
				continue;
			dl = strlen(o->v.string);
			if (!om_port_name_valid(o->v.string, dl))
				continue;
			if (len && len + 1 + dl < sizeof(spec))
				spec[len++] = ',';
			if (len + dl < sizeof(spec)) {
				memcpy(spec + len, o->v.string, dl);
				len += dl;
			}
		}
	}
	/* update only when a backhaul section exists */
	if (uci_load(c, OM_UCI_PKG, &po) == UCI_OK) {
		uci_foreach_element(&po->sections, e) {
			if (!strcmp(uci_to_section(e)->type, "backhaul")) {
				s = uci_to_section(e);
				break;
			}
		}
	}
	if (s) {
		memset(&ptr, 0, sizeof(ptr));
		ptr.package = OM_UCI_PKG;
		ptr.p = po;
		ptr.s = s;
		ptr.section = s->e.name;
		ptr.option = "lan_ports";
		ptr.value = spec;
		uci_set(c, &ptr);
		uci_save(c, po);
		uci_commit(c, &po, false);
	}
	uci_free_context(c);
}
