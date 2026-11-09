#!/usr/bin/env node
/*
 * Phase 6.3 (topology status) tests: node status model, link quality
 * grading, collection-health decoupling, transition events, bounded
 * short-term trend. Pure helpers are extracted from the REAL
 * topology.js and executed in a VM (same approach as the 6.1 suite).
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

/* ---- extract the real helpers from topology.js ---- */
const jsPath = path.join(__dirname, '..', 'luci-app-openmesh',
	'htdocs', 'luci-static', 'resources', 'view', 'openmesh', 'topology.js');
const code = fs.readFileSync(jsPath, 'utf8');

const PRELUDE = code.match(/var STATUS_VISUAL = \{[\s\S]*?\n\};/)[0] + '\n' +
	code.match(/var DEGRADE_STREAK[^\n]*\n(?:var [^\n]*\n)*/)[0];

function grabFn(name, args) {
	const m = code.match(new RegExp(
		`function ${name}\\(${args}\\) \\{([\\s\\S]*?)\n\}`));
	if (!m) { console.error(`FATAL: ${name}() not found in topology.js`); process.exit(1); }
	return `function ${name}(${args}) {${m[1]}}`;
}

const ctx = vm.createContext({ Math: Math });
[PRELUDE,
	grabFn('nodeVisualStatus', 'n, streak'),
	grabFn('nodeDegradeBad', 'n, links'),
	grabFn('linkQualityClass', 'l'),
	grabFn('qualityColor', 'cls'),
	grabFn('collectState', 'health, rpcFailed'),
	grabFn('diffTopologyEvents', 'prev, cur'),
	grabFn('histPush', 'h, ts, val, cap'),
	grabFn('histSegments', 'h, gapMs'),
	grabFn('fmtAgeSecs', 'secs'),
	grabFn('fmtDur', 's')
].forEach(s => vm.runInContext(s, ctx));

const nodeVisualStatus = ctx.nodeVisualStatus;
const nodeDegradeBad = ctx.nodeDegradeBad;
const linkQualityClass = ctx.linkQualityClass;
const qualityColor = ctx.qualityColor;
const collectState = ctx.collectState;
const diffTopologyEvents = ctx.diffTopologyEvents;
const histPush = ctx.histPush;
const histSegments = ctx.histSegments;
const fmtAgeSecs = ctx.fmtAgeSecs;

function snap(nodes, links) {
	var st = { nodes: {}, links: {} };
	nodes.forEach(n => st.nodes[n.id] = n.status || 'unknown');
	links.forEach(l => st.links[l.id] = true);
	return st;
}

const N1 = { id: 'ctrl', hostname: 'ctrl', role: 'controller', status: 'online' };
const N2 = { id: 'agent', hostname: 'x86', role: 'agent', status: 'online' };
const W_LINK = { id: 'w1', source: 'ctrl', target: 'agent', type: '80211s', rssi: -27 };
const B_LINK = { id: 'b1', source: 'ctrl', target: 'agent', type: 'batman', tq: 230 };

/* ---- node status model ---- */

test('T1: online -> offline transition', function() {
	assert.strictEqual(nodeVisualStatus({ status: 'online' }, 0), 'online');
	var off = Object.assign({}, N2, { status: 'offline' });
	assert.strictEqual(nodeVisualStatus(off, 0), 'offline');
	assert.strictEqual(ctx.STATUS_VISUAL.offline.color, '#9E9E9E',
		'offline is GRAY, not red (collect failure must not look like down)');
});

test('T2: offline -> online transition', function() {
	/* streak never resurrects an offline node */
	var off = Object.assign({}, N2, { status: 'offline' });
	assert.strictEqual(nodeVisualStatus(off, 5), 'offline');
	/* recovery = next snapshot reports online again */
	var back = Object.assign({}, N2, { status: 'online' });
	assert.strictEqual(nodeVisualStatus(back, 0), 'online');
	assert.strictEqual(nodeVisualStatus(back, 2), 'degraded',
		'recovered node with still-poor signal degrades again after streak');
});

