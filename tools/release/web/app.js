import { app, krt } from './common/krt.js';

const $ = (id) => document.getElementById(id);
const dirOf = (p) => (p || '').replace(/[\\/][^\\/]*$/, '');
const VERSION_KEYS = ['FileDescription', 'ProductName', 'CompanyName', 'LegalCopyright', 'FileVersion', 'ProductVersion'];
let securityKeys = [];   // exe にある項目
let security = {};       // 設定 (変える項目だけ)

const base = () => dirOf($('file').value);

function collect() {
	const version = {};
	for (const k of VERSION_KEYS) version[k] = $('v_' + k).value;
	const sec = {};
	for (const sel of document.querySelectorAll('#security select')) if (sel.value !== '') sec[sel.dataset.key] = Number(sel.value);
	for (const [k, v] of Object.entries(security)) if (!securityKeys.includes(k)) sec[k] = v;   // まだ調べていない項目は保つ
	return {
		exe: $('exe').value, output: $('output').value,
		setOptions: $('setOptions').checked, options: $('options').value,
		icon: $('icon').value, version, security: sec,
		dataMode: document.querySelector('input[name=dataMode]:checked').value,
		data: $('data').value, rpf: $('rpf').value, dataName: $('dataName').value || 'data.xp3',
		signKey: $('signKey').value,
	};
}

function apply(s) {
	$('exe').value = s.exe || '';
	$('output').value = s.output || '';
	$('setOptions').checked = !!s.setOptions;
	$('options').value = s.options || '';
	$('icon').value = s.icon || '';
	for (const k of VERSION_KEYS) $('v_' + k).value = (s.version || {})[k] || '';
	security = s.security || {};
	for (const r of document.querySelectorAll('input[name=dataMode]')) r.checked = r.value === (s.dataMode || 'none');
	$('data').value = s.data || '';
	$('rpf').value = s.rpf || '';
	$('dataName').value = s.dataName || 'data.xp3';
	$('signKey').value = s.signKey || '';
	renderSecurity();
	updateData();
}

function renderSecurity() {
	const box = $('security');
	box.replaceChildren();
	const keys = securityKeys.length ? securityKeys : Object.keys(security);
	if (!keys.length) { box.append(krt.el('span', { class: 'krt-note' }, '«調べる» で exe の項目を読み取ります')); return; }
	for (const k of keys) {
		const sel = krt.el('select', { 'data-key': k });
		const opts = [['', '変えない'], ['0', '0'], ['1', '1']];
		if (k === 'forcedataxp3') opts.push(['2', '2']);
		for (const [v, t] of opts) {
			const o = krt.el('option', { value: v }, t);
			if (String(security[k] ?? '') === v) o.selected = true;
			sel.append(o);
		}
		box.append(krt.el('label', {}, k + ' ', sel));
	}
}

function updateData() {
	$('dataRows').hidden = document.querySelector('input[name=dataMode]:checked').value === 'none';
}
for (const r of document.querySelectorAll('input[name=dataMode]')) r.addEventListener('change', updateData);

async function inspect() {
	$('info').textContent = '';
	if (!$('exe').value) return;
	try {
		const i = await app.post('/api/release/inspect', { exe: $('exe').value, base: base() });
		securityKeys = Object.keys(i.security);
		security = collect().security;
		renderSecurity();
		for (const k of VERSION_KEYS) $('v_' + k).placeholder = i.version[k] ? `空 = 変えない (今: ${i.version[k]})` : '空 = 変えない';
		const lines = [`${i.kind === 'winver' ? 'WINVER' : 'SDL'} 版、${i.fileSize} バイト`];
		if (i.hasOverlay) lines.push(`⚠ 後ろにデータが付いています (${i.fileSize - i.imageEnd} バイト)。何も結合していない exe を指定してください`);
		lines.push('セキュリティ設定: ' + Object.entries(i.security).map(([k, v]) => `${k}(${v})`).join(' '));
		lines.push('埋め込みオプション: ' + (i.options ? '\n' + i.options : '(なし)'));
		$('info').textContent = lines.join('\n');
	} catch (e) { $('info').textContent = String(e.message || e); }
}

