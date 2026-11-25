// SPDX-License-Identifier: GPL-2.0-only
'use strict';
'require view';
'require rpc';
'require poll';
'require form';
'require uci';
'require ui';

const stats = rpc.declare({ object: 'sacnmon', method: 'stats', reject: true });
const channels = rpc.declare({
	object: 'sacnmon', method: 'channels', params: ['cid', 'universe'], reject: true
});
const status = rpc.declare({ object: 'sacnmon.control', method: 'status', reject: true });
const devices = rpc.declare({ object: 'luci-rpc', method: 'getNetworkDevices', expect: { '': {} } });
const actions = Object.fromEntries(['start', 'stop', 'enable', 'disable'].map(function(method) {
	return [method, rpc.declare({ object: 'sacnmon.control', method: method, reject: true })];
}));

const columns = [
	['active', _('State')],
	['ip', _('Source IP')],
	['name', _('Source Name')],
	['cid', _('CID')],
	['universe', _('Universe')],
	['priority', _('Priority')],
	['pps', _('Packets/s')],
	['packets', _('Packets')],
	['discontinuities', _('Sequence discontinuities')],
	['age_ms', _('Last packet')]
];
const numeric = new Intl.NumberFormat();
const clock = new Intl.DateTimeFormat(undefined, { hour: '2-digit', minute: '2-digit', second: '2-digit' });

function number(value) {
	return value == null ? '—' : numeric.format(value);
}

function label(active) {
	return E('span', { 'class': active ? 'sacn-active' : 'sacn-inactive' },
		[active ? _('Active') : _('Inactive')]);
}

function compare(left, right, key) {
	if (key === 'ip') {
		const value = ip => ip.split('.').reduce((total, octet) => total * 256 + Number(octet), 0);
		return value(left.ip) - value(right.ip);
	}
	if (typeof left[key] === 'string')
		return left[key].localeCompare(right[key]);
	return Number(left[key]) - Number(right[key]);
}

function trafficChart() {
	const windowMs = 120000;
	const points = [];
	const graph = E('canvas', { width: 720, height: 144, role: 'img', 'aria-label': _('Traffic history') });
	const peak = E('span');
	const current = E('strong');
	const ctx = graph.getContext('2d');
	const node = E('section', { 'class': 'sacn-panel' }, [
		E('h3', {}, [_('sACN traffic')]),
		E('div', { 'class': 'sacn-chart-labels' }, [current, peak]),
		graph,
		E('div', { 'class': 'sacn-chart-labels' }, [_('2 minutes ago'), _('Now')])
	]);

	return { node, update: function(pps) {
		const now = performance.now();
		points.push({ time: now, pps });
		while (points.length > 61 || points[0].time < now - windowMs)
			points.shift();
		const values = points.filter(point => point.pps != null);
		const max = Math.max(1, ...values.map(point => point.pps));
		current.textContent = _('%s packets/s').format(number(pps));
		peak.textContent = values.length ? _('Peak: %s packets/s').format(number(Math.max(...values.map(point => point.pps)))) : '';
		graph.setAttribute('aria-label', [_('Traffic history'), current.textContent, peak.textContent].join(' · '));
		if (!ctx)
			return;
		const width = graph.width;
		const height = graph.height;
		const left = 2, right = width - 2, top = 3, bottom = height - 3;
		ctx.clearRect(0, 0, width, height);
		ctx.strokeStyle = getComputedStyle(graph).color || '#808080';
		ctx.globalAlpha = 0.22;
		ctx.lineWidth = 1;
		ctx.beginPath();
		for (const y of [top, (top + bottom) / 2, bottom]) {
			ctx.moveTo(left, y);
			ctx.lineTo(right, y);
		}
		ctx.stroke();
		ctx.globalAlpha = 1;
		ctx.strokeStyle = '#248fba';
		ctx.fillStyle = ctx.strokeStyle;
		ctx.lineWidth = 2.5;
		ctx.beginPath();
		let previous = null;
		for (const point of points) {
			if (point.pps == null) {
				previous = null;
				continue;
			}
			const x = right - (now - point.time) / windowMs * (right - left);
			const y = bottom - point.pps / max * (bottom - top);
			ctx.fillRect(x - 1, y - 1, 2, 2);
			if (previous != null && point.time - previous <= 5000)
				ctx.lineTo(x, y);
			else
				ctx.moveTo(x, y);
			previous = point.time;
		}
		ctx.stroke();
	} };
}

