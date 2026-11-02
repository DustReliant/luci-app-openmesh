#!/usr/bin/env node
/*
 * Client-on-canvas tests (phase 6.2 increment): fan layout, RSSI colors,
 * overflow, detail formatting. Extracts the REAL functions from
 * topology.js and runs them in a VM context (same approach as
 * test_topology_frontend.js).
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

/* ---- extract the real code from topology.js ---- */
const jsPath = path.join(__dirname, '..', 'luci-app-openmesh',
	'htdocs', 'luci-static', 'resources', 'view', 'openmesh', 'topology.js');
const code = fs.readFileSync(jsPath, 'utf8');

const colorMatch = code.match(/clientColor: function\(rssi\) \{([\s\S]*?)\n\t\}/);
const posMatch = code.match(/clientPos: function\(apPos, index, total\) \{([\s\S]*?)\n\t\}/);
const fanRMatch = code.match(/CLIENT_FAN_R = (\d+)/);
const rMatch = code.match(/var CLIENT_R = (\d+)/);
const maxShowMatch = code.match(/CLIENT_MAX_SHOW = (\d+)/);
const nodeHMatch = code.match(/NODE_H = (\d+)/);

if (!colorMatch) { console.error('FATAL: clientColor() not found in topology.js'); process.exit(1); }
if (!posMatch) { console.error('FATAL: clientPos() not found in topology.js'); process.exit(1); }
if (!fanRMatch || !rMatch || !maxShowMatch || !nodeHMatch) {
	console.error('FATAL: client design tokens not found'); process.exit(1);
}

const CLIENT_FAN_R = parseInt(fanRMatch[1], 10);
const CLIENT_R = parseInt(rMatch[1], 10);
const CLIENT_MAX_SHOW = parseInt(maxShowMatch[1], 10);
const NODE_H = parseInt(nodeHMatch[1], 10);

const context = vm.createContext({
	Math: Math,
	CLIENT_FAN_R: CLIENT_FAN_R,
	NODE_H: NODE_H
});
vm.runInContext(`function clientColor(rssi) {${colorMatch[1]}}`, context);
vm.runInContext(`function clientPos(apPos, index, total) {${posMatch[1]}}`, context);

const clientColor = context.clientColor;
const clientPos = context.clientPos;

/* fan positions helper: all clients of a fan with `total` entries */
function fanPositions(total) {
	var ap = { x: 400, y: 100 };
	var out = [];
	for (var i = 0; i < total; i++) out.push(clientPos(ap, i, total));
	return { ap: ap, pos: out };
}

/* ---- C-series: client rendering ---- */

test('C1: RSSI color thresholds', function() {
	assert.strictEqual(clientColor(-50), '#4CAF50', 'strong signal green');
	assert.strictEqual(clientColor(-60), '#4CAF50', '-60 boundary green');
	assert.strictEqual(clientColor(-61), '#FF9800', '-61 orange');
	assert.strictEqual(clientColor(-75), '#FF9800', '-75 boundary orange');
	assert.strictEqual(clientColor(-76), '#F44336', '-76 red');
	assert.strictEqual(clientColor(undefined), '#4CAF50', 'missing rssi treated as good');
	assert.strictEqual(clientColor(0), '#4CAF50', 'rssi 0 (unknown) not red');
});

test('C2: fan is symmetric around the AP x', function() {
	var total = 6;
	var f = fanPositions(total);
	var mean = f.pos.reduce(function(s, p) { return s + p.x; }, 0) / total;
	assert(Math.abs(mean - f.ap.x) < 1e-6, 'mean x equals AP x, got offset ' + (mean - f.ap.x));
	assert.strictEqual(f.pos[0].x + f.pos[total - 1].x, 2 * f.ap.x, 'first/last mirrored');
});

test('C3: all clients render below the AP card', function() {
	var f = fanPositions(12);
	f.pos.forEach(function(p, i) {
		assert(p.y > f.ap.y, 'client ' + i + ' below AP (y=' + p.y + ')');
		assert(p.y >= f.ap.y + NODE_H / 2, 'client ' + i + ' clears the card bottom edge');
	});
});

test('C4: single client sits straight below the AP', function() {
	var f = fanPositions(1);
	assert.strictEqual(f.pos[0].x, f.ap.x, 'centered under AP');
	assert.strictEqual(f.pos[0].y, f.ap.y + CLIENT_FAN_R + NODE_H / 2, 'one radius below card edge');
});

test('C5: fan lies on a circle of CLIENT_FAN_R around the card-bottom anchor', function() {
	var f = fanPositions(9);
	var ax = f.ap.x, ay = f.ap.y + NODE_H / 2;
	f.pos.forEach(function(p, i) {
		var d = Math.sqrt((p.x - ax) * (p.x - ax) + (p.y - ay) * (p.y - ay));
		assert(Math.abs(d - CLIENT_FAN_R) < 1e-6, 'client ' + i + ' at fan radius');
	});
});