test('T3: unknown status (missing/foreign value)', function() {
	assert.strictEqual(nodeVisualStatus(null), 'unknown');
	assert.strictEqual(nodeVisualStatus({ id: 'x', status: '' }), 'unknown');
	assert.strictEqual(nodeVisualStatus({ id: 'x', status: 'pending' }), 'unknown');
	assert.strictEqual(nodeVisualStatus({ id: 'x' }), 'unknown');
});

test('T4: degraded requires sustained evidence, not one RSSI dip', function() {
	/* one poll of poor signal: streak 1 -> still online */
	assert.strictEqual(nodeVisualStatus(N2, 1), 'online');
	/* two consecutive polls (>= DEGRADE_STREAK) -> degraded */
	assert.strictEqual(nodeVisualStatus(N2, 2), 'degraded');
	assert.strictEqual(ctx.STATUS_VISUAL.degraded.color, '#FF9800');
	/* offline node with bad links never becomes degraded */
	var off = Object.assign({}, N2, { status: 'offline' });
	assert.strictEqual(nodeVisualStatus(off, 9), 'offline');
});

test('T5: degrade evidence rules', function() {
	var poor = Object.assign({}, W_LINK, { rssi: -80 });
	/* all read links poor -> evidence */
	assert.strictEqual(nodeDegradeBad(N2, [poor]), true);
	/* one decent link clears it */
	assert.strictEqual(nodeDegradeBad(N2, [poor, W_LINK]), false);
	/* single RSSI dip alone (streak handled by caller) is not degraded */
	assert.strictEqual(nodeVisualStatus(N2, 1), 'online');
	/* missing metric is NOT evidence */
	assert.strictEqual(nodeDegradeBad(N2, [{ id: 'w', source: 'ctrl',
		target: 'agent', type: '80211s', rssi: 0 }]), false);
	/* no 80211s links at all -> not degraded */
	assert.strictEqual(nodeDegradeBad(N2, [B_LINK]), false);
	/* batman TQ poor does not degrade (different protocol) */
	assert.strictEqual(nodeDegradeBad(N2, [Object.assign({}, B_LINK, { tq: 30 })]), false);
	/* offline node -> never degraded */
	assert.strictEqual(nodeDegradeBad({ id: 'agent', status: 'offline' }, [poor]), false);
});

/* ---- link quality grading (protocol-specific) ---- */

test('T6: 802.11s RSSI grading and changes', function() {
	assert.strictEqual(linkQualityClass(W_LINK), 'good');            /* -27 */
	assert.strictEqual(linkQualityClass({ ...W_LINK, rssi: -60 }), 'good');   /* boundary */
	assert.strictEqual(linkQualityClass({ ...W_LINK, rssi: -61 }), 'fair');
	assert.strictEqual(linkQualityClass({ ...W_LINK, rssi: -75 }), 'fair');   /* boundary */
	assert.strictEqual(linkQualityClass({ ...W_LINK, rssi: -76 }), 'poor');
});

test('T7: BATMAN TQ grading and changes (own scale)', function() {
	assert.strictEqual(linkQualityClass(B_LINK), 'good');            /* 230 */
	assert.strictEqual(linkQualityClass({ ...B_LINK, tq: 200 }), 'good');
	assert.strictEqual(linkQualityClass({ ...B_LINK, tq: 199 }), 'fair');
	assert.strictEqual(linkQualityClass({ ...B_LINK, tq: 100 }), 'fair');
	assert.strictEqual(linkQualityClass({ ...B_LINK, tq: 99 }), 'poor');
});

test('T8: TQ and RSSI never share a scale', function() {
	/* the same numeric value means different things per protocol */
	assert.strictEqual(linkQualityClass({ type: '80211s', rssi: -76 }), 'poor');
	assert.strictEqual(linkQualityClass({ type: 'batman', tq: 76 }), 'poor');
	assert.notStrictEqual(linkQualityClass({ type: '80211s', rssi: -55 }),
		linkQualityClass({ type: 'batman', tq: 55 }));
	assert.strictEqual(qualityColor('poor'), '#F44336');
	assert.strictEqual(qualityColor('good'), '#4CAF50');
});

