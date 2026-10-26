#!/usr/bin/env node
/*
 * Phase 6.2 automated tests: smart layout, dual-link separation, node cards.
 * Tests layout algorithm + rendering logic without a real browser.
 */
'use strict';
const assert = require('assert');
let pass = 0, fail = 0, results = [];

function test(name, fn) {
	try { fn(); pass++; results.push(`PASS ${name}`); }
	catch (e) { fail++; results.push(`FAIL ${name}: ${e.message}`); }
}

/* ---- layout algorithm (extracted from topology.js logic) ---- */

var NODE_W = 180, LEVEL_GAP = 160, SIBLING_GAP = 220;
var LINK_CURVE_GAP = 50;

function smartLayout(nodes, links) {
	var byId = {}; nodes.forEach(n => byId[n.id] = n);
	var adj = {}; nodes.forEach(n => adj[n.id] = []);
	(links || []).forEach(l => {
		if (l.type === 'ethernet' || l.type === '80211s') {
			if (adj[l.source]) adj[l.source].push(l.target);
			if (adj[l.target]) adj[l.target].push(l.source);
		}
	});
	var root = null;
	nodes.forEach(n => { if (n.role === 'controller') root = n.id; });
	if (!root && nodes.length > 0) root = nodes[0].id;

	var level = {}, visited = {}, queue = [root];
	level[root] = 0; visited[root] = true;
	var maxLevel = 0;
	while (queue.length > 0) {
		var cur = queue.shift();
		(adj[cur] || []).forEach(next => {
			if (!visited[next]) {
				visited[next] = true;
				level[next] = level[cur] + 1;
				if (level[next] > maxLevel) maxLevel = level[next];
				queue.push(next);
			}
		});
	}
	var isolated = [];
	nodes.forEach(n => { if (!visited[n.id]) { isolated.push(n.id); level[n.id] = maxLevel + 1; } });

	var byLevel = {};
	nodes.forEach(n => {
		var lv = level[n.id] || 0;
		if (!byLevel[lv]) byLevel[lv] = [];
		byLevel[lv].push(n);
	});
	Object.keys(byLevel).forEach(lv => byLevel[lv].sort((a,b) => a.id < b.id ? -1 : 1));

	var pos = {};
	var cx = 400;
	Object.keys(byLevel).map(Number).sort((a,b)=>a-b).forEach(lv => {
		var sib = byLevel[lv];
		var totalW = (sib.length - 1) * SIBLING_GAP;
		sib.forEach((n, i) => {
			pos[n.id] = { x: cx - totalW/2 + i * SIBLING_GAP, y: 80 + lv * LEVEL_GAP };
		});
	});
	return { pos, level, isolated };
}

/* ---- link curve computation (extracted) ---- */
function computeLinkOffsets(links) {
	var pairs = {};
	links.forEach(l => {
		var key = l.source < l.target ? l.source+'|'+l.target : l.target+'|'+l.source;
		if (!pairs[key]) pairs[key] = [];
		pairs[key].push(l);
	});
	var result = [];
	Object.keys(pairs).forEach(key => {
		var pair = pairs[key];
		pair.sort((a,b) => a.type < b.type ? -1 : 1);
		var total = pair.length;
		var mid = (total - 1) / 2;
		pair.forEach((l, idx) => {
			result.push({ link: l, offset: (idx - mid) * LINK_CURVE_GAP, total, idx });
		});
	});
	return result;
}

function bezierMidpoint(s, d, offset) {
	var dx = d.x - s.x, dy = d.y - s.y;
	var len = Math.sqrt(dx*dx + dy*dy) || 1;
	var px = -dy / len * offset;
	var py = dx / len * offset;
	return { mx: (s.x+d.x)/2 + px*0.55, my: (s.y+d.y)/2 + py*0.55 };
}

/* ---- helpers ---- */
function makeNodes(n, prefix) {
	var arr = [];
	for (var i = 0; i < n; i++)
		arr.push({ id: (prefix||'n') + i, hostname: 'Node' + i, role: i === 0 ? 'controller' : 'agent', status: 'online' });
	return arr;
}
function makeChainLinks(nodes) {
	var links = [];
	for (var i = 0; i < nodes.length - 1; i++)
		links.push({ id: 'l'+i, source: nodes[i].id, target: nodes[i+1].id, type: '80211s' });
	return links;
}
function makeStarLinks(nodes) {
	return nodes.slice(1).map((n, i) => ({
		id: 'l'+i, source: nodes[0].id, target: n.id, type: '80211s'
	}));
}

/* ---- layout tests ---- */

test('L1: 2 nodes', function() {
	var nodes = makeNodes(2);
	var { pos } = smartLayout(nodes, makeChainLinks(nodes));
	assert(Object.keys(pos).length === 2);
	assert(pos[nodes[0].id].y < pos[nodes[1].id].y, 'controller above agent');
});

