/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Read-only physical-port discovery and wired-backhaul planning
 * (m21, Phase 7.0/7.1). Sources: /sys/class/net + UCI network config.
 * This file performs NO writes, NO system(), NO reloads -- the whole
 * phase is preview-only; the 7.2 write path must revalidate here.
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <uci.h>

#include "ports_classify.h"
#include "ports.h"

#define NET_SYSFS "/sys/class/net"
#define LAN_BRIDGE "br-lan"

/* ---------------- small sysfs helpers (read-only) ---------------- */

static int read_file_int(const char *path)
{
	FILE *f = fopen(path, "r");
	int v = 0;

	if (!f)
		return 0;
	if (fscanf(f, "%d", &v) != 1)
		v = 0;
	fclose(f);
	return v;
}

static bool path_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

static bool dev_has_dir(const char *dev, const char *sub)
{
	char path[64];

	snprintf(path, sizeof(path), NET_SYSFS "/%s/%s", dev, sub);
	return path_exists(path);
}

/* ---------------- UCI facts (network package) ---------------- */

static void list_add(char arr[][16], int *n, int max, const char *v)
{
	int i;

	if (!v || !*v)
		return;
	for (i = 0; i < *n; i++)
		if (!strcmp(arr[i], v))
			return;
	if (*n < max) {
		snprintf(arr[*n], 16, "%s", v);
		(*n)++;
	}
}

static bool list_has(const char arr[][16], int n, const char *v)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp(arr[i], v))
			return true;
	return false;
}

static void collect_dev_names(struct uci_context *c, struct uci_section *s,
			      char arr[][16], int *n, int max)
{
	struct uci_option *o = uci_lookup_option(c, s, "ports");

	if (!o)
		return;
	if (o->type == UCI_TYPE_LIST) {
		struct uci_element *e;

		uci_foreach_element(&o->v.list, e) {
			char dev[16];
			char *colon;

			/* bridge-vlan members look like "lan1:u*" */
			snprintf(dev, sizeof(dev), "%s", e->name);
			colon = strchr(dev, ':');
			if (colon)
				*colon = '\0';
			list_add(arr, n, max, dev);
		}
	} else if (o->type == UCI_TYPE_STRING && o->v.string) {
		list_add(arr, n, max, o->v.string);
	}
}

static void read_net_facts(struct om_net_facts *nf)
{
	struct uci_context *c = uci_alloc_context();
	struct uci_package *p = NULL;
	struct uci_element *e;

	memset(nf, 0, sizeof(*nf));
	if (!c)
		return;
	if (uci_load(c, "network", &p) != UCI_OK) {
		uci_free_context(c);
		return;
	}

	uci_foreach_element(&p->sections, e) {
		struct uci_section *s = uci_to_section(e);
		struct uci_option *o;
		const char *type = s->type;
		const char *devname = NULL;

		if (!strcmp(type, "device")) {
			/* br-lan device section: bridge membership */
			o = uci_lookup_option(c, s, "name");
			if (o && o->type == UCI_TYPE_STRING &&
			    o->v.string && !strcmp(o->v.string, LAN_BRIDGE))
				collect_dev_names(c, s, nf->lan_ports,
						  &nf->n_lan_ports,
						  OM_PORTS_MAX);
			continue;
		}
		/* bridge-vlan / 8021q sections create vlan deps */
		if (strstr(type, "vlan"))
			collect_dev_names(c, s, nf->vlan_devs, &nf->n_vlan,
					  8);

		if (!strcmp(type, "interface") && s->e.name) {
			/* wan* interfaces: role + pppoe */
			if (!strncmp(s->e.name, "wan", 3)) {
				o = uci_lookup_option(c, s, "device");
				if (o && o->type == UCI_TYPE_STRING &&
				    o->v.string)
					devname = o->v.string;
				if (devname)
					list_add(nf->wan_devs,
						 &nf->n_wan_devs, 4, devname);
				o = uci_lookup_option(c, s, "proto");
				if (o && o->type == UCI_TYPE_STRING &&
				    !strcmp(o->v.string, "pppoe") && devname)
					list_add(nf->pppoe_devs,
						 &nf->n_pppoe, 4, devname);
			}
			/* m19 wired-leg hardifs: already meshed */
			if (!strncmp(s->e.name, "om_eth_", 7)) {
				o = uci_lookup_option(c, s, "device");
				if (o && o->type == UCI_TYPE_STRING &&
				    o->v.string)
					list_add(nf->mesh_devs,
						 &nf->n_mesh, 8, o->v.string);
			}
		}
	}
	uci_free_context(c);
}

