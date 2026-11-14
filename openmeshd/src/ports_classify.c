/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure port classifier for the wired-backhaul port picker (m21,
 * Phase 7.0/7.1). Zero runtime dependencies: host-testable via
 * tests/test_ports_classify.c. NEVER decides anything writeable --
 * eligibility only feeds the read-only preview UI; the future write
 * path (7.2) must revalidate from scratch.
 */
#include <stdio.h>
#include <string.h>

#include "ports_classify.h"

static void set_v(struct om_port_verdict *v, bool eligible,
		  const char *reason, const char *role, int mgmt)
{
	v->eligible = eligible;
	snprintf(v->reason, sizeof(v->reason), "%s", reason);
	snprintf(v->role, sizeof(v->role), "%s", role);
	v->mgmt = mgmt;
}

const char *om_port_mgmt_str(int mgmt)
{
	switch (mgmt) {
	case OM_MGMT_YES: return "yes";
	case OM_MGMT_NO: return "no";
	default: return "unknown";
	}
}

int om_port_apply_gate(const struct om_port_verdict *v, bool ack_unknown)
{
	if (!v || !v->eligible) {
		if (v && !strcmp(v->reason, "mgmt_unknown"))
			return ack_unknown ?
				OM_APPLY_ALLOW : OM_APPLY_NEED_ACK;
		return OM_APPLY_DENY;
	}
	return OM_APPLY_ALLOW;
}

bool om_autojoin_should(const struct om_port_verdict *v, bool auto_enabled,
			bool txn_active, bool has_mesh, bool hint_stable)
{
	return auto_enabled && hint_stable && has_mesh && !txn_active &&
	       v && v->eligible;
}

void om_port_classify(const struct om_port_facts *f,
		      struct om_port_verdict *v)
{
	memset(v, 0, sizeof(*v));

	if (!f || !f->exists) {
		set_v(v, false, "not_exist", "unknown", OM_MGMT_UNKNOWN);
		return;
	}

	/* management-path evidence, independent of eligibility ordering */
	if (f->carrier && f->in_lan_bridge && f->lan_carrier_count == 1)
		v->mgmt = OM_MGMT_YES;
	else if (f->carrier && f->in_lan_bridge && f->lan_carrier_count > 1)
		v->mgmt = OM_MGMT_UNKNOWN;
	else
		v->mgmt = OM_MGMT_NO;

	/* operator-forbidden ports are absolute and shown first: nothing
	 * outranks the user's own decree */
	if (f->user_protected) {
		set_v(v, false, "protected", f->in_lan_bridge ? "lan" :
							"unused", v->mgmt);
		return;
	}

	/* virtual family: bridges, wifi ifaces, batman softifs, enslaved
	 * devices -- shown but never selectable */
	if (f->is_bridge) {
		set_v(v, false, "virtual", "virtual", v->mgmt);
		return;
	}
	if (f->is_batman_soft) {
		set_v(v, false, "virtual", "mesh", v->mgmt);
		return;
	}
	if (f->wireless) {
		set_v(v, false, "wireless", "wireless", v->mgmt);
		return;
	}
	if (f->has_master) {
		set_v(v, false, "master", "mesh", v->mgmt);
		return;
	}

	/* on a DSA system a physical ethX without phys_port_name is the
	 * switch CPU port: moving it kills the whole switch */
	if (f->dsa_present && !f->dsa_user && !f->is_bridge) {
		set_v(v, false, "dsa_cpu", "dsa-cpu", v->mgmt);
		return;
	}

	if (f->om_mesh_port) {
		set_v(v, false, "mesh", "mesh", v->mgmt);
		return;
	}
	/* WAN beats the virtual look: "eth0.2" as wan shows the real risk */
	if (f->is_wan) {
		set_v(v, false, "wan", "wan", v->mgmt);
		return;
	}
	if (f->pppoe) {
		set_v(v, false, "pppoe", "wan", v->mgmt);
		return;
	}
	/* swconfig trunk (eth0 with vlan children) carries every vlan:
	 * moving it strands LAN and WAN at once */
	if (f->is_vlan_parent) {
		set_v(v, false, "vlan_parent", "virtual", v->mgmt);
		return;
	}
	if (f->is_vlan_subif) {
		set_v(v, false, "virtual", "virtual", v->mgmt);
		return;
	}
	if (f->vlan_dep) {
		set_v(v, false, "vlan", "lan", v->mgmt);
		return;
	}
	if (v->mgmt == OM_MGMT_YES) {
		set_v(v, false, "mgmt", "lan", v->mgmt);
		return;
	}
	if (v->mgmt == OM_MGMT_UNKNOWN) {
		set_v(v, false, "mgmt_unknown", "lan", v->mgmt);
		return;
	}

	set_v(v, true, "", f->in_lan_bridge ? "lan" : "unused", v->mgmt);
}