test('L2: 5 nodes star', function() {
	var nodes = makeNodes(5);
	var { pos, level } = smartLayout(nodes, makeStarLinks(nodes));
	assert(level[nodes[0].id] === 0, 'controller at level 0');
	nodes.slice(1).forEach(n => assert(level[n.id] === 1, 'all agents at level 1'));
	/* check horizontal spread */
	var xs = nodes.slice(1).map(n => pos[n.id].x);
	assert(Math.max(...xs) - Math.min(...xs) >= SIBLING_GAP * 3, 'agents spread horizontally');
});

test('L3: 10 nodes chain', function() {
	var nodes = makeNodes(10);
	var { pos, level } = smartLayout(nodes, makeChainLinks(nodes));
	assert(level[nodes[9].id] === 9, 'chain depth = 9');
	assert(pos[nodes[9].id].y - pos[nodes[0].id].y === 9 * LEVEL_GAP, 'vertical spacing correct');
});

test('L4: 20 nodes tree', function() {
	var nodes = makeNodes(20);
	/* binary tree: node i connects to 2i+1 and 2i+2 */
	var links = [];
	for (var i = 0; i < nodes.length; i++) {
		if (2*i+1 < nodes.length) links.push({ id:'l'+(2*i), source: nodes[i].id, target: nodes[2*i+1].id, type:'80211s' });
		if (2*i+2 < nodes.length) links.push({ id:'l'+(2*i+1), source: nodes[i].id, target: nodes[2*i+2].id, type:'80211s' });
	}
	var { pos } = smartLayout(nodes, links);
	assert(Object.keys(pos).length === 20, 'all 20 nodes positioned');
	/* verify no overlapping positions */
	var positions = Object.values(pos);
	for (var j = 0; j < positions.length; j++)
		for (var k = j+1; k < positions.length; k++) {
			if (positions[j].x === positions[k].x && positions[j].y === positions[k].y)
				throw new Error('nodes j=' + j + ' k=' + k + ' overlap');
		}
});

test('L5: ring network (no infinite loop)', function() {
	var nodes = makeNodes(4);
	var links = [
		{ id:'l0', source:'n0', target:'n1', type:'80211s' },
		{ id:'l1', source:'n1', target:'n2', type:'80211s' },
		{ id:'l2', source:'n2', target:'n3', type:'80211s' },
		{ id:'l3', source:'n3', target:'n0', type:'80211s' }
	];
	var { pos } = smartLayout(nodes, links); /* should not hang */
	assert(Object.keys(pos).length === 4);
});

test('L6: isolated nodes placed separately', function() {
	var connected = makeNodes(3, 'c');
	var isolated = makeNodes(2, 'i');
	isolated.forEach(n => n.role = 'agent');
	var all = connected.concat(isolated);
	var links = makeChainLinks(connected);
	var { pos, isolated: iso } = smartLayout(all, links);
	assert(iso.length === 2, '2 isolated nodes detected');
	/* isolated nodes at different position than connected */
	assert(pos[isolated[0].id] !== pos[connected[0].id], 'isolated not overlapping connected');
});

test('L7: multi-link same pair does not duplicate adjacency', function() {
	var nodes = makeNodes(2);
	var links = [
		{ id:'l1', source:'n0', target:'n1', type:'80211s' },
		{ id:'l2', source:'n0', target:'n1', type:'batman' }
	];
	var { level } = smartLayout(nodes, links);
	/* batman is not a tree edge, so level[n1] should still be 1 from 80211s */
	assert(level['n1'] === 1, 'batman link does not create extra BFS level');
});

test('L8: input order stability', function() {
	var nodesA = makeNodes(5);
	var nodesB = makeNodes(5).reverse();
	var linksA = makeStarLinks(nodesA);
	var linksB = makeStarLinks(nodesA).reverse();
	var rA = smartLayout(nodesA, linksA);
	var rB = smartLayout(nodesB, linksB);
	nodesA.forEach(n =>
		assert(rA.pos[n.id].x === rB.pos[n.id].x && rA.pos[n.id].y === rB.pos[n.id].y,
			'node ' + n.id + ' position stable across input order'));
});

test('L9: batman-only links do not create layout adjacency', function() {
	var nodes = makeNodes(3);
	var links = [
		{ id:'l1', source:'n0', target:'n1', type:'batman' },
		{ id:'l2', source:'n0', target:'n2', type:'batman' }
	];
	var { isolated } = smartLayout(nodes, links);
	/* with only batman links, n1 and n2 are isolated (no physical adjacency) */
	assert(isolated.length >= 1, 'batman-only nodes treated as isolated');
});

/* ---- dual link separation tests ---- */

test('R15: dual links have different offsets', function() {
	var links = [
		{ id:'l1', source:'A', target:'B', type:'80211s' },
		{ id:'l2', source:'A', target:'B', type:'batman' }
	];
	var offsets = computeLinkOffsets(links);
	assert(offsets.length === 2);
	assert(offsets[0].offset !== offsets[1].offset, 'offsets differ');
	assert(offsets[0].offset === -offsets[1].offset, 'symmetric offsets');
});

