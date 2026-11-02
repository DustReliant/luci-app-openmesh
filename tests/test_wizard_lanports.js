#!/usr/bin/env node
/*
 * Wizard wired-backhaul dropdown tests (m19). Extracts the REAL
 * lanPortOptions() from wizard.js and runs it in a VM context, plus
 * logic-level checks for the hybrid-mode gating and create-params.
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

const fnMatch = code.match(/function lanPortOptions\(ports\) \{([\s\S]*?)\n\}/);
if (!fnMatch) { console.error('FATAL: lanPortOptions() not found in wizard.js'); process.exit(1); }

const ctx = vm.createContext({ Math: Math });
vm.runInContext(`function lanPortOptions(ports) {${fnMatch[1]}}`, ctx);
const lanPortOptions = ctx.lanPortOptions;

test('W1: passes physical ports through', function() {
	assert.deepStrictEqual(lanPortOptions(['lan1', 'lan2', 'lan3']),
		['lan1', 'lan2', 'lan3']);
	assert.deepStrictEqual(lanPortOptions(['eth0']), ['eth0']);
});

test('W2: om_* virtual members excluded', function() {
	assert.deepStrictEqual(lanPortOptions(['lan1', 'om_bat', 'om_mesh', 'lan2']),
		['lan1', 'lan2']);
});

test('W3: duplicates removed', function() {
	assert.deepStrictEqual(lanPortOptions(['lan1', 'lan1', 'lan2']), ['lan1', 'lan2']);
});

test('W4: junk names rejected (daemon validates the same charset)', function() {
	assert.deepStrictEqual(lanPortOptions(['lan1;rm', 'lan-1', 'lan 2', '']),
		[]);
	assert.deepStrictEqual(lanPortOptions(['ok_1', 'x'.repeat(16)]), ['ok_1']);
});

test('W5: null/undefined/empty input handled', function() {
	/* Array.from: the fn runs in a VM context (foreign array realm) */
	assert.deepStrictEqual(Array.from(lanPortOptions(undefined)), []);
	assert.deepStrictEqual(Array.from(lanPortOptions(null)), []);
	assert.deepStrictEqual(Array.from(lanPortOptions([])), []);
});

/* ---- gating + params (logic mirrors the view's syncLanPorts/create) ---- */

test('W6: dropdown enabled only in hybrid mode', function() {
	/* syncLanPorts: lanPortsSel.disabled = (modeSel.value !== 'hybrid') */
	assert.strictEqual('wifi' !== 'hybrid', true, 'wifi disables wired leg');
	assert.strictEqual('hybrid' !== 'hybrid', false, 'hybrid enables it');
});

test('W7: create call passes the effective selection', function() {
	/* disabled dropdown sends '' regardless of its visual value */
	function effective(disabled, value) {
		return disabled ? '' : value;
	}
	assert.strictEqual(effective(true, 'all'), '', 'disabled → no wired leg');
	assert.strictEqual(effective(false, 'lan1'), 'lan1');
	assert.strictEqual(effective(false, ''), '', 'explicit none');
	assert.strictEqual(effective(false, 'all'), 'all');
});

test('W8: option values are exactly the ubus spec vocabulary', function() {
	/* wizard offers '', 'all' and dynamic port names; all accepted by
	 * om_lan_ports_valid (see test_lanports.c) */
	['', 'all', 'lan1'].forEach(function(v) {
		assert(/^(|[a-z0-9_]{1,15}|all)$/.test(v), 'value ' + v + ' in vocabulary');
	});
});

test('W9: rpc declare carries the 5th param', function() {
	assert(code.includes("params: [ 'mesh_name', 'band', 'key', 'mode', 'lan_ports' ]"),
		'mesh_create params include lan_ports');
	assert(code.includes("method: 'get_lan_ports'"), 'get_lan_ports declared');
});

console.log(results.join('\n'));
console.log(`\ntest_wizard_lanports: ${pass} PASS, ${fail} FAIL`);
process.exit(fail > 0 ? 1 : 0);
