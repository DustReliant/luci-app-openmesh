/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENMESHD_PORTS_CLASSIFY_H
#define OPENMESHD_PORTS_CLASSIFY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Pure per-port facts, filled by the discovery layer (ports.c) from
 * sysfs + UCI. Zero dependencies so the classifier is host-testable.
 */
struct om_port_facts {
	char device[16];
	bool exists;		/* netdev present in /sys/class/net */
	bool wireless;		/* phy80211 dir */
	bool is_bridge;		/* bridge dir (br-lan, ...) */
	bool is_batman_soft;	/* mesh dir (batX) */
	bool is_vlan_subif;	/* "eth0.1" style name */
	bool has_master;	/* enslaved: batman hardif / bond / team */
	bool dsa_user;		/* phys_port_name present (DSA user port) */
	bool dsa_present;	/* any DSA user port exists on the system */
	bool carrier;
	int speed;		/* Mbps; 0 = unknown */
	bool in_lan_bridge;	/* UCI br-lan ports member */
	bool is_wan;		/* referenced by a wan interface (UCI) */
	bool pppoe;		/* pppoe proto rides on it */
	bool vlan_dep;		/* vlan subif or vlan section references it */
	bool om_mesh_port;	/* om_eth_* section or enslaved to om_bat */
	bool user_protected;	/* operator-forbidden (UCI protected_ports) */
	int lan_carrier_count;	/* physical br-lan members with carrier now */
};

/* tri-state management-path verdict */
#define OM_MGMT_UNKNOWN 0
#define OM_MGMT_YES     1
#define OM_MGMT_NO      2

struct om_port_verdict {
	bool eligible;
	char reason[24];	/* token, "" when eligible:
				 * not_exist / virtual / wireless / dsa_cpu /
				 * wan / vlan / mesh / mgmt / mgmt_unknown /
				 * pppoe / master */
	char role[12];		/* lan / wan / dsa-cpu / mesh / virtual /
				 * wireless / unused / unknown */
	int mgmt;		/* OM_MGMT_* */
};

/* First-match-wins eligibility. Management path is evidence-based:
 * a lone carrier port inside br-lan is the likely management path
 * (YES, blocked); several carrier ports cannot be told apart
 * (UNKNOWN, blocked); a port without carrier is never the path (NO). */
void om_port_classify(const struct om_port_facts *f,
		      struct om_port_verdict *v);

/* token -> human text is the frontend's job (i18n); keep tokens stable */
const char *om_port_mgmt_str(int mgmt);

/* ---- apply gate (Phase 7.2) ---- */
#define OM_APPLY_ALLOW    0	/* eligible: proceed */
#define OM_APPLY_NEED_ACK 1	/* mgmt_unknown: user must acknowledge */
#define OM_APPLY_DENY     2	/* never, regardless of acknowledgement */

/*
 * Write-path gate for port_apply. Eligible ports pass; the ONLY
 * overridable block is mgmt_unknown (multiple live br-lan members --
 * the user may positively confirm after seeing them). Every other
 * block (mgmt/wan/vlan/dsa_cpu/mesh/virtual/master/not_exist) is
 * absolute: acknowledgement changes nothing.
 */
int om_port_apply_gate(const struct om_port_verdict *v, bool ack_unknown);

#endif
