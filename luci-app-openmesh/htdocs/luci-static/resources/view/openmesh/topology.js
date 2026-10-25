// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * OpenMesh Manager topology v3: two views on one page.
 *   - Network topology: controller-centric managed-network graph
 *     (adopted ring, foreign peers, pending strip) + backhaul table.
 *   - Mesh topology: 802.11s/batman-adv logical graph (mesh cloud,
 *     RSSI-colored peer edges, originator next-hop labels).
 * Links bind to nodes via announced MACs (mesh iface first, br-lan
 * fallback); all drawing is hand-rolled SVG (no graph libraries).
 */
'use strict';
'require view';
'require rpc';
'require poll';

var callStatus    = rpc.declare({ object: 'openmesh', method: 'status' });
var callAdopted   = rpc.declare({ object: 'openmesh', method: 'list_adopted' });
var callNodes     = rpc.declare({ object: 'openmesh', method: 'list_nodes' });
var callPending   = rpc.declare({ object: 'openmesh', method: 'list_pending' });
var callLinks     = rpc.declare({ object: 'openmesh', method: 'get_links' });
var callMeshInfo  = rpc.declare({ object: 'openmesh', method: 'get_mesh_info' });

function esc(s) {
	return String(s || '').replace(/&/g, '&amp;')
		.replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function rssiColor(rssi) {
	if (!rssi) return '#888';
	if (rssi >= -60) return '#2e9e2e';
	if (rssi >= -75) return '#e0a800';
	return '#c0504d';
}

return view.extend({
	/* bind links to nodes by announced MAC; returns
	 * { bound: [{link, node}], foreign: [link] } */
	bindLinks: function(links, nodes, adopted) {
		var byMac = {};
		var res = { bound: [], foreign: [] };

		(nodes || []).forEach(function(n) {
			if (n.mac) byMac[n.mac] = n;
		});
		(links || []).forEach(function(l) {
			var n = byMac[l.peer];
			if (n)
				res.bound.push({ link: l, node: n });
			else
				res.foreign.push(l);
		});
		return res;
	},

	/* --- view 1: network topology (managed network as a whole) --- */
	draw: function(st, adopted, pending, nodes, links) {
		var W = 760, H = 470, cx = W / 2, cy = H / 2;
		var mesh = adopted.nodes || [];
		var pend = pending.nodes || [];
		var bind = this.bindLinks(links.links || [], nodes.nodes, mesh);
		var svg = '';
		var pos = {};

		/* controller card */
		svg += '<rect x="%d" y="%d" width="104" height="44" rx="8" fill="#2d6b2d"/>'.format(cx - 52, cy - 22);
		svg += '<text x="%d" y="%d" text-anchor="middle" fill="#fff" font-size="11" font-weight="bold">CTRL</text>'.format(cx, cy - 4);
		svg += '<text x="%d" y="%d" text-anchor="middle" fill="#cfe6cf" font-size="8">%s</text>'
			.format(cx, cy + 12, esc((st.node_id || '').substring(0, 10)) + '…');

		/* adopted nodes on inner ring */
		mesh.forEach(function(n, i) {
			var ang = 2 * Math.PI * i / Math.max(mesh.length, 1) - Math.PI / 2;
			pos[n.id] = { x: cx + Math.cos(ang) * 155, y: cy + Math.sin(ang) * 155 };
		});

		/* bound links: quality-colored edges + labels */
		bind.bound.forEach(function(b) {
			var p = pos[b.node.id];
			var l = b.link;
			var col = l.type == 'batman' ? '#3a6ea5' : rssiColor(l.rssi);
			var tip = '%s → %s (%s%s%s)'.format(
				esc(b.node.name), esc(l.peer), l.type,
				l.rssi ? ', ' + l.rssi + ' dBm' : '',
				l.rate_kbps ? ', ' + l.rate_kbps + ' kbit/s' : '');

			svg += '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="%d"><title>%s</title></line>'
				.format(cx, cy, p.x, p.y, col,
					b.node.online ? 3 : 1.2, tip);
			var mx = (cx + p.x) / 2, my = (cy + p.y) / 2;
			var label = l.type == 'batman'
				? 'batman' + (l.via ? ' · ' + esc(l.via) : '')
				: (l.rssi ? l.rssi + ' dBm' : 'wifi');
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="%s" font-size="9">%s</text>'
				.format(mx, my - 4, col, label);
		});

		/* adopted node cards */
		mesh.forEach(function(n) {
			var p = pos[n.id];
			var col = n.online ? '#3aa835' : '#c0504d';

			svg += '<circle cx="%d" cy="%d" r="21" fill="%s"><title>%s\n%s</title></circle>'
				.format(p.x, p.y, col,
					esc(n.name), esc(n.id));
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="#fff" font-size="9">%s</text>'
				.format(p.x, p.y + 3, esc(n.name).substring(0, 10));
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="#555" font-size="8">%s</text>'
				.format(p.x, p.y + 34, n.online ? _('online') : _('offline'));
		});

		/* foreign links: unmatched peer MACs on an outer dashed ring */
		bind.foreign.forEach(function(l, i) {
			var ang = 2 * Math.PI * i / Math.max(bind.foreign.length, 1) - Math.PI / 2;
			var x = cx + Math.cos(ang) * 250, y = cy + Math.sin(ang) * 250;

			svg += '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#999" stroke-dasharray="5 4" stroke-width="1.5"><title>%s</title></line>'
				.format(cx, cy, x, y, esc(l.peer));
			svg += '<circle cx="%d" cy="%d" r="12" fill="none" stroke="#999" stroke-dasharray="4 3"/>'.format(x, y);
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="#777" font-size="8">%s</text>'
				.format(x, y + 26, esc(l.peer.substring(0, 11)) + '…');
		});

		/* pending (discovered, not adopted) nodes: bottom strip */
		pend.forEach(function(n, i) {
			var x = 70 + i * 160, y = H - 26;

			svg += '<circle cx="%d" cy="%d" r="9" fill="none" stroke="#b60" stroke-dasharray="3 3"/>'.format(x, y);
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="#865" font-size="8">%s</text>'
				.format(x + 14, y + 3, esc(n.model || '').substring(0, 14));
		});

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Network topology')),
			E('div', { 'style': 'text-align:center' },
				E('svg', {
					'viewBox': '0 0 %d %d'.format(W, H),
					'width': '100%',
					'max-width': '%d'.format(W),
					'style': 'background:#fafafa;border:1px solid #e0e0e0;border-radius:6px'
				}, svg)),
			E('p', { 'style': 'color:#666;font-size:small' }, [
				E('span', { 'style': 'color:#2e9e2e' }, '⬤'), ' ≥ -60 dBm  ',
				E('span', { 'style': 'color:#e0a800' }, '⬤'), ' -60 ~ -75 dBm  ',
				E('span', { 'style': 'color:#c0504d' }, '⬤'), ' < -75 dBm  ',
				E('span', { 'style': 'color:#3a6ea5' }, '⬤'), ' batman-adv  ',
				'◌ ', _('Discovered, not adopted')
			])
		]);
	},

	/* --- view 2: mesh topology (802.11s / batman-adv logical net) --- */
	drawMesh: function(st, adopted, pending, nodes, links, minfo) {
		if (!minfo || !minfo.configured)
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Mesh topology')),
				E('p', {}, _('Mesh network not created yet')),
				E('p', { 'style': 'color:#666' },
					_('Use the Mesh Wizard to create or join a mesh network.')),
				E('p', {},
					E('a', {
						'class': 'cbi-button',
						'href': L.url('admin/network/openmesh/wizard')
					}, _('Mesh Wizard')))
			]);

		var W = 760, H = 500, cx = W / 2, cy = 235;
		var mesh = adopted.nodes || [];
		var bind = this.bindLinks(links.links || [], nodes.nodes, mesh);
		var svg = '';
		var pos = {};

		/* mesh cloud: mesh_id + band/mode (+ live iface name) */
		var sub = (minfo.band || '?') + ' · ' + (minfo.mode || 'wifi') +
			(minfo.mesh_ifname ? ' · ' + esc(minfo.mesh_ifname) : '');
		svg += '<rect x="%d" y="24" width="240" height="54" rx="16" fill="#eef4fb" stroke="#3a6ea5" stroke-width="1.5" stroke-dasharray="7 4"/>'.format(cx - 120);
		svg += '<text x="%d" y="46" text-anchor="middle" fill="#2c5f8a" font-size="12" font-weight="bold">%s</text>'
			.format(cx, 46, esc(minfo.mesh_id));
		svg += '<text x="%d" y="64" text-anchor="middle" fill="#5a7a99" font-size="9">%s</text>'
			.format(cx, 64, sub);

		/* controller under the cloud */
		svg += '<line x1="%d" y1="78" x2="%d" y2="%d" stroke="#3a6ea5" stroke-width="2"/>'
			.format(cx, cx, cy - 24);
		svg += '<rect x="%d" y="%d" width="104" height="44" rx="8" fill="#2d6b2d"/>'.format(cx - 52, cy - 22);
		svg += '<text x="%d" y="%d" text-anchor="middle" fill="#fff" font-size="11" font-weight="bold">CTRL</text>'.format(cx, cy - 4);
		svg += '<text x="%d" y="%d" text-anchor="middle" fill="#cfe6cf" font-size="8">%s</text>'
			.format(cx, cy + 12, esc((st.node_id || '').substring(0, 10)) + '…');

		/* participating nodes: semicircle below the controller */
		bind.bound.forEach(function(b, i) {
			var ang = Math.PI * (i + 0.5) / Math.max(bind.bound.length, 1);

			pos[b.node.id + '|' + b.link.type] = {
				x: cx + Math.cos(ang) * 150,
				y: cy + Math.sin(ang) * 150
			};
		});

		/* peer edges: wifi = RSSI-colored direct link, batman = blue path */
		bind.bound.forEach(function(b) {
			var l = b.link;
			var p = pos[b.node.id + '|' + l.type];
			var col = l.type == 'batman' ? '#3a6ea5' : rssiColor(l.rssi);
			var tip = '%s → %s (%s%s%s)'.format(
				esc(b.node.name), esc(l.peer), l.type,
				l.rssi ? ', ' + l.rssi + ' dBm' : '',
				l.rate_kbps ? ', ' + l.rate_kbps + ' kbit/s' : '');

			svg += '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="3"><title>%s</title></line>'
				.format(cx, cy, p.x, p.y, col, tip);
			var mx = (cx + p.x) / 2, my = (cy + p.y) / 2;
			var label = l.type == 'batman'
				? (l.via ? esc(l.via) : 'batman')
				: (l.rssi ? l.rssi + ' dBm' : '802.11s');
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="%s" font-size="9">%s</text>'
				.format(mx, my + (l.type == 'batman' ? 10 : -4), col, label);
		});

		/* node cards */
		bind.bound.forEach(function(b) {
			var l = b.link;
			var p = pos[b.node.id + '|' + l.type];
			var col = l.type == 'batman' ? '#3a6ea5' : '#3aa835';

			svg += '<circle cx="%d" cy="%d" r="20" fill="%s"><title>%s\n%s</title></circle>'
				.format(p.x, p.y, col, esc(b.node.name), esc(l.peer));
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="#fff" font-size="9">%s</text>'
				.format(p.x, p.y + 3, esc(b.node.name).substring(0, 10));
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="#555" font-size="8">%s</text>'
				.format(p.x, p.y + 33, esc(l.peer.substring(0, 11)) + '…');
		});

		/* foreign peers (seen on the mesh, MAC not announced by a
		 * managed node) on an outer dashed arc */
		bind.foreign.forEach(function(l, i) {
			var ang = Math.PI * (i + 0.5) / Math.max(bind.foreign.length, 1);
			var x = cx + Math.cos(ang) * 235, y = cy + Math.sin(ang) * 235;

			svg += '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#999" stroke-dasharray="5 4" stroke-width="1.5"><title>%s</title></line>'
				.format(cx, cy, x, y, esc(l.peer));
			svg += '<circle cx="%d" cy="%d" r="12" fill="none" stroke="#999" stroke-dasharray="4 3"/>'.format(x, y);
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="#777" font-size="8">%s</text>'
				.format(x, y + 26, esc(l.peer.substring(0, 11)) + '…');
		});

		if (!bind.bound.length && !bind.foreign.length)
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="#999" font-size="10">%s</text>'
				.format(cx, cy + 150, _('Waiting for peers (single node)'));

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Mesh topology')),
			E('div', { 'style': 'text-align:center' },
				E('svg', {
					'viewBox': '0 0 %d %d'.format(W, H),
					'width': '100%',
					'max-width': '%d'.format(W),
					'style': 'background:#fafafa;border:1px solid #e0e0e0;border-radius:6px'
				}, svg)),
			E('p', { 'style': 'color:#666;font-size:small' }, [
				E('span', { 'style': 'color:#2e9e2e' }, '⬤'), ' ≥ -60 dBm  ',
				E('span', { 'style': 'color:#e0a800' }, '⬤'), ' -60 ~ -75 dBm  ',
				E('span', { 'style': 'color:#c0504d' }, '⬤'), ' < -75 dBm  ',
				E('span', { 'style': 'color:#3a6ea5' }, '⬤'), ' batman-adv  ',
				'◌ ', _('Peer not managed by this controller')
			])
		]);
	},

	renderLinks: function(links) {
		var rows = (links.links || []).map(function(l) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, l.type),
				E('td', { 'class': 'td' }, l.ifname || l.via || '-'),
				E('td', { 'class': 'td' }, l.peer),
				E('td', { 'class': 'td' },
				  l.rssi ? _('%d dBm').format(l.rssi) : '-'),
				E('td', { 'class': 'td' },
				  l.rate_kbps ? _('%d kbit/s').format(l.rate_kbps) : '-')
			]);
		});

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Backhaul links')),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('Type')),
					E('th', { 'class': 'th' }, _('Interface')),
					E('th', { 'class': 'th' }, _('Peer')),
					E('th', { 'class': 'th' }, _('RSSI')),
					E('th', { 'class': 'th' }, _('Rate'))
				]),
				rows.length ? rows :
					E('tr', { 'class': 'tr placeholder' }, [
						E('td', { 'class': 'td center', colspan: 5 },
						  _('No mesh links (single node or not deployed)'))
					])
			])
		]);
	},

	load: function() {
		return this.fetchAll();
	},

	fetchAll: function() {
		return Promise.all([
			L.resolveDefault(callStatus(), {}),
			L.resolveDefault(callAdopted(), { nodes: [] }),
			L.resolveDefault(callPending(), { nodes: [] }),
			L.resolveDefault(callNodes(), { nodes: [] }),
			L.resolveDefault(callLinks(), { links: [] }),
			L.resolveDefault(callMeshInfo(), {})
		]);
	},

	render: function(data) {
		var self = this;
		var lastData = null;
		var tabs = E('ul', { 'class': 'cbi-tabmenu cbi-tabmenu-network' });
		var body = E('div');

		if (!data[0] || !data[0].node_id)
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('OpenMesh agent node')),
				E('p', {},
					_('This device runs as a mesh agent (or the openmeshd service is unreachable).')),
				E('p', { 'style': 'color:#666' },
					_('Mesh topology is rendered on the controller. Open the OpenMesh pages there.'))
			]);

		this.mode = this.mode || 'net';

		function renderTabs() {
			var items = [ [ 'net', _('Network topology') ],
				      [ 'mesh', _('Mesh topology') ] ];

			tabs.innerHTML = '';
			items.forEach(function(m) {
				tabs.appendChild(E('li', {
					'class': self.mode == m[0] ? 'cbi-tab' : 'cbi-tab-disabled'
				}, E('a', {
					'href': '#',
					'click': function(ev) {
						ev.preventDefault();
						self.mode = m[0];
						renderTabs();
						renderBody();
					}
				}, m[1])));
			});
		}

		function renderBody() {
			var d = lastData;

			body.innerHTML = '';
			if (!d)
				return;
			if (self.mode == 'mesh')
				body.appendChild(self.drawMesh(d[0], d[1], d[2], d[3], d[4], d[5]));
			else {
				body.appendChild(self.draw(d[0], d[1], d[2], d[3], d[4]));
				body.appendChild(self.renderLinks(d[4]));
			}
		}

		function refresh() {
			return self.fetchAll().then(function(d) {
				lastData = d;
				renderTabs();
				renderBody();
			});
		}

		refresh();
		poll.add(refresh, 5);
		return E('div', {}, [ tabs, body ]);
	}
});
