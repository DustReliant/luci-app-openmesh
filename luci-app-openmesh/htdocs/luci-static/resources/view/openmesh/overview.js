// SPDX-License-Identifier: GPL-3.0-or-later
/* OpenMesh Manager overview: status, adopted nodes, pending adoption. */
'use strict';
'require view';
'require rpc';
'require poll';

var callStatus  = rpc.declare({ object: 'openmesh', method: 'status' });
var callAdopted = rpc.declare({ object: 'openmesh', method: 'list_adopted' });
var callPending = rpc.declare({ object: 'openmesh', method: 'list_pending' });
var callNodes   = rpc.declare({ object: 'openmesh', method: 'list_nodes' });
var callAdopt   = rpc.declare({ object: 'openmesh', method: 'adopt',
				params: [ 'name', 'id' ] });
var callRevoke  = rpc.declare({ object: 'openmesh', method: 'revoke',
				params: [ 'id' ] });
var callClients = rpc.declare({ object: 'openmesh', method: 'get_clients' });

function shortId(id) {
	return (id || '').substring(0, 16) + '…';
}

function modelOf(id, nodes) {
	for (var i = 0; i < (nodes || []).length; i++)
		if (nodes[i].id == id)
			return '%s / %s'.format(nodes[i].model, nodes[i].fw);
	return '';
}