function channelViewer() {
	const pageSize = 64;
	const columns = 8;
	const key = row => row.universe + '/' + row.cid;
	let selected = '';
	let choices = '';
	let streams = [];
	let levels = null;
	let request = 0;

	const select = E('select', { change: function() {
		selected = select.value;
		range.value = '0';
		update();
	} });
	const range = E('select', { change: draw }, Array.from({ length: 8 }, (_, i) =>
		E('option', { value: i }, [(i * pageSize + 1) + '–' + ((i + 1) * pageSize)])));
	const caption = E('p');
	const tbody = E('tbody');
	const table = E('table', { 'class': 'sacn-channels', 'aria-label': _('DMX channel values') }, [
		E('thead', {}, [E('tr', {}, [
			E('th', { scope: 'col' }, [_('Channels')]),
			...Array.from({ length: columns }, (_, i) => E('th', { scope: 'col' }, ['+' + i]))
		])]), tbody
	]);
	const node = E('section', { 'class': 'sacn-panel' }, [
		E('h3', {}, [_('DMX channel values')]),
		E('div', { 'class': 'sacn-controls' }, [
			E('label', {}, [_('Universe / source'), ' ', select]),
			E('label', {}, [_('Channel range'), ' ', range])
		]),
		caption,
		E('div', { 'class': 'sacn-table-wrap' }, [table])
	]);

	function draw() {
		table.hidden = levels == null || levels.length === 0;
		range.disabled = table.hidden;
		if (table.hidden) {
			tbody.replaceChildren();
			return;
		}
		const first = Number(range.value) * pageSize;
		const rows = [];
		for (let start = first; start < first + pageSize; start += columns) {
			const cells = Array.from({ length: columns }, function(_, offset) {
				const index = start + offset;
				const value = index < levels.length ? levels[index] : null;
				const attrs = { title: _('Channel %s').format(index + 1) };
				if (value > 0)
					attrs.style = 'box-shadow: inset 0 -0.16rem 0 rgba(36, 143, 186, ' + (0.2 + 0.8 * value / 255) + ')';
				return E('td', attrs, [value == null ? '—' : String(value)]);
			});
			rows.push(E('tr', {}, [E('th', { scope: 'row' }, [String(start + 1)]), ...cells]));
		}
		tbody.replaceChildren(...rows);
	}

	async function update() {
		const ticket = ++request;
		const stream = streams.find(row => key(row) === selected);
		if (!stream || caption.dataset.stream !== selected) {
			caption.dataset.stream = selected;
			caption.textContent = stream ? _('Loading channels…') :
				selected ? _('Selected stream is unavailable') : _('Waiting for an sACN stream');
			levels = null;
			draw();
		}
		if (!stream)
			return;
		const result = await channels(stream.cid, stream.universe).catch(() => null);
		if (ticket !== request)
			return;
		const hex = result?.levels;
		levels = typeof hex === 'string' && /^(?:[0-9a-f]{2}){0,512}$/.test(hex) ?
			Array.from(hex.match(/../g) || [], pair => parseInt(pair, 16)) : null;
		caption.textContent = levels == null ? _('Channel values unavailable') :
			(levels.length ? _('%s DMX channels · values 0–255').format(number(levels.length)) :
				_('This packet contains no DMX channels')) +
			(result.active ? '' : ' · ' + _('Inactive. Showing last received values.'));
		draw();
	}

	return { node, refresh: function(rows) {
		streams = rows.slice().sort((a, b) => a.universe - b.universe || compare(a, b, 'cid'));
		if (!selected && streams.length)
			selected = key(streams.find(row => row.active) || streams[0]);
		const options = streams.map(row => [
			key(row), 'U' + row.universe + ' · ' + (row.name || row.cid) + ' · ' + row.ip +
				(row.active ? '' : ' · ' + _('Inactive'))
		]);
		if (selected && !options.some(option => option[0] === selected))
			options.unshift([selected, _('Selected stream is unavailable')]);
		const signature = JSON.stringify(options);
		if (signature !== choices) {
			choices = signature;
			select.replaceChildren(...options.map(([value, text]) => E('option', { value }, [text])));
			select.value = selected;
		}
		select.disabled = options.length === 0;
		return update();
	} };
}

