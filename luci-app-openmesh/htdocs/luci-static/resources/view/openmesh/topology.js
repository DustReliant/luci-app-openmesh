// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * OpenMesh Manager interactive topology canvas (Phase 6.2).
 *
 * P0: Modern node cards (180×85, status dots), dual-link Bezier curves,
 *     mobile auto-fit initial view.
 * P1: BFS hierarchical smart layout (star/chain/tree/ring/isolated),
 *     Chinese toolbar, detail panel grouping.
 * Inherited from 6.1: layout persistence, DOM textContent security,
 *     Pointer Events (mouse+touch), debounced save.
 */
'use strict';
'require view';
'require rpc';
'require poll';

var callTopology = rpc.declare({ object: 'openmesh', method: 'topology' });

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
	_pointers: {}, _pinchDist: 0,
	_layoutScope: '', _saveTimer: null,

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

	statusColor: function(n) {
		if (n.status === 'online') return '#4CAF50';
		if (n.status === 'offline') return '#F44336';
		return '#FF9800'; /* unknown/warning */
	},

	renderAll: function() {
		if (!this.svgGroup || !this.lastNodes) return;
		this.renderNodes();
		this.renderLinks();
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
			var sc = self.statusColor(n);

			var grp = self._svg('g', {
				'class': 'topo-node', 'data-id': n.id,
				'transform': 'translate(' + p.x + ',' + p.y + ')'
			}, g);
			grp.style.cursor = 'pointer';
			grp.style.touchAction = 'none';

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

			/* status dot (top-left) */
			self._svg('circle', {
				cx: -NODE_W/2 + 18, cy: -NODE_H/2 + 18, r: 7,
				fill: sc, stroke: '#FFFFFF', 'stroke-width': 2
			}, grp);

			/* hostname (bold, right of dot) */
			var hn = n.hostname || '';
			if (hn.length > 16) hn = hn.substring(0, 14) + '…';
			self._text({
				x: -NODE_W/2 + 32, y: -NODE_H/2 + 22,
				'font-size': 14, 'font-weight': 'bold', fill: '#333'
			}, hn, grp);

			/* role badge (below hostname) */
			self._text({
				x: -NODE_W/2 + 32, y: -NODE_H/2 + 40,
				'font-size': 11, fill: '#888'
			}, n.role === 'controller' ? _('Controller') : _('Agent'), grp);

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

				/* label at curve midpoint */
				var mx = (s.x + d.x) / 2 + px * 0.55;
				var my = (s.y + d.y) / 2 + py * 0.55;
				var labelText = '';
				if (l.type === '80211s' && l.rssi) labelText = l.rssi + ' dBm';
				else if (l.type === 'batman' && l.tq) labelText = 'TQ ' + l.tq;
				else if (l.type === 'ethernet') labelText = 'Ethernet';

				if (labelText) {
					/* label background plate */
					var tw = labelText.length * 7 + 12;
					self._svg('rect', {
						x: mx - tw/2, y: my - 10,
						width: tw, height: 18, rx: 4,
						fill: 'rgba(255,255,255,0.92)',
						stroke: st.stroke, 'stroke-width': 0.5
					}, grp);
					self._text({
						x: mx, y: my + 3,
						'font-size': 11, fill: st.stroke,
						'font-weight': sel ? 'bold' : 'normal',
						'text-anchor': 'middle'
					}, labelText, grp);
				}
			});
		});
	},

	/* ==================== detail panel (P1: grouped, DOM-safe) ==================== */

	renderDetail: function() {
		var el = document.querySelector('.topo-detail-body');
		if (!el) return;
		while (el.firstChild) el.removeChild(el.firstChild);

		var title = document.createElement('div');
		title.style.cssText = 'font-weight:bold;font-size:14px;margin-bottom:8px';

		if (this.selectedNode && this.lastNodes) {
			var n = null;
			for (var i = 0; i < this.lastNodes.length; i++)
				if (this.lastNodes[i].id === this.selectedNode) { n = this.lastNodes[i]; break; }
			if (n) {
				title.textContent = n.hostname;
				el.appendChild(title);
				this._section(el, _('Device'));
				this._row(el, _('Role'), n.role);
				this._row(el, _('Status'), n.status, this.statusColor(n));
				this._row(el, _('IP'), n.ip || '-');
				this._row(el, _('MAC'), shortMac(n.mac));
				this._section(el, _('Info'));
				this._row(el, _('Model'), n.model || '-');
				this._row(el, _('Last Seen'), fmtAgo(n.last_seen));
				this._row(el, _('Clients'), String(n.client_count || 0));
			}
		} else if (this.selectedLink && this.lastLinks) {
			var l = null;
			for (var j = 0; j < this.lastLinks.length; j++)
				if (this.lastLinks[j].id === this.selectedLink) { l = this.lastLinks[j]; break; }
			if (l && this.lastNodes) {
				var self = this;
				var nm = function(id) {
					for (var k = 0; k < self.lastNodes.length; k++)
						if (self.lastNodes[k].id === id) return self.lastNodes[k].hostname;
					return shortId(id);
				};
				title.textContent = _('Link Details');
				el.appendChild(title);
				this._section(el, _('Connection'));
				this._row(el, _('Source'), nm(l.source));
				this._row(el, _('Target'), nm(l.target));
				this._row(el, _('Type'), l.type, (LINK_STYLES[l.type] || {}).stroke);
				this._row(el, _('Interface'), l.interface || '-');
				this._section(el, _('Quality'));
				if (l.tq) this._row(el, _('TQ'), String(l.tq));
				if (l.rssi) this._row(el, _('RSSI'), l.rssi + ' dBm');
				if (l.tx_rate) this._row(el, _('TX Rate'), Math.round(l.tx_rate / 1000) + ' Mbps');
			}
		} else {
			var hint = document.createElement('div');
			hint.style.cssText = 'color:#999;font-size:small;padding:20px 0;text-align:center';
			hint.textContent = _('Tap a node or link to view details');
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
		var health = (data && data.batman_collect) || 'ok';
		var hc = health === 'ok' ? '#4caf50' : health === 'stale' ? '#ff9800' : '#f44336';
		[[' nodes ', String(data && data.nodes ? data.nodes.length : 0)],
		 [' links ', String(data && data.links ? data.links.length : 0)],
		 [' data ', health]
		].forEach(function(p, i) {
			if (i > 0) el.appendChild(document.createTextNode(' |'));
			var span = document.createElement('span');
			span.textContent = p[0];
			var b = document.createElement('b');
			if (i === 2) b.style.color = hc;
			b.textContent = p[1];
			span.appendChild(b);
			el.appendChild(span);
		});
	},

	/* ==================== refresh (5s, preserves layout) ==================== */

	refresh: function() {
		var self = this;
		callTopology().then(function(data) {
			if (!data || !data.nodes) return;
			self.lastNodes = data.nodes;
			self.lastLinks = data.links || [];
			self.lastData = data;

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
		}).catch(function() {});
	},

	/* ==================== pointer events ==================== */

	bindEvents: function(svg) {
		var self = this;
		svg.style.touchAction = 'none';

		svg.addEventListener('pointerdown', function(evt) {
			var nodeEl = evt.target.closest ? evt.target.closest('.topo-node') : null;
			var linkEl = evt.target.closest ? evt.target.closest('.topo-link') : null;

			if (nodeEl) {
				self._pointers[evt.pointerId] = { type: 'node' };
				self.dragTarget = nodeEl.getAttribute('data-id');
				var pt = self.clientToSvg(evt.clientX, evt.clientY);
				var p = self.nodePos[self.dragTarget];
				if (p) self.dragStart = { ox: pt.x - p.x, oy: pt.y - p.y };
				self.selectedNode = self.dragTarget;
				self.selectedLink = null;
				self.renderDetail(); self.renderAll();
			} else if (linkEl) {
				self._pointers[evt.pointerId] = { type: 'link' };
				self.selectedLink = linkEl.getAttribute('data-id');
				self.selectedNode = null;
				self.renderDetail(); self.renderAll();
			} else {
				self._pointers[evt.pointerId] = { type: 'pan' };
				self.dragStart = { x: evt.clientX, y: evt.clientY, px: self.pan.x, py: self.pan.y };
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
		viewport.appendChild(linkLayer);
		viewport.appendChild(nodeLayer);
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
			self.renderDetail();
			self.renderStatus(data);
			poll.add(L.bind(self.refresh, self), POLL_INTERVAL);
		});

		return container;
	}
});
