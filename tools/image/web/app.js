import { app, krt } from './common/krt.js';

const $ = (id) => document.getElementById(id);
const dirOf = (p) => (p || '').replace(/[\\/][^\\/]*$/, '');
const baseName = (p) => (p || '').replace(/^.*[\\/]/, '');
let files = [];

async function refreshFiles() {
	$('files').replaceChildren();
	if (!files.length) return;
	const r = await app.post('/api/image/info', { files });
	for (const i of r.files) {
		if (i.error) {
			$('files').append(krt.el('tr', {}, krt.el('td', {}, i.path), krt.el('td', { class: 'krt-bad', colspan: '5' }, i.error)));
			continue;
		}
		const notes = [];
		if (i.mask) notes.push('マスク ' + baseName(i.mask));
		if (i.layers !== undefined) notes.push(`レイヤ ${i.layers} 枚`);
		if (i.tags.mode) notes.push('mode=' + i.tags.mode);
		if (i.tags.offs_x !== undefined) notes.push(`offs ${i.tags.offs_x},${i.tags.offs_y}`);
		$('files').append(krt.el('tr', {},
			krt.el('td', {}, i.path),
			krt.el('td', {}, i.format.toUpperCase()),
			krt.el('td', { class: 'num' }, i.width),
			krt.el('td', { class: 'num' }, i.height),
			krt.el('td', {}, i.transparent ? 'あり' : ''),
			krt.el('td', { class: 'msg' }, notes.join(' / '))));
	}
	for (const s of r.skipped)
		$('files').append(krt.el('tr', {}, krt.el('td', {}, s), krt.el('td', { class: 'msg', colspan: '5' }, 'メイン画像のマスクとして読みます')));
}

$('add').addEventListener('click', async () => {
	const fs = await krt.pickFiles(dirOf(files[files.length - 1]) || undefined, { multiple: true });
	if (!fs) return;
	for (const f of fs) if (!files.includes(f)) files.push(f);
	refreshFiles();
});
$('clear').addEventListener('click', () => { files = []; refreshFiles(); });

function updateOpts() {
	$('qWrap').hidden = $('opaque').value !== 'jpg';
	$('sepWrap').hidden = $('alpha').value !== 'sep';
	if ($('inputAddAlpha').checked) $('addAlpha').checked = true;
	$('addAlpha').disabled = $('inputAddAlpha').checked;
	$('transparent').disabled = $('addAlpha').checked;
}
for (const id of ['opaque', 'alpha', 'inputAddAlpha', 'addAlpha']) $(id).addEventListener('change', updateOpts);

$('outBrowse').addEventListener('click', async () => {
	const d = await krt.pickFolder($('outDir').value || dirOf(files[0]) || undefined);
	if (d) $('outDir').value = d;
});

$('convert').addEventListener('click', async () => {
	$('jobArea').hidden = false;
	$('results').replaceChildren();
	$('jobStatus').textContent = '';
	try {
		await app.post('/api/image/run', {
			files, opaque: $('opaque').value, alpha: $('alpha').value,
			sepMain: $('sepMain').value, sepMask: $('sepMask').value,
			quality: Number($('quality').value) || 90, mainQuality: Number($('mainQuality').value) || 90,
			maskQuality: Number($('maskQuality').value) || 90,
			transparent: $('transparent').value, inputAddAlpha: $('inputAddAlpha').checked, addAlpha: $('addAlpha').checked,
			outDir: $('outDir').value, force: $('force').checked,
		});
	} catch (e) { $('jobStatus').textContent = String(e.message || e); }
});
$('cancel').addEventListener('click', () => krt.cancelJob());

function render(r) {
	const tb = $('results');
	tb.replaceChildren();
	for (const e of r.entries) {
		tb.append(krt.el('tr', {}, krt.el('td', {}, e.path),
			krt.el('td', { class: e.ok ? 'krt-ok' : 'krt-bad' }, e.ok ? '変換した' : '失敗'),
			krt.el('td', { class: 'msg' }, e.ok ? `${e.outputs.map(baseName).join(' / ')}  (${e.mode})` : e.message)));
	}
}

function onJob(st) {
	const running = st.state === 'running';
	$('convert').disabled = running;
	$('cancel').disabled = !running;
	const bar = $('progress').firstElementChild;
	bar.style.width = running ? Math.max(0, Math.round(st.ratio * 100)) + '%' : (st.state === 'done' ? '100%' : '0');
	if (running) $('jobStatus').textContent = st.message || '';
	else if (st.state === 'done' && st.result) { $('jobStatus').textContent = ''; render(st.result); }
	else if (st.state === 'failed') $('jobStatus').textContent = '失敗しました: ' + (st.error || '');
	else if (st.state === 'canceled') $('jobStatus').textContent = '中断しました';
}

async function main() {
	await krt.init();
	updateOpts();
	files = (krt.info.args || []).slice();
	refreshFiles();
	await krt.watchJob(onJob);
}
main();
