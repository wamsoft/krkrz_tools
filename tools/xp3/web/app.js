import { app, krt } from './common/krt.js';

const $ = (id) => document.getElementById(id);
const dirOf = (p) => (p || '').replace(/[\\/][^\\/]*$/, '');
const stem = (p) => (p || '').replace(/^.*[\\/]/, '').replace(/\.[^.]*$/, '');

for (const b of document.querySelectorAll('.krt-tabs button')) {
	b.addEventListener('click', () => {
		for (const x of document.querySelectorAll('.krt-tabs button')) x.classList.toggle('active', x === b);
		for (const p of document.querySelectorAll('.page')) p.hidden = p.id !== 'page-' + b.dataset.page;
		$('jobArea').hidden = true;
	});
}

//---------------------------------------------------------------------------
// 作成
//---------------------------------------------------------------------------
const actionLabel = { compress: '圧縮', store: '格納のみ', discard: '入れない' };

async function scan() {
	const dir = $('dir').value;
	if (!dir) return;
	$('exts').replaceChildren();
	try {
		const r = await app.post('/api/xp3/scan', { dir });
		$('out').value = r.output;
		$('rpfNote').textContent = r.rpf ? `プロファイル ${r.rpf} の設定を読みました。` : '';
		$('protect').checked = r.protect;
		$('compressIndex').checked = r.compressIndex;
		$('doLimit').checked = r.doLimit;
		$('limitKB').value = r.limitKB;
		for (const e of r.exts) {
			const sel = krt.el('select', { 'data-ext': e.ext });
			for (const [k, v] of Object.entries(actionLabel)) {
				const o = krt.el('option', { value: k }, v);
				if (k === e.action) o.selected = true;
				sel.append(o);
			}
			$('exts').append(krt.el('tr', {},
				krt.el('td', {}, e.ext || '(拡張子なし)'),
				krt.el('td', { class: 'num' }, e.count),
				krt.el('td', { class: 'num' }, krt.formatSize(e.bytes)),
				krt.el('td', {}, sel)));
		}
	} catch (e) {
		$('rpfNote').textContent = String(e.message || e);
	}
}
$('dir').addEventListener('change', scan);
$('dirBrowse').addEventListener('click', async () => {
	const d = await krt.pickFolder($('dir').value);
	if (d) { $('dir').value = d; scan(); }
});
$('packRun').addEventListener('click', async () => {
	const actions = {};
	for (const s of document.querySelectorAll('#exts select')) actions[s.dataset.ext] = s.value;
	showJob();
	try {
		await app.post('/api/xp3/pack', {
			dir: $('dir').value, out: $('out').value, actions,
			protect: $('protect').checked, compressIndex: $('compressIndex').checked,
			doLimit: $('doLimit').checked, limitKB: Number($('limitKB').value) || 0,
			saveRpf: $('saveRpf').checked,
		});
	} catch (e) { $('jobStatus').textContent = String(e.message || e); }
});

