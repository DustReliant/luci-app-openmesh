// SPDX-License-Identifier: GPL-3.0-or-later
/* Placeholder view for M0. Real overview UI lands with milestone M6. */
'use strict';

return L.view.extend({
	title: _('OpenMesh Manager'),

	render: function() {
		return E('div', { 'class': 'cbi-section' }, [
			E('p', {}, _(
				'OpenMesh Manager skeleton installed. ' +
				'The management interface arrives with milestone M6; ' +
				'the daemon heartbeat is visible via logread.'
			))
		]);
	}
});
