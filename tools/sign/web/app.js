import { app, krt } from './common/krt.js';

const $ = (id) => document.getElementById(id);
const lines = (id) => $(id).value.split(/\r?\n/).map(s => s.trim()).filter(Boolean);
const dirOf = (p) => (p || '').replace(/[\\/][^\\/]*$/, '');

// タブ
for (const b of document.querySelectorAll('.krt-tabs button')) {
	b.addEventListener('click', () => {
		for (const x of document.querySelectorAll('.krt-tabs button')) x.classList.toggle('active', x === b);
		for (const p of document.querySelectorAll('.page')) p.hidden = p.id !== 'page-' + b.dataset.page;
		$('jobArea').hidden = true;
	});
}

// 鍵の生成
$('keygen').addEventListener('click', async () => {
	$('keygen').disabled = true;
	$('keygenStatus').textContent = '鍵を作っています…';
	try {
		const r = await app.post('/api/sign/keygen', { bits: Number($('bits').value) });
		$('publicKey').value = r.publicKey;
		$('privateKey').value = r.privateKey;
		$('saveKeys').disabled = false;
		$('keygenStatus').textContent = '鍵を作りました。保存してください。';
	} catch (e) {
		$('keygenStatus').textContent = '鍵を作れませんでした: ' + (e.message || e);
	}
	$('keygen').disabled = false;
});
$('saveDirBrowse').addEventListener('click', async () => {
	const d = await krt.pickFolder($('saveDir').value);
	if (d) $('saveDir').value = d;
});
$('saveKeys').addEventListener('click', async () => {
	const dir = $('saveDir').value.replace(/[\\/]+$/, '');
	const pub = dir + '/' + $('publicName').value;
	const priv = dir + '/' + $('privateName').value;
	try {
		await app.post('/api/sign/savekeys', {
			publicPath: pub, privatePath: priv,
			publicKey: $('publicKey').value, privateKey: $('privateKey').value,
			overwrite: $('overwrite').checked,
		});
		$('keygenStatus').textContent = `保存しました: ${pub} / ${priv}`;
		// 次の手順で使えるよう、署名・確認の鍵欄へ入れておく
		$('signKey').value = priv;
		$('verifyKey').value = pub;
	} catch (e) {
		$('keygenStatus').textContent = '保存できませんでした: ' + (e.message || e);
	}
});

// ファイル選択
async function browseKey(inputId) {
	const f = await krt.pickFiles(dirOf($(inputId).value) || undefined);
	if (f) $(inputId).value = f;
}
async function addFiles(textareaId) {
	const cur = lines(textareaId);
	const fs = await krt.pickFiles(dirOf(cur[cur.length - 1]) || undefined, { multiple: true });
	if (fs) $(textareaId).value = cur.concat(fs.filter(f => !cur.includes(f))).join('\n');
}
$('signKeyBrowse').addEventListener('click', () => browseKey('signKey'));
$('verifyKeyBrowse').addEventListener('click', () => browseKey('verifyKey'));
$('signAdd').addEventListener('click', () => addFiles('signFiles'));
$('verifyAdd').addEventListener('click', () => addFiles('verifyFiles'));
$('signClear').addEventListener('click', () => { $('signFiles').value = ''; });
$('verifyClear').addEventListener('click', () => { $('verifyFiles').value = ''; });

// 署名 / 確認
async function run(kind) {
	$('rows').replaceChildren();
	$('jobArea').hidden = false;
	$('jobStatus').textContent = '';
	try {
		await app.post('/api/sign/' + kind, {
			files: lines(kind + 'Files'),
			keyFile: $(kind + 'Key').value,
		});
	} catch (e) {
		$('jobStatus').textContent = String(e.message || e);
	}
}
$('signRun').addEventListener('click', () => run('sign'));
$('verifyRun').addEventListener('click', () => run('verify'));

const cls = { ok: 'krt-ok', broken: 'krt-bad', error: 'krt-warn', canceled: 'krt-warn' };
function render(result) {
	const rows = $('rows');
	rows.replaceChildren();
	for (const e of result.entries) {
		let label, c, msg = e.message || '';
		if (result.mode === 'sign') {
			label = e.ok ? '署名した' : '失敗';
			c = e.ok ? 'krt-ok' : 'krt-bad';
			if (e.ok) msg = e.embedded ? 'exe に埋め込み' : e.written;
		} else {
			label = e.label;
			c = cls[e.status] || '';
		}
		rows.append(krt.el('tr', {}, krt.el('td', {}, e.path), krt.el('td', { class: c }, label), krt.el('td', { class: 'msg' }, msg)));
	}
}

function onJob(st) {
	const running = st.state === 'running';
	for (const id of ['signRun', 'verifyRun', 'keygen']) $(id).disabled = running;
	const bar = $('progress').firstElementChild;
	bar.style.width = running ? Math.max(0, Math.round(st.ratio * 100)) + '%' : (st.state === 'done' ? '100%' : '0');
	if (running) $('jobStatus').textContent = st.message || '';
	else if (st.state === 'done' && st.result) { $('jobStatus').textContent = ''; render(st.result); }
	else if (st.state === 'failed') $('jobStatus').textContent = '失敗しました: ' + (st.error || '');
}

async function main() {
	const info = await krt.init();
	$('saveDir').value = dirOf(info.self);
	await krt.watchJob(onJob);
}
main();
