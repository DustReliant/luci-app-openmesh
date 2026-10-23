// SPDX-License-Identifier: GPL-3.0-or-later
/* OpenMesh Manager topology: controller-centric SVG view (V0.1). */
'use strict';
'require view';
'require rpc';
'require poll';

var callStatus  = rpc.declare({ object: 'openmesh', method: 'status' });
var callAdopted = rpc.declare({ object: 'openmesh', method: 'list_adopted' });
var callPending = rpc.declare({ object: 'openmesh', method: 'list_pending' });
var callLinks   = rpc.declare({ object: 'openmesh', method: 'get_links' });

return view.extend({
	draw: function(st, adopted, pending) {
		var W = 640, H = 420, cx = W / 2, cy = H / 2;
		var nodes = adopted.nodes || [];
		var pend = pending.nodes || [];
		var svg = '';

		function esc(s) {
			return String(s || '').replace(/&/g, '&amp;')
				.replace(/</g, '&lt;').replace(/>/g, '&gt;');
		}

		/* controller */
		svg += '<circle cx="%d" cy="%d" r="26" fill="#2d7d2d"/>'
			.format(cx, cy);
		svg += '<text x="%d" y="%d" text-anchor="middle" fill="#fff" font-size="11">CTRL</text>'
			.format(cx, cy + 4);

		/* adopted nodes on an inner ring */
		for (var i = 0; i < nodes.length; i++) {
			var n = nodes[i];
			var ang = (2 * Math.PI * i / Math.max(nodes.length, 1)) - Math.PI / 2;
			var r = 140;
			var x = cx + Math.cos(ang) * r;
			var y = cy + Math.sin(ang) * r;
			var col = n.online ? '#3aa835' : '#c0504d';

			svg += '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="%d"/>'
				.format(cx, cy, x, y, col, n.online ? 3 : 1);
			svg += '<circle cx="%d" cy="%d" r="20" fill="%s"/>'
				.format(x, y, col);
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="#fff" font-size="9">%s</text>'
				.format(x, y + 3, esc(n.name).substring(0, 10));
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="#444" font-size="8">%s</text>'
				.format(x, y + 32, n.online ? _('online') : _('offline'));
		}

		/* pending nodes on an outer dashed ring */
		for (var j = 0; j < pend.length; j++) {
			var p = pend[j];
			var ang2 = (2 * Math.PI * j / Math.max(pend.length, 1)) - Math.PI / 2;
			var x2 = cx + Math.cos(ang2) * 215;
			var y2 = cy + Math.sin(ang2) * 215;

			svg += '<circle cx="%d" cy="%d" r="14" fill="none" stroke="#999" stroke-dasharray="4 3"/>'
				.format(x2, y2);
			svg += '<text x="%d" y="%d" text-anchor="middle" fill="#777" font-size="8">%s</text>'
				.format(x2, y2 + 28, esc(p.model).substring(0, 12));
		}

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Mesh topology')),
			E('div', {
				'style': 'text-align:center'
			}, E('svg', {
				'viewBox': '0 0 %d %d'.format(W, H),
				'width': '100%',
				'max-width': '%d'.format(W)
			}, svg)),
			E('p', { 'style': 'color:#777;font-size:small' },
			  _('Solid: adopted (green online / red offline); dashed: discovered, not adopted.'))
		]);
	},

	renderLinks: function(links) {
		var rows = (links.links || []).map(function(l) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, l.type),
				E('td', { 'class': 'td' }, l.ifname || l.via || '-'),
				E('td', { 'class': 'td' }, l.peer),
				E('td', { 'class': 'td' },
				  l.rssi ? '%d dBm'.format(l.rssi) : '-'),
				E('td', { 'class': 'td' },
				  l.rate_kbps ? '%d kbit/s'.format(l.rate_kbps) : '-')
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
		return Promise.all([
			L.resolveDefault(callStatus(), {}),
			L.resolveDefault(callAdopted(), { nodes: [] }),
			L.resolveDefault(callPending(), { nodes: [] })
		]);
	},

	render: function(data) {
		var self = this;
		var container = E('div');

		function refresh() {
			return Promise.all([
				L.resolveDefault(callStatus(), {}),
				L.resolveDefault(callAdopted(), { nodes: [] }),
				L.resolveDefault(callPending(), { nodes: [] }),
				L.resolveDefault(callLinks(), { links: [] })
			]).then(function(d) {
				container.innerHTML = '';
				container.appendChild(self.draw(d[0], d[1], d[2]));
				container.appendChild(self.renderLinks(d[3]));
			});
		}

		refresh();
		poll.add(refresh, 5);
		return container;
	}
});