/* ---------------- discovery ---------------- */

static bool name_ok(const char *n)
{
	for (; *n; n++)
		if (!((*n >= 'a' && *n <= 'z') || (*n >= 'A' && *n <= 'Z') ||
		      (*n >= '0' && *n <= '9') || *n == '_' || *n == '-' ||
		      *n == '.'))
			return false;
	return true;
}

static bool is_physical_eth(const struct om_port_facts *f)
{
	return f->exists && !f->wireless && !f->is_bridge &&
	       !f->is_batman_soft && !f->is_vlan_subif && !f->has_master;
}

static void fill_facts(struct om_port_facts *f, const char *dev,
		       const struct om_net_facts *nf, bool dsa_present)
{
	char path[64];

	memset(f, 0, sizeof(*f));
	snprintf(f->device, sizeof(f->device), "%s", dev);
	f->exists = true;

	f->wireless = dev_has_dir(dev, "phy80211");
	f->is_bridge = dev_has_dir(dev, "bridge");
	f->is_batman_soft = dev_has_dir(dev, "mesh");
	snprintf(path, sizeof(path), NET_SYSFS "/%s/master", dev);
	f->has_master = path_exists(path);
	snprintf(path, sizeof(path), NET_SYSFS "/%s/phys_port_name", dev);
	f->dsa_user = path_exists(path);
	f->dsa_present = dsa_present;
	f->is_vlan_subif = strchr(dev, '.') != NULL;

	snprintf(path, sizeof(path), NET_SYSFS "/%s/carrier", dev);
	f->carrier = read_file_int(path) == 1;
	snprintf(path, sizeof(path), NET_SYSFS "/%s/speed", dev);
	f->speed = read_file_int(path);
	if (f->speed < 0)
		f->speed = 0;

	f->in_lan_bridge = list_has(nf->lan_ports, nf->n_lan_ports, dev);
	f->is_wan = list_has(nf->wan_devs, nf->n_wan_devs, dev);
	f->pppoe = list_has(nf->pppoe_devs, nf->n_pppoe, dev);
	f->vlan_dep = list_has(nf->vlan_devs, nf->n_vlan, dev);
	f->om_mesh_port = list_has(nf->mesh_devs, nf->n_mesh, dev);
}

static bool is_our_virtual(const char *dev)
{
	static const char *pfx[] = { "om_", "bat", "br-", "tun", "tap", "gre",
				     "pppoe", "wg", "ifb", "veth", "mon.",
				     "macvtap", "phy" };
	size_t i;

	for (i = 0; i < sizeof(pfx) / sizeof(pfx[0]); i++)
		if (!strncmp(dev, pfx[i], strlen(pfx[i])))
			return true;
	return false;
}

