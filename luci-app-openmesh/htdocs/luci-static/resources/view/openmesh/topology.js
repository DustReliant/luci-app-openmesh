// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * OpenMesh Manager topology v2: controller-centric graph with real
 * backhaul links bound to nodes via announced MACs (mesh iface first,
 * br-lan fallback), RSSI-quality coloring and hover details.
 */
'use strict';
'require view';
'require rpc';
'require poll';

var callStatus  = rpc.declare({ object: 'openmesh', method: 'status' });
var callAdopted = rpc.declare({ object: 'openmesh', method: 'list_adopted' });
var callNodes   = rpc.declare({ object: 'openmesh', method: 'list_nodes' });
var callPending = rpc.declare({ object: 'openmesh', method: 'list_pending' });
var callLinks   = rpc.declare({ object: 'openmesh', method: 'get_links' });

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

	draw: function(st, adopted, pending, nodes, links) {
		var W = 760, H = 470, cx = W / 2, cy = H / 2;
		var mesh = adopted.nodes || [];
		var pend = pending.nodes || [];
		var bind = this.bindLinks(links.links || [], nodes.nodes, mesh);
		var svg = '';
		var pos = {};
		var self = this;

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
				'◌ ', _('Discovered, not adopted')
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
			L.resolveDefault(callLinks(), { links: [] })
		]);
	},

	render: function(data) {
		var self = this;
		var container = E('div');

		function refresh() {
			return self.fetchAll().then(function(d) {
				container.innerHTML = '';
				container.appendChild(self.draw(d[0], d[1], d[2], d[3], d[4]));
				container.appendChild(self.renderLinks(d[4]));
			});
		}

		refresh();
		poll.add(refresh, 5);
		return container;
	}
});