return view.extend({
	load: function() {
		return Promise.all([
			L.resolveDefault(callStatus(), {}),
			L.resolveDefault(callAdopted(), { nodes: [] }),
			L.resolveDefault(callPending(), { nodes: [] }),
			L.resolveDefault(callNodes(), { nodes: [] }),
			L.resolveDefault(callClients(), { clients: [] })
		]);
	},

	renderStatus: function(st) {
		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Controller')),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('Node ID')),
					E('th', { 'class': 'th' }, _('Role')),
					E('th', { 'class': 'th' }, _('Version')),
					E('th', { 'class': 'th' }, _('Nodes'))
				]),
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, st.node_id || '-'),
					E('td', { 'class': 'td' }, st.role || '-'),
					E('td', { 'class': 'td' }, st.version || '-'),
					E('td', { 'class': 'td' },
					  '%d / %d'.format(st.nodes_total || 0,
							   st.nodes_total || 0))
				])
			])
		]);
	},

	renderPending: function(nodes, ev) {
		var self = this;

		if (!nodes.length)
			return E([]);
		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Discovered nodes (not yet adopted)')),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('Node ID')),
					E('th', { 'class': 'th' }, _('Model')),
					E('th', { 'class': 'th' }, _('Source')),
					E('th', { 'class': 'th' }, _('Action'))
				]),
				nodes.map(function(n) {
					return E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' },
						  shortId(n.id)),
						E('td', { 'class': 'td' },
						  '%s / %s'.format(n.model, n.fw)),
						E('td', { 'class': 'td' }, n.addr),
						E('td', { 'class': 'td' }, [
							E('button', {
								'class': 'btn cbi-button cbi-button-apply important',
								'click': ev(function(ui, e) {
									var name = window.prompt(
										_('Name for this node'),
										'');
									return callAdopt(name || '', n.id)
										.then(ev.refresh);
								})
							}, _('Adopt'))
						])
					]);
				})
			])
		]);
	},

	renderAdopted: function(nodes, disc, ev) {
		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Mesh nodes')),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('Name')),
					E('th', { 'class': 'th' }, _('Node ID')),
					E('th', { 'class': 'th' }, _('Model')),
					E('th', { 'class': 'th' }, _('Status')),
					E('th', { 'class': 'th' }, _('Actions'))
				]),
				nodes.length ? nodes.map(function(n) {
					return E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, n.name),
						E('td', { 'class': 'td' },
						  shortId(n.id)),
						E('td', { 'class': 'td' },
						  modelOf(n.id, disc)),
						E('td', { 'class': 'td' },
						  E('span', {
							'style': 'color:%s'.format(
								n.online ? 'green' : 'red')
						  }, n.online ? _('Online') : _('Offline'))),
						E('td', { 'class': 'td' }, [
							E('button', {
								'class': 'btn cbi-button cbi-button-action',
								'click': ev(function(ui, e) {
									var name = window.prompt(
										_('New name'), n.name);
									if (name)
										return callAdopt(name, n.id)
											.then(ev.refresh);
								})
							}, _('Rename')),
							' ',
							E('button', {
								'class': 'btn cbi-button cbi-button-remove',
								'click': ev(function(ui, e) {
									if (window.confirm(
										_('Remove this node from the mesh?')))
										return callRevoke(n.id)
											.then(ev.refresh);
								})
							}, _('Remove'))
						])
					]);
				}) : E('tr', { 'class': 'tr placeholder' }, [
					E('td', { 'class': 'td center', colspan: 5 },
					  _('No nodes adopted yet'))
				])
			])
		]);
	},

	renderClients: function(clients) {
		var rows = (clients.clients || []).map(function(c) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, c.mac),
				E('td', { 'class': 'td' }, c.ssid || '-'),
				E('td', { 'class': 'td' }, c.ifname),
				E('td', { 'class': 'td' },
				  c.rssi ? _('%d dBm').format(c.rssi) : '-'),
				E('td', { 'class': 'td' },
				  c.rx_kbps ? _('%d kbit/s').format(c.rx_kbps) : '-'),
				E('td', { 'class': 'td' },
				  c.tx_kbps ? _('%d kbit/s').format(c.tx_kbps) : '-')
			]);
		});

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Local clients')),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('MAC address')),
					E('th', { 'class': 'th' }, _('SSID')),
					E('th', { 'class': 'th' }, _('Interface')),
					E('th', { 'class': 'th' }, _('RSSI')),
					E('th', { 'class': 'th' }, 'RX'),
					E('th', { 'class': 'th' }, 'TX')
				]),
				rows.length ? rows :
					E('tr', { 'class': 'tr placeholder' }, [
						E('td', { 'class': 'td center', colspan: 6 },
						  _('No clients connected'))
					])
			]),
			E('p', { 'style': 'color:#666;font-size:small' },
			  _('Clients on this controller. Clients on mesh nodes appear after dual-node support.'))
		]);
	},

	render: function(data) {
		var self = this;
		var container = E('div', {}, [ E('em', {}, _('Loading…')) ]);

		/* agent nodes have no openmesh ubus object (controller-only RPC,
		 * by design): show a pointer to the controller instead of
		 * misleading empty tables */
		if (!data[0] || !data[0].node_id)
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('OpenMesh agent node')),
				E('p', {},
					_('This device runs as a mesh agent (or the openmeshd service is unreachable).')),
				E('p', { 'style': 'color:#666' },
					_('Discovery, adoption, mesh topology and client lists are managed from the controller. Open the OpenMesh pages there.'))
			]);

		var ev = function(fn) {
			return function() {
				try {
					return fn.apply(this, arguments);
				} catch (e) {
					console.error(e);
				}
			};
		};
		ev.refresh = function() {
			return self.refresh(container);
		};

		this.refresh = function(container) {
			return Promise.all([
				L.resolveDefault(callStatus(), {}),
				L.resolveDefault(callAdopted(), { nodes: [] }),
				L.resolveDefault(callPending(), { nodes: [] }),
				L.resolveDefault(callNodes(), { nodes: [] }),
				L.resolveDefault(callClients(), { clients: [] })
			]).then(function(d) {
				container.innerHTML = '';
				container.appendChild(self.renderStatus(d[0]));
				container.appendChild(self.renderPending(d[2], ev));
				container.appendChild(self.renderAdopted(d[1], d[3], ev));
				container.appendChild(self.renderClients(d[4]));
			});
		};

		this.refresh(container);
		poll.add(L.bind(function() {
			return this.refresh(container);
		}, this), 5);

		return container;
	}
});
