// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * OpenMesh Manager interactive topology canvas (Phase 5 MVP).
 *
 * SVG-based: nodes draggable, canvas pannable, wheel zoom, auto-layout,
 * node/link detail panel, 5s polling refresh preserving positions.
 * All data from ubus "openmesh" "topology" — no direct batctl/iw.
 *
 * Network state (nodes/links/metrics) and UI layout (positions) are
 * deliberately separate objects; refreshes update data, not layout.
 */
'use strict';
'require view';
'require rpc';
'require poll';

var callTopology = rpc.declare({ object: 'openmesh', method: 'topology' });

var ZOOM_MIN = 0.3, ZOOM_MAX = 3.0;
var NODE_W = 110, NODE_H = 52;

function esc(s) {
	return String(s || '').replace(/&/g, '&amp;')
		.replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function shortId(id) {
	return id ? id.substring(0, 12) + '…' : '';
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
	canvasW: 760, canvasH: 500,
	lastNodes: null,
	lastLinks: null,

	load: function() {
		return L.resolveDefault(callTopology(), {});
	},

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
			var ang = 2 * Math.PI * i / Math.max(agents.length, 1) + Math.PI / 2;
			var r = agents.length > 3 ? 200 : 160;
			self.nodePos[n.id] = {
				x: cx + Math.cos(ang) * r,
				y: cy + Math.abs(Math.sin(ang)) * r + 80
			};
		});
		self.applyTransform();
		self.renderNodes(nodes);
		self.renderLinks(nodes, self.lastLinks || []);
	},

	fitScreen: function() {
		this.zoom = 1;
		this.pan = { x: 0, y: 0 };
		this.applyTransform();
	},

	applyTransform: function() {
		if (!this.svgGroup) return;
		this.svgGroup.setAttribute('transform',
			'translate(%d,%d) scale(%f)'.format(this.pan.x, this.pan.y, this.zoom));
		var zl = document.querySelector('.zoom-label');
		if (zl) zl.textContent = Math.round(this.zoom * 100) + '%';
	},

	clientToSvg: function(evt) {
		var rect = this.svgRoot.getBoundingClientRect();
		return {
			x: (evt.clientX - rect.left - this.pan.x) / this.zoom,
			y: (evt.clientY - rect.top - this.pan.y) / this.zoom
		};
	},

	nodeColor: function(n) {
		if (n.status === 'online') return n.role === 'controller' ? '#2d6b2d' : '#3aa835';
		if (n.status === 'offline') return '#c0504d';
		return '#888';
	},

	linkStyle: function(l) {
		switch (l.type) {
		case 'ethernet': return { stroke: '#555', dash: 'none', width: 3 };
		case '80211s': return { stroke: '#4a90d9', dash: '6,4', width: 2 };
		case 'batman': return { stroke: '#8B5CF6', dash: '2,3', width: 2 };
		default: return { stroke: '#999', dash: '3,3', width: 1 };
		}
	},

	linkLabel: function(l) {
		var parts = [];
		if (l.tq) parts.push('TQ:' + l.tq);
		if (l.rssi) parts.push(l.rssi + 'dBm');
		return parts.join(' ');
	},

	renderNodes: function(nodes) {
		var self = this;
		var g = this.svgGroup.querySelector('.node-layer');
		if (!g) return;
		g.innerHTML = '';
		nodes.forEach(function(n) {
			var p = self.nodePos[n.id];
			if (!p) {
				p = self.nodePos[n.id] = { x: self.canvasW / 2, y: self.canvasH / 2 };
			}
			var fill = self.nodeColor(n);
			var isSel = self.selectedNode === n.id;
			var el = document.createElementNS('http://www.w3.org/2000/svg', 'g');
			el.setAttribute('class', 'topo-node');
			el.setAttribute('data-id', n.id);
			el.setAttribute('transform', 'translate(%d,%d)'.format(p.x, p.y));
			el.style.cursor = 'move';
			el.innerHTML =
				'<rect x="%d" y="%d" width="%d" height="%d" rx="10" fill="%s" stroke="%s" stroke-width="%d"/>'.format(
					-NODE_W/2, -NODE_H/2, NODE_W, NODE_H, fill,
					isSel ? '#ff9800' : 'rgba(0,0,0,0.2)', isSel ? 3 : 1) +
				'<text x="0" y="-8" text-anchor="middle" fill="#fff" font-size="12" font-weight="bold">%s</text>'.format(esc(n.hostname)) +
				'<text x="0" y="6" text-anchor="middle" fill="rgba(255,255,255,0.8)" font-size="10">%s</text>'.format(esc(n.role)) +
				'<text x="0" y="18" text-anchor="middle" fill="rgba(255,255,255,0.6)" font-size="9">%s</text>'.format(esc(n.ip || shortId(n.id)));
			g.appendChild(el);
		});
	},

	renderLinks: function(nodes, links) {
		var self = this;
		var g = this.svgGroup.querySelector('.link-layer');
		if (!g) return;
		g.innerHTML = '';
		links.forEach(function(l) {
			var src = self.nodePos[l.source];
			var dst = self.nodePos[l.target];
			if (!src || !dst) return;
			var st = self.linkStyle(l);
			var mx = (src.x + dst.x) / 2, my = (src.y + dst.y) / 2;
			var isSel = self.selectedLink === l.id;
			var el = document.createElementNS('http://www.w3.org/2000/svg', 'g');
			el.setAttribute('class', 'topo-link');
			el.setAttribute('data-id', l.id);
			el.style.cursor = 'pointer';
			el.innerHTML =
				'<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="%d" stroke-dasharray="%s" opacity="%s"/>'.format(
					src.x, src.y, dst.x, dst.y, st.stroke,
					isSel ? st.width + 2 : st.width, st.dash, isSel ? '1.0' : '0.7') +
				'<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="transparent" stroke-width="16"/>'.format(
					src.x, src.y, dst.x, dst.y) +
				(self.linkLabel(l) ?
				'<text x="%d" y="%d" text-anchor="middle" fill="%s" font-size="9">%s</text>'.format(
					mx, my - 6, st.stroke, esc(self.linkLabel(l)))
				: '');
			g.appendChild(el);
		});
	},

	renderDetail: function() {
		var el = document.querySelector('.topo-detail');
		if (!el) return;
		var html = '';
		if (this.selectedNode && this.lastNodes) {
			var n = null;
			for (var i = 0; i < this.lastNodes.length; i++)
				if (this.lastNodes[i].id === this.selectedNode) { n = this.lastNodes[i]; break; }
			if (n) {
				html = '<h4>%s</h4><table class="table">'.format(esc(n.hostname));
				[['Role', n.role], ['Status', n.status],
				 ['IP', n.ip || '-'], ['MAC', n.mac || '-'],
				 ['Model', n.model || '-'],
				 ['Last Seen', n.last_seen ? new Date(n.last_seen * 1000).toLocaleTimeString() : '-'],
				 ['Clients', String(n.client_count || 0)],
				 ['Uptime', n.uptime ? Math.floor(n.uptime / 60) + ' min' : '-']
				].forEach(function(r) {
					html += '<tr><td style="color:#888;width:40%%">%s</td><td>%s</td></tr>'.format(_(r[0]), esc(r[1]));
				});
				html += '</table>';
			}
		} else if (this.selectedLink && this.lastLinks && this.lastNodes) {
			var l = null;
			for (var j = 0; j < this.lastLinks.length; j++)
				if (this.lastLinks[j].id === this.selectedLink) { l = this.lastLinks[j]; break; }
			if (l) {
				var nameOf = function(id) {
					for (var k = 0; k < self.lastNodes.length; k++)
						if (self.lastNodes[k].id === id) return self.lastNodes[k].hostname;
					return shortId(id);
				};
				html = '<h4>%s</h4><table class="table">'.format(_('Link Details'));
				[['Source', nameOf(l.source)], ['Target', nameOf(l.target)],
				 ['Type', l.type], ['Status', l.status],
				 ['Interface', l.interface || '-'],
				 ['TQ', l.tq ? String(l.tq) : '-'],
				 ['RSSI', l.rssi ? l.rssi + ' dBm' : '-'],
				 ['TX Rate', l.tx_rate ? Math.round(l.tx_rate / 1000) + ' Mbps' : '-']
				].forEach(function(r) {
					html += '<tr><td style="color:#888;width:40%%">%s</td><td>%s</td></tr>'.format(_(r[0]), esc(r[1]));
				});
				html += '</table>';
			}
		} else {
			html = '<p style="color:#999;font-size:small">%s</p>'.format(
				_('Click a node or link to view details'));
		}
		el.innerHTML = html;
	},

	renderStatus: function(data) {
		var el = document.querySelector('.topo-status');
		if (!el) return;
		var nNodes = (data.nodes || []).length;
		var nLinks = (data.links || []).length;
		var health = data.batman_collect || 'ok';
		var hc = health === 'ok' ? '#4caf50' : health === 'stale' ? '#ff9800' : '#f44336';
		el.innerHTML =
			'<span>Nodes: <b>%d</b></span>  <span>Links: <b>%d</b></span>  '.format(nNodes, nLinks) +
			'<span>Data: <b style="color:%s">%s</b></span>'.format(hc, health);
	},

	refresh: function() {
		var self = this;
		callTopology().then(function(data) {
			if (!data || !data.nodes) return;
			self.lastNodes = data.nodes;
			self.lastLinks = data.links || [];
			var needLayout = false;
			data.nodes.forEach(function(n) {
				if (!self.nodePos[n.id]) needLayout = true;
			});
			if (needLayout)
				self.autoLayout(data.nodes);
			else {
				self.renderNodes(data.nodes);
				self.renderLinks(data.nodes, self.lastLinks);
			}
			self.renderDetail();
			self.renderStatus(data);
		}).catch(function() {});
	},

	bindEvents: function(svgRoot) {
		var self = this;

		svgRoot.addEventListener('mousedown', function(evt) {
			evt.preventDefault();
			var nodeEl = evt.target.closest('.topo-node');
			var linkEl = evt.target.closest('.topo-link');
			if (nodeEl) {
				self.dragMode = 'node';
				self.dragTarget = nodeEl.getAttribute('data-id');
				var pt = self.clientToSvg(evt);
				var p = self.nodePos[self.dragTarget];
				if (p) {
					self.dragStart = { offX: pt.x - p.x, offY: pt.y - p.y };
				}
				self.selectedNode = self.dragTarget;
				self.selectedLink = null;
				self.renderDetail();
				if (self.lastNodes) {
					self.renderNodes(self.lastNodes);
					self.renderLinks(self.lastNodes, self.lastLinks || []);
				}
			} else if (linkEl) {
				self.selectedLink = linkEl.getAttribute('data-id');
				self.selectedNode = null;
				self.renderDetail();
				if (self.lastNodes) {
					self.renderNodes(self.lastNodes);
					self.renderLinks(self.lastNodes, self.lastLinks || []);
				}
			} else {
				self.dragMode = 'pan';
				self.dragStart = {
					x: evt.clientX, y: evt.clientY,
					px: self.pan.x, py: self.pan.y
				};
			}
		});

		window.addEventListener('mousemove', function(evt) {
			if (!self.dragMode) return;
			if (self.dragMode === 'node' && self.dragTarget) {
				var pt = self.clientToSvg(evt);
				var p = self.nodePos[self.dragTarget];
				if (p) {
					p.x = pt.x - self.dragStart.offX;
					p.y = pt.y - self.dragStart.offY;
					var el = self.svgGroup.querySelector('.topo-node[data-id="%s"]'.format(self.dragTarget));
					if (el) el.setAttribute('transform', 'translate(%d,%d)'.format(p.x, p.y));
					if (self.lastNodes)
						self.renderLinks(self.lastNodes, self.lastLinks || []);
				}
			} else if (self.dragMode === 'pan') {
				self.pan.x = self.dragStart.px + (evt.clientX - self.dragStart.x);
				self.pan.y = self.dragStart.py + (evt.clientY - self.dragStart.y);
				self.applyTransform();
			}
		});

		window.addEventListener('mouseup', function() {
			self.dragMode = null;
			self.dragTarget = null;
		});

		svgRoot.addEventListener('wheel', function(evt) {
			evt.preventDefault();
			var rect = svgRoot.getBoundingClientRect();
			var mx = evt.clientX - rect.left, my = evt.clientY - rect.top;
			var factor = evt.deltaY < 0 ? 1.1 : 0.9;
			var nz = self.zoom * factor;
			if (nz < ZOOM_MIN || nz > ZOOM_MAX) return;
			self.pan.x = mx - (mx - self.pan.x) * (nz / self.zoom);
			self.pan.y = my - (my - self.pan.y) * (nz / self.zoom);
			self.zoom = nz;
			self.applyTransform();
		}, { passive: false });

		window.addEventListener('keydown', function(evt) {
			if (evt.key === 'Escape') {
				self.selectedNode = null;
				self.selectedLink = null;
				self.renderDetail();
				if (self.lastNodes) {
					self.renderNodes(self.lastNodes);
					self.renderLinks(self.lastNodes, self.lastLinks || []);
				}
			}
		});
	},

	render: function(data) {
		var self = this;

		if (!data || !data.nodes) {
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('OpenMesh agent node')),
				E('p', { 'style': 'color:#666' },
					_('Topology is rendered on the controller. Open the OpenMesh pages there.'))
			]);
		}

		self.lastNodes = data.nodes;
		self.lastLinks = data.links || [];

		var toolbar = E('div', { 'style': 'padding:8px 12px;display:flex;gap:8px;align-items:center;background:#fff;border:1px solid #e0e0e0;border-bottom:none;border-radius:8px 8px 0 0' }, [
			E('button', {
				'class': 'btn cbi-button cbi-button-apply',
				'click': function() { self.autoLayout(self.lastNodes); }
			}, _('Auto Layout')),
			E('button', {
				'class': 'btn cbi-button cbi-button-neutral',
				'click': function() { self.fitScreen(); }
			}, _('Fit Screen')),
			E('button', {
				'class': 'btn cbi-button cbi-button-neutral',
				'click': function() { self.refresh(); }
			}, _('Refresh')),
			E('span', { 'style': 'color:#888;font-size:small;margin-left:8px' }, _('Zoom:')),
			E('span', { 'class': 'zoom-label', 'style': 'font-size:small;font-weight:bold;min-width:40px' }, '100%')
		]);

		var svgInner =
			'<g class="topo-viewport">' +
			'<g class="link-layer"></g>' +
			'<g class="node-layer"></g>' +
			'</g>';

		var canvasWrap = E('div', {
			'style': 'flex:1;background:#f8f9fa;border:1px solid #e0e0e0;overflow:hidden;position:relative;min-height:420px'
		}, E('svg', {
			'viewBox': '0 0 760 500',
			'width': '100%',
			'height': '100%',
			'style': 'display:block;touch-action:none'
		}, svgInner));

		var detailPanel = E('div', {
			'class': 'topo-detail',
			'style': 'width:260px;background:#fff;border:1px solid #e0e0e0;border-left:none;border-radius:0 8px 8px 0;padding:12px;overflow-y:auto'
		}, E('p', { 'style': 'color:#999;font-size:small' }, _('Click a node or link to view details')));

		var mainArea = E('div', { 'style': 'display:flex' }, [canvasWrap, detailPanel]);

		var statusBar = E('div', {
			'class': 'topo-status',
			'style': 'padding:6px 12px;background:#f0f0f0;border:1px solid #e0e0e0;border-top:none;border-radius:0 0 8px 8px;font-size:small;display:flex;gap:16px'
		});

		var container = E('div');
		container.appendChild(toolbar);
		container.appendChild(mainArea);
		container.appendChild(statusBar);

		setTimeout(function() {
			self.svgRoot = canvasWrap.querySelector('svg');
			self.svgGroup = self.svgRoot.querySelector('.topo-viewport');
			self.bindEvents(self.svgRoot);
			self.autoLayout(data.nodes);
			self.renderLinks(data.nodes, self.lastLinks);
			self.renderStatus(data);
			poll.add(L.bind(self.refresh, self), 5);
		}, 50);

		return container;
	}
});
