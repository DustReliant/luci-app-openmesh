// SPDX-License-Identifier: GPL-3.0-or-later
/* OpenMesh Manager wizard: create a mesh network with a confirm window. */
'use strict';
'require view';
'require rpc';
'require ui';

var callMeshCreate = rpc.declare({ object: 'openmesh', method: 'mesh_create',
				   params: [ 'mesh_name', 'band', 'key', 'mode', 'lan_ports' ] });
var callLanPorts    = rpc.declare({ object: 'openmesh', method: 'get_lan_ports' });
var callPorts       = rpc.declare({ object: 'openmesh', method: 'ports' });
var callPortPlan    = rpc.declare({ object: 'openmesh', method: 'port_plan',
				   params: [ 'device', 'mode' ] });
var callRoaming    = rpc.declare({ object: 'openmesh', method: 'roaming_apply' });
var callConfirm    = rpc.declare({ object: 'openmesh', method: 'apply_confirm' });
var callRollback   = rpc.declare({ object: 'openmesh', method: 'apply_rollback' });

/* dropdown entries: physical br-lan members only (m19) */
function lanPortOptions(ports) {
	var seen = {};
	return (ports || []).filter(function(p) {
		if (!p || seen[p] || /^om_/.test(p) ||
		    !/^[A-Za-z0-9_]{1,15}$/.test(p))
			return false;
		seen[p] = true;
		return true;
	});
}

/* ---- m21 wired-backhaul port picker (Phase 7.1, preview-only) ---- */

/* dropdown candidates: physical ethernet only; wireless/virtual/bridge
 * entries stay visible in the info layer but are never offered.
 * Defensive against malformed/absent ubus payloads. */
function dropdownPorts(ports) {
	if (!Array.isArray(ports)) return [];
	return ports.filter(function(p) {
		return p && p.device && p.type === 'ethernet';
	});
}

function riskText(tok) {
	var m = {
		mgmt: _('Management path'),
		mgmt_unknown: _('Cannot exclude management path'),
		wan: _('WAN in use'),
		vlan: _('VLAN dependency'),
		mesh: _('Already used for mesh'),
		dsa_cpu: _('Switch CPU port'),
		pppoe: 'PPPoE',
		virtual: _('Virtual device'),
		wireless: _('Wireless device'),
		master: _('Enslaved')
	};
	return m[tok] || _('Unknown');
}

function roleText(r) {
	var m = {
		lan: 'LAN', wan: 'WAN', 'dsa-cpu': _('Switch CPU port'),
		mesh: 'Mesh', virtual: _('Virtual device'),
		wireless: _('Wireless device'), unused: _('Unused port'),
		unknown: _('Unknown')
	};
	return m[r] || r || '-';
}

function portOptionLabel(p) {
	if (!p || !p.device) return '';
	if (p.management === 'yes') return p.device + ' — ' + _('Management path');
	if (!p.eligible) return p.device + ' — ' + riskText(p.reason);
	return p.device;
}

