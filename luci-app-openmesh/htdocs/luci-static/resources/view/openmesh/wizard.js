// SPDX-License-Identifier: GPL-3.0-or-later
/* OpenMesh Manager wizard: create a mesh network with a confirm window. */
'use strict';
'require view';
'require rpc';
'require ui';

var callMeshCreate = rpc.declare({ object: 'openmesh', method: 'mesh_create',
				   params: [ 'mesh_name', 'band', 'key', 'mode' ] });
var callRoaming    = rpc.declare({ object: 'openmesh', method: 'roaming_apply' });
var callConfirm    = rpc.declare({ object: 'openmesh', method: 'apply_confirm' });
var callRollback   = rpc.declare({ object: 'openmesh', method: 'apply_rollback' });

return view.extend({
	render: function() {
		var nameInput, bandSel, modeSel, keyInput, statusLine;

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
						      modeSel.value)
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
							E('option', { 'value': '5g' }, '5 GHz'),
							E('option', { 'value': '2g' }, '2.4 GHz')
						]))
				]),
				E('div', { 'class': 'cbi-value' }, [
					E('label', { 'class': 'cbi-value-title' },
					  _('Backhaul mode')),
					E('div', { 'class': 'cbi-value-field' },
						modeSel = E('select', {
							'class': 'cbi-input-select'
						}, [
							E('option', { 'value': 'wifi' },
							  _('Wireless only (802.11s)')),
							E('option', { 'value': 'hybrid' },
							  _('Hybrid (+batman-adv)'))
						]))
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
			])
		]);
	}
});