int om_ports_discover(struct om_port_entry out[], int max)
{
	struct om_net_facts nf;
	struct dirent *de;
	DIR *d;
	int n = 0, i;
	bool dsa_present = false;
	char names[OM_PORTS_MAX][16];
	int n_names = 0;

	read_net_facts(&nf);

	d = opendir(NET_SYSFS);
	if (!d)
		return -1;
	while ((de = readdir(d)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!strcmp(de->d_name, "lo"))
			continue;	/* loopback always excluded */
		if (!name_ok(de->d_name))
			continue;	/* charset guard (test #13) */
		if (strlen(de->d_name) > 15)
			continue;
		snprintf(names[n_names % OM_PORTS_MAX], 16, "%s", de->d_name);
		n_names++;
	}
	closedir(d);

	/* pass 1: does this system have DSA user ports at all? */
	for (i = 0; i < n_names && i < OM_PORTS_MAX; i++) {
		char path[64];

		snprintf(path, sizeof(path), NET_SYSFS "/%s/phys_port_name",
			 names[i]);
		if (path_exists(path)) {
			dsa_present = true;
			break;
		}
	}

	/* pass 2: build entries */
	n = 0;
	for (i = 0; i < n_names && n < max; i++) {
		fill_facts(&out[n].f, names[i], &nf, dsa_present);
		n++;
	}

	/* management counting needs carrier facts of physical lan members */
	{
		int lan_carrier = 0, j;

		for (j = 0; j < n; j++)
			if (is_physical_eth(&out[j].f) &&
			    !is_our_virtual(out[j].f.device) &&
			    out[j].f.in_lan_bridge && out[j].f.carrier)
				lan_carrier++;
		for (j = 0; j < n; j++)
			out[j].f.lan_carrier_count = lan_carrier;
	}

	for (i = 0; i < n; i++)
		om_port_classify(&out[i].f, &out[i].v);

	return n;
}

/* ---------------- plan (read-only preview + checks) ---------------- */

static void chk(struct om_plan_check *c, const char *name,
		const char *state, const char *detail)
{
	snprintf(c->name, sizeof(c->name), "%s", name);
	snprintf(c->state, sizeof(c->state), "%s", state);
	snprintf(c->detail, sizeof(c->detail), "%s", detail);
}

int om_port_plan(const char *device, const char *mode,
		 struct om_port_plan *plan)
{
	struct om_port_entry ports[OM_PORTS_MAX];
	struct om_port_entry *pe = NULL;
	int n, i, k = 0;

	memset(plan, 0, sizeof(*plan));
	if (!device || !*device)
		return -1;
	snprintf(plan->mode, sizeof(plan->mode), "%s",
		 mode && *mode ? mode : "wireless_only");

	n = om_ports_discover(ports, OM_PORTS_MAX);
	if (n < 0)
		return -1;
	/* backend revalidation: never trust the frontend verdict (test #18) */
	for (i = 0; i < n; i++)
		if (!strcmp(ports[i].f.device, device)) {
			pe = &ports[i];
			break;
		}
	if (!pe)
		return -1;		/* nonexistent port */

	plan->entry = *pe;

	/* ---- the 10 readonly prechecks (§五) ---- */
	chk(&plan->checks[k++], "exists", "pass", "网口存在");

	if (pe->f.wireless)
		chk(&plan->checks[k++], "physical", "blocked",
		    "无线接口不参与有线回程");
	else if (pe->f.is_bridge || pe->f.is_vlan_subif || pe->f.has_master ||
		 pe->f.is_batman_soft)
		chk(&plan->checks[k++], "physical", "blocked",
		    "虚拟/子接口/已 enslaved");
	else if (!strcmp(pe->v.reason, "dsa_cpu"))
		chk(&plan->checks[k++], "physical", "blocked",
		    "DSA 交换机 CPU 口，移出将断整个交换机");
	else
		chk(&plan->checks[k++], "physical", "pass", "物理以太网口");

	if (pe->v.mgmt == OM_MGMT_YES)
		chk(&plan->checks[k++], "mgmt", "blocked",
		    "当前管理路径（唯一 carrier 的 br-lan 成员）");
	else if (pe->v.mgmt == OM_MGMT_UNKNOWN)
		chk(&plan->checks[k++], "mgmt", "unknown",
		    "多个 br-lan 成员有 carrier，无法排除管理路径");
	else
		chk(&plan->checks[k++], "mgmt", "pass",
		    "不在管理路径（无 carrier 或非 br-lan 成员）");