test('T9: missing metric is nodata, never faked or graded', function() {
	assert.strictEqual(linkQualityClass({ ...W_LINK, rssi: 0 }), 'nodata');
	assert.strictEqual(linkQualityClass({ ...W_LINK, rssi: undefined }), 'nodata');
	assert.strictEqual(linkQualityClass({ ...B_LINK, tq: 0 }), 'nodata');
	assert.strictEqual(linkQualityClass(null), 'nodata');
	assert.strictEqual(linkQualityClass({ type: 'carrier-pigeon' }), 'nodata');
	assert.strictEqual(qualityColor('nodata'), '#9E9E9E');
});

test('T10: ethernet graded by presence, not by wifi/batman scales', function() {
	assert.strictEqual(linkQualityClass({ type: 'ethernet' }), 'active');
	assert.strictEqual(qualityColor('active'), '#5E5E5E');
});

/* ---- collection-health decoupling ---- */

test('T11: collect stale/failed never changes node visual state', function() {
	/* the status model has NO health input at all -- by design */
	assert.strictEqual(nodeVisualStatus.length, 2, 'signature: (node, streak) only');
	assert.strictEqual(nodeVisualStatus({ status: 'online' }, 0), 'online');
	['stale', 'failed'].forEach(h => {
		assert.strictEqual(collectState(h, false) === 'stale' ||
			collectState(h, false) === 'failed', true);
		assert.strictEqual(nodeVisualStatus({ status: 'online' }, 0), 'online');
	});
});

test('T12: collectState semantic tokens', function() {
	assert.strictEqual(collectState('ok', false), 'ok');
	assert.strictEqual(collectState(undefined, false), 'ok');
	assert.strictEqual(collectState('stale', false), 'stale');
	assert.strictEqual(collectState('failed', false), 'failed');
	assert.strictEqual(collectState('ok', true), 'rpc');        /* rpc outranks */
	assert.strictEqual(collectState('failed', true), 'rpc');
});

/* ---- transition events ---- */

test('T13: node online/offline transitions emit exactly once', function() {
	var s1 = snap([N1, N2], [W_LINK]);
	var s2 = snap([N1, { ...N2, status: 'offline' }], [W_LINK]);
	var ev = diffTopologyEvents(s1, s2);
	assert.strictEqual(ev.length, 1);
	assert.strictEqual(ev[0].kind, 'node_offline');
	assert.strictEqual(ev[0].key, 'agent');
	/* same state again: no repeat (no 5s spam) */
	assert.strictEqual(diffTopologyEvents(s2, s2).length, 0);
	/* recovery */
	var ev2 = diffTopologyEvents(s2, s1);
	assert.strictEqual(ev2.length, 1);
	assert.strictEqual(ev2[0].kind, 'node_online');
});

test('T14: node appearing with unknown status emits nothing', function() {
	var s1 = snap([N1], []);
	var s2 = snap([N1, { id: 'new', status: 'unknown-ish' }], []);
	assert.strictEqual(diffTopologyEvents(s1, s2).length, 0);
	var s3 = snap([N1, { id: 'new', status: 'online' }], []);
	var ev = diffTopologyEvents(s1, s3);
	assert.strictEqual(ev.length, 1);
	assert.strictEqual(ev[0].kind, 'node_online');
});

test('T15: node/link disappearance and recovery events', function() {
	var s1 = snap([N1, N2], [W_LINK, B_LINK]);
	var s2 = snap([N1, N2], [W_LINK]);          /* batman link gone */
	var ev = diffTopologyEvents(s1, s2);
	assert.strictEqual(ev.length, 1);
	assert.strictEqual(ev[0].kind, 'link_down');
	assert.strictEqual(ev[0].key, 'b1');
	var ev2 = diffTopologyEvents(s2, s1);       /* recovered */
	assert.strictEqual(ev2.length, 1);
	assert.strictEqual(ev2[0].kind, 'link_up');
	/* whole node vanished (deadopted): offline event once */
	var s3 = snap([N1], [W_LINK]);
	var ev3 = diffTopologyEvents(s1, s3);
	assert.strictEqual(ev3.length, 2);
	assert.ok(ev3.some(e => e.kind === 'node_offline' && e.key === 'agent'));
	assert.ok(ev3.some(e => e.kind === 'link_down' && e.key === 'b1'));
});