$('inspect').addEventListener('click', inspect);
$('exeBrowse').addEventListener('click', async () => {
	const f = await krt.pickFiles(dirOf($('exe').value) || base() || undefined, { multiple: false });
	if (f && f.length) { $('exe').value = f[0]; inspect(); }
});
$('outBrowse').addEventListener('click', async () => {
	const d = await krt.pickFolder(dirOf($('output').value) || base() || undefined);
	if (d) $('output').value = d + '/' + ($('output').value.replace(/^.*[\\/]/, '') || 'game.exe');
});
for (const [btn, id] of [['iconBrowse', 'icon'], ['dataFile', 'data'], ['rpfBrowse', 'rpf'], ['keyBrowse', 'signKey']]) {
	$(btn).addEventListener('click', async () => {
		const f = await krt.pickFiles(dirOf($(id).value) || base() || undefined, { multiple: false });
		if (f && f.length) $(id).value = f[0];
	});
}
$('dataDir').addEventListener('click', async () => {
	const d = await krt.pickFolder($('data').value || base() || undefined);
	if (d) $('data').value = d;
});

$('open').addEventListener('click', async () => {
	const f = await krt.pickFiles(base() || undefined, { multiple: false });
	if (!f || !f.length) return;
	try {
		const s = await app.post('/api/release/load', { file: f[0] });
		$('file').value = f[0];
		apply(s);
		inspect();
	} catch (e) { $('jobArea').hidden = false; $('jobStatus').textContent = String(e.message || e); }
});
$('save').addEventListener('click', async () => {
	$('jobArea').hidden = false;
	if (!$('file').value) { $('jobStatus').textContent = '設定ファイルの名前を入れてください'; return; }
	try {
		await app.post('/api/release/save', { file: $('file').value, settings: collect() });
		$('jobStatus').textContent = '保存しました: ' + $('file').value;
	} catch (e) { $('jobStatus').textContent = String(e.message || e); }
});

$('run').addEventListener('click', async () => {
	$('jobArea').hidden = false;
	$('result').replaceChildren();
	$('jobStatus').textContent = '';
	try {
		await app.post('/api/release/run', { settings: collect(), base: base(), force: $('force').checked });
	} catch (e) { $('jobStatus').textContent = String(e.message || e); }
});

function onJob(st) {
	const running = st.state === 'running';
	$('run').disabled = running;
	const bar = $('progress').firstElementChild;
	bar.style.width = running ? Math.max(0, Math.round(st.ratio * 100)) + '%' : (st.state === 'done' ? '100%' : '0');
	if (running) { $('jobStatus').textContent = st.message || ''; return; }
	if (st.state === 'failed') { $('jobStatus').textContent = '失敗しました: ' + (st.error || ''); return; }
	if (st.state !== 'done' || !st.result) return;
	const r = st.result;
	if (!r.ok) { $('jobStatus').textContent = '失敗しました: ' + r.message; return; }
	$('jobStatus').textContent = '作成しました';
	$('result').replaceChildren(
		krt.el('ul', { class: 'notes' }, ...r.notes.map(n => krt.el('li', {}, n))),
		krt.el('div', { class: 'krt-note' }, '作ったファイル:\n' + r.outputs.join('\n')));
}

async function main() {
	await krt.init();
	renderSecurity();
	updateData();
	const args = krt.info.args || [];
	if (args.length) {
		try {
			const s = await app.post('/api/release/load', { file: args[0] });
			$('file').value = args[0];
			apply(s);
			inspect();
		} catch (e) { /* 設定ファイルでなければ無視 */ }
	}
	await krt.watchJob(onJob);
}
main();