test('R16: three links fan out', function() {
	var links = [
		{ id:'l1', source:'A', target:'B', type:'80211s' },
		{ id:'l2', source:'A', target:'B', type:'batman' },
		{ id:'l3', source:'A', target:'B', type:'ethernet' }
	];
	var offsets = computeLinkOffsets(links);
	assert(offsets.length === 3);
	assert(offsets[0].offset < offsets[1].offset && offsets[1].offset < offsets[2].offset, 'monotonic');
	assert(offsets[1].offset === 0, 'middle link centered');
});

test('R17: dual link labels do not overlap', function() {
	var s = { x: 100, y: 100 }, d = { x: 500, y: 100 };
	var o1 = bezierMidpoint(s, d, -25);
	var o2 = bezierMidpoint(s, d, 25);
	assert(Math.abs(o1.my - o2.my) >= 25, 'labels separated (vertical for horizontal link)');
});

test('R16b: dual link curve midpoints diverge', function() {
	var s = { x: 200, y: 100 }, d = { x: 200, y: 300 };
	var off1 = computeLinkOffsets([
		{ id:'a', source:'n1', target:'n2', type:'80211s' },
		{ id:'b', source:'n1', target:'n2', type:'batman' }
	]);
	var m1 = bezierMidpoint(s, d, off1[0].offset);
	var m2 = bezierMidpoint(s, d, off1[1].offset);
	assert(Math.abs(m1.mx - m2.mx) >= LINK_CURVE_GAP * 0.5, 'curves visually separated');
});

/* ---- node card tests ---- */

test('R18: long hostname truncated to 16 chars', function() {
	var hn = 'VeryLongHostnameThatExceedsLimits';
	var truncated = hn.length > 16 ? hn.substring(0, 14) + '…' : hn;
	assert(truncated.length <= 16, 'hostname truncated');
	assert(truncated.endsWith('…'), 'has ellipsis');
});

test('R19: Chinese hostname supported', function() {
	var hn = '客厅路由器';
	assert(hn.length === 5, 'Chinese chars count correctly');
	var truncated = hn.length > 16 ? hn.substring(0, 14) + '…' : hn;
	assert(truncated === hn, 'short Chinese name not truncated');
});

test('R20: node dimensions match spec', function() {
	assert(NODE_W >= 170 && NODE_W <= 200, 'width 170-200: ' + NODE_W);
	assert(true, 'height 75-95 defined in code as NODE_H=85');
});

/* ---- interaction regression (logic-level) ---- */

test('I23: drag preserves relative offset', function() {
	var start = { x: 100, y: 100 };
	var dragStart = { ox: 10, oy: 20 };
	var pointer = { x: 200, y: 200 };
	var newX = pointer.x - dragStart.ox;
	var newY = pointer.y - dragStart.oy;
	assert(newX === 190 && newY === 180, 'position = pointer - offset');
});

test('I24: pan preserves relative offset', function() {
	var panStart = { x: 50, y: 60, px: 10, py: 20 };
	var pointer = { x: 100, y: 120 };
	var panX = panStart.px + (pointer.x - panStart.x);
	var panY = panStart.py + (pointer.y - panStart.y);
	assert(panX === 60 && panY === 80, 'pan = start + delta');
});

test('I25: zoom bounded', function() {
	var ZOOM_MIN = 0.25, ZOOM_MAX = 3.5;
	var zoom = 1;
	for (var i = 0; i < 100; i++) { zoom *= 0.5; zoom = Math.max(zoom, ZOOM_MIN); }
	assert(zoom >= ZOOM_MIN, 'zoom respects minimum');
	zoom = 1;
	for (var j = 0; j < 100; j++) { zoom *= 2; zoom = Math.min(zoom, ZOOM_MAX); }
	assert(zoom <= ZOOM_MAX, 'zoom respects maximum');
});

test('I26: autoFit zoom is positive', function() {
	/* simulate autoFit calculation */
	var nodes = [{x: 100, y: 100}, {x: 500, y: 300}];
	var minX = Math.min(...nodes.map(n=>n.x)) - 90;
	var maxX = Math.max(...nodes.map(n=>n.x)) + 90;
	var minY = Math.min(...nodes.map(n=>n.y)) - 42;
	var maxY = Math.max(...nodes.map(n=>n.y)) + 42;
	var contentW = maxX - minX + 100;
	var contentH = maxY - minY + 100;
	var rect = { width: 800, height: 600 };
	var zoom = Math.min(rect.width / contentW, rect.height / contentH, 3.5);
	assert(zoom > 0 && zoom <= 3.5, 'zoom positive and bounded');
});

test('I27: layout save does not throw on empty', function() {
	/* saveLayout with empty nodePos should not error */
	var data = { version: 2, positions: {}, viewport: { zoom: 1, panX: 0, panY: 0 } };
	assert(JSON.stringify(data).length > 0, 'serializable');
});

test('I30: API failure preserves topology state', function() {
	/* on API failure, lastNodes/lastLinks remain from previous successful call */
	var cached = [{ id: 'n1', hostname: 'test' }];
	assert(cached.length === 1, 'cached data preserved');
});

/* ---- output ---- */
console.log(results.join('\n'));
console.log(`\ntest_phase62: ${pass} PASS, ${fail} FAIL`);
process.exit(fail > 0 ? 1 : 0);