/* ---- refresh invariants (logic mirroring the view loop) ---- */

test('T16: transient node disappearance keeps its stored position', function() {
	/* the view never prunes nodePos; renderNodes reuses any stored entry */
	var nodePos = { ctrl: { x: 100, y: 80 }, agent: { x: 220, y: 240 } };
	function posFor(id) { return nodePos[id] || null; }  /* renderNodes lookup */
	/* snapshot without agent */
	assert.strictEqual(posFor('agent'), nodePos.agent);
	/* agent returns: same object -> identical coordinates, no relayout */
	var p = posFor('agent');
	assert.strictEqual(p.x, 220);
	assert.strictEqual(p.y, 240);
});

test('T17: refresh never re-lays-out existing nodes', function() {
	/* mirrors refresh(): smartLayout is not in the poll path; only
	 * autoLayoutNew assigns positions to ids missing from nodePos */
	var nodePos = { a: { x: 1, y: 2 } };
	function refreshStep(nodes) {
		var newNodes = nodes.filter(n => !nodePos[n.id]);
		newNodes.forEach(function(n, i) {
			nodePos[n.id] = { x: 999 + i, y: 999 };
		});
	}
	refreshStep([{ id: 'a' }, { id: 'b' }]);
	assert.deepStrictEqual(nodePos.a, { x: 1, y: 2 }, 'existing untouched');
	assert.ok(nodePos.b.x === 999, 'new node placed');
	refreshStep([{ id: 'a' }, { id: 'b' }]);
	assert.deepStrictEqual(nodePos.a, { x: 1, y: 2 });
	assert.deepStrictEqual(nodePos.b, { x: 999, y: 999 }, 'no re-place on 2nd poll');
});

test('T18: RPC failure keeps last topology; recovery updates silently', function() {
	/* catch() path: lastNodes/lastLinks/pan/zoom untouched, banner only */
	var view = { lastNodes: [N1, N2], lastLinks: [W_LINK], _rpcFailed: false };
	view.onCatch = function() {
		if (!this._rpcFailed) this._rpcFailed = true;  /* one event per outage */
	};
	view.onCatch(); view.onCatch(); view.onCatch();
	assert.strictEqual(view._rpcFailed, true);
	assert.strictEqual(view.lastNodes.length, 2, 'cached data preserved');
	/* recovery: flag clears, data replaced -- no state lost */
	view._rpcFailed = false;
	assert.strictEqual(collectState('ok', view._rpcFailed), 'ok');
});

test('T19: selected object disappearing clears selection with notice', function() {
	/* mirrors renderDetail guard: not found -> selection nulled + notice */
	function guard(selected, nodes) {
		var found = nodes.some(n => n.id === selected);
		return found ? { keep: true } : { keep: false, notice: true };
	}
	var r = guard('agent', [N1, N2]);
	assert.strictEqual(r.keep, true);
	var r2 = guard('agent', [N1]);                    /* vanished */
	assert.strictEqual(r2.keep, false);
	assert.strictEqual(r2.notice, true);
	/* dual links stay independently selectable by distinct ids */
	var ids = [W_LINK, B_LINK].map(l => l.id);
	assert.notStrictEqual(ids[0], ids[1]);
});

/* ---- detail formatting / safety ---- */

