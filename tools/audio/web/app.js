import { app, krt } from './common/krt.js';

const $ = (id) => document.getElementById(id);
const dirOf = (p) => (p || '').replace(/[\\/][^\\/]*$/, '');
let files = [];

for (const b of document.querySelectorAll('.krt-tabs button')) {
	b.addEventListener('click', () => {
		for (const x of document.querySelectorAll('.krt-tabs button')) x.classList.toggle('active', x === b);
		for (const p of document.querySelectorAll('.page')) p.hidden = p.id !== 'page-' + b.dataset.page;
		$('jobArea').hidden = true;
	});
}

async function refreshFiles() {
	$('files').replaceChildren();
	if (!files.length) return;
	const infos = await app.post('/api/audio/info', { files });
	for (const i of infos) {
		if (i.error) {
			$('files').append(krt.el('tr', {}, krt.el('td', {}, i.path), krt.el('td', { class: 'krt-bad', colspan: '6' }, i.error)));
			continue;
		}
		$('files').append(krt.el('tr', {},
			krt.el('td', {}, i.path),
			krt.el('td', {}, i.format === 'ogg' ? 'Ogg Vorbis' : i.format === 'opus' ? 'Ogg Opus' : `WAV ${i.bits}bit${i.float ? ' float' : ''}`),
			krt.el('td', { class: 'num' }, i.sampleRate),
			krt.el('td', { class: 'num' }, i.channels),
			krt.el('td', { class: 'num' }, i.seconds.toFixed(2)),
			krt.el('td', {}, i.format === 'opus' && i.headerGainDb ? `${i.headerGainDb > 0 ? '+' : ''}${i.headerGainDb.toFixed(2)} dB` :
				(i.tags.REPLAYGAIN_TRACK_GAIN || i.tags.replaygain_track_gain || '')),
			krt.el('td', {}, i.hasSli ? 'あり' : '')));
	}
}

$('add').addEventListener('click', async () => {
	const fs = await krt.pickFiles(dirOf(files[files.length - 1]) || undefined, { multiple: true });
	if (!fs) return;
	for (const f of fs) if (!files.includes(f)) files.push(f);
	refreshFiles();
});
$('clear').addEventListener('click', () => { files = []; refreshFiles(); });

function updateFormatOpts() {
	const to = $('to').value;
	$('qWrap').hidden = to !== 'ogg';
	$('bWrap').hidden = to !== 'opus';
	$('wWrap').hidden = to !== 'wav';
	$('rgWrap').hidden = to !== 'ogg';
	$('gainNote').textContent = to === 'opus'
		? 'Opus の音量はヘッダゲインで指定します (波形は変えません。本体は常に適用します)。Opus は 48kHz になるので、.sli の位置も換算します。'
		: '音量は波形に焼き込みます。Opus から変換するときは、元のヘッダゲインも焼き込みます。';
}
$('to').addEventListener('change', updateFormatOpts);
$('outBrowse').addEventListener('click', async () => {
	const d = await krt.pickFolder($('outDir').value || dirOf(files[0]) || undefined);
	if (d) $('outDir').value = d;
});

async function run(mode) {
	$('jobArea').hidden = false;
	$('results').replaceChildren();
	$('jobStatus').textContent = '';
	try {
		await app.post('/api/audio/run', {
			files, mode, to: $('to').value, outDir: $('outDir').value,
			quality: Number($('quality').value), bitrate: Number($('bitrate').value) || 0, bits: Number($('bits').value),
			gain: Number($('gain').value) || 0,
			normalize: $('doNormalize').checked ? Number($('normalize').value) : null,
			replaygain: $('replaygain').checked, force: $('force').checked,
		});
	} catch (e) { $('jobStatus').textContent = String(e.message || e); }
}
$('convert').addEventListener('click', () => run('convert'));
$('measure').addEventListener('click', () => run('loudness'));
$('cancel').addEventListener('click', () => krt.cancelJob());

$('lipRun').addEventListener('click', async () => {
	if (!files.length) return;
	try {
		const r = await app.post('/api/audio/lipsync', {
			file: files[0], fps: Number($('fps').value) || 30, format: $('lipFormat').value, out: $('lipOut').value,
		});
		$('lipText').value = r.text;
	} catch (e) { $('lipText').value = String(e.message || e); }
});

function render(r) {
	const tb = $('results');
	tb.replaceChildren();
	for (const e of r.entries) {
		if (r.mode === 'loudness') {
			tb.append(krt.el('tr', {}, krt.el('td', {}, e.path), e.ok
				? krt.el('td', {}, `${e.lufs.toFixed(2)} LUFS / ピーク ${e.peak.toFixed(4)} / -18 LUFS まで ${e.replayGainDb >= 0 ? '+' : ''}${e.replayGainDb.toFixed(2)} dB`)
				: krt.el('td', { class: 'krt-bad' }, e.message)));
		} else {
			let note = e.ok ? e.output : e.message;
			if (e.ok && e.sli) note += `  (.sli${e.sliRescaled ? ' ' + e.sliRescaled : ''})`;
			if (e.ok && e.replayGain) note += `  ReplayGain ${e.replayGain}`;
			tb.append(krt.el('tr', {}, krt.el('td', {}, e.path),
				krt.el('td', { class: e.ok ? 'krt-ok' : 'krt-bad' }, e.ok ? '変換した' : '失敗'),
				krt.el('td', { class: 'msg' }, note)));
		}
	}
}

function onJob(st) {
	const running = st.state === 'running';
	for (const id of ['convert', 'measure']) $(id).disabled = running;
	$('cancel').disabled = !running;
	const bar = $('progress').firstElementChild;
	bar.style.width = running ? Math.max(0, Math.round(st.ratio * 100)) + '%' : (st.state === 'done' ? '100%' : '0');
	if (running) $('jobStatus').textContent = st.message || '';
	else if (st.state === 'done' && st.result) { $('jobStatus').textContent = ''; render(st.result); if (st.result.mode === 'convert') refreshFiles(); }
	else if (st.state === 'failed') $('jobStatus').textContent = '失敗しました: ' + (st.error || '');
	else if (st.state === 'canceled') $('jobStatus').textContent = '中断しました';
}

async function main() {
	await krt.init();
	updateFormatOpts();
	files = (krt.info.args || []).slice();
	refreshFiles();
	await krt.watchJob(onJob);
}
main();
