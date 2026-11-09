// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * OpenMesh Manager interactive topology canvas.
 *
 * Phase 6.2: node cards, dual-link Bezier curves, BFS smart layout,
 *   layout persistence, Pointer Events, client fan rendering.
 * Phase 6.3 (topology status, m20): four-state node model
 *   (online/offline/degraded/unknown, session-layer truth only),
 *   protocol-specific link quality grading (missing != poor),
 *   collection-health decoupling (cached topology + stale banner,
 *   never recolors nodes), transition-only event ticker, bounded
 *   short-term link trend with visible gaps (no fabricated history).
 */
'use strict';
'require view';
'require rpc';
'require poll';

var callTopology = rpc.declare({ object: 'openmesh', method: 'topology' });
var callClients = rpc.declare({ object: 'openmesh', method: 'get_clients' });

/* ---- design tokens ---- */
var ZOOM_MIN = 0.25, ZOOM_MAX = 3.5;
var NODE_W = 180, NODE_H = 85;
var LINK_CURVE_GAP = 50;     /* perpendicular offset between parallel links */
var POLL_INTERVAL = 5;
var LAYOUT_VERSION = 2;
var STORAGE_KEY_PREFIX = 'openmesh-topo-layout-';
var LEVEL_GAP = 160;          /* vertical distance between BFS levels */
var SIBLING_GAP = 220;        /* horizontal distance between same-level nodes */
var FIT_PADDING = 50;
var CLIENT_R = 16;           /* client node radius */
/* fan geometry: adjacent chord = 2*FAN_R*sin(PI/(2*(MAX_SHOW+1))) must
 * exceed 2*CLIENT_R, else circles overlap: 100/8 → 34.7px > 32px */
var CLIENT_FAN_R = 100;      /* distance from AP anchor to client fan */
var CLIENT_MAX_SHOW = 8;     /* max clients to render per AP (overflow: summary) */

function esc(s) {
	return String(s || '').replace(/&/g, '&amp;')
		.replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;')
		.replace(/'/g, '&#39;');
}
function cssEsc(s) {
	return String(s || '').replace(/"/g, '\\"').replace(/]/g, '\\]');
}
function shortMac(m) { return m && m.length > 17 ? m.substring(0, 8) + '…' : (m || '-'); }
function shortId(id) { return id ? id.substring(0, 8) + '…' : '-'; }
function fmtAgo(ts) { return ts ? new Date(ts * 1000).toLocaleTimeString() : '-'; }

/* ==================== Phase 6.3 status model (pure, host-tested) ============
 * Rules (m20): collection health NEVER changes node online/offline state --
 * online/offline comes from the session layer only. degraded needs sustained
 * evidence (>= DEGRADE_STREAK polls with every read 802.11s link poor);
 * a single RSSI dip or a missing metric is never degraded. Missing metric
 * (rssi/tq absent) is 'nodata', distinct from known-poor.
 */
var STATUS_VISUAL = {
	online:   { color: '#4CAF50' },
	offline:  { color: '#9E9E9E' },
	degraded: { color: '#FF9800' },
	unknown:  { color: '#9E9E9E' }
};
var DEGRADE_STREAK = 2;		/* consecutive polls, ~10s sustained */
var RSSI_FAIR = -60, RSSI_POOR = -75;
var TQ_FAIR = 200, TQ_POOR = 100;
var HIST_CAP = 36;		/* samples per link (~3min at 5s poll) */
var HIST_GAP_MS = 12500;	/* >2 missed polls splits the trend line */
var HIST_TTL_MS = 600000;	/* drop trend data for links unseen 10min */

function nodeVisualStatus(n, streak) {
	if (!n) return 'unknown';
	if (n.status === 'online')
		return (streak || 0) >= DEGRADE_STREAK ? 'degraded' : 'online';
	if (n.status === 'offline') return 'offline';
	return 'unknown';
}

/* degraded evidence: online node whose every 802.11s link WITH a reading
 * is poor. A single decent link clears it; links without rssi are not
 * evidence (missing != poor). Streak accumulation is the caller's job. */
function nodeDegradeBad(n, links) {
	if (!n || n.status !== 'online' || !links) return false;
	var anyPoor = false;
	for (var i = 0; i < links.length; i++) {
		var l = links[i];
		if (l.type !== '80211s') continue;
		if (l.source !== n.id && l.target !== n.id) continue;
		if (!l.rssi) continue;
		if (l.rssi > RSSI_POOR) return false;
		anyPoor = true;
	}
	return anyPoor;
}

/* protocol-specific quality grading; TQ and RSSI never share a scale */
function linkQualityClass(l) {
	if (!l) return 'nodata';
	if (l.type === '80211s') {
		if (!l.rssi) return 'nodata';
		return l.rssi >= RSSI_FAIR ? 'good'
			: l.rssi >= RSSI_POOR ? 'fair' : 'poor';
	}
	if (l.type === 'batman') {
		if (!l.tq) return 'nodata';
		return l.tq >= TQ_FAIR ? 'good'
			: l.tq >= TQ_POOR ? 'fair' : 'poor';
	}
	if (l.type === 'ethernet') return 'active';	/* presence only */
	return 'nodata';
}

function qualityColor(cls) {
	return { good: '#4CAF50', fair: '#FF9800', poor: '#F44336',
		 active: '#5E5E5E', nodata: '#9E9E9E' }[cls] || '#BDBDBD';
}

/* semantic collection state for the banner; rpc outranks daemon health */
function collectState(health, rpcFailed) {
	if (rpcFailed) return 'rpc';
	if (health === 'failed') return 'failed';
	if (health === 'stale') return 'stale';
	return 'ok';
}

/* real transitions only: nothing is emitted while a state persists */
function diffTopologyEvents(prev, cur) {
	var ev = [], k, was, now;
	var pn = (prev && prev.nodes) || {}, cn = (cur && cur.nodes) || {};
	var pl = (prev && prev.links) || {}, cl = (cur && cur.links) || {};
	for (k in cn) {
		was = pn[k]; now = cn[k];
		if (was === undefined) {
			if (now === 'online') ev.push({ kind: 'node_online', key: k });
			else if (now === 'offline') ev.push({ kind: 'node_offline', key: k });
		} else if (was !== now) {
			if (was === 'online' && now === 'offline')
				ev.push({ kind: 'node_offline', key: k });
			else if (was === 'offline' && now === 'online')
				ev.push({ kind: 'node_online', key: k });
		}
	}
	for (k in pn)
		if (cn[k] === undefined) ev.push({ kind: 'node_offline', key: k });
	for (k in cl)
		if (pl[k] === undefined) ev.push({ kind: 'link_up', key: k });
	for (k in pl)
		if (cl[k] === undefined) ev.push({ kind: 'link_down', key: k });
	return ev;
}

/* bounded ring buffer sample for the short-term trend */
function histPush(h, ts, val, cap) {
	h = h || { t: [], v: [] };
	h.t.push(ts); h.v.push(val);
	if (h.t.length > cap) { h.t.shift(); h.v.shift(); }
	return h;
}

/* split into continuous segments at sampling gaps (missed polls) */
function histSegments(h, gapMs) {
	var segs = [], cur = null, i;

	if (!h || !h.t.length) return segs;
	for (i = 0; i < h.t.length; i++) {
		if (!cur || (i > 0 && h.t[i] - h.t[i - 1] > gapMs)) {
			cur = [];
			segs.push(cur);
		}
		cur.push([h.t[i], h.v[i]]);
	}
	return segs;
}

function fmtAgeSecs(secs) {
	if (!(secs >= 0)) secs = 0;
	if (secs < 60) return { n: Math.round(secs), unit: 's' };
	if (secs < 3600) return { n: Math.floor(secs / 60), unit: 'm' };
	return { n: Math.floor(secs / 3600), unit: 'h' };
}

function fmtDur(s) {
	s = Math.max(0, Math.floor(s || 0));
	var d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600),
	    m = Math.floor((s % 3600) / 60);
	if (d > 0) return d + 'd ' + h + 'h';
	if (h > 0) return h + 'h ' + m + 'm';
	return m + 'm';
}

