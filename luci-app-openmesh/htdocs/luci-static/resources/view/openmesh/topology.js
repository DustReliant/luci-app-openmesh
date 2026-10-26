// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * OpenMesh Manager interactive topology canvas (Phase 6).
 *
 * Phase 6.0: HTML injection hardening (esc() on all dynamic text,
 * safe querySelector, attribute-only data-id binding).
 * Phase 6.1: Layout persistence via localStorage (positions + viewport
 * auto-saved on drag end/layout change, auto-restored on page load,
 * new-node position protection, explicit reset).
 *
 * Network state and UI layout are strictly separate: layout data
 * lives only in the browser, keyed by stable node_id, never in the
 * backend model.
 */
'use strict';
'require view';
'require rpc';
'require poll';

var callTopology = rpc.declare({ object: 'openmesh', method: 'topology' });

var ZOOM_MIN = 0.3, ZOOM_MAX = 3.0;
var NODE_W = 110, NODE_H = 52;
var POLL_INTERVAL = 5;
var LAYOUT_VERSION = 1;
var STORAGE_KEY_PREFIX = 'openmesh-topo-layout-';

function esc(s) {
	return String(s || '').replace(/&/g, '&amp;')
		.replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;')
		.replace(/'/g, '&#39;');
}

function shortMac(mac) {
	if (!mac) return '-';
	return mac.length > 17 ? mac.substring(0, 8) + '…' : mac;
}

function shortId(id) {
	return id ? id.substring(0, 8) + '…' : '-';
}

function fmtAgo(ts) {
	if (!ts) return '-';
	return new Date(ts * 1000).toLocaleTimeString();
}

/* CSS-safe attribute value for querySelector: escape quotes and brackets */
function cssEsc(s) {
	return String(s || '').replace(/"/g, '\\"').replace(/]/g, '\\]');
}

return view.extend({
	nodePos: {},
	zoom: 1,
	pan: { x: 0, y: 0 },
	selectedNode: null,
	selectedLink: null,
	dragMode: null,
	dragTarget: null,
	dragStart: null,
	svgRoot: null,
	svgGroup: null,
	canvasW: 760, canvasH: 480,
	lastNodes: null,
	lastLinks: null,
	lastData: null,
	_pointers: {},
	_pinchDist: 0,
	_layoutScope: '',
	_saveTimer: null,

	load: function() {
		return L.resolveDefault(callTopology(), {});
	},

	/* ---- layout persistence (Phase 6.1) ---- */

	_layoutKey: function() {
		/* scope by controller node_id prefix (stable identity, first 16 hex
		 * chars is enough to disambiguate networks without storing the full
		 * key); falls back to hostname-based key if nodes unavailable */
		if (this._layoutScope)
			return STORAGE_KEY_PREFIX + this._layoutScope;
		return STORAGE_KEY_PREFIX + 'default';
	},

	saveLayout: function() {
		try {
			var data = {
				version: LAYOUT_VERSION,
				positions: this.nodePos,
				viewport: { zoom: this.zoom, panX: this.pan.x, panY: this.pan.y }
			};
			localStorage.setItem(this._layoutKey(), JSON.stringify(data));
		} catch (e) {
			/* storage full or unavailable: layout persistence is best-effort */
		}
	},

	saveLayoutDebounced: function() {
		var self = this;
		if (this._saveTimer) clearTimeout(this._saveTimer);
		this._saveTimer = setTimeout(function() {
			self.saveLayout();
			self._saveTimer = null;
		}, 300);
	},

	loadLayout: function() {
		try {
			var raw = localStorage.getItem(this._layoutKey());
			if (!raw) return false;
			var data = JSON.parse(raw);
			if (!data || data.version !== LAYOUT_VERSION) return false;
			if (!data.positions || typeof data.positions !== 'object') return false;
			var restored = false;
			var self = this;
			Object.keys(data.positions).forEach(function(id) {
				var p = data.positions[id];
				if (p && typeof p.x === 'number' && typeof p.y === 'number' &&
				    isFinite(p.x) && isFinite(p.y) &&
				    p.x > -10000 && p.x < 10000 &&
				    p.y > -10000 && p.y < 10000) {
					self.nodePos[id] = { x: p.x, y: p.y };
					restored = true;
				}
			});
			if (data.viewport) {
				var v = data.viewport;
				if (typeof v.zoom === 'number' && isFinite(v.zoom) &&
				    v.zoom >= ZOOM_MIN && v.zoom <= ZOOM_MAX)
					this.zoom = v.zoom;
				if (typeof v.panX === 'number' && isFinite(v.panX))
					this.pan.x = v.panX;
				if (typeof v.panY === 'number' && isFinite(v.panY))
					this.pan.y = v.panY;
			}
			return restored;
		} catch (e) {
			return false; /* corrupted JSON: safely ignore, use defaults */
		}
	},

	resetLayout: function() {
		try { localStorage.removeItem(this._layoutKey()); } catch (e) {}
		this.nodePos = {};
		this.zoom = 1;
		this.pan = { x: 0, y: 0 };
		if (this.lastNodes) this.autoLayout(this.lastNodes);
		this.applyTransform();
		this.saveLayout();
	},

	/* ---- layout ---- */
	autoLayout: function(nodes) {
		var self = this;
		var ctrl = null, agents = [];
		nodes.forEach(function(n) {
			if (n.role === 'controller') ctrl = n;
			else agents.push(n);
		});
		var cx = self.canvasW / 2, cy = 100;
		if (ctrl) self.nodePos[ctrl.id] = { x: cx, y: cy };
		agents.forEach(function(n, i) {
			var ang = Math.PI * (i + 1) / (agents.length + 1);
			var r = agents.length > 3 ? 200 : 160;
			self.nodePos[n.id] = {
				x: cx + Math.cos(ang) * r * 1.2,
				y: cy + Math.sin(ang) * r + 120
			};
		});
		self.applyTransform();
		self.renderAll();
		self.saveLayoutDebounced();
	},

	fitScreen: function() {
		this.zoom = 1;
		this.pan = { x: 0, y: 0 };
		this.applyTransform();
		this.saveLayoutDebounced();
	},

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

	/* ---- visual ---- */
	nodeColor: function(n) {
		if (n.status === 'online') return n.role === 'controller' ? '#2d6b2d' : '#3aa835';
		return '#c0504d';
	},

	linkStyle: function(l) {
		switch (l.type) {
		case 'ethernet': return { stroke: '#555', dash: 'none', w: 3 };
		case '80211s':   return { stroke: '#4a90d9', dash: '6,4', w: 2 };
		case 'batman':   return { stroke: '#8B5CF6', dash: '2,3', w: 2 };
		default:         return { stroke: '#999', dash: '3,3', w: 1 };
		}
	},

	linkLabel: function(l) {
		var p = [];
		if (l.tq) p.push('TQ:' + l.tq);
		if (l.rssi) p.push(l.rssi + 'dBm');
		return p.join(' ');
	},

	renderAll: function() {
		if (!this.svgGroup || !this.lastNodes) return;
		this.renderNodes();
		this.renderLinks();
	},

	renderNodes: function() {
		var self = this;
		var g = this.svgGroup.querySelector('.node-layer');
		if (!g) return;
		while (g.firstChild) g.removeChild(g.firstChild);
		this.lastNodes.forEach(function(n) {
			var p = self.nodePos[n.id];
			if (!p) p = self.nodePos[n.id] = { x: self.canvasW/2, y: self.canvasH/2 };
			var fill = self.nodeColor(n);
			var sel = self.selectedNode === n.id;
			var el = document.createElementNS('http://www.w3.org/2000/svg', 'g');
			el.setAttribute('class', 'topo-node');
			el.setAttribute('data-id', n.id);
			el.setAttribute('transform', 'translate(' + p.x + ',' + p.y + ')');
			el.style.cursor = 'pointer';
			el.style.touchAction = 'none';

			var r = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
			r.setAttribute('x', -NODE_W/2); r.setAttribute('y', -NODE_H/2);
			r.setAttribute('width', NODE_W); r.setAttribute('height', NODE_H);
			r.setAttribute('rx', 10); r.setAttribute('fill', fill);
			r.setAttribute('stroke', sel ? '#ff9800' : 'rgba(0,0,0,0.15)');
			r.setAttribute('stroke-width', sel ? 3 : 1);
			el.appendChild(r);

			[['y',-8,n.hostname,'#fff',12,'bold'],
			 ['y',6,n.role,'rgba(255,255,255,0.8)',10,''],
			 ['y',18,n.ip || shortId(n.id),'rgba(255,255,255,0.6)',9,'']
			].forEach(function(a) {
				var t = document.createElementNS('http://www.w3.org/2000/svg', 'text');
				t.setAttribute(a[0], a[1]);
				t.setAttribute('text-anchor', 'middle');
				t.setAttribute('fill', a[2 + 1]);
				t.setAttribute('font-size', a[4]);
				if (a[5]) t.setAttribute('font-weight', a[5]);
				t.textContent = a[2]; /* DOM text node: immune to HTML injection */
				el.appendChild(t);
			});
			g.appendChild(el);
		});
	},

	renderLinks: function() {
		var self = this;
		var g = this.svgGroup.querySelector('.link-layer');
		if (!g) return;
		while (g.firstChild) g.removeChild(g.firstChild);
		(self.lastLinks || []).forEach(function(l) {
			var s = self.nodePos[l.source];
			var d = self.nodePos[l.target];
			if (!s || !d) return;
			var st = self.linkStyle(l);
			var sel = self.selectedLink === l.id;
			var el = document.createElementNS('http://www.w3.org/2000/svg', 'g');
			el.setAttribute('class', 'topo-link');
			el.setAttribute('data-id', l.id);
			el.style.cursor = 'pointer';

			var line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
			line.setAttribute('x1', s.x); line.setAttribute('y1', s.y);
			line.setAttribute('x2', d.x); line.setAttribute('y2', d.y);
			line.setAttribute('stroke', st.stroke);
			line.setAttribute('stroke-width', sel ? st.w + 2 : st.w);
			line.setAttribute('stroke-dasharray', st.dash);
			line.setAttribute('opacity', sel ? '1.0' : '0.7');
			el.appendChild(line);

			var hit = document.createElementNS('http://www.w3.org/2000/svg', 'line');
			hit.setAttribute('x1', s.x); hit.setAttribute('y1', s.y);
			hit.setAttribute('x2', d.x); hit.setAttribute('y2', d.y);
			hit.setAttribute('stroke', 'transparent');
			hit.setAttribute('stroke-width', 20);
			el.appendChild(hit);

			var label = self.linkLabel(l);
			if (label) {
				var t = document.createElementNS('http://www.w3.org/2000/svg', 'text');
				t.setAttribute('x', (s.x + d.x) / 2);
				t.setAttribute('y', (s.y + d.y) / 2 - 6);
				t.setAttribute('text-anchor', 'middle');
				t.setAttribute('fill', st.stroke);
				t.setAttribute('font-size', 9);
				t.textContent = label;
				el.appendChild(t);
			}
			g.appendChild(el);
		});
	},

	/* ---- detail panel (uses textContent, not innerHTML, for values) ---- */
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
				this._addRow(el, 'Role', n.role);
				this._addRow(el, 'Status', n.status, n.status === 'online' ? '#4caf50' : '#f44336');
				this._addRow(el, 'IP', n.ip || '-');
				this._addRow(el, 'MAC', shortMac(n.mac));
				this._addRow(el, 'Model', n.model || '-');
				this._addRow(el, 'Last Seen', fmtAgo(n.last_seen));
				this._addRow(el, 'Clients', String(n.client_count || 0));
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
				title.textContent = _('Link');
				el.appendChild(title);
				this._addRow(el, 'Source', nm(l.source));
				this._addRow(el, 'Target', nm(l.target));
				this._addRow(el, 'Type', l.type, this.linkStyle(l).stroke);
				this._addRow(el, 'Status', l.status);
				this._addRow(el, 'Interface', l.interface || '-');
				if (l.tq) this._addRow(el, 'TQ', String(l.tq));
				if (l.rssi) this._addRow(el, 'RSSI', l.rssi + ' dBm');
				if (l.tx_rate) this._addRow(el, 'TX Rate', Math.round(l.tx_rate / 1000) + ' Mbps');
			}
		} else {
			var hint = document.createElement('div');
			hint.style.cssText = 'color:#999;font-size:small;padding:20px 0;text-align:center';
			hint.textContent = _('Tap a node or link to view details');
			el.appendChild(hint);
		}
	},

	_addRow: function(parent, key, value, color) {
		var row = document.createElement('div');
		row.style.cssText = 'display:flex;justify-content:space-between;padding:4px 0;' +
			'border-bottom:1px solid #f0f0f0;font-size:12px';
		var k = document.createElement('span');
		k.style.color = '#888';
		k.textContent = key;
		var v = document.createElement('span');
		if (color) v.style.color = color;
		v.textContent = value;
		row.appendChild(k);
		row.appendChild(v);
		parent.appendChild(row);
	},

	renderStatus: function(data) {
		var el = document.querySelector('.topo-status');
		if (!el) return;
		while (el.firstChild) el.removeChild(el.firstChild);
		var health = (data && data.batman_collect) || 'ok';
		var hc = health === 'ok' ? '#4caf50' : health === 'stale' ? '#ff9800' : '#f44336';
		var parts = [
			[' nodes ', String(data && data.nodes ? data.nodes.length : 0)],
			[' links ', String(data && data.links ? data.links.length : 0)],
			[' data ', health]
		];
		parts.forEach(function(p, i) {
			if (i > 0) {
				var sep = document.createElement('span');
				sep.textContent = ' |';
				el.appendChild(sep);
			}
			var span = document.createElement('span');
			span.textContent = p[0];
			var b = document.createElement('b');
			if (i === 2) b.style.color = hc;
			b.textContent = p[1];
			span.appendChild(b);
			el.appendChild(span);
		});
	},

	/* ---- refresh (5s, preserves positions, saves new node positions) ---- */
	refresh: function() {
		var self = this;
		callTopology().then(function(data) {
			if (!data || !data.nodes) return;
			self.lastNodes = data.nodes;
			self.lastLinks = data.links || [];
			self.lastData = data;

			/* scope layout key by controller id (set once) */
			if (!self._layoutScope)
				for (var i = 0; i < data.nodes.length; i++)
					if (data.nodes[i].role === 'controller') {
						self._layoutScope = data.nodes[i].id.substring(0, 16);
						break;
					}

			var isNew = false;
			data.nodes.forEach(function(n) {
				if (!self.nodePos[n.id]) isNew = true;
			});
			if (isNew) {
				/* new nodes: autoLayout assigns positions but must not
				 * move existing nodes that already have positions */
				var newNodes = data.nodes.filter(function(n) { return !self.nodePos[n.id]; });
				self.autoLayoutNew(newNodes, data.nodes);
			}
			self.renderAll();
			self.renderDetail();
			self.renderStatus(data);
		}).catch(function() {});
	},

	/* place new nodes without disturbing existing positions */
	autoLayoutNew: function(newNodes, allNodes) {
		var self = this;
		var ctrl = null, agents = [];
		allNodes.forEach(function(n) {
			if (n.role === 'controller') ctrl = n;
			else agents.push(n);
		});
		var cx = self.canvasW / 2, cy = 100;
		newNodes.forEach(function(n) {
			if (n.role === 'controller') {
				self.nodePos[n.id] = { x: cx, y: cy };
			} else {
				var idx = agents.indexOf(n);
				var ang = Math.PI * (idx + 1) / (agents.length + 1);
				var r = agents.length > 3 ? 200 : 160;
				self.nodePos[n.id] = {
					x: cx + Math.cos(ang) * r * 1.2,
					y: cy + Math.sin(ang) * r + 120
				};
			}
		});
		self.saveLayoutDebounced();
	},

	/* ---- Pointer Events: unified mouse + touch ---- */
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
				self.renderDetail();
				self.renderAll();
			} else if (linkEl) {
				self._pointers[evt.pointerId] = { type: 'link' };
				self.selectedLink = linkEl.getAttribute('data-id');
				self.selectedNode = null;
				self.renderDetail();
				self.renderAll();
			} else {
				self._pointers[evt.pointerId] = { type: 'pan' };
				self.dragStart = {
					x: evt.clientX, y: evt.clientY,
					px: self.pan.x, py: self.pan.y
				};
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
			if (Object.keys(self._pointers).length < 2)
				self._pinchDist = 0;
			if (Object.keys(self._pointers).length === 0) {
				if (self.dragTarget) self.saveLayoutDebounced(); /* save after node drag */
				self.dragTarget = null;
			}
		};
		svg.addEventListener('pointerup', endPointer);
		svg.addEventListener('pointercancel', endPointer);
		svg.addEventListener('pointerleave', function(evt) {
			if (self._pointers[evt.pointerId]) endPointer(evt);
		});

		/* wheel zoom (desktop): debounce save */
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

		/* ESC deselect (desktop) */
		window.addEventListener('keydown', function(evt) {
			if (evt.key === 'Escape') {
				self.selectedNode = null;
				self.selectedLink = null;
				self.renderDetail();
				self.renderAll();
			}
		});
	},

	/* ---- main render ---- */
	render: function(data) {
		var self = this;

		if (!data || !data.nodes) {
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('OpenMesh agent node')),
				E('p', { 'style': 'color:#666' },
					_('Topology is rendered on the controller.'))
			]);
		}

		self.lastNodes = data.nodes;
		self.lastLinks = data.links || [];
		self.lastData = data;

		/* determine layout scope from controller id */
		for (var i = 0; i < data.nodes.length; i++)
			if (data.nodes[i].role === 'controller') {
				self._layoutScope = data.nodes[i].id.substring(0, 16);
				break;
			}

		/* restore saved layout before first render */
		var restored = self.loadLayout();

		var isNarrow = window.innerWidth < 768;

		var toolbar = E('div', { 'style':
			'padding:8px 12px;display:flex;gap:6px;align-items:center;flex-wrap:wrap;' +
			'background:#fff;border:1px solid #e0e0e0;border-bottom:none;' +
			'border-radius:8px 8px 0 0'
		}, [
			E('button', {
				'class': 'btn cbi-button cbi-button-apply',
				'style': 'padding:4px 12px;font-size:12px',
				'click': function() { self.autoLayout(self.lastNodes); }
			}, _('Layout')),
			E('button', {
				'class': 'btn cbi-button cbi-button-neutral',
				'style': 'padding:4px 12px;font-size:12px',
				'click': function() { self.fitScreen(); }
			}, _('Fit')),
			E('button', {
				'class': 'btn cbi-button cbi-button-remove',
				'style': 'padding:4px 12px;font-size:12px',
				'click': function() { self.resetLayout(); }
			}, _('Reset')),
			E('button', {
				'class': 'btn cbi-button cbi-button-neutral',
				'style': 'padding:4px 12px;font-size:12px',
				'click': function() { self.refresh(); }
			}, _('↻')),
			E('span', { 'style': 'color:#888;font-size:small;margin-left:4px' }, _('Zoom:')),
			E('span', { 'class': 'zoom-label', 'style': 'font-size:small;font-weight:bold;min-width:36px' },
			  Math.round(self.zoom * 100) + '%')
		]);

		var NS = 'http://www.w3.org/2000/svg';
		var svg = document.createElementNS(NS, 'svg');
		svg.setAttribute('viewBox', '0 0 760 480');
		svg.setAttribute('width', '100%');
		svg.style.cssText = isNarrow
			? 'width:100%;height:320px;display:block;touch-action:none;background:#f8f9fa;border:1px solid #e0e0e0'
			: 'width:100%;height:100%;min-height:420px;display:block;touch-action:none;background:#f8f9fa;border:1px solid #e0e0e0';

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
				? 'width:100%;height:320px;background:#f8f9fa;border:1px solid #e0e0e0;overflow:hidden;position:relative'
				: 'flex:1;min-height:420px;background:#f8f9fa;border:1px solid #e0e0e0;overflow:hidden;position:relative'
		}, svg);

		var detailPanel = E('div', {
			'class': 'topo-detail',
			'style': isNarrow
				? 'width:100%;max-height:200px;background:#fff;border:1px solid #e0e0e0;border-top:none;border-radius:0 0 8px 8px;padding:10px;overflow-y:auto'
				: 'width:250px;background:#fff;border:1px solid #e0e0e0;border-left:none;border-radius:0 8px 8px 0;padding:10px;overflow-y:auto'
		}, [
			E('div', { 'class': 'topo-detail-body' })
		]);

		var mainArea = E('div', { 'style': isNarrow ? '' : 'display:flex' }, [canvasWrap, detailPanel]);
		var statusBar = E('div', { 'class': 'topo-status',
			'style': 'padding:6px 12px;background:#f5f5f5;border:1px solid #e0e0e0;border-top:none;border-radius:0 0 8px 8px;font-size:12px;display:flex;gap:16px;justify-content:center'
		});

		var container = E('div', {}, [toolbar, mainArea, statusBar]);

		requestAnimationFrame(function() {
			self.svgRoot = svg;
			self.svgGroup = viewport;
			self.bindEvents(svg);
			if (!restored) {
				self.autoLayout(data.nodes);
			} else {
				/* restored: place any new nodes that have no saved position */
				var newNodes = data.nodes.filter(function(n) { return !self.nodePos[n.id]; });
				if (newNodes.length > 0) self.autoLayoutNew(newNodes, data.nodes);
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