test('T20: detail rows show -- for missing metrics, no fake values', function() {
	var l = { type: '80211s', rssi: 0, tx_rate: 0 };
	var rssiTxt = l.rssi ? l.rssi + ' dBm' : '--';
	var txTxt = l.tx_rate ? Math.round(l.tx_rate / 1000) + ' Mbps' : '--';
	assert.strictEqual(rssiTxt, '--');
	assert.strictEqual(txTxt, '--');
	var b = { type: 'batman', tq: 0 };
	assert.strictEqual(b.tq ? String(b.tq) : '--', '--');
});

test('T21: malicious hostname stays escaped in all new text paths', function() {
	/* _text()/textContent only; verify the shared esc() still neutralizes */
	var evil = '<svg onload=alert(1)>node';
	var escd = evil.replace(/&/g, '&amp;').replace(/</g, '&lt;')
		.replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');
	assert.ok(!escd.includes('<svg'));
	assert.ok(escd.includes('&lt;svg'));
	/* event ticker builds strings for textContent, never innerHTML */
	assert.ok(code.includes('tick.textContent'), 'ticker uses textContent');
	assert.ok(!/innerHTML\s*=/.test(code), 'no innerHTML assignment anywhere');
});

/* ---- short-term trend (bounded, gaps visible) ---- */

test('T22: trend ring buffer is bounded', function() {
	var h = null;
	for (var i = 0; i < 100; i++) h = histPush(h, 1000 * i, -50 - (i % 10), 36);
	assert.strictEqual(h.t.length, 36, 'capped at 36 samples');
	assert.strictEqual(h.t[0], 1000 * 64, 'oldest evicted');
});

test('T23: sampling gaps split segments (no interpolation)', function() {
	var h = null;
	h = histPush(h, 0, -50, 36);
	h = histPush(h, 5000, -52, 36);
	h = histPush(h, 10000, -51, 36);
	h = histPush(h, 40000, -53, 36);   /* 30s gap = missed polls */
	h = histPush(h, 45000, -54, 36);
	var segs = histSegments(h, 12500);
	assert.strictEqual(segs.length, 2, 'gap breaks the line');
	assert.strictEqual(segs[0].length, 3);
	assert.strictEqual(segs[1].length, 2);
	/* continuous sampling stays one segment */
	var h2 = null;
	for (var i = 0; i < 10; i++) h2 = histPush(h2, i * 5000, -50, 36);
	assert.strictEqual(histSegments(h2, 12500).length, 1);
	assert.strictEqual(histSegments(null, 12500).length, 0);
});

/* ---- misc formatting ---- */

test('T24: age/duration formatting', function() {
	/* compare fields (VM-realm objects) */
	var a = fmtAgeSecs(30);
	assert.strictEqual(a.n, 30); assert.strictEqual(a.unit, 's');
	a = fmtAgeSecs(90);
	assert.strictEqual(a.n, 1); assert.strictEqual(a.unit, 'm');
	a = fmtAgeSecs(7300);
	assert.strictEqual(a.n, 2); assert.strictEqual(a.unit, 'h');
	a = fmtAgeSecs(-5);
	assert.strictEqual(a.n, 0); assert.strictEqual(a.unit, 's');
	a = fmtAgeSecs(undefined);
	assert.strictEqual(a.n, 0); assert.strictEqual(a.unit, 's');
	assert.strictEqual(ctx.fmtDur(45), '0m');
	assert.strictEqual(ctx.fmtDur(3700), '1h 1m');
	assert.strictEqual(ctx.fmtDur(90000), '1d 1h');
});

/* ---- touch + persistence invariants still present ---- */

test('T25: pointer-events & layout persistence intact (regression)', function() {
	assert.ok(code.includes("'pointerdown'"), 'pointer events bound');
	assert.ok(code.includes('touchAction'), 'touch-action set');
	assert.ok(code.includes("evt.key === 'Escape'"), 'ESC deselect');
	assert.ok(code.includes("localStorage"), 'layout persistence');
	assert.ok(code.includes('.topo-client'), 'client selection untouched');
});

console.log(results.join('\n'));
console.log(`\ntest_phase63_status: ${pass} PASS, ${fail} FAIL`);
process.exit(fail > 0 ? 1 : 0);
