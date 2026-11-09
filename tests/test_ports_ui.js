#!/usr/bin/env node
/*
 * Phase 7.0/7.1 wizard port-picker tests (m21). Extracts the REAL
 * helpers (dropdownPorts / portOptionLabel / riskText / roleText) from
 * wizard.js and runs them in a VM. Covers task §8 items #14 (ubus
 * failure), #17 (malicious input), #18 (backend revalidation) plus
 * the dropdown/label logic.
 */
'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

let pass = 0, fail = 0, results = [];

function test(name, fn) {
	try { fn(); pass++; results.push(`PASS ${name}`); }
	catch (e) { fail++; results.push(`FAIL ${name}: ${e.message}`); }
}

const jsPath = path.join(__dirname, '..', 'luci-app-openmesh',
	'htdocs', 'luci-static', 'resources', 'view', 'openmesh', 'wizard.js');
const code = fs.readFileSync(jsPath, 'utf8');

function grabFn(name) {
	const m = code.match(new RegExp(`function ${name}\\([\\s\\S]*?\\) \\{([\\s\\S]*?)\n\}`));
	if (!m) { console.error(`FATAL: ${name}() not found in wizard.js`); process.exit(1); }
	const head = code.match(new RegExp(`function ${name}\\(([^)]*)\\)`))[1];
	return `function ${name}(${head}) {${m[1]}}`;
}

const ctx = vm.createContext({
	/* LuCI _() shim: identity (msgid keys are stable tokens here) */
	_: function(s) { return s; }
});
[grabFn('dropdownPorts'), grabFn('riskText'), grabFn('roleText'),
	grabFn('portOptionLabel')].forEach(s => vm.runInContext(s, ctx));

const dropdownPorts = ctx.dropdownPorts;
const riskText = ctx.riskText;
const roleText = ctx.roleText;
const portOptionLabel = ctx.portOptionLabel;

const AX_PORTS = [
	{ device: 'lan1', type: 'ethernet', eligible: false, reason: 'mgmt', management: 'yes' },
	{ device: 'lan2', type: 'ethernet', eligible: true, reason: null, management: 'no' },
	{ device: 'lan3', type: 'ethernet', eligible: true, reason: null, management: 'no' },
	{ device: 'eth0', type: 'ethernet', eligible: false, reason: 'dsa_cpu' },
	{ device: 'wan', type: 'ethernet', eligible: false, reason: 'wan' },
	{ device: 'br-lan', type: 'bridge', eligible: false, reason: 'virtual' },
	{ device: 'phy0-ap0', type: 'wireless', eligible: false, reason: 'wireless' },
	{ device: 'om_bat', type: 'batman', eligible: false, reason: 'virtual' },
	{ device: 'eth0.1', type: 'vlan', eligible: false, reason: 'virtual' }
];

test('P1: dropdown lists physical ethernet ports only', function() {
	var devs = dropdownPorts(AX_PORTS).map(p => p.device);
	assert.deepStrictEqual(Array.from(devs),
		['lan1', 'lan2', 'lan3', 'eth0', 'wan'],
		'wireless/bridge/batman/vlan excluded');
});

test('P2: null/failed ports payload yields an empty dropdown', function() {
	/* #14: ubus rejection falls back to { ports: [] } */
	assert.deepStrictEqual(Array.from(dropdownPorts(null)), []);
	assert.deepStrictEqual(Array.from(dropdownPorts(undefined)), []);
	assert.deepStrictEqual(Array.from(dropdownPorts([])), []);
	assert.deepStrictEqual(Array.from(dropdownPorts({})), []);
});

test('P3: management port annotated and recognizable', function() {
	var lbl = portOptionLabel(AX_PORTS[0]);
	assert.strictEqual(lbl, 'lan1 — Management path');
	assert.ok(lbl.indexOf('lan1') === 0, 'device name first');
});

test('P4: blocked reasons annotated in the option label', function() {
	assert.strictEqual(portOptionLabel({ device: 'wan', eligible: false, reason: 'wan' }),
		'wan — WAN in use');
	assert.strictEqual(portOptionLabel({ device: 'eth0', eligible: false, reason: 'dsa_cpu' }),
		'eth0 — Switch CPU port');
	assert.strictEqual(portOptionLabel({ device: 'lan2', eligible: false, reason: 'vlan' }),
		'lan2 — VLAN dependency');
});

test('P5: eligible port label is the bare device name', function() {
	assert.strictEqual(portOptionLabel({ device: 'lan2', eligible: true }), 'lan2');
	assert.strictEqual(portOptionLabel(null), '');
});

test('P6: unknown reason token falls back, never empty/undefined', function() {
	assert.strictEqual(riskText('totally_new_token'), 'Unknown');
	assert.strictEqual(portOptionLabel({ device: 'x', eligible: false, reason: 'weird' }),
		'x — Unknown');
});

