/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host tests for the pure port classifier (m21, Phase 7.0/7.1).
 * Scenarios mirror task §8 (1-13, 15, 16) at the classifier level;
 * frontend-side cases (14, 17, 18) live in tests/test_ports_ui.js.
 * Build & run via tests/host-build.sh.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ports_classify.h"

static int pass, fail;

#define CHECK(cond) do { \
	if (cond) { pass++; } \
	else { fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static struct om_port_facts base_eth(const char *dev)
{
	struct om_port_facts f;

	memset(&f, 0, sizeof(f));
	snprintf(f.device, sizeof(f.device), "%s", dev);
	f.exists = true;
	return f;
}

static struct om_port_verdict v_of(const struct om_port_facts *f)
{
	struct om_port_verdict v;

	om_port_classify(f, &v);
	return v;
}

int main(void)
{
	/* 1. AX3000T DSA: lan1-3 user ports, one carrier -> management */
	{
		struct om_port_facts lan1 = base_eth("lan1");
		struct om_port_facts lan2 = base_eth("lan2");

		lan1.dsa_user = lan2.dsa_user = true;
		lan1.dsa_present = lan2.dsa_present = true;
		lan1.in_lan_bridge = lan2.in_lan_bridge = true;
		lan1.carrier = true;
		lan1.lan_carrier_count = 1;
		lan2.lan_carrier_count = 1;

		struct om_port_verdict v1 = v_of(&lan1), v2 = v_of(&lan2);

		CHECK(!v1.eligible && !strcmp(v1.reason, "mgmt"));
		CHECK(v1.mgmt == OM_MGMT_YES);
		CHECK(v2.eligible && v2.reason[0] == '\0');
		CHECK(v2.mgmt == OM_MGMT_NO);
		CHECK(!strcmp(v2.role, "lan"));
	}
	/* DSA CPU port (ethX without phys_port_name on a DSA system) */
	{
		struct om_port_facts cpu = base_eth("eth0");

		cpu.dsa_present = true;
		struct om_port_verdict v = v_of(&cpu);

		CHECK(!v.eligible && !strcmp(v.reason, "dsa_cpu"));
		CHECK(!strcmp(v.role, "dsa-cpu"));
	}

	/* 2/4. x86 independent NICs: eth0=mgmt (only carrier), eth1 free */
	{
		struct om_port_facts eth0 = base_eth("eth0"), eth1 = base_eth("eth1");

		eth0.in_lan_bridge = true;
		eth0.carrier = true;
		eth0.lan_carrier_count = 1;
		eth1.lan_carrier_count = 1;
		struct om_port_verdict v0 = v_of(&eth0), v1 = v_of(&eth1);

		CHECK(!v0.eligible && !strcmp(v0.reason, "mgmt"));
		CHECK(v1.eligible && !strcmp(v1.role, "unused"));
	}

	/* 3. single-port device: the only port is the management path */
	{
		struct om_port_facts only = base_eth("eth0");

		only.in_lan_bridge = true;
		only.carrier = true;
		only.lan_carrier_count = 1;
		CHECK(!v_of(&only).eligible);
	}

	/* 5. WAN occupied */
	{
		struct om_port_facts wan = base_eth("eth1");

		wan.is_wan = true;
		struct om_port_verdict v = v_of(&wan);

		CHECK(!v.eligible && !strcmp(v.reason, "wan"));
		CHECK(!strcmp(v.role, "wan"));
	}
	/* PPPoE on wan */
	{
		struct om_port_facts p = base_eth("eth1");

		p.pppoe = true;
		CHECK(!v_of(&p).eligible && !strcmp(v_of(&p).reason, "pppoe"));
	}

	/* 8. no-carrier port stays configurable */
	{
		struct om_port_facts f = base_eth("lan3");

		f.in_lan_bridge = true;
		f.carrier = false;
		f.lan_carrier_count = 1;	/* sibling carries mgmt */
		struct om_port_verdict v = v_of(&f);

		CHECK(v.eligible);
		CHECK(v.mgmt == OM_MGMT_NO);
	}

	/* 9. already in BATMAN */
	{
		struct om_port_facts hardif = base_eth("lan2"), softif = base_eth("bat0");

		hardif.has_master = true;
		softif.is_batman_soft = true;
		CHECK(!v_of(&hardif).eligible && !strcmp(v_of(&hardif).reason, "master"));
		CHECK(!v_of(&softif).eligible && !strcmp(v_of(&softif).role, "mesh"));
		/* om_eth_* section also marks mesh usage */
		struct om_port_facts om = base_eth("lan2");

		om.om_mesh_port = true;
		CHECK(!v_of(&om).eligible && !strcmp(v_of(&om).reason, "mesh"));
	}

	/* 10. VLAN dependency blocks */
	{
		struct om_port_facts f = base_eth("lan2");

		f.in_lan_bridge = true;
		f.vlan_dep = true;
		f.lan_carrier_count = 0;
		CHECK(!v_of(&f).eligible && !strcmp(v_of(&f).reason, "vlan"));
	}

	/* 11. virtual family filtered */
	{
		struct om_port_facts br = base_eth("br-lan");
		struct om_port_facts wifi = base_eth("phy0-ap0");
		struct om_port_facts sub = base_eth("eth0.1");

		br.is_bridge = true;
		wifi.wireless = true;
		sub.is_vlan_subif = true;
		CHECK(!v_of(&br).eligible && !strcmp(v_of(&br).reason, "virtual"));
		CHECK(!v_of(&wifi).eligible && !strcmp(v_of(&wifi).reason, "wireless"));
		CHECK(!v_of(&sub).eligible && !strcmp(v_of(&sub).reason, "virtual"));
		CHECK(!strcmp(v_of(&wifi).role, "wireless"));
	}

	/* 12/16. nonexistent device (or vanished after reboot) */
	{
		struct om_port_facts f = base_eth("lan9");

		f.exists = false;
		struct om_port_verdict v = v_of(&f);

		CHECK(!v.eligible && !strcmp(v.reason, "not_exist"));
		CHECK(!strcmp(v.role, "unknown"));
		CHECK(v.mgmt == OM_MGMT_UNKNOWN);
	}
	/* NULL facts are not a crash */
	CHECK(!v_of(NULL).eligible);

	/* 15. carrier/state change flips the management verdict */
	{
		struct om_port_facts a = base_eth("lan1"), b = base_eth("lan2");

		a.in_lan_bridge = b.in_lan_bridge = true;
		a.carrier = b.carrier = true;
		a.lan_carrier_count = b.lan_carrier_count = 2;
		/* two live members: cannot tell them apart */
		struct om_port_verdict va = v_of(&a);

		CHECK(!va.eligible && !strcmp(va.reason, "mgmt_unknown"));
		CHECK(va.mgmt == OM_MGMT_UNKNOWN);
		/* one drops carrier: the other becomes the clear mgmt path */
		b.carrier = false;
		a.lan_carrier_count = b.lan_carrier_count = 1;
		CHECK(v_of(&a).mgmt == OM_MGMT_YES);
		CHECK(v_of(&b).mgmt == OM_MGMT_NO);
		CHECK(v_of(&b).eligible);
	}

	/* mgmt helper strings */
	CHECK(!strcmp(om_port_mgmt_str(OM_MGMT_YES), "yes"));
	CHECK(!strcmp(om_port_mgmt_str(OM_MGMT_NO), "no"));
	CHECK(!strcmp(om_port_mgmt_str(OM_MGMT_UNKNOWN), "unknown"));

	/* 13-ish: odd-but-charset-valid names classify normally */
	{
		struct om_port_facts f = base_eth("lan-1_x");

		CHECK(v_of(&f).eligible);
	}

	/* bridge device itself is virtual even on DSA systems */
	{
		struct om_port_facts br = base_eth("br-lan");

		br.is_bridge = true;
		br.dsa_present = true;
		CHECK(!v_of(&br).eligible && !strcmp(v_of(&br).reason, "virtual"));
	}

	printf("test_ports_classify: %d PASS, %d FAIL\n", pass, fail);
	return fail ? 1 : 0;
}