	if (pe->f.is_wan)
		chk(&plan->checks[k++], "wan", "blocked", "WAN 上联占用");
	else if (pe->f.pppoe)
		chk(&plan->checks[k++], "wan", "blocked", "PPPoE 依赖");
	else
		chk(&plan->checks[k++], "wan", "pass", "非 WAN");

	if (pe->f.vlan_dep)
		chk(&plan->checks[k++], "vlan", "blocked", "存在 VLAN 依赖");
	else
		chk(&plan->checks[k++], "vlan", "pass", "无 VLAN 依赖");

	if (pe->f.om_mesh_port)
		chk(&plan->checks[k++], "mesh", "blocked", "已用于 Mesh 回程");
	else
		chk(&plan->checks[k++], "mesh", "pass", "未用于 Mesh");

	if (pe->f.in_lan_bridge)
		chk(&plan->checks[k++], "loop", "pass",
		    "方案会把端口移出 br-lan，避免与 bat0 桥接成环");
	else
		chk(&plan->checks[k++], "loop", "pass",
		    "端口不在 br-lan，无成环路径");

	if (path_exists("/sys/module/batman_adv"))
		chk(&plan->checks[k++], "batman", "pass",
		    "batman-adv 内核模块已加载");
	else
		chk(&plan->checks[k++], "batman", "unknown",
		    "batman-adv 模块未加载（需 kmod-batman-adv）");

	chk(&plan->checks[k++], "peer", "unknown",
	    "对端同步配置无法在本机判定（7.2 将经会话通道推送）");

	chk(&plan->checks[k++], "recovery", "pass",
	    "30 秒事务自动回滚 + network 快照复原（netconf 既有机制）");

	plan->n_checks = k;
	plan->ok = pe->v.eligible;

	/* ---- preview (§六): descriptive strings, never shell ---- */
	if (pe->v.eligible) {
		int u = 0;

		if (pe->f.in_lan_bridge)
			snprintf(plan->uci_changes[u++], 96,
				 "network 设备段 %s 的 ports 列表移除 '%s'",
				 LAN_BRIDGE, device);
		snprintf(plan->uci_changes[u++], 96,
			 "新增 network.om_eth_%s：proto batadv_hardif，"
			 "master om_bat", device);
		snprintf(plan->uci_changes[u++], 96,
			 "%s 的 ports 保持包含 om_bat（hybrid 既有）",
			 LAN_BRIDGE);
		snprintf(plan->uci_changes[u++], 96,
			 "openmesh.backhaul 段 lan_ports='%s'", device);
		plan->n_uci = u;

		snprintf(plan->ops[0], 96, "ubus call network reload（netifd 重载）");
		snprintf(plan->ops[1], 96, "等待对端链路 carrier 建立");
		snprintf(plan->ops[2], 96, "30 秒内 apply_confirm，否则自动回滚");
		plan->n_ops = 3;

		snprintf(plan->interruptions[0], 96,
			 "该端口下现有 LAN 设备将断开（移出 br-lan）");
		plan->n_interruptions = 1;
	} else {
		snprintf(plan->uci_changes[0], 96,
			 "无（当前判定 %s，不生成变更）", pe->v.reason[0] ?
			 pe->v.reason : "不可用");
		plan->n_uci = 1;
		plan->n_ops = 0;
		plan->n_interruptions = 0;
	}

	if (pe->v.mgmt == OM_MGMT_YES)
		snprintf(plan->mgmt_risk, sizeof(plan->mgmt_risk),
			 "高：该口疑似当前管理路径，执行会失联");
	else if (pe->v.mgmt == OM_MGMT_UNKNOWN)
		snprintf(plan->mgmt_risk, sizeof(plan->mgmt_risk),
			 "中：多个活动口无法排除管理路径，需人工确认");
	else
		snprintf(plan->mgmt_risk, sizeof(plan->mgmt_risk),
			 "低：该口不在管理路径");
	snprintf(plan->rollback, sizeof(plan->rollback),
		 "30 秒确认窗口 + network/wireless 快照自动复原；breed/串口为最终兜底");

	return 0;
}