return view.extend({
	load: function() {
		return Promise.all([uci.load('sacnmon'), devices().catch(() => ({})), status().catch(() => null)]);
	},

	render: async function(data) {
		let info = data[2];
		let snapshot = null;
		let received = 0;
		let sortKey = 'active';
		let direction = -1;
		let busy = false;

		const daemon = E('strong');
		const boot = E('span');
		const body = E('tbody');
		const chart = trafficChart();
		const viewer = channelViewer();
		const cards = [
			['active_sources', _('Active sources')],
			['active_universes', _('Active universes')],
			['pps', _('Packets/s')],
			['discontinuities', _('Sequence discontinuities')],
			['packets', _('Packets received')]
		].map(function([key, title]) {
			const value = E('strong', {}, ['—']);
			return { key, value, node: E('div', { 'class': 'sacn-card' }, [E('span', {}, [title]), value]) };
		});

		const headers = [];
		function drawRows() {
			for (const [key, heading] of headers)
				heading.setAttribute('aria-sort', key !== sortKey ? 'none' :
					direction > 0 ? 'ascending' : 'descending');
			const streams = (snapshot?.streams || []).slice().sort((a, b) =>
				direction * compare(a, b, sortKey) || compare(a, b, 'universe') || compare(a, b, 'cid'));
			const rows = streams.map(function(row) {
				return E('tr', {}, columns.map(function([key, title]) {
					let value = row[key];
					if (key === 'active')
						value = label(row.active);
					else if (key === 'age_ms')
						value = clock.format(new Date(received - row.age_ms));
					else if (typeof value === 'number')
						value = number(value);
					return E('td', { 'data-label': title }, [value || '—']);
				}));
			});
			body.replaceChildren(...(rows.length ? rows : [
				E('tr', {}, [E('td', { colspan: columns.length, 'class': 'sacn-empty' },
					[_('No sACN streams observed')])])
			]));
		}

		const head = E('tr', {}, columns.map(function([key, title]) {
			const button = E('button', { type: 'button', click: function() {
				direction = sortKey === key ? -direction : 1;
				sortKey = key;
				drawRows();
			} }, [title]);
			const heading = E('th', { scope: 'col' }, [button]);
			headers.push([key, heading]);
			return heading;
		}));

		const sort = E('select', { 'aria-label': _('Sort by'), change: function(event) {
			sortKey = event.target.value;
			drawRows();
		} }, columns.map(([key, title]) => E('option', { value: key }, [title])));
		const controls = E('div', { 'class': 'sacn-controls' }, [boot]);
		const buttons = [];
		async function refresh() {
			try {
				snapshot = await stats();
				received = Date.now();
			} catch (error) {
				snapshot = null;
				info = await status().catch(() => null);
			}
			daemon.textContent = snapshot ? (snapshot.capturing ? _('Capturing on %s').format(snapshot.interface) :
				_('Running · %s').format(snapshot.capture_error || _(snapshot.capture_status))) :
				info ? (info.running ? _('Running · Statistics unavailable') : _('Stopped')) : _('Unavailable');
			boot.textContent = info ? (info.enabled ? _('Autostart enabled') : _('Autostart disabled')) :
				_('Autostart status unavailable');
			for (const card of cards)
				card.value.textContent = number(snapshot?.[card.key]);
			for (const [method, button] of buttons)
				button.disabled = busy || (method === 'start' && (snapshot || info?.running)) ||
					(method === 'stop' && info && !snapshot && !info.running) ||
					(method === 'enable' && info?.enabled) || (method === 'disable' && info && !info.enabled);
			drawRows();
			chart.update(snapshot?.capturing ? snapshot.pps : null);
			await viewer.refresh(snapshot?.streams || []);
		}

		if (L.hasViewPermission()) {
			for (const [method, title] of [
				['start', _('Start')], ['stop', _('Stop')],
				['enable', _('Enable autostart')], ['disable', _('Disable autostart')]
			]) {
				const button = E('button', { type: 'button', 'class': 'btn cbi-button', click: async function() {
					if (busy)
						return;
					busy = true;
					for (const [, control] of buttons)
						control.disabled = true;
					try {
						if (!(await actions[method]()).ok)
							throw new Error(title);
					} catch (error) {
						ui.addNotification(null, E('p', {}, [_('SACNMON service action failed: %s').format(title)]));
					} finally {
						info = await status().catch(() => null);
						busy = false;
						await refresh();
					}
				} }, [title]);
				buttons.push([method, button]);
				controls.appendChild(button);
			}
		}

		const map = new form.Map('sacnmon');
		const section = map.section(form.NamedSection, 'capture', 'monitor', _('Capture settings'));
		const option = section.option(form.ListValue, 'interface', _('Network device'));
		option.default = 'br-lan';
		option.rmempty = false;
		const configured = uci.get('sacnmon', 'capture', 'interface') || 'br-lan';
		const interfaces = Object.keys(data[1]).filter(name => data[1][name]?.type === 1);
		for (const name of Array.from(new Set([configured, 'br-lan', ...interfaces])).sort())
			option.value(name);

		const settings = await map.render();
		await refresh();
		poll.add(refresh, 2);

		return E('div', { 'class': 'sacnmon' }, [
			E('style', {}, [`
				.sacnmon .sacn-summary { display: grid; grid-template-columns: repeat(auto-fit, minmax(9rem, 1fr)); gap: .65rem; margin: 1rem 0; }
				.sacnmon .sacn-card { padding: .7rem; border: 1px solid; border-radius: .4rem; }
				.sacnmon .sacn-card strong { display: block; font-size: 1.4em; }
				.sacnmon .sacn-controls { display: flex; flex-wrap: wrap; align-items: center; gap: .5rem; margin-top: .6rem; }
				.sacnmon .sacn-table { width: 100%; }
				.sacnmon .sacn-table-wrap { overflow-x: auto; margin: 1rem 0; }
				.sacnmon .sacn-table th, .sacnmon .sacn-table td { text-align: start; padding: .45rem; overflow-wrap: anywhere; }
				.sacnmon .sacn-table th button { font: inherit; background: transparent; color: inherit; border: 0; text-align: start; cursor: pointer; }
				.sacnmon [aria-sort=ascending] button::after { content: ' ↑'; }
				.sacnmon [aria-sort=descending] button::after { content: ' ↓'; }
				.sacnmon .sacn-active, .sacnmon .sacn-inactive { border-inline-start: .25rem solid; padding-inline-start: .35rem; }
				.sacnmon .sacn-active { border-color: #21a366; }
				.sacnmon .sacn-inactive { border-color: #d18717; }
				.sacnmon .sacn-mobile-sort { display: none; }
				.sacnmon .sacn-panel { padding: .8rem; border: 1px solid; border-radius: .4rem; margin: 1rem 0; }
				.sacnmon .sacn-panel canvas { display: block; width: 100%; height: 9rem; }
				.sacnmon .sacn-chart-labels { display: flex; flex-wrap: wrap; justify-content: space-between; gap: .35rem; }
				.sacnmon .sacn-controls select { max-width: min(65vw, 28rem); }
				.sacnmon .sacn-channels { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
				.sacnmon .sacn-channels th, .sacnmon .sacn-channels td { padding: .33rem .18rem; text-align: center; }
				.sacnmon .sacn-channels tr { border-bottom: 1px solid rgba(128, 128, 128, .25); }
				@media (max-width: 760px) {
					.sacnmon .sacn-mobile-sort { display: flex; flex-wrap: wrap; gap: .5rem; }
					.sacnmon .sacn-table thead { display: none; }
					.sacnmon .sacn-table, .sacnmon .sacn-table tbody { display: block; }
					.sacnmon .sacn-table tr { display: block; border: 1px solid; border-radius: .4rem; margin-bottom: .65rem; }
					.sacnmon .sacn-table td[data-label] { display: grid; grid-template-columns: minmax(7rem, 36%) 1fr; gap: .5rem; }
					.sacnmon .sacn-table td[data-label]::before { content: attr(data-label); font-weight: 600; }
					.sacnmon .sacn-table .sacn-empty { display: block; }
				}
			`]),
			E('h2', {}, ['SACNMON']),
			E('div', {}, [_('Daemon'), ' · ', daemon]),
			controls,
			E('div', { 'class': 'sacn-summary' }, cards.map(card => card.node)),
			chart.node,
			viewer.node,
			E('div', { 'class': 'sacn-mobile-sort' }, [
				E('label', {}, [_('Sort by'), ' ', sort]),
				E('button', { type: 'button', 'class': 'btn', click: function() {
					direction = -direction;
					drawRows();
				} }, [_('Reverse sort')])
			]),
			E('div', { 'class': 'sacn-table-wrap' }, [
				E('table', { 'class': 'sacn-table', 'aria-label': _('sACN streams') }, [E('thead', {}, [head]), body])
			]),
			settings
		]);
	}
});