/* ---- link type visual config ---- */
var LINK_STYLES = {
	'ethernet': { stroke: '#5E5E5E', dash: 'none',   width: 3, label: 'Ethernet' },
	'80211s':   { stroke: '#2196F3', dash: '8,5',   width: 2.5, label: 'RSSI' },
	'batman':   { stroke: '#9C27B0', dash: '3,4',   width: 2.5, label: 'TQ' },
	'unknown':  { stroke: '#BDBDBD', dash: '2,4',   width: 1.5, label: '' }
};

return view.extend({
	nodePos: {}, zoom: 1, pan: { x: 0, y: 0 },
	selectedNode: null, selectedLink: null,
	dragMode: null, dragTarget: null, dragStart: null,
	svgRoot: null, svgGroup: null,
	canvasW: 800, canvasH: 560,
	lastNodes: null, lastLinks: null, lastData: null,
	lastClients: null,
	selectedClient: null,
	_pointers: {}, _pinchDist: 0,
	_layoutScope: '', _saveTimer: null,
	/* Phase 6.3 state */
	_prevState: null, _degradeStreak: {}, _events: [],
	_linkHist: {}, _rpcFailed: false, _lastHealth: 'ok',

	load: function() { return L.resolveDefault(callTopology(), {}); },

	/* ==================== layout persistence (6.1, version 2) ==================== */

	_layoutKey: function() {
		return STORAGE_KEY_PREFIX + (this._layoutScope || 'default');
	},

	saveLayout: function() {
		try {
			localStorage.setItem(this._layoutKey(), JSON.stringify({
				version: LAYOUT_VERSION, positions: this.nodePos,
				viewport: { zoom: this.zoom, panX: this.pan.x, panY: this.pan.y }
			}));
		} catch (e) {}
	},

	saveLayoutDebounced: function() {
		var self = this;
		if (this._saveTimer) clearTimeout(this._saveTimer);
		this._saveTimer = setTimeout(function() { self.saveLayout(); self._saveTimer = null; }, 300);
	},

	loadLayout: function() {
		try {
			var raw = localStorage.getItem(this._layoutKey());
			if (!raw) return false;
			var data = JSON.parse(raw);
			if (!data || data.version !== LAYOUT_VERSION) return false;
			if (!data.positions || typeof data.positions !== 'object') return false;
			var restored = false, self = this;
			Object.keys(data.positions).forEach(function(id) {
				var p = data.positions[id];
				if (p && typeof p.x === 'number' && typeof p.y === 'number' &&
				    isFinite(p.x) && isFinite(p.y) &&
				    p.x > -10000 && p.x < 10000 && p.y > -10000 && p.y < 10000) {
					self.nodePos[id] = { x: p.x, y: p.y };
					restored = true;
				}
			});
			if (data.viewport) {
				var v = data.viewport;
				if (typeof v.zoom === 'number' && isFinite(v.zoom) && v.zoom >= ZOOM_MIN && v.zoom <= ZOOM_MAX)
					this.zoom = v.zoom;
				if (typeof v.panX === 'number' && isFinite(v.panX)) this.pan.x = v.panX;
				if (typeof v.panY === 'number' && isFinite(v.panY)) this.pan.y = v.panY;
			}
			return restored;
		} catch (e) { return false; }
	},

	resetLayout: function() {
		try { localStorage.removeItem(this._layoutKey()); } catch (e) {}
		var self = this;
		Object.keys(this.nodePos).forEach(function(k) { delete self.nodePos[k]; });
		this.zoom = 1; this.pan = { x: 0, y: 0 };
		if (this.lastNodes) { this.smartLayout(this.lastNodes, this.lastLinks); this.autoFit(); }
		this.saveLayout();
	},

	/* ==================== smart layout (P1: BFS hierarchical) ==================== */

	smartLayout: function(nodes, links) {
		var self = this;
		var byId = {};
		nodes.forEach(function(n) { byId[n.id] = n; });

		/* Build adjacency from physical links only (ethernet + 802.11s);
		 * batman is logical reachability, not a tree edge */
		var adj = {};
		nodes.forEach(function(n) { adj[n.id] = []; });
		(links || []).forEach(function(l) {
			if (l.type === 'ethernet' || l.type === '80211s') {
				if (adj[l.source]) adj[l.source].push(l.target);
				if (adj[l.target]) adj[l.target].push(l.source);
			}
		});

		/* Find controller (BFS root) */
		var root = null;
		nodes.forEach(function(n) { if (n.role === 'controller') root = n.id; });
		if (!root && nodes.length > 0) root = nodes[0].id;

		/* BFS assign levels */
		var level = {}, visited = {};
		var queue = [root];
		level[root] = 0; visited[root] = true;
		var maxLevel = 0;
		while (queue.length > 0) {
			var cur = queue.shift();
			(adj[cur] || []).forEach(function(next) {
				if (!visited[next]) {
					visited[next] = true;
					level[next] = level[cur] + 1;
					if (level[next] > maxLevel) maxLevel = level[next];
					queue.push(next);
				}
			});
		}

		/* Isolated nodes (not reachable via physical links) go to maxLevel+1 */
		var isolated = [];
		nodes.forEach(function(n) {
			if (!visited[n.id]) { isolated.push(n.id); level[n.id] = maxLevel + 1; }
		});

		/* Group by level, stable sort by id for determinism */
		var byLevel = {};
		nodes.forEach(function(n) {
			var lv = level[n.id] || 0;
			if (!byLevel[lv]) byLevel[lv] = [];
			byLevel[lv].push(n);
		});
		Object.keys(byLevel).forEach(function(lv) {
			byLevel[lv].sort(function(a, b) { return a.id < b.id ? -1 : 1; });
		});

		/* Position: level = y, index = x (centered) */
		var cx = self.canvasW / 2;
		var levelKeys = Object.keys(byLevel).map(Number).sort(function(a, b) { return a - b; });
		levelKeys.forEach(function(lv) {
			var siblings = byLevel[lv];
			var totalW = (siblings.length - 1) * SIBLING_GAP;
			siblings.forEach(function(n, i) {
				self.nodePos[n.id] = {
					x: cx - totalW / 2 + i * SIBLING_GAP,
					y: 80 + lv * LEVEL_GAP
				};
			});
		});

		this.renderAll();
		this.saveLayoutDebounced();
	},

	/* Place only new nodes without disturbing existing positions */
	autoLayoutNew: function(newNodes, allNodes, links) {
		if (newNodes.length === 0) return;
		/* Simply run smartLayout for new nodes at their BFS level position,
		 * offset from existing nodes at same level */
		var self = this;
		newNodes.forEach(function(n) {
			/* find a spot below existing nodes */
			var maxY = 0;
			allNodes.forEach(function(m) {
				if (self.nodePos[m.id] && self.nodePos[m.id].y > maxY)
					maxY = self.nodePos[m.id].y;
			});
			self.nodePos[n.id] = {
				x: self.canvasW / 2 + (newNodes.indexOf(n) - newNodes.length / 2 + 0.5) * SIBLING_GAP,
				y: maxY + LEVEL_GAP
			};
		});
		this.saveLayoutDebounced();
	},

	/* Auto-fit: adjust zoom/pan so all nodes visible with padding */
	autoFit: function() {
		if (!this.svgRoot || !this.lastNodes) return;
		var self = this;
		var minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
		this.lastNodes.forEach(function(n) {
			var p = self.nodePos[n.id];
			if (!p) return;
			if (p.x - NODE_W/2 < minX) minX = p.x - NODE_W/2;
			if (p.y - NODE_H/2 < minY) minY = p.y - NODE_H/2;
			if (p.x + NODE_W/2 > maxX) maxX = p.x + NODE_W/2;
			if (p.y + NODE_H/2 > maxY) maxY = p.y + NODE_H/2;
		});
		if (minX === Infinity) return;

		var rect = this.svgRoot.getBoundingClientRect();
		var contentW = maxX - minX + FIT_PADDING * 2;
		var contentH = maxY - minY + FIT_PADDING * 2;
		var zoomX = rect.width / contentW;
		var zoomY = rect.height / contentH;
		var zoom = Math.min(zoomX, zoomY, ZOOM_MAX);
		zoom = Math.max(zoom, ZOOM_MIN);

		/* center content in viewport */
		var centerX = (minX + maxX) / 2;
		var centerY = (minY + maxY) / 2;
		this.zoom = zoom;
		this.pan.x = rect.width / 2 - centerX * zoom;
		this.pan.y = rect.height / 2 - centerY * zoom;
		this.applyTransform();
		this.saveLayoutDebounced();
	},

	fitScreen: function() { this.autoFit(); },

	applyTransform: function() {
		if (!this.svgGroup) return;
		this.svgGroup.setAttribute('transform',
			'translate(' + this.pan.x + ',' + this.pan.y + ') scale(' + this.zoom + ')');
		var zl = document.querySelector('.zoom-label');
		if (zl) zl.textContent = Math.round(this.zoom * 100) + '%';
	},

	clientToSvg: function(cx, cy) {
		var rect = this.svgRoot.getBoundingClientRect();
		return {
			x: (cx - rect.left - this.pan.x) / this.zoom,
			y: (cy - rect.top - this.pan.y) / this.zoom
		};
	},

	/* ==================== rendering (P0: modern cards + dual-link curves) ======== */

	clientColor: function(rssi) {
		if (!rssi || rssi >= -60) return '#4CAF50';
		if (rssi >= -75) return '#FF9800';
		return '#F44336';
	},

	/* compute client position: fan below the AP card, symmetric around it */
	clientPos: function(apPos, index, total) {
		var ang = Math.PI * (index + 1) / (total + 1);
		return {
			x: apPos.x + Math.cos(ang) * CLIENT_FAN_R,
			y: apPos.y + Math.sin(ang) * CLIENT_FAN_R + NODE_H / 2
		};
	},

	statusColor: function(n) {
		var sv = STATUS_VISUAL[nodeVisualStatus(n,
			this._degradeStreak[n && n.id])];
		return sv ? sv.color : STATUS_VISUAL.unknown.color;
	},

	/* event log: real transitions only, newest first, bounded */
	logEvent: function(kind, key) {
		this._events.unshift({ ts: Date.now(), kind: kind, key: key || '' });
		if (this._events.length > 50) this._events.length = 50;
		this.renderEventTicker();
	},

	nodeName: function(id) {
		if (!this.lastNodes) return shortId(id);
		for (var i = 0; i < this.lastNodes.length; i++)
			if (this.lastNodes[i].id === id)
				return this.lastNodes[i].hostname || shortId(id);
		return shortId(id);
	},

	eventText: function(e) {
		var map = {
			node_online: _('Node online'),
			node_offline: _('Node offline'),
			link_up: _('Link up'),
			link_down: _('Link down'),
			collect_fail: _('Data collection failed'),
			collect_recover: _('Data collection recovered'),
			rpc_down: _('RPC failed, showing cached data'),
			node_degraded: _('Weak signal'),
			node_recovered: _('Signal recovered')
		};
		var who = (e.kind.indexOf('link_') === 0) ?
			this.linkName(e.key) : this.nodeName(e.key);
		return new Date(e.ts).toLocaleTimeString() + ' ' +
			(map[e.kind] || e.kind) + (who ? ' · ' + who : '');
	},

	linkName: function(id) {
		if (!this.lastLinks) return shortId(id);
		for (var i = 0; i < this.lastLinks.length; i++)
			if (this.lastLinks[i].id === id)
				return this.nodeName(this.lastLinks[i].source) + '↔' +
					this.nodeName(this.lastLinks[i].target);
		return shortId(id);
	},

	snapshotState: function(data) {
		var st = { nodes: {}, links: {} }, i;
		for (i = 0; i < data.nodes.length; i++)
			st.nodes[data.nodes[i].id] = data.nodes[i].status || 'unknown';
		for (i = 0; i < (data.links || []).length; i++)
			st.links[data.links[i].id] = true;
		return st;
	},

	updateDegradeStreaks: function(data) {
		var self = this, i, id, n, vis, was;

		for (i = 0; i < data.nodes.length; i++) {
			n = data.nodes[i]; id = n.id;
			was = nodeVisualStatus(n, self._degradeStreak[id]);
			if (nodeDegradeBad(n, data.links))
				self._degradeStreak[id] = (self._degradeStreak[id] || 0) + 1;
			else
				delete self._degradeStreak[id];
			vis = nodeVisualStatus(n, self._degradeStreak[id]);
			if (vis !== was && was !== 'unknown') {
				if (vis === 'degraded') self.logEvent('node_degraded', id);
				else if (was === 'degraded') self.logEvent('node_recovered', id);
			}
		}
	},

	collectHealthEvent: function(data) {
		var h = (data && data.batman_collect) || 'ok';

		if (h !== this._lastHealth) {
			if (h === 'failed') this.logEvent('collect_fail');
			else if (this._lastHealth === 'failed') this.logEvent('collect_recover');
			this._lastHealth = h;
		}
	},

	/* short-term per-link sampling (bounded; gaps visible, not fabricated) */
	pushLinkHist: function(data) {
		var now = Date.now(), seen = {}, self = this;

		(data.links || []).forEach(function(l) {
			var val = (l.type === '80211s' && l.rssi) ? l.rssi :
				(l.type === 'batman' && l.tq) ? l.tq : null;
			if (val === null) return;
			self._linkHist[l.id] = histPush(self._linkHist[l.id], now,
				val, HIST_CAP);
			seen[l.id] = true;
		});
		/* prune trend data for links that vanished long ago */
		Object.keys(this._linkHist).forEach(function(id) {
			var h = self._linkHist[id];
			if (!seen[id] && h.t.length &&
			    now - h.t[h.t.length - 1] > HIST_TTL_MS)
				delete self._linkHist[id];
		});
	},

	renderAll: function() {
		if (!this.svgGroup || !this.lastNodes) return;
		this.renderLinks();
		this.renderNodes();
		this.renderClients();
	},

	/* SVG helper: create element with attrs */
	_svg: function(tag, attrs, parent) {
		var el = document.createElementNS('http://www.w3.org/2000/svg', tag);
		if (attrs) Object.keys(attrs).forEach(function(k) {
			el.setAttribute(k, attrs[k]);
		});
		if (parent) parent.appendChild(el);
		return el;
	},

	/* SVG helper: create text with textContent (injection-safe) */
	_text: function(attrs, content, parent) {
		var el = this._svg('text', attrs, parent);
		el.textContent = content;
		return el;
	},

	renderNodes: function() {
		var self = this;
		var g = this.svgGroup.querySelector('.node-layer');
		if (!g) return;
		while (g.firstChild) g.removeChild(g.firstChild);

		this.lastNodes.forEach(function(n) {
			var p = self.nodePos[n.id];
			if (!p) p = self.nodePos[n.id] = { x: self.canvasW/2, y: self.canvasH/2 };
			var sel = self.selectedNode === n.id;
			var visual = nodeVisualStatus(n, self._degradeStreak[n.id]);
			var sc = STATUS_VISUAL[visual].color;
			var now = Math.floor(Date.now() / 1000);

			var grp = self._svg('g', {
				'class': 'topo-node', 'data-id': n.id,
				'transform': 'translate(' + p.x + ',' + p.y + ')'
			}, g);
			grp.style.cursor = 'pointer';
			grp.style.touchAction = 'none';
			if (visual === 'offline' || visual === 'unknown')
				grp.style.opacity = '0.6';

			/* card shadow (subtle) */
			self._svg('rect', {
				x: -NODE_W/2 + 2, y: -NODE_H/2 + 3,
				width: NODE_W, height: NODE_H, rx: 12,
				fill: 'rgba(0,0,0,0.08)'
			}, grp);

			/* card body */
			self._svg('rect', {
				x: -NODE_W/2, y: -NODE_H/2,
				width: NODE_W, height: NODE_H, rx: 12,
				fill: '#FFFFFF',
				stroke: sel ? '#FF9800' : '#E0E0E0',
				'stroke-width': sel ? 3 : 1.5
			}, grp);

			/* status dot (top-left); unknown renders a question mark */
			self._svg('circle', {
				cx: -NODE_W/2 + 18, cy: -NODE_H/2 + 18, r: 7,
				fill: sc, stroke: '#FFFFFF', 'stroke-width': 2
			}, grp);
			if (visual === 'unknown')
				self._text({
					x: -NODE_W/2 + 18, y: -NODE_H/2 + 21.5,
					'font-size': 9, 'font-weight': 'bold', fill: '#FFFFFF',
					'text-anchor': 'middle'
				}, '?', grp);

			/* hostname (bold, right of dot) */
			var hn = n.hostname || '';
			if (hn.length > 16) hn = hn.substring(0, 14) + '…';
			self._text({
				x: -NODE_W/2 + 32, y: -NODE_H/2 + 22,
				'font-size': 14, 'font-weight': 'bold', fill: '#333'
			}, hn, grp);

			/* role badge + status word; offline keeps its data age */
			var roleTxt = n.role === 'controller' ? _('Controller') : _('Agent');
			if (visual === 'offline' && n.last_seen) {
				var age = fmtAgeSecs(now - n.last_seen);
				roleTxt += ' · ' + _('offline') + ' ' + age.n +
					(age.unit === 's' ? _('s ago') :
					 age.unit === 'm' ? _('min ago') : _('h ago'));
			} else if (visual === 'degraded')
				roleTxt += ' · ' + _('Weak signal');
			else if (visual === 'unknown')
				roleTxt += ' · ' + _('Unknown');
			self._text({
				x: -NODE_W/2 + 32, y: -NODE_H/2 + 40,
				'font-size': 11, fill: '#888'
			}, roleTxt, grp);

			/* separator line */
			self._svg('line', {
				x1: -NODE_W/2 + 12, y1: -NODE_H/2 + 50,
				x2: NODE_W/2 - 12, y2: -NODE_H/2 + 50,
				stroke: '#F0F0F0', 'stroke-width': 1
			}, grp);

			/* IP address (bottom, monospace style) */
			self._text({
				x: 0, y: NODE_H/2 - 14,
				'font-size': 12, fill: '#555',
				'text-anchor': 'middle', 'font-family': 'monospace'
			}, n.ip || shortId(n.id), grp);

			/* client count (if > 0) */
			if (n.client_count && n.client_count > 0) {
				self._text({
					x: NODE_W/2 - 14, y: -NODE_H/2 + 18,
					'font-size': 10, fill: '#999', 'text-anchor': 'end'
				}, n.client_count + ' CLI', grp);
			}
		});
	},

	/* ---- render client devices attached to APs ---- */
	renderClients: function() {
		var self = this;
		var g = this.svgGroup.querySelector('.client-layer');
		if (!g) return;
		while (g.firstChild) g.removeChild(g.firstChild);
		if (!this.lastClients || !this.lastNodes) return;

		/* M16 get_clients reports the controller's local AP only:
		 * all clients fan out below the controller card for now */
		var ctrl = null;
		this.lastNodes.forEach(function(n) { if (n.role === 'controller') ctrl = n; });

		var clients = this.lastClients.clients || [];
		clients.slice(0, CLIENT_MAX_SHOW).forEach(function(c, idx) {
			/* client attaches to controller by default (local AP clients) */
			var ap = ctrl;
			var apPos = ap ? self.nodePos[ap.id] : null;
			if (!apPos) return;
			var total = Math.min(clients.length, CLIENT_MAX_SHOW);
			var pos = self.clientPos(apPos, idx, total);
			var col = self.clientColor(c.rssi);
			var sel = self.selectedClient === c.mac;

			/* thin line from AP to client */
			self._svg('line', {
				x1: apPos.x, y1: apPos.y + NODE_H/2,
				x2: pos.x, y2: pos.y,
				stroke: '#E0E0E0', 'stroke-width': 1.5,
				opacity: 0.6
			}, g);

			/* client circle */
			var grp = self._svg('g', {
				'class': 'topo-client', 'data-id': c.mac,
				'transform': 'translate(' + pos.x + ',' + pos.y + ')'
			}, g);
			grp.style.cursor = 'pointer';
			grp.style.touchAction = 'none';

			/* outer ring (selection highlight) */
			if (sel) self._svg('circle', { cx: 0, cy: 0, r: CLIENT_R + 3,
				fill: 'none', stroke: '#FF9800', 'stroke-width': 2 }, grp);

			/* device body */
			self._svg('circle', {
				cx: 0, cy: 0, r: CLIENT_R,
				fill: '#FFFFFF', stroke: col, 'stroke-width': 2.5
			}, grp);

			/* RSSI dot inside */
			self._svg('circle', { cx: 0, cy: -4, r: 4, fill: col }, grp);

			/* short MAC label */
			var mac = c.mac || '';
			var shortM = mac.length > 11 ? mac.substring(0, 8) + '…' : mac;
			self._text({
				x: 0, y: CLIENT_R + 12, 'font-size': 9, fill: '#999',
				'text-anchor': 'middle'
			}, shortM, grp);
		});

		/* overflow indicator */
		if (clients.length > CLIENT_MAX_SHOW) {
			var apPos = ctrl ? self.nodePos[ctrl.id] : null;
			if (apPos) {
				self._text({
					x: apPos.x, y: apPos.y + NODE_H/2 + CLIENT_FAN_R + 25,
					'font-size': 10, fill: '#999', 'text-anchor': 'middle'
				}, '+' + (clients.length - CLIENT_MAX_SHOW) + ' ' + _('more clients'), g);
			}
		}
	},

	/* Group links by node pair, render with perpendicular offsets */
	renderLinks: function() {
		var self = this;
		var g = this.svgGroup.querySelector('.link-layer');
		if (!g) return;
		while (g.firstChild) g.removeChild(g.firstChild);

		/* group by unordered node pair */
		var pairs = {};
		(this.lastLinks || []).forEach(function(l) {
			var key = l.source < l.target ? l.source + '|' + l.target : l.target + '|' + l.source;
			if (!pairs[key]) pairs[key] = [];
			pairs[key].push(l);
		});

		Object.keys(pairs).forEach(function(key) {
			var links = pairs[key];
			links.sort(function(a, b) { return a.type < b.type ? -1 : 1; }); /* stable */
			var total = links.length;
			var mid = (total - 1) / 2;

			links.forEach(function(l, idx) {
				var s = self.nodePos[l.source];
				var d = self.nodePos[l.target];
				if (!s || !d) return;
				var st = LINK_STYLES[l.type] || LINK_STYLES['unknown'];
				var sel = self.selectedLink === l.id;
				var offset = (idx - mid) * LINK_CURVE_GAP;

				/* compute perpendicular offset for curve */
				var dx = d.x - s.x, dy = d.y - s.y;
				var len = Math.sqrt(dx*dx + dy*dy) || 1;
				var px = -dy / len * offset;  /* perpendicular x */
				var py = dx / len * offset;   /* perpendicular y */

				/* control point for quadratic Bezier */
				var cpx = (s.x + d.x) / 2 + px;
				var cpy = (s.y + d.y) / 2 + py;

				grp = self._svg('g', {
					'class': 'topo-link', 'data-id': l.id
				}, g);
				grp.style.cursor = 'pointer';

				/* visible curve */
				self._svg('path', {
					d: 'M ' + s.x + ' ' + s.y + ' Q ' + cpx + ' ' + cpy + ' ' + d.x + ' ' + d.y,
					stroke: st.stroke,
					'stroke-width': sel ? st.width + 1.5 : st.width,
					'stroke-dasharray': st.dash,
					fill: 'none',
					opacity: sel ? 1.0 : 0.75
				}, grp);

				/* invisible hit area (wider) */
				self._svg('path', {
					d: 'M ' + s.x + ' ' + s.y + ' Q ' + cpx + ' ' + cpy + ' ' + d.x + ' ' + d.y,
					stroke: 'transparent', 'stroke-width': 24, fill: 'none'
				}, grp);

				/* label at curve midpoint: protocol-specific metric,
				 * quality-colored; missing metric is '--', never faked */
				var mx = (s.x + d.x) / 2 + px * 0.55;
				var my = (s.y + d.y) / 2 + py * 0.55;
				var qcls = linkQualityClass(l);
				var qcol = qualityColor(qcls);
				var labelText = '';
				if (l.type === '80211s')
					labelText = l.rssi ? l.rssi + ' dBm' : '-- dBm';
				else if (l.type === 'batman') {
					labelText = l.tq ? 'TQ ' + l.tq : 'TQ --';
					/* batman data may lag when collection degrades */
					if (self._lastHealth !== 'ok')
						labelText += ' ·' + _('Stale');
				} else if (l.type === 'ethernet')
					labelText = _('Ethernet');

				if (labelText) {
					/* label background plate */
					var tw = labelText.length * 7 + 12;
					self._svg('rect', {
						x: mx - tw/2, y: my - 10,
						width: tw, height: 18, rx: 4,
						fill: 'rgba(255,255,255,0.92)',
						stroke: qcol, 'stroke-width': 0.5
					}, grp);
					self._text({
						x: mx, y: my + 3,
						'font-size': 11, fill: qcol,
						'font-weight': sel ? 'bold' : 'normal',
						'text-anchor': 'middle'
					}, labelText, grp);
				}
			});
		});
	},

	/* ==================== detail panel (P1: grouped, DOM-safe) ==================== */

	/* tiny SVG sparkline for the bounded short-term trend; gaps between
	 * segments are visible breaks, missing polls are never interpolated */
	drawSpark: function(parent, hist, type) {
		if (!hist || hist.t.length < 2) return;
		var segs = histSegments(hist, HIST_GAP_MS);
		if (!segs.length) return;
		var svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
		svg.setAttribute('width', '100%');
		svg.setAttribute('height', '40');
		svg.setAttribute('viewBox', '0 0 220 40');
		svg.style.cssText = 'display:block;margin-top:4px';
		parent.appendChild(svg);

		var t0 = hist.t[0], t1 = hist.t[hist.t.length - 1] || t0 + 1;
		var span = Math.max(t1 - t0, 1);
		var lo = type === '80211s' ? -90 : 0;
		var hi = type === '80211s' ? -40 : 255;
		var self = this;
		segs.forEach(function(seg) {
			var pts = seg.map(function(s) {
				var x = 6 + (s[0] - t0) / span * 208;
				var y = 34 - (s[1] - lo) / (hi - lo) * 28;
				return x.toFixed(1) + ',' + y.toFixed(1);
			}).join(' ');
			self._svg('polyline', {
				points: pts, fill: 'none',
				stroke: type === '80211s' ? '#2196F3' : '#9C27B0',
				'stroke-width': 1.5
			}, svg);
		});
		var cap = document.createElement('div');
		cap.style.cssText = 'font-size:10px;color:#aaa;margin-top:2px';
		cap.textContent = _('Short-term sampling (~3 min), not long-term history') +
			' · ' + hist.t.length;
		parent.appendChild(cap);
	},

	renderDetail: function() {
		var el = document.querySelector('.topo-detail-body');
		if (!el) return;
		while (el.firstChild) el.removeChild(el.firstChild);

		var title = document.createElement('div');
		title.style.cssText = 'font-weight:bold;font-size:14px;margin-bottom:8px';

		/* selections that vanished from the latest snapshot are dropped
		 * with an explicit notice -- no stale "live" data is kept */
		var missing = false, n = null, c = null, l = null, i;

		if (this.selectedNode) {
			if (this.lastNodes)
				for (i = 0; i < this.lastNodes.length; i++)
					if (this.lastNodes[i].id === this.selectedNode) {
						n = this.lastNodes[i];
						break;
					}
			if (!n) { this.selectedNode = null; missing = true; }
		} else if (this.selectedClient && this.lastClients) {
			var cs = this.lastClients.clients || [];
			for (i = 0; i < cs.length; i++)
				if (cs[i].mac === this.selectedClient) { c = cs[i]; break; }
			if (!c) { this.selectedClient = null; missing = true; }
		} else if (this.selectedLink) {
			if (this.lastLinks)
				for (i = 0; i < this.lastLinks.length; i++)
					if (this.lastLinks[i].id === this.selectedLink) {
						l = this.lastLinks[i];
						break;
					}
			if (!l) { this.selectedLink = null; missing = true; }
		}

		if (n) {
			var visual = nodeVisualStatus(n, this._degradeStreak[n.id]);
			var now = Math.floor(Date.now() / 1000);
			title.textContent = n.hostname;
			el.appendChild(title);
			this._section(el, _('Device'));
			this._row(el, _('Role'), n.role);
			this._row(el, _('Status'), visual,
				STATUS_VISUAL[visual].color);
			this._row(el, _('IP'), n.ip || '-');
			this._row(el, _('MAC'), shortMac(n.mac));
			this._section(el, _('Info'));
			this._row(el, _('Model'), n.model || '-');
			this._row(el, _('Last Seen'), fmtAgo(n.last_seen));
			if (n.last_seen && n.status !== 'online') {
				var age = fmtAgeSecs(now - n.last_seen);
				this._row(el, _('Data Age'), age.n + (age.unit === 's' ?
					_('s ago') : age.unit === 'm' ? _('min ago') : _('h ago')));
			}
			this._row(el, _('Uptime'), n.uptime ? fmtDur(n.uptime) : '-');
			this._row(el, _('Clients'), String(n.client_count || 0));
		} else if (c) {
			title.textContent = c.mac || _('Client');
			el.appendChild(title);
			this._section(el, _('Device'));
			this._row(el, _('MAC'), c.mac || '-');
			this._row(el, _('SSID'), c.ssid || '-');
			this._row(el, _('Interface'), c.ifname || '-');
			this._section(el, _('Quality'));
			this._row(el, _('RSSI'), c.rssi ? c.rssi + ' dBm' : '-',
				this.clientColor(c.rssi));
			if (c.rx_kbps) this._row(el, _('RX Rate'), Math.round(c.rx_kbps / 1000) + ' Mbps');
			if (c.tx_kbps) this._row(el, _('TX Rate'), Math.round(c.tx_kbps / 1000) + ' Mbps');
		} else if (l && this.lastNodes) {
			var self = this;
			var nm = function(id) {
				for (var k = 0; k < self.lastNodes.length; k++)
					if (self.lastNodes[k].id === id) return self.lastNodes[k].hostname;
				return shortId(id);
			};
			var qcls = linkQualityClass(l);
			var qword = qcls === 'nodata' ? _('No data') :
				qcls === 'active' ? _('Active') : qcls;
			title.textContent = _('Link Details');
			el.appendChild(title);
			this._section(el, _('Connection'));
			this._row(el, _('Source'), nm(l.source));
			this._row(el, _('Target'), nm(l.target));
			this._row(el, _('Type'), l.type, (LINK_STYLES[l.type] || {}).stroke);
			this._row(el, _('Interface'), l.interface || '-');
			if (l.type === 'ethernet')
				this._row(el, _('Link State'), l.status || '--');
			this._section(el, _('Quality'));
			if (l.type === '80211s') {
				this._row(el, _('RSSI'), l.rssi ? l.rssi + ' dBm' : '--',
					qualityColor(qcls));
				this._row(el, _('TX Rate'),
					l.tx_rate ? Math.round(l.tx_rate / 1000) + ' Mbps' : '--');
				this._row(el, _('Quality'), qword, qualityColor(qcls));
			} else if (l.type === 'batman') {
				this._row(el, _('TQ'), l.tq ? String(l.tq) : '--',
					qualityColor(qcls));
				this._row(el, _('Quality'), qword, qualityColor(qcls));
			}
			this._row(el, _('Collection Status'),
				l.type === 'batman' ?
					(this._lastHealth === 'ok' ? _('Realtime') :
					 this._lastHealth === 'stale' ? _('Stale') :
					 _('No data')) : '-');
			if (l.last_seen)
				this._row(el, _('Last Seen'), fmtAgo(l.last_seen));
			if (l.type === '80211s' || l.type === 'batman')
				this.drawSpark(el, this._linkHist[l.id], l.type);
		} else {
			var hint = document.createElement('div');
			hint.style.cssText = 'color:#999;font-size:small;padding:20px 0;text-align:center';
			hint.textContent = missing ?
				_('Selected object no longer in topology') :
				_('Tap a node, link or client to view details');
			el.appendChild(hint);
		}
	},

	_section: function(parent, label) {
		var s = document.createElement('div');
		s.style.cssText = 'font-size:11px;color:#999;text-transform:uppercase;margin:10px 0 4px;letter-spacing:0.5px';
		s.textContent = label;
		parent.appendChild(s);
	},

	_row: function(parent, key, value, color) {
		var row = document.createElement('div');
		row.style.cssText = 'display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #f5f5f5;font-size:12px';
		var k = document.createElement('span');
		k.style.color = '#888'; k.textContent = key;
		var v = document.createElement('span');
		if (color) v.style.color = color;
		v.textContent = value;
		row.appendChild(k); row.appendChild(v);
		parent.appendChild(row);
	},

	renderStatus: function(data) {
		var el = document.querySelector('.topo-status');
		if (!el) return;
		while (el.firstChild) el.removeChild(el.firstChild);

		/* collection health NEVER recolors nodes (decoupling rule);
		 * it only drives this banner + the batman link stale marks */
		var health = (data && data.batman_collect) || 'ok';
		var cs = collectState(health, this._rpcFailed);
		var notice = {
			ok: [_('Data OK'), '#4caf50'],
			stale: [_('Data may be stale'), '#ff9800'],
			failed: [_('Collect failed, showing cached topology'), '#f44336'],
			rpc: [_('RPC failed, showing cached data'), '#ff9800']
		}[cs];

		var mk = function(txt, color) {
			var span = document.createElement('span');
			var b = document.createElement('b');
			if (color) b.style.color = color;
			b.textContent = txt;
			span.appendChild(b);
			return span;
		};
		el.appendChild(mk(String(data && data.nodes ? data.nodes.length : 0)));
		el.appendChild(document.createTextNode(' 节点 |'));
		el.appendChild(mk(String(data && data.links ? data.links.length : 0)));
		el.appendChild(document.createTextNode(' 链路 |'));
		el.appendChild(mk(data && data.timestamp ?
			fmtAgo(data.timestamp) : '-', null));
		el.appendChild(document.createTextNode(' | '));
		el.appendChild(mk(notice[0], notice[1]));

		var tick = document.createElement('span');
		tick.className = 'topo-ticker';
		tick.style.cssText = 'margin-left:auto;color:#999;overflow:hidden;' +
			'text-overflow:ellipsis;white-space:nowrap;flex:1;min-width:0;' +
			'text-align:right;padding-left:12px';
		el.appendChild(tick);
		this.renderEventTicker();
	},

	renderEventTicker: function() {
		var tick = document.querySelector('.topo-ticker');
		if (!tick) return;
		tick.textContent = this._events.length ?
			this.eventText(this._events[0]) : '';
	},

	/* ==================== refresh (5s, preserves layout) ==================== */

	refresh: function() {
		var self = this;
		callTopology().then(function(data) {
			if (self._rpcFailed) {
				self._rpcFailed = false;
				self.logEvent('collect_recover');
			}
			if (!data || !data.nodes) return;
			self.lastNodes = data.nodes;
			self.lastLinks = data.links || [];
			self.lastData = data;

			/* state transitions (real changes only, deduped by design);
			 * layout, viewport and user selection are never touched */
			diffTopologyEvents(self._prevState, data).forEach(function(e) {
				self.logEvent(e.kind, e.key);
			});
			self._prevState = self.snapshotState(data);
			self.collectHealthEvent(data);
			self.updateDegradeStreaks(data);
			self.pushLinkHist(data);

			/* fetch client devices in parallel */
			L.resolveDefault(callClients(), { clients: [] }).then(function(cd) {
				self.lastClients = cd || { clients: [] };
				self.renderClients();
			});

			if (!self._layoutScope)
				for (var i = 0; i < data.nodes.length; i++)
					if (data.nodes[i].role === 'controller') {
						self._layoutScope = data.nodes[i].id.substring(0, 16);
						break;
					}

			var isNew = false;
			data.nodes.forEach(function(n) { if (!self.nodePos[n.id]) isNew = true; });

			if (isNew) {
				var newNodes = data.nodes.filter(function(n) { return !self.nodePos[n.id]; });
				self.autoLayoutNew(newNodes, data.nodes, data.links);
			}
			self.renderAll();
			self.renderDetail();
			self.renderStatus(data);
		}).catch(function() {
			/* keep the last good topology; one event per outage, no
			 * recurring error popups while the failure persists */
			if (!self._rpcFailed) {
				self._rpcFailed = true;
				self.logEvent('rpc_down');
			}
			self.renderStatus(self.lastData);
		});
	},

	/* ==================== pointer events ==================== */

	bindEvents: function(svg) {
		var self = this;
		svg.style.touchAction = 'none';

		svg.addEventListener('pointerdown', function(evt) {
			var nodeEl = evt.target.closest ? evt.target.closest('.topo-node') : null;
			var linkEl = evt.target.closest ? evt.target.closest('.topo-link') : null;
			var clientEl = evt.target.closest ? evt.target.closest('.topo-client') : null;

			if (nodeEl) {
				self._pointers[evt.pointerId] = { type: 'node' };
				self.dragTarget = nodeEl.getAttribute('data-id');
				var pt = self.clientToSvg(evt.clientX, evt.clientY);
				var p = self.nodePos[self.dragTarget];
				if (p) self.dragStart = { ox: pt.x - p.x, oy: pt.y - p.y };
				self.selectedNode = self.dragTarget;
				self.selectedLink = null;
				self.selectedClient = null;
				self.renderDetail(); self.renderAll();
			} else if (clientEl) {
				self._pointers[evt.pointerId] = { type: 'client' };
				self.selectedClient = clientEl.getAttribute('data-id');
				self.selectedNode = null;
				self.selectedLink = null;
				self.renderDetail(); self.renderAll();
			} else if (linkEl) {
				self._pointers[evt.pointerId] = { type: 'link' };
				self.selectedLink = linkEl.getAttribute('data-id');
				self.selectedNode = null;
				self.selectedClient = null;
				self.renderDetail(); self.renderAll();
			} else {
				self._pointers[evt.pointerId] = { type: 'pan' };
				self.dragStart = { x: evt.clientX, y: evt.clientY, px: self.pan.x, py: self.pan.y };
				self.selectedClient = null;
			}
			try { svg.setPointerCapture(evt.pointerId); } catch (e) {}
			evt.preventDefault();
		});

		svg.addEventListener('pointermove', function(evt) {
			var ptr = self._pointers[evt.pointerId];
			if (!ptr) return;

			if (ptr.type === 'node' && self.dragTarget) {
				var pt = self.clientToSvg(evt.clientX, evt.clientY);
				var p = self.nodePos[self.dragTarget];
				if (p) {
					p.x = pt.x - self.dragStart.ox;
					p.y = pt.y - self.dragStart.oy;
					var sel = '[data-id="' + cssEsc(self.dragTarget) + '"]';
					var el = self.svgGroup.querySelector('.topo-node' + sel);
					if (el) el.setAttribute('transform', 'translate(' + p.x + ',' + p.y + ')');
					self.renderLinks();
					self.renderClients();
				}
			} else if (ptr.type === 'pan') {
				self.pan.x = self.dragStart.px + (evt.clientX - self.dragStart.x);
				self.pan.y = self.dragStart.py + (evt.clientY - self.dragStart.y);
				self.applyTransform();
			}
		});

		var endPointer = function(evt) {
			delete self._pointers[evt.pointerId];
			if (Object.keys(self._pointers).length < 2) self._pinchDist = 0;
			if (Object.keys(self._pointers).length === 0) {
				if (self.dragTarget) self.saveLayoutDebounced();
				self.dragTarget = null;
			}
		};
		svg.addEventListener('pointerup', endPointer);
		svg.addEventListener('pointercancel', endPointer);
		svg.addEventListener('pointerleave', function(evt) {
			if (self._pointers[evt.pointerId]) endPointer(evt);
		});

		svg.addEventListener('wheel', function(evt) {
			evt.preventDefault();
			var rect = svg.getBoundingClientRect();
			var mx = evt.clientX - rect.left, my = evt.clientY - rect.top;
			var f = evt.deltaY < 0 ? 1.1 : 0.9;
			var nz = self.zoom * f;
			if (nz < ZOOM_MIN || nz > ZOOM_MAX) return;
			self.pan.x = mx - (mx - self.pan.x) * (nz / self.zoom);
			self.pan.y = my - (my - self.pan.y) * (nz / self.zoom);
			self.zoom = nz;
			self.applyTransform();
			self.saveLayoutDebounced();
		}, { passive: false });

		window.addEventListener('keydown', function(evt) {
			if (evt.key === 'Escape') {
				self.selectedNode = null; self.selectedLink = null;
				self.selectedClient = null;
				self.renderDetail(); self.renderAll();
			}
		});
	},

	/* ==================== main render ==================== */

	render: function(data) {
		var self = this;

		if (!data || !data.nodes) {
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('OpenMesh agent node')),
				E('p', { 'style': 'color:#666' }, _('Topology is rendered on the controller.'))
			]);
		}

		self.lastNodes = data.nodes;
		self.lastLinks = data.links || [];
		self.lastData = data;

		for (var i = 0; i < data.nodes.length; i++)
			if (data.nodes[i].role === 'controller') {
				self._layoutScope = data.nodes[i].id.substring(0, 16);
				break;
			}

		var restored = self.loadLayout();
		var isNarrow = window.innerWidth < 768;

		/* ---- toolbar (Chinese, touch-friendly) ---- */
		var toolbar = E('div', { 'style':
			'padding:8px 12px;display:flex;gap:8px;align-items:center;flex-wrap:wrap;' +
			'background:#fff;border:1px solid #e0e0e0;border-bottom:none;' +
			'border-radius:12px 12px 0 0'
		}, [
			E('button', {
				'class': 'btn cbi-button cbi-button-apply',
				'style': 'padding:6px 16px;font-size:13px;border-radius:6px',
				'click': function() { self.smartLayout(self.lastNodes, self.lastLinks); self.autoFit(); }
			}, _('Auto Layout')),
			E('button', {
				'class': 'btn cbi-button cbi-button-neutral',
				'style': 'padding:6px 16px;font-size:13px;border-radius:6px',
				'click': function() { self.autoFit(); }
			}, _('Fit')),
			E('button', {
				'class': 'btn cbi-button cbi-button-remove',
				'style': 'padding:6px 16px;font-size:13px;border-radius:6px',
				'click': function() { self.resetLayout(); }
			}, _('Reset')),
			E('span', { 'style': 'color:#aaa;font-size:small;margin-left:auto' },
			  _('Zoom') + ' '),
			E('span', { 'class': 'zoom-label', 'style': 'font-size:small;font-weight:bold;min-width:40px;color:#555' },
			  Math.round(self.zoom * 100) + '%')
		]);

		/* ---- SVG canvas ---- */
		var NS = 'http://www.w3.org/2000/svg';
		var svg = document.createElementNS(NS, 'svg');
		svg.setAttribute('viewBox', '0 0 ' + self.canvasW + ' ' + self.canvasH);
		svg.setAttribute('width', '100%');
		svg.style.cssText = isNarrow
			? 'width:100%;height:340px;display:block;touch-action:none;background:linear-gradient(135deg,#FAFBFE 0%,#F3F4F8 100%);border:1px solid #E8E8E8'
			: 'width:100%;height:100%;min-height:440px;display:block;touch-action:none;background:linear-gradient(135deg,#FAFBFE 0%,#F3F4F8 100%);border:1px solid #E8E8E8';

		var viewport = document.createElementNS(NS, 'g');
		viewport.setAttribute('class', 'topo-viewport');
		var linkLayer = document.createElementNS(NS, 'g');
		linkLayer.setAttribute('class', 'link-layer');
		var nodeLayer = document.createElementNS(NS, 'g');
		nodeLayer.setAttribute('class', 'node-layer');
		var clientLayer = document.createElementNS(NS, 'g');
		clientLayer.setAttribute('class', 'client-layer');
		viewport.appendChild(linkLayer);
		viewport.appendChild(nodeLayer);
		viewport.appendChild(clientLayer);
		svg.appendChild(viewport);

		var canvasWrap = E('div', {
			'style': isNarrow
				? 'width:100%;height:340px;overflow:hidden;position:relative;border-radius:0'
				: 'flex:1;min-height:440px;overflow:hidden;position:relative;border-radius:0'
		}, svg);

		var detailPanel = E('div', {
			'class': 'topo-detail',
			'style': isNarrow
				? 'width:100%;max-height:220px;background:#fff;border:1px solid #E8E8E8;border-top:none;border-radius:0 0 12px 12px;padding:12px;overflow-y:auto'
				: 'width:260px;background:#fff;border:1px solid #E8E8E8;border-left:none;border-radius:0 12px 12px 0;padding:12px;overflow-y:auto'
		}, [E('div', { 'class': 'topo-detail-body' })]);

		var mainArea = E('div', { 'style': isNarrow ? '' : 'display:flex' }, [canvasWrap, detailPanel]);
		var statusBar = E('div', { 'class': 'topo-status',
			'style': 'padding:8px 12px;background:#FAFAFA;border:1px solid #E8E8E8;border-top:none;border-radius:0 0 12px 12px;font-size:12px;display:flex;gap:20px;justify-content:center;color:#666'
		});

		var container = E('div', { 'style': 'margin-bottom:20px' }, [toolbar, mainArea, statusBar]);

		/* hide LuCI save/reset buttons (not applicable to read-only topology) */
		container.appendChild(E('style', {}, '.cbi-page-actions{display:none!important}'));

		requestAnimationFrame(function() {
			self.svgRoot = svg;
			self.svgGroup = viewport;
			self.bindEvents(svg);
			if (!restored) {
				self.smartLayout(data.nodes, self.lastLinks);
				self.autoFit();
			} else {
				var newNodes = data.nodes.filter(function(n) { return !self.nodePos[n.id]; });
				if (newNodes.length > 0) self.autoLayoutNew(newNodes, data.nodes, self.lastLinks);
				self.applyTransform();
				self.renderAll();
			}
			/* fetch clients for initial render */
			L.resolveDefault(callClients(), { clients: [] }).then(function(cd) {
				self.lastClients = cd || { clients: [] };
				self.renderClients();
			});
			/* baseline state: no false transitions on the first poll */
			self._prevState = self.snapshotState(data);
			self._lastHealth = (data && data.batman_collect) || 'ok';
			self.updateDegradeStreaks(data);
			self.pushLinkHist(data);
			self.renderDetail();
			self.renderStatus(data);
			poll.add(L.bind(self.refresh, self), POLL_INTERVAL);
		});

		return container;
	}
});
