// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * OpenMesh Manager interactive topology canvas (Phase 5.2).
 *
 * SVG-based with Pointer Events (mouse + touch unified).
 * Features: node drag, canvas pan, pinch/wheel zoom, auto-layout,
 * node/link detail panel, 5s polling preserving positions.
 * Responsive: detail panel stacks below canvas on narrow screens.
 */
'use strict';
'require view';
'require rpc';
'require poll';

var callTopology = rpc.declare({ object: 'openmesh', method: 'topology' });

var ZOOM_MIN = 0.3, ZOOM_MAX = 3.0;
var NODE_W = 110, NODE_H = 52;
var POLL_INTERVAL = 5;

function esc(s) {
	return String(s || '').replace(/&/g, '&amp;')
		.replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
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
	var d = new Date(ts * 1000);
	return d.toLocaleTimeString();
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

	load: function() {
		return L.resolveDefault(callTopology(), {});
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
	},

	fitScreen: function() {
		this.zoom = 1;
		this.pan = { x: 0, y: 0 };
		this.applyTransform();
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
		g.innerHTML = '';
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
			el.innerHTML =
				'<rect x="-55" y="-26" width="110" height="52" rx="10"' +
				' fill="' + fill + '" stroke="' + (sel ? '#ff9800' : 'rgba(0,0,0,0.15)') + '"' +
				' stroke-width="' + (sel ? 3 : 1) + '"/>' +
				'<text y="-8" text-anchor="middle" fill="#fff" font-size="12" font-weight="bold">' + esc(n.hostname) + '</text>' +
				'<text y="6" text-anchor="middle" fill="rgba(255,255,255,0.8)" font-size="10">' + esc(n.role) + '</text>' +
				'<text y="18" text-anchor="middle" fill="rgba(255,255,255,0.6)" font-size="9">' + esc(n.ip || shortId(n.id)) + '</text>';
			g.appendChild(el);
		});
	},

	renderLinks: function() {
		var self = this;
		var g = this.svgGroup.querySelector('.link-layer');
		if (!g) return;
		g.innerHTML = '';
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
			el.innerHTML =
				'<line x1="' + s.x + '" y1="' + s.y + '" x2="' + d.x + '" y2="' + d.y + '"' +
				' stroke="' + st.stroke + '" stroke-width="' + (sel ? st.w + 2 : st.w) + '"' +
				' stroke-dasharray="' + st.dash + '" opacity="' + (sel ? '1.0' : '0.7') + '"/>' +
				'<line x1="' + s.x + '" y1="' + s.y + '" x2="' + d.x + '" y2="' + d.y + '"' +
				' stroke="transparent" stroke-width="20"/>' +
				(self.linkLabel(l) ?
				 '<text x="' + (s.x+d.x)/2 + '" y="' + ((s.y+d.y)/2 - 6) + '"' +
				 ' text-anchor="middle" fill="' + st.stroke + '" font-size="9">' + esc(self.linkLabel(l)) + '</text>' : '');
			g.appendChild(el);
		});
	},

	/* ---- detail panel ---- */
	renderDetail: function() {
		var el = document.querySelector('.topo-detail-body');
		if (!el) return;
		var h = '';

		if (this.selectedNode && this.lastNodes) {
			var n = null;
			for (var i = 0; i < this.lastNodes.length; i++)
				if (this.lastNodes[i].id === this.selectedNode) { n = this.lastNodes[i]; break; }
			if (n) {
				h = '<div style="font-weight:bold;font-size:14px;margin-bottom:8px">' + esc(n.hostname) + '</div>';
				h += this._kv('Role', n.role);
				h += this._kv('Status', '<span style="color:' + (n.status === 'online' ? '#4caf50' : '#f44336') + '">' + esc(n.status) + '</span>');
				h += this._kv('IP', esc(n.ip || '-'));
				h += this._kv('MAC', esc(shortMac(n.mac)));
				h += this._kv('Model', esc(n.model || '-'));
				h += this._kv('Last Seen', fmtAgo(n.last_seen));
				h += this._kv('Clients', String(n.client_count || 0));
			}
		} else if (this.selectedLink && this.lastLinks) {
			var l = null;
			for (var j = 0; j < this.lastLinks.length; j++)
				if (this.lastLinks[j].id === this.selectedLink) { l = this.lastLinks[j]; break; }
			if (l && this.lastNodes) {
				var nm = function(id) {
					for (var k = 0; k < self.lastNodes.length; k++)
						if (self.lastNodes[k].id === id) return self.lastNodes[k].hostname;
					return shortId(id);
				};
				h = '<div style="font-weight:bold;font-size:14px;margin-bottom:8px">' + _('Link') + '</div>';
				h += this._kv('Source', esc(nm(l.source)));
				h += this._kv('Target', esc(nm(l.target)));
				h += this._kv('Type', '<span style="color:' + this.linkStyle(l).stroke + '">' + esc(l.type) + '</span>');
				h += this._kv('Status', esc(l.status));
				h += this._kv('Interface', esc(l.interface || '-'));
				if (l.tq) h += this._kv('TQ', String(l.tq));
				if (l.rssi) h += this._kv('RSSI', l.rssi + ' dBm');
				if (l.tx_rate) h += this._kv('TX Rate', Math.round(l.tx_rate / 1000) + ' Mbps');
			}
		} else {
			h = '<div style="color:#999;font-size:small;padding:20px 0;text-align:center">' +
				_('Tap a node or link<br/>to view details') + '</div>';
		}
		el.innerHTML = h;
	},

	_kv: function(k, v) {
		return '<div style="display:flex;justify-content:space-between;padding:4px 0;' +
			'border-bottom:1px solid #f0f0f0;font-size:12px">' +
			'<span style="color:#888">' + _(k) + '</span><span>' + v + '</span></div>';
	},

	renderStatus: function(data) {
		var el = document.querySelector('.topo-status');
		if (!el) return;
		var health = (data && data.batman_collect) || 'ok';
		var hc = health === 'ok' ? '#4caf50' : health === 'stale' ? '#ff9800' : '#f44336';
		el.innerHTML =
			'<span> nodes <b>' + (data && data.nodes ? data.nodes.length : 0) + '</b></span>' +
			'<span> links <b>' + (data && data.links ? data.links.length : 0) + '</b></span>' +
			'<span> data <b style="color:' + hc + '">' + health + '</b></span>';
	},

	/* ---- refresh (5s, preserves positions) ---- */
	refresh: function() {
		var self = this;
		callTopology().then(function(data) {
			if (!data || !data.nodes) return;
			self.lastNodes = data.nodes;
			self.lastLinks = data.links || [];
			self.lastData = data;
			var isNew = false;
			data.nodes.forEach(function(n) {
				if (!self.nodePos[n.id]) isNew = true;
			});
			if (isNew)
				self.autoLayout(data.nodes);
			else
				self.renderAll();
			self.renderDetail();
			self.renderStatus(data);
		}).catch(function() {});
	},

	/* ---- Pointer Events: unified mouse + touch ---- */
	bindEvents: function(svg) {
		var self = this;
		svg.style.touchAction = 'none';

		svg.addEventListener('pointerdown', function(evt) {
			var nodeEl = evt.target.closest ? evt.target.closest('.topo-node') : null;
			var linkEl = evt.target.closest ? evt.target.closest('.topo-link') : null;

			if (nodeEl) {
				self._pointers[evt.pointerId] = { type: 'node', el: nodeEl };
				self.dragTarget = nodeEl.getAttribute('data-id');
				var pt = self.clientToSvg(evt.clientX, evt.clientY);
				var p = self.nodePos[self.dragTarget];
				if (p) self.dragStart = { ox: pt.x - p.x, oy: pt.y - p.y };
				self.selectedNode = self.dragTarget;
				self.selectedLink = null;
				self.renderDetail();
				self.renderAll();
			} else if (linkEl) {
				self._pointers[evt.pointerId] = { type: 'link', el: linkEl };
				self.selectedLink = linkEl.getAttribute('data-id');
				self.selectedNode = null;
				self.renderDetail();
				self.renderAll();
			} else {
				self._pointers[evt.pointerId] = { type: 'pan' };
				self.dragStart = { x: evt.clientX, y: evt.clientY, px: self.pan.x, py: self.pan.y };
			}
			svg.setPointerCapture && svg.setPointerCapture(evt.pointerId);
			evt.preventDefault();
		});

		svg.addEventListener('pointermove', function(evt) {
			var ptr = self._pointers[evt.pointerId];
			if (!ptr) return;

			var ptrs = Object.keys(self._pointers).map(function(k) { return self._pointers[k]; });

			/* two-finger pinch zoom */
			if (ptrs.length === 2 && ptrs[0].type === 'pan' && ptrs[1].type === 'pan') {
				var ids = Object.keys(self._pointers);
				var p1 = self._pointers[ids[0]], p2 = self._pointers[ids[1]];
				if (!p1.cx) { p1.cx = p1.lastX || evt.clientX; p1.cy = p1.lastY || evt.clientY; }
				if (!p2.cx) { p2.cx = evt.clientX; p2.cy = evt.clientY; }

				var d1 = Math.sqrt(Math.pow(p1.cx - p2.cx, 2) + Math.pow(p1.cy - p2.cy, 2));
				var d2 = Math.sqrt(Math.pow((p1.lastX||p1.cx) - evt.clientX, 2) + Math.pow((p1.lastY||p2.cy) - evt.clientY, 2));

				if (self._pinchDist > 0 && d1 > 0) {
					var factor = d1 / self._pinchDist;
					var nz = self.zoom * factor;
					if (nz >= ZOOM_MIN && nz <= ZOOM_MAX) {
						self.zoom = nz;
						self.applyTransform();
					}
				}
				self._pinchDist = d1;
				/* update tracked positions */
				p1.lastX = p1.cx; p1.lastY = p1.cy;
				if (self._pointers[evt.pointerId] === p2) { p2.lastX = evt.clientX; p2.lastY = evt.clientY; }
				else { p2.lastX = p2.cx; p2.lastY = p2.cy; }
				return;
			}

			if (ptr.type === 'node' && self.dragTarget) {
				var pt = self.clientToSvg(evt.clientX, evt.clientY);
				var p = self.nodePos[self.dragTarget];
				if (p) {
					p.x = pt.x - self.dragStart.ox;
					p.y = pt.y - self.dragStart.oy;
					var el = self.svgGroup.querySelector('.topo-node[data-id="' + self.dragTarget + '"]');
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
				self.dragTarget = null;
			}
		};
		svg.addEventListener('pointerup', endPointer);
		svg.addEventListener('pointercancel', endPointer);

		/* wheel zoom (desktop) */
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

		/* responsive: use viewport width to decide layout */
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
				'class': 'btn cbi-button cbi-button-neutral',
				'style': 'padding:4px 12px;font-size:12px',
				'click': function() { self.refresh(); }
			}, _('↻')),
			E('span', { 'style': 'color:#888;font-size:small;margin-left:4px' }, _('Zoom:')),
			E('span', { 'class': 'zoom-label', 'style': 'font-size:small;font-weight:bold;min-width:36px' }, '100%')
		]);

		var svgInner =
			'<g class="topo-viewport">' +
			'<g class="link-layer"></g>' +
			'<g class="node-layer"></g>' +
			'</g>';

		var canvasWrap = E('div', {
			'style': isNarrow
				? 'width:100%;height:320px;background:#f8f9fa;border:1px solid #e0e0e0;overflow:hidden;position:relative'
				: 'flex:1;min-height:420px;background:#f8f9fa;border:1px solid #e0e0e0;overflow:hidden;position:relative',
		}, E('svg', {
			'viewBox': '0 0 760 480',
			'width': '100%',
			'height': '100%',
			'style': 'display:block;touch-action:none'
		}, svgInner));

		var detailPanel = E('div', {
			'style': isNarrow
				? 'width:100%;max-height:200px;background:#fff;border:1px solid #e0e0e0;border-top:none;border-radius:0 0 8px 8px;padding:10px;overflow-y:auto'
				: 'width:250px;background:#fff;border:1px solid #e0e0e0;border-left:none;border-radius:0 8px 8px 0;padding:10px;overflow-y:auto'
		}, [
			E('div', { 'class': 'topo-detail-body' },
			  E('div', { 'style': 'color:#999;font-size:small;text-align:center;padding:30px 0' },
			    _('Tap a node or link to view details')))
		]);

		var mainArea = E('div', { 'style': isNarrow ? '' : 'display:flex' }, [canvasWrap, detailPanel]);

		var statusBar = E('div', {
			'class': 'topo-status',
			'style': 'padding:6px 12px;background:#f5f5f5;border:1px solid #e0e0e0;border-top:none;border-radius:0 0 8px 8px;font-size:12px;display:flex;gap:16px;justify-content:center'
		});

		var container = E('div', {}, [toolbar, mainArea, statusBar]);

		/* wire up after DOM insertion */
		requestAnimationFrame(function() {
			self.svgRoot = canvasWrap.querySelector('svg');
			self.svgGroup = self.svgRoot.querySelector('.topo-viewport');
			self.bindEvents(self.svgRoot);
			self.autoLayout(data.nodes);
			self.renderStatus(data);
			poll.add(L.bind(self.refresh, self), POLL_INTERVAL);
		});

		return container;
	}
});