return view.extend({
	load: function() {
		return Promise.all([
			L.resolveDefault(callLanPorts(), { ports: [] }),
			L.resolveDefault(callPorts(), { ports: [] })
		]).then(function(r) {
			return { lanPorts: r[0], ports: r[1].ports || [] };
		}, function() {
			return { lanPorts: { ports: [] }, ports: [] };
		});
	},

	render: function(data) {
		var nameInput, bandSel, modeSel, keyInput, lanPortsSel, statusLine;
		var wiredEnable, wiredMode, wiredPort, wiredInfo, wiredPlan;
		var portList = data.ports || [];

		function syncLanPorts() {
			/* wired leg only exists under batman-adv (mode B) */
			lanPortsSel.disabled = (modeSel.value !== 'hybrid');
		}

		function setCountdown(reply) {
			var left = reply.confirm_window || 30;
			var key = reply.key || '';

			statusLine.textContent =
				_('Mesh applied. Key: %s - confirming in %d s')
					.format(key, left);
			var iv = window.setInterval(function() {
				left--;
				if (left > 0) {
					statusLine.textContent =
						_('Mesh applied. Key: %s - confirming in %d s')
							.format(key, left);
					return;
				}
				window.clearInterval(iv);
			}, 1000);
			statusLine.dataset.key = key;
			statusLine.dataset.left = left;
		}

		var createBtn = E('button', {
			'class': 'btn cbi-button cbi-button-apply important',
			'click': function() {
				if (!nameInput.value || !nameInput.value.trim())
					return;
				return callMeshCreate(nameInput.value.trim(),
						      bandSel.value,
						      keyInput.value || '',
						      modeSel.value,
						      lanPortsSel.disabled ?
							      '' : lanPortsSel.value)
					.then(setCountdown);
			}
		}, _('Create mesh (30s auto-rollback)'));

		var keepBtn = E('button', {
			'class': 'btn cbi-button cbi-button-save',
			'click': function() {
				return callConfirm().then(function() {
					statusLine.textContent =
						_('Configuration kept.');
				}, function() {
					statusLine.textContent =
						_('No pending transaction.');
				});
			}
		}, _('Keep configuration'));

		var undoBtn = E('button', {
			'class': 'btn cbi-button cbi-button-remove',
			'click': function() {
				return callRollback().then(function() {
					statusLine.textContent =
						_('Rolled back.');
				}, function() {
					statusLine.textContent =
						_('No pending transaction.');
				});
			}
		}, _('Roll back now'));

		var roamBtn = E('button', {
			'class': 'btn cbi-button cbi-button-apply',
			'click': function() {
				return callRoaming().then(function(rv) {
					statusLine.textContent =
						_('Roaming (k/v/r) applied. Domain: %s - confirm with Keep')
							.format(rv.mobility_domain || '');
				}, function() {
					statusLine.textContent =
						_('Roaming apply failed (no AP interfaces?)');
				});
			}
		}, _('Apply roaming enhancement (802.11k/v/r)'));

		/* ---- m21 wired-backhaul preview (read-only, Phase 7.1) ---- */

		function infoRow(box, key, val, color) {
			var row = E('div', { 'style':
				'display:flex;justify-content:space-between;' +
				'padding:3px 0;font-size:12px;border-bottom:1px solid #f5f5f5' });
			row.appendChild(E('span', { 'style': 'color:#888' }, key));
			row.appendChild(E('span', {
				'style': color ? 'color:' + color : '' }, String(val)));
			box.appendChild(row);
		}

		function findPort(dev) {
			for (var i = 0; i < portList.length; i++)
				if (portList[i].device === dev) return portList[i];
			return null;
		}

		function renderPortInfo() {
			wiredInfo.style.display = '';
			while (wiredInfo.firstChild)
				wiredInfo.removeChild(wiredInfo.firstChild);
			var p = findPort(wiredPort.value);
			if (!p) {
				wiredInfo.appendChild(E('div', { 'style':
					'color:#999;font-size:12px;padding:8px 0' },
					_('Select a port')));
				return;
			}
			infoRow(wiredInfo, _('Device'), p.device);
			infoRow(wiredInfo, _('Role'), roleText(p.role));
			infoRow(wiredInfo, _('Carrier'),
				p.carrier ? _('Yes') : _('No'),
				p.carrier ? '#4CAF50' : '#999');
			infoRow(wiredInfo, _('Speed'),
				p.speed_mbps ? p.speed_mbps + ' Mbps' : '--');
			infoRow(wiredInfo, _('Bridge'), p.bridge || '-');
			infoRow(wiredInfo, _('Used for mesh'),
				p.reason === 'mesh' ? _('Yes') : _('No'));
			infoRow(wiredInfo, _('Management path'),
				p.management === 'yes' ? _('Yes') :
				p.management === 'no' ? _('No') : _('Unknown'),
				p.management === 'yes' ? '#F44336' :
				p.management === 'unknown' ? '#FF9800' : '#4CAF50');
			infoRow(wiredInfo, _('Eligible'), p.eligible ? _('Yes') : _('No'),
				p.eligible ? '#4CAF50' : '#F44336');
			if (!p.eligible)
				infoRow(wiredInfo, _('Reason'), riskText(p.reason), '#F44336');
		}

		function planList(box, title, items) {
			if (!items || !items.length) return;
			box.appendChild(E('div', { 'style':
				'font-size:11px;color:#999;margin:8px 0 2px;' +
				'text-transform:uppercase' }, title));
			items.forEach(function(it) {
				box.appendChild(E('div', { 'style':
					'font-size:12px;padding:2px 0 2px 10px;' +
					'border-left:2px solid #e0e0e0' }, String(it)));
			});
		}

		function renderPlan(plan) {
			while (wiredPlan.firstChild)
				wiredPlan.removeChild(wiredPlan.firstChild);
			if (!plan) return;
			var head = E('div', { 'style': 'font-weight:bold;font-size:13px' },
				_('Preview (read-only, nothing is executed)'));
			head.appendChild(E('span', { 'style':
				'margin-left:8px;font-size:12px;color:' +
				(plan.ok ? '#4CAF50' : '#F44336') },
				plan.ok ? '✓ ' + _('Configurable') :
				'✗ ' + _('Not configurable')));
			wiredPlan.appendChild(head);

			(plan.checks || []).forEach(function(c) {
				var col = c.state === 'pass' ? '#4CAF50' :
					c.state === 'blocked' ? '#F44336' : '#FF9800';
				var row = E('div', { 'style':
					'display:flex;gap:8px;padding:3px 0;' +
					'font-size:12px;border-bottom:1px solid #f5f5f5' });
				row.appendChild(E('span', { 'style': 'width:10px;color:' + col }, '●'));
				row.appendChild(E('span', { 'style': 'color:#555;min-width:70px' }, c.name));
				row.appendChild(E('span', { 'style': 'color:' + col + ';min-width:52px' }, c.state));
				row.appendChild(E('span', { 'style': 'color:#888;flex:1' }, c.detail));
				wiredPlan.appendChild(row);
			});
			planList(wiredPlan, _('UCI changes'), plan.preview && plan.preview.uci_changes);
			planList(wiredPlan, _('System operations (7.2)'), plan.preview && plan.preview.ops);
			planList(wiredPlan, _('Possible interruptions'), plan.preview && plan.preview.interruptions);
			if (plan.preview && plan.preview.mgmt_risk)
				infoRow(wiredPlan, _('Management risk'), plan.preview.mgmt_risk);
			if (plan.preview && plan.preview.rollback)
				infoRow(wiredPlan, _('Rollback strategy'), plan.preview.rollback);
		}

		function fetchPlan() {
			renderPortInfo();
			if (!wiredEnable.checked || !wiredPort.value) {
				renderPlan(null);
				return;
			}
			/* backend revalidates from live sysfs/UCI; the frontend
			 * verdict is only a hint, never a permission */
			callPortPlan(wiredPort.value, wiredMode.value).then(renderPlan,
				function() {
					while (wiredPlan.firstChild)
						wiredPlan.removeChild(wiredPlan.firstChild);
					wiredPlan.appendChild(E('div', { 'style':
						'color:#F44336;font-size:12px' },
						_('Plan query failed')));
				});
		}

		function syncWiredEnabled() {
			wiredMode.disabled = !wiredEnable.checked;
			wiredPort.disabled = !wiredEnable.checked;
			fetchPlan();
		}

		function refreshPorts() {
			callPorts().then(function(rv) {
				portList = (rv && rv.ports) || [];
				while (wiredPort.firstChild)
					wiredPort.removeChild(wiredPort.firstChild);
				wiredPort.appendChild(E('option', { 'value': '' },
					_('Select a port')));
				dropdownPorts(portList).forEach(function(p) {
					wiredPort.appendChild(E('option', {
						'value': p.device,
						'disabled': !p.eligible
					}, portOptionLabel(p)));
				});
				fetchPlan();
			}, function() {
				while (wiredPort.firstChild)
					wiredPort.removeChild(wiredPort.firstChild);
				wiredPort.appendChild(E('option', { 'value': '' },
					_('Port query failed')));
			});
		}

		var wiredSection = E('div', { 'class': 'cbi-section' }, [
			E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' },
				  _('Enable wired mesh backhaul')),
				E('div', { 'class': 'cbi-value-field' },
					wiredEnable = E('input', {
						'type': 'checkbox',
						'change': syncWiredEnabled
					}))
			]),
			E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' },
				  _('Backhaul mode (intent)')),
				E('div', { 'class': 'cbi-value-field' },
					wiredMode = E('select', {
						'class': 'cbi-input-select',
						'disabled': true,
						'change': fetchPlan
					}, [
						E('option', { 'value': 'wireless_only' },
						  _('Wireless only')),
						E('option', { 'value': 'wired_only' },
						  _('Wired only')),
						E('option', { 'value': 'wired_preferred' },
						  _('Wired preferred, wireless backup'))
					]))
			]),
			E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' },
				  _('Physical port')),
				E('div', { 'class': 'cbi-value-field' }, [
					wiredPort = E('select', {
						'class': 'cbi-input-select',
						'disabled': true,
						'change': fetchPlan
					}, [E('option', { 'value': '' },
					       _('Select a port'))]
					.concat(dropdownPorts(portList).map(function(p) {
						return E('option', {
							'value': p.device,
							'disabled': !p.eligible
						}, portOptionLabel(p));
					}))),
					' ',
					E('button', {
						'class': 'btn cbi-button cbi-button-neutral',
						'click': refreshPorts
					}, _('Refresh ports'))
				])
			]),
			E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' },
				  _('Current port info')),
				E('div', { 'class': 'cbi-value-field' },
					wiredInfo = E('div', { 'style': 'min-width:260px' }))
			]),
			E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' }, ' '),
				E('div', { 'class': 'cbi-value-field' },
					wiredPlan = E('div', { 'style': 'min-width:320px' }))
			]),
			E('p', { 'style': 'color:#999;font-size:small' },
			  _('This phase only previews; no network configuration is changed.'))
		]);
		wiredInfo.style.display = 'none';
		renderPortInfo();

		return E('div', {}, [
			E('h3', {}, _('Create mesh network')),
			E('div', { 'class': 'cbi-section' }, [
				E('div', { 'class': 'cbi-value' }, [
					E('label', { 'class': 'cbi-value-title' },
					  _('Mesh name')),
					E('div', { 'class': 'cbi-value-field' },
						nameInput = E('input', {
							'type': 'text',
							'value': 'openmesh',
							'class': 'cbi-input-text'
						}))
				]),
				E('div', { 'class': 'cbi-value' }, [
					E('label', { 'class': 'cbi-value-title' },
					  _('Backhaul band')),
					E('div', { 'class': 'cbi-value-field' },
						bandSel = E('select', {
							'class': 'cbi-input-select'
						}, [
							/* default 2.4g: single-band agents
							 * (rtl8188) live here; picking 5g by
							 * accident wiped committed 2g mesh via
							 * rollback purge (r19 incident) */
							E('option', { 'value': '2g' }, '2.4 GHz'),
							E('option', { 'value': '5g' }, '5 GHz')
						]))
				]),
				E('div', { 'class': 'cbi-value' }, [
					E('label', { 'class': 'cbi-value-title' },
					  _('Backhaul mode')),
					E('div', { 'class': 'cbi-value-field' },
						modeSel = E('select', {
							'class': 'cbi-input-select',
							'change': syncLanPorts
						}, [
							E('option', { 'value': 'wifi' },
							  _('Wireless only (802.11s)')),
							E('option', { 'value': 'hybrid' },
							  _('Hybrid (+batman-adv)'))
						]))
				]),
				E('div', { 'class': 'cbi-value' }, [
					E('label', { 'class': 'cbi-value-title' },
					  _('Wired backhaul LAN port')),
					E('div', { 'class': 'cbi-value-field' },
						lanPortsSel = E('select', {
							'class': 'cbi-input-select',
							'disabled': true
						}, [
							E('option', { 'value': '' },
							  _('No wired backhaul')),
							E('option', { 'value': 'all' },
							  _('All LAN ports'))
						].concat(lanPortOptions(data.ports).map(
							function(p) {
								return E('option',
									{ 'value': p }, p);
							}))))
				]),
				E('div', { 'class': 'cbi-value' }, [
					E('label', { 'class': 'cbi-value-title' },
					  _('SAE key (blank = generate)')),
					E('div', { 'class': 'cbi-value-field' },
						keyInput = E('input', {
							'type': 'text',
							'class': 'cbi-input-text'
						}))
				]),
				E('div', { 'class': 'cbi-page-actions' }, [
					createBtn, ' ', keepBtn, ' ', undoBtn
				]),
				E('hr'),
				E('div', { 'class': 'cbi-page-actions' }, [
					roamBtn
				]),
				statusLine = E('p', {}, '')
			]),
			E('h3', {}, _('Wired mesh backhaul (preview)')),
			wiredSection
		]);
	}
});