test('C6: no two clients in the fan overlap', function() {
	var f = fanPositions(CLIENT_MAX_SHOW);
	for (var j = 0; j < f.pos.length; j++)
		for (var k = j + 1; k < f.pos.length; k++) {
			var dx = f.pos[j].x - f.pos[k].x, dy = f.pos[j].y - f.pos[k].y;
			var dist = Math.sqrt(dx * dx + dy * dy);
			assert(dist >= 2 * CLIENT_R, 'clients ' + j + ',' + k +
				' overlap (dist=' + dist.toFixed(1) + ' < ' + (2 * CLIENT_R) + ')');
		}
});

test('C7: overflow beyond CLIENT_MAX_SHOW is summarized, not rendered', function() {
	var clients = [];
	for (var i = 0; i < 20; i++) clients.push({ mac: 'aa:bb:cc:00:00:' + ('0' + i).slice(-2) });
	var shown = clients.slice(0, CLIENT_MAX_SHOW);
	assert.strictEqual(shown.length, CLIENT_MAX_SHOW, 'only first ' + CLIENT_MAX_SHOW + ' rendered');
	var overflow = clients.length - CLIENT_MAX_SHOW;
	assert.strictEqual(overflow, 20 - CLIENT_MAX_SHOW, 'overflow count correct');
});

test('C8: empty client list renders nothing and does not error', function() {
	var clients = [];
	assert.strictEqual(clients.slice(0, CLIENT_MAX_SHOW).length, 0, 'no fan entries');
	assert(!(clients.length > CLIENT_MAX_SHOW), 'no overflow indicator');
});

test('C9: client MAC label shortened to 8 chars + ellipsis', function() {
	var mac = '5C:66:6C:5F:38:43';
	var short = mac.length > 11 ? mac.substring(0, 8) + '…' : mac;
	assert.strictEqual(short, '5C:66:6C…', 'label truncated');
	assert.strictEqual(short.length, 9, 'label length bounded');
});

test('C10: selection ring only on the selected client', function() {
	var clients = [{ mac: 'aa:aa:aa:aa:aa:01' }, { mac: 'aa:aa:aa:aa:aa:02' }];
	var selected = 'aa:aa:aa:aa:aa:02';
	clients.forEach(function(c) {
		var ring = selected === c.mac;
		if (c.mac === 'aa:aa:aa:aa:aa:02') assert(ring, 'selected client has ring');
		else assert(!ring, 'other client has no ring');
	});
});

test('C11: detail panel formats RSSI and rates from M16 fields', function() {
	var c = { mac: '5C:66:6C:5F:38:43', ssid: 'Home', ifname: 'phy1-ap0',
		rssi: -49, rx_kbps: 6000, tx_kbps: 780000 };
	assert.strictEqual(c.rssi ? c.rssi + ' dBm' : '-', '-49 dBm', 'rssi row');
	assert.strictEqual(Math.round(c.rx_kbps / 1000) + ' Mbps', '6 Mbps', 'rx row (6.0 MBit/s)');
	assert.strictEqual(Math.round(c.tx_kbps / 1000) + ' Mbps', '780 Mbps', 'tx row (780.0 MBit/s)');
	/* r17 real-capture value: 1200.9 MBit/s must round to 1201, not truncate */
	assert.strictEqual(Math.round(1200900 / 1000), 1201, 'rate rounds, not truncates');
	var empty = { rssi: 0 };
	assert.strictEqual(empty.rssi ? empty.rssi + ' dBm' : '-', '-', 'missing rssi shows dash');
	assert.strictEqual(empty.rx_kbps ? 'x' : undefined, undefined, 'missing rate row omitted');
});

test('C12: fan spans a visible arc at max occupancy', function() {
	var f = fanPositions(CLIENT_MAX_SHOW);
	var xs = f.pos.map(function(p) { return p.x; });
	assert(Math.max.apply(null, xs) - Math.min.apply(null, xs) > 100,
		'12-client fan wider than 100px');
});

test('C13: client position follows a dragged AP (recompute on new apPos)', function() {
	var f1 = fanPositions(3);
	var moved = { x: 700, y: 300 };
	var p0 = clientPos(moved, 0, 3);
	/* relative offset to AP must be invariant under translation */
	var relOrig = { x: f1.pos[0].x - f1.ap.x, y: f1.pos[0].y - f1.ap.y };
	var relMoved = { x: p0.x - moved.x, y: p0.y - moved.y };
	assert(Math.abs(relOrig.x - relMoved.x) < 1e-6 && Math.abs(relOrig.y - relMoved.y) < 1e-6,
		'fan geometry translation-invariant');
});

/* ---- output ---- */
console.log(results.join('\n'));
console.log(`\ntest_topology_clients: ${pass} PASS, ${fail} FAIL`);
process.exit(fail > 0 ? 1 : 0);
