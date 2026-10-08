import { app, krt } from './common/krt.js';

const $ = (id) => document.getElementById(id);

const statusClass = { ok: 'krt-ok', broken: 'krt-bad', error: 'krt-warn', canceled: 'krt-warn' };

function render(result) {
	const rows = $('rows');
	rows.replaceChildren();
	for (const e of result.entries) {
		rows.append(krt.el('tr', {},
			krt.el('td', {}, e.path),
			krt.el('td', { class: statusClass[e.status] || '' }, e.label),
			krt.el('td', { class: 'num' }, krt.formatSize(e.size)),
			krt.el('td', {}, krt.formatTime(e.mtime)),
			krt.el('td', { class: 'msg' }, e.message || '')));
	}
	let s = `正常 ${result.ok} / 破損 ${result.broken} / エラー ${result.error}`;
	if (result.canceled) s += ' (中断)';
	$('summary').textContent = s;
	$('summary').className = result.broken ? 'krt-bad' : (result.error ? 'krt-warn' : 'krt-ok');
	if (!result.entries.length) $('status').textContent = 'チェックの対象になるファイル (.sig のあるファイル / 署名付きの exe) がありませんでした。';
}

function onJob(st) {
	const running = st.state === 'running';
	$('check').disabled = running;
	$('browse').disabled = running;
	$('cancel').disabled = !running;
	const bar = $('progress');
	bar.classList.toggle('indeterminate', running && st.ratio < 0);
	bar.firstElementChild.style.width = running
		? (st.ratio < 0 ? '' : Math.round(st.ratio * 100) + '%')
		: (st.state === 'done' ? '100%' : '0');
	if (running) {
		$('status').textContent = st.message || 'チェック中…';
	} else if (st.state === 'done' && st.result) {
		$('status').textContent = '';
		render(st.result);
		$('copy').disabled = false;
	} else if (st.state === 'failed') {
		$('status').textContent = 'チェックできませんでした: ' + (st.error || '');
	} else if (st.state === 'canceled') {
		$('status').textContent = '中断しました';
		if (st.result) render(st.result);
	}
}

async function main() {
	await krt.init();
	const cfg = await app.get('/api/check/config');
	$('caption').textContent = cfg.caption;
	document.title = cfg.caption;
	$('notice').textContent = cfg.notice || '';
	$('root').value = cfg.root;
	if (!cfg.keyLoaded) {
		$('keyError').hidden = false;
		$('keyError').textContent = '公開鍵を読めません: ' + (cfg.error || '') +
			'\n(実行ファイルと同じ名前の .ini を置くか、--key で指定してください)';
		$('check').disabled = true;
	}

	$('browse').addEventListener('click', async () => {
		const dir = await krt.pickFolder($('root').value);
		if (dir) $('root').value = dir;
	});
	$('check').addEventListener('click', async () => {
		$('rows').replaceChildren();
		$('summary').textContent = '';
		$('copy').disabled = true;
		try {
			await app.post('/api/check/start', { root: $('root').value });
		} catch (e) {
			$('status').textContent = String(e.message || e);
		}
	});
	$('cancel').addEventListener('click', () => krt.cancelJob());
	$('copy').addEventListener('click', async () => {
		const tsv = await app.get('/api/check/report.tsv');
		await navigator.clipboard.writeText(tsv);
		$('status').textContent = '結果をクリップボードにコピーしました。';
	});

	if (cfg.keyLoaded) await krt.watchJob(onJob);
}

main();