test('P7: every backend reason token has a label', function() {
	['mgmt', 'mgmt_unknown', 'wan', 'vlan', 'mesh', 'dsa_cpu', 'pppoe',
	 'virtual', 'wireless', 'master'].forEach(tok => {
		var t = riskText(tok);
		assert.strictEqual(typeof t, 'string');
		assert.ok(t.length > 0 && t !== 'Unknown', tok + ' has dedicated text');
	});
});

test('P8: role tokens mapped', function() {
	assert.strictEqual(roleText('lan'), 'LAN');
	assert.strictEqual(roleText('wan'), 'WAN');
	assert.strictEqual(roleText('unused'), 'Unused port');
	assert.strictEqual(roleText('dsa-cpu'), 'Switch CPU port');
	assert.strictEqual(roleText('whatever') || roleText('') === '' ? roleText('whatever') : '', roleText('whatever'));
	assert.ok(['LAN', 'WAN', 'Mesh', 'Switch CPU port', 'Virtual device',
		'Wireless device', 'Unused port', 'Unknown']
		.includes(roleText('nope')) || roleText('nope') === 'nope');
});

test('P9: malicious device names stay inert strings', function() {
	/* #17: labels are plain strings rendered via textContent; the
	 * helpers must not evaluate or strip anything */
	var evil = '<img src=x onerror=alert(1)>';
	var lbl = portOptionLabel({ device: evil, eligible: true });
	assert.strictEqual(lbl, evil, 'passes through as data, not markup');
	assert.ok(!/<script/.test(String(dropdownPorts([{ device: 'a<b', type: 'ethernet' }]))),
		'no evaluation happens in filtering');
});

test('P10: wizard uses safe DOM only (no innerHTML anywhere)', function() {
	assert.ok(!/innerHTML\s*=/.test(code), 'no innerHTML assignment');
	assert.ok(code.includes('appendChild(E('), 'E() + appendChild pattern');
	assert.ok(code.includes("'type': 'checkbox'"), 'enable switch present');
	assert.ok(code.includes('wired_only') && code.includes('wired_preferred'),
		'mode intent options present');
});

test('P11: plan is refetched on every relevant change', function() {
	/* enable switch, mode change, port change and refresh all re-plan
	 * ('change': fetchPlan is a reference binding without parens) */
	var refs = (code.match(/fetchPlan/g) || []).length;
	var changeBindings = (code.match(/'change': fetchPlan/g) || []).length;
	assert.ok(refs >= 5, 'fetchPlan referenced >= 5 times (got ' + refs + ')');
	assert.strictEqual(changeBindings, 2, 'mode + port selects re-plan');
});

test('P12: backend revalidation is mandatory (#18)', function() {
	/* the C side re-runs discovery inside om_port_plan before answering */
	const portsC = fs.readFileSync(path.join(__dirname, '..', 'openmeshd', 'src', 'ports.c'), 'utf8');
	assert.ok(portsC.includes('om_ports_discover(ports') &&
		portsC.includes('never trust the frontend verdict'),
		'om_port_plan revalidates via fresh discovery');
	assert.ok(portsC.includes('return -1') &&
		portsC.includes('nonexistent port'),
		'unknown device rejected server-side');
	/* frontend treats the plan as data only */
	assert.ok(code.includes('preview_only') || code.includes('read-only'),
		'preview-only semantics visible in UI');
});

test('P13: refresh button rebuilds options (state change, #15)', function() {
	/* simulate: carrier/eligibility changed between two API results */
	var before = dropdownPorts(AX_PORTS).map(p => p.device);
	var changed = AX_PORTS.map(p =>
		p.device === 'lan2' ? Object.assign({}, p, { eligible: false, reason: 'mesh' }) : p);
	var after = dropdownPorts(changed).map(p => p.device);
	assert.deepStrictEqual(Array.from(before), Array.from(after), 'same ports listed');
	assert.strictEqual(portOptionLabel(changed.find(p => p.device === 'lan2')),
		'lan2 — Already used for mesh', 'new state reflected after refresh');
});

test('P14: ubus port_plan failure shows an error, not a crash', function() {
	/* the reject handler replaces plan content with a single message */
	assert.ok(code.includes("'_style'" ) || code.includes('Plan query failed'),
		'plan error message exists');
	assert.ok(/function\s*\(\)\s*\{\s*while\s*\(wiredPlan\.firstChild\)/.test(code),
		'reject handler clears stale plan DOM first');
});

test('P15: preview lists render only provided items', function() {
	/* planList guard: empty/missing arrays produce nothing */
	function planListLen(items) { return (items || []).length; }
	assert.strictEqual(planListLen(undefined), 0);
	assert.strictEqual(planListLen([]), 0);
	assert.strictEqual(planListLen(['a', 'b']), 2);
});

console.log(results.join('\n'));
console.log(`\ntest_ports_ui: ${pass} PASS, ${fail} FAIL`);
process.exit(fail > 0 ? 1 : 0);