//---------------------------------------------------------------------------
// 中身を見る
//---------------------------------------------------------------------------
async function openArchive() {
	$('list').replaceChildren();
	try {
		const r = await app.post('/api/xp3/list', { file: $('file').value });
		let prot = 0;
		for (const e of r.entries) {
			if (e.protected) prot++;
			$('list').append(krt.el('tr', {},
				krt.el('td', {}, e.name),
				krt.el('td', { class: 'num' }, krt.formatSize(e.size)),
				krt.el('td', { class: 'num' }, krt.formatSize(e.arcSize)),
				krt.el('td', {}, e.compressed ? 'zlib' : '-'),
				krt.el('td', { class: e.protected ? 'krt-warn' : '' }, e.protected ? '保護' : '')));
		}
		$('listNote').textContent = `${r.entries.length} ファイル` + (prot ? ` (展開プロテクト ${prot} — 展開しません)` : '') +
			(r.offset ? ` / 実行ファイルに結合 (位置 ${r.offset})` : '');
		if (!$('extractDir').value) $('extractDir').value = dirOf($('file').value) + '/' + stem($('file').value);
	} catch (e) {
		$('listNote').textContent = '開けません: ' + (e.message || e);
	}
}
$('open').addEventListener('click', openArchive);
$('fileBrowse').addEventListener('click', async () => {
	const f = await krt.pickFiles(dirOf($('file').value) || undefined);
	if (f) { $('file').value = f; openArchive(); }
});
$('extractBrowse').addEventListener('click', async () => {
	const d = await krt.pickFolder(dirOf($('extractDir').value) || undefined);
	if (d) $('extractDir').value = d;
});
$('verify').addEventListener('click', async () => {
	showJob();
	try { await app.post('/api/xp3/verify', { file: $('file').value }); }
	catch (e) { $('jobStatus').textContent = String(e.message || e); }
});
$('extract').addEventListener('click', async () => {
	showJob();
	try { await app.post('/api/xp3/extract', { file: $('file').value, dir: $('extractDir').value }); }
	catch (e) { $('jobStatus').textContent = String(e.message || e); }
});

//---------------------------------------------------------------------------
// 進捗と結果
//---------------------------------------------------------------------------
function showJob() {
	$('jobArea').hidden = false;
	$('jobStatus').textContent = '';
	$('jobResult').replaceChildren();
}
for (const id of ['cancel1', 'cancel2']) $(id).addEventListener('click', () => krt.cancelJob());

function renderResult(r, name) {
	const box = $('jobResult');
	box.replaceChildren();
	if (name === 'pack') {
		box.append(krt.el('div', { class: r.canceled ? 'krt-warn' : 'krt-ok' },
			`${r.canceled ? '中断しました' : '作成しました'}: ${r.output}`),
			krt.el('div', { class: 'krt-note' },
				`${r.files} ファイル (圧縮 ${r.compressed} / 中身が同じで共有 ${r.deduplicated} / 入れない ${r.discarded})  ` +
				`${krt.formatSize(r.orgBytes)} → ${krt.formatSize(r.arcBytes)}`));
	} else if (r.mode === 'verify') {
		box.append(krt.el('div', { class: r.broken ? 'krt-bad' : 'krt-ok' }, `正常 ${r.ok} / 破損 ${r.broken}`));
		for (const e of r.errors) box.append(krt.el('div', { class: 'krt-note' }, `破損: ${e.name} — ${e.message}`));
	} else if (r.mode === 'extract') {
		box.append(krt.el('div', { class: r.failed ? 'krt-bad' : 'krt-ok' },
			`展開 ${r.ok} / 展開プロテクトで除外 ${r.protected} / 失敗 ${r.failed}`));
	}
}

function onJob(st) {
	const running = st.state === 'running';
	for (const id of ['packRun', 'verify', 'extract', 'open']) $(id).disabled = running;
	for (const id of ['cancel1', 'cancel2']) $(id).disabled = !running;
	const bar = $('progress').firstElementChild;
	bar.style.width = running ? Math.max(0, Math.round(st.ratio * 100)) + '%' : (st.state === 'done' ? '100%' : '0');
	if (running) $('jobStatus').textContent = st.message || '';
	else if (st.state === 'done' && st.result) { $('jobStatus').textContent = ''; renderResult(st.result, st.name); }
	else if (st.state === 'failed') $('jobStatus').textContent = '失敗しました: ' + (st.error || '');
	else if (st.state === 'canceled') $('jobStatus').textContent = '中断しました';
}

async function main() {
	await krt.init();
	const args = krt.info.args || [];
	if (args[0]) {
		if (/\.(xp3|exe)$/i.test(args[0])) {
			document.querySelector('[data-page=view]').click();
			$('file').value = args[0];
			openArchive();
		} else {
			$('dir').value = args[0];
			scan();
		}
	}
	await krt.watchJob(onJob);
}
main();
