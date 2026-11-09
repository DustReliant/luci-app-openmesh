/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESHD_PORTS_H
#define OPENMESHD_PORTS_H

#include <stdbool.h>

#include "ports_classify.h"

#define OM_PORTS_MAX    16
#define OM_PLAN_CHANGES 6
#define OM_PLAN_OPS     4
#define OM_PLAN_CHECKS  10

/* UCI network facts shared by discovery (filled read-only) */
struct om_net_facts {
	char lan_ports[OM_PORTS_MAX][16];
	int n_lan_ports;
	char wan_devs[4][16];
	int n_wan_devs;
	char pppoe_devs[4][16];
	int n_pppoe;
	char vlan_devs[8][16];
	int n_vlan;
	char mesh_devs[8][16];
	int n_mesh;
};

struct om_port_entry {
	struct om_port_facts f;
	struct om_port_verdict v;
};

struct om_plan_check {
	char name[12];
	char state[10];		/* pass / blocked / unknown */
	char detail[96];
};

struct om_port_plan {
	struct om_port_entry entry;
	char mode[20];		/* wireless_only / wired_only / wired_preferred */
	bool ok;		/* entry eligible AND no blocked checks */
	struct om_plan_check checks[OM_PLAN_CHECKS];
	int n_checks;
	char uci_changes[OM_PLAN_CHANGES][96];	/* descriptive, never shell */
	int n_uci;
	char ops[OM_PLAN_OPS][96];
	int n_ops;
	char interruptions[OM_PLAN_OPS][96];
	int n_interruptions;
	char mgmt_risk[96];
	char rollback[96];
};

/* enumerate netdevs (lo excluded) with facts + verdicts; -1 on error */
int om_ports_discover(struct om_port_entry out[], int max);

/* readonly precheck + preview for one port; -1 when device unknown */
int om_port_plan(const char *device, const char *mode,
		 struct om_port_plan *p);

#endif /* OPENMESHD_PORTS_H */
