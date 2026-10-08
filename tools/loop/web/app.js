// ループチューナの画面
//
// 波形の表示・編集は全部ここ (サーバは音声の読み込みと .sli の読み書きだけ)。
// 再生は loop-worklet.js (本体 WaveLoopManager::Decode の移植)。
import { app, krt } from './common/krt.js';

const $ = (id) => document.getElementById(id);
const dirOf = (p) => (p || '').replace(/[\\/][^\\/]*$/, '');
const CONDITIONS = [
	['no', '条件なし'], ['eq', '= (等しい)'], ['ne', '≠ (等しくない)'],
	['gt', '> (フラグが値より大きい)'], ['ge', '≥ (フラグが値以上)'], ['lt', '< (フラグが値より小さい)'], ['le', '≤ (フラグが値以下)'],
];
const COND_SYMBOL = { no: '', eq: '=', ne: '≠', gt: '>', ge: '≥', lt: '<', le: '≤' };
const BLOCK = 256;          // 波形の縮約単位 (サンプル)
const UNDO_MAX = 200;

//---------------------------------------------------------------------------
// 状態
//---------------------------------------------------------------------------
const st = {
	file: '', sliPath: '', sampleRate: 44100, channels: [], frames: 0,
	peaks: [],                 // チャンネルごとの { min, max } (BLOCK 単位)
	links: [], labels: [],
	sel: null,                 // { kind: 'link' | 'label', index }
	caret: 0, rangeA: -1, rangeB: -1,
	view: { start: 0, spp: 1 },   // 左端のサンプル位置と 1px あたりのサンプル数
	undo: [], redo: [], modified: false,
	playing: false, playPos: -1, flags: new Array(16).fill(0),
};

//---------------------------------------------------------------------------
// 表示用の書式
//---------------------------------------------------------------------------
function fmtTime(samples) {
	const t = samples / st.sampleRate;
	const m = Math.floor(t / 60);
	const s = t - m * 60;
	return `${m}:${s.toFixed(3).padStart(6, '0')}`;
}
const fmtPos = (s) => `${fmtTime(s)} (${Math.round(s)})`;

function setMsg(text, bad = false) {
	$('stMsg').textContent = text;
	$('stMsg').classList.toggle('bad', bad);
}

//---------------------------------------------------------------------------
// 編集 (元に戻す / やり直し)
//---------------------------------------------------------------------------
const snapshot = () => JSON.stringify({ links: st.links, labels: st.labels });
function restore(s) {
	const o = JSON.parse(s);
	st.links = o.links;
	st.labels = o.labels;
	st.sel = null;
}
/// 変更の前に呼ぶ
function beginEdit() {
	st.undo.push(snapshot());
	if (st.undo.length > UNDO_MAX) st.undo.shift();
	st.redo = [];
}
function endEdit() {
	st.modified = true;
	sendData();
	refreshAll();
}
function undo() {
	if (!st.undo.length) return;
	st.redo.push(snapshot());
	restore(st.undo.pop());
	endEdit();
}
function redo() {
	if (!st.redo.length) return;
	st.undo.push(snapshot());
	restore(st.redo.pop());
	endEdit();
}

//---------------------------------------------------------------------------
// 読み込み・保存
//---------------------------------------------------------------------------
async function openFile(path) {
	if (st.modified && !confirm('保存していない変更があります。破棄して開きますか?')) return;
	stop();
	setMsg('読み込み中…');
	let info;
	try {
		info = await app.post('/api/loop/open', { file: path });
	} catch (e) { setMsg(String(e.message || e), true); return; }
	const buf = await app.bytes('/api/loop/pcm');
	const pcm = new Int16Array(buf);
	const ch = info.channels, frames = info.frames;
	const channels = [];
	for (let c = 0; c < ch; c++) channels.push(new Float32Array(frames));
	for (let i = 0, k = 0; i < frames; i++)
		for (let c = 0; c < ch; c++) channels[c][i] = pcm[k++] / 32768;
	st.file = info.path;
	st.sliPath = info.sliPath;
	st.sampleRate = info.sampleRate;
	st.channels = channels;
	st.frames = frames;
	st.peaks = channels.map(buildPeaks);
	st.links = (info.sli?.links || []).map(l => ({ ...l }));
	st.labels = (info.sli?.labels || []).map(l => ({ ...l }));
	st.sel = null;
	st.caret = 0; st.rangeA = st.rangeB = -1;
	st.undo = []; st.redo = []; st.modified = false;
	st.playPos = -1;
	overviewCache = null;
	$('file').textContent = info.path;
	$('file').title = info.path;
	zoomAll();
	await loadAudio();
	refreshAll();
	if (info.sliError) setMsg(info.sliError, true);
	else setMsg(`${info.format.toUpperCase()} ${info.sampleRate} Hz ${ch} ch ${fmtTime(frames)}` + (info.sli ? '  .sli を読み込みました' : '  .sli はありません (保存すると作ります)'));
}

async function save() {
	if (!st.file) return;
	try {
		const r = await app.post('/api/loop/save', { file: st.file, links: st.links, labels: st.labels, frames: st.frames });
		st.modified = false;
		refreshAll();
		if (r.problems.length) setMsg('保存しました。注意: ' + r.problems.join(' / '), true);
		else setMsg('保存しました: ' + r.sliPath);
	} catch (e) { setMsg('保存できません: ' + (e.message || e), true); }
}

function buildPeaks(data) {
	const n = Math.ceil(data.length / BLOCK);
	const min = new Float32Array(n), max = new Float32Array(n);
	for (let b = 0; b < n; b++) {
		let lo = 1, hi = -1;
		const end = Math.min(data.length, (b + 1) * BLOCK);
		for (let i = b * BLOCK; i < end; i++) { const v = data[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
		min[b] = lo; max[b] = hi;
	}
	return { min, max };
}

//---------------------------------------------------------------------------
// 再生 (AudioWorklet)
//---------------------------------------------------------------------------
let ctx = null, node = null;

async function loadAudio() {
	if (ctx && ctx.sampleRate !== st.sampleRate) { await ctx.close(); ctx = null; node = null; }
	if (!ctx) {
		// 音声と同じサンプルレートで作る (再生位置 = ファイルのサンプル位置)
		ctx = new AudioContext({ sampleRate: st.sampleRate });
		await ctx.audioWorklet.addModule('./loop-worklet.js');
	}
	if (node) node.disconnect();
	node = new AudioWorkletNode(ctx, 'loop-processor', { outputChannelCount: [Math.max(1, st.channels.length)] });
	node.connect(ctx.destination);
	node.port.onmessage = (e) => onWorklet(e.data);
	node.port.postMessage({ type: 'load', channels: st.channels.map(c => c.slice()), frames: st.frames });
	sendData();
	sendFlags();
	sendOptions();
}

function sendData() { node?.port.postMessage({ type: 'data', links: st.links, labels: st.labels }); }
function sendFlags() { node?.port.postMessage({ type: 'flags', flags: st.flags }); }
function sendOptions() { node?.port.postMessage({ type: 'options', looping: $('looping').checked, ignoreLinks: $('ignoreLinks').checked }); }

async function play(position, duration) {
	if (!node) return;
	await ctx.resume();
	node.port.postMessage({ type: 'play', position, duration });
}
function stop() { node?.port.postMessage({ type: 'stop' }); }

function onWorklet(m) {
	if (m.type !== 'state') return;
	const wasPlaying = st.playing;
	st.playing = m.playing;
	st.playPos = m.playing ? m.position : (wasPlaying ? m.position : st.playPos);
	let changed = false;
	for (let i = 0; i < 16; i++) if (st.flags[i] !== m.flags[i]) { st.flags[i] = m.flags[i]; changed = true; }
	if (changed) refreshFlags();
	for (const ev of m.events) {
		if (ev.kind === 'jump') setMsg(`リンク ${fmtPos(ev.from)} → ${fmtPos(ev.to)}`);
		else if (ev.kind === 'label') setMsg(`ラベル «${ev.name}» (${fmtPos(ev.position)})`);
		else if (ev.kind === 'rewind') setMsg('末尾から先頭へ');
	}
	if (st.playing && $('follow').checked) followPlay();
	refreshButtons();
	requestDraw();
}

function followPlay() {
	const w = waveWidth();
	const vs = st.view.start, ve = vs + w * st.view.spp;
	if (st.playPos < vs || st.playPos > ve - w * st.view.spp * 0.1) setViewStart(st.playPos - w * st.view.spp * 0.1);
}

//---------------------------------------------------------------------------
// 表示範囲
//---------------------------------------------------------------------------
const waveCanvas = $('wave'), ovCanvas = $('overview');
const waveWidth = () => waveCanvas.clientWidth || 1;
function clampView() {
	const w = waveWidth();
	const maxSpp = Math.max(st.frames / w, 1 / 64);
	st.view.spp = Math.min(Math.max(st.view.spp, 1 / 64), maxSpp * 1.0001);
	const maxStart = Math.max(0, st.frames - w * st.view.spp);
	st.view.start = Math.min(Math.max(st.view.start, 0), maxStart);
}
function setViewStart(s) { st.view.start = s; clampView(); requestDraw(); }
function zoomAt(factor, px) {
	const at = st.view.start + px * st.view.spp;
	st.view.spp *= factor;
	clampView();
	st.view.start = at - px * st.view.spp;
	clampView();
	refreshStatus();
	requestDraw();
}
function zoomAll() { st.view.spp = Math.max(st.frames / waveWidth(), 1 / 64); st.view.start = 0; clampView(); refreshStatus(); requestDraw(); }
const xOf = (s) => (s - st.view.start) / st.view.spp;
const sampleAt = (x) => Math.round(st.view.start + x * st.view.spp);
const clampSample = (s) => Math.min(Math.max(0, Math.round(s)), st.frames);

//---------------------------------------------------------------------------
// 描画
//---------------------------------------------------------------------------
const RULER_H = 20, LABEL_H = 22, TIER_H = 16;
let drawPending = false;
function requestDraw() {
	if (drawPending) return;
	drawPending = true;
	requestAnimationFrame(() => { drawPending = false; draw(); });
}

function css(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

function fitCanvas(c) {
	const dpr = window.devicePixelRatio || 1;
	const w = c.clientWidth, h = c.clientHeight;
	if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
		c.width = Math.round(w * dpr);
		c.height = Math.round(h * dpr);
	}
	const g = c.getContext('2d');
	g.setTransform(dpr, 0, 0, dpr, 0, 0);
	return { g, w, h };
}

/// リンクの段 (重ならないように振り分け)
function linkTiers() {
	const order = st.links.map((l, i) => ({ i, a: Math.min(l.from, l.to), b: Math.max(l.from, l.to) }))
		.sort((p, q) => p.a - q.a || p.b - q.b);
	const ends = [];
	const tier = new Array(st.links.length).fill(0);
	for (const o of order) {
		let t = ends.findIndex(e => e < o.a);
		if (t < 0) { t = ends.length; ends.push(0); }
		ends[t] = o.b;
		tier[o.i] = t;
	}
	return { tier, count: Math.max(ends.length, 2) };
}

function layout(h) {
	const tiers = linkTiers();
	const linkH = tiers.count * TIER_H + 8;
	const waveTop = RULER_H + LABEL_H;
	const waveH = Math.max(40, h - waveTop - linkH);
	return { tiers, linkTop: waveTop + waveH, linkH, waveTop, waveH };
}

function drawWaveLane(g, data, peaks, x0, w, top, h) {
	const mid = top + h / 2, amp = h / 2 - 2;
	g.beginPath();
	const spp = st.view.spp;
	if (spp < 1.5) {
		// 1 サンプルずつの線 (十分に拡大したとき)
		const s0 = Math.max(0, Math.floor(st.view.start) - 1), s1 = Math.min(data.length - 1, Math.ceil(st.view.start + w * spp) + 1);
		for (let s = s0; s <= s1; s++) {
			const x = xOf(s), y = mid - data[s] * amp;
			if (s === s0) g.moveTo(x, y); else g.lineTo(x, y);
		}
		g.stroke();
		if (spp < 0.25) {
			for (let s = s0; s <= s1; s++) g.fillRect(xOf(s) - 1.5, mid - data[s] * amp - 1.5, 3, 3);
		}
		return;
	}
	for (let x = x0; x < x0 + w; x++) {
		const a = st.view.start + x * spp, b = a + spp;
		let lo = 1, hi = -1;
		if (spp >= BLOCK) {
			const ba = Math.floor(a / BLOCK), bb = Math.min(peaks.min.length, Math.ceil(b / BLOCK));
			for (let k = ba; k < bb; k++) { if (peaks.min[k] < lo) lo = peaks.min[k]; if (peaks.max[k] > hi) hi = peaks.max[k]; }
		} else {
			const sa = Math.max(0, Math.floor(a)), sb = Math.min(data.length, Math.ceil(b));
			for (let k = sa; k < sb; k++) { const v = data[k]; if (v < lo) lo = v; if (v > hi) hi = v; }
		}
		if (hi < lo) continue;
		g.moveTo(x + 0.5, mid - hi * amp);
		g.lineTo(x + 0.5, mid - lo * amp + 1);
	}
	g.stroke();
}

function niceStep(minSeconds) {
	const steps = [0.0001, 0.0002, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600];
	return steps.find(s => s >= minSeconds) || 600;
}

function draw() {
	const { g, w, h } = fitCanvas(waveCanvas);
	if (w <= 0 || h <= 0) return;
	const C = { bg: css('--panel'), fg: css('--fg'), muted: css('--muted'), line: css('--line'), accent: css('--accent'), ok: css('--ok'), bad: css('--bad'), warn: css('--warn') };
	g.fillStyle = C.bg;
	g.fillRect(0, 0, w, h);
	if (!st.frames) {
		g.fillStyle = C.muted;
		g.font = '14px sans-serif';
		g.textAlign = 'center';
		g.fillText('«開く…» で音声ファイル (WAV / Ogg Vorbis / Ogg Opus) を開いてください', w / 2, h / 2);
		g.textAlign = 'start';
		drawOverview();
		return;
	}
	const L = layout(h);

	// 選択範囲
	if (st.rangeA >= 0 && st.rangeB >= 0 && st.rangeA !== st.rangeB) {
		const a = xOf(Math.min(st.rangeA, st.rangeB)), b = xOf(Math.max(st.rangeA, st.rangeB));
		g.fillStyle = C.accent + '33';
		g.fillRect(a, L.waveTop, b - a, L.waveH);
	}

	// ルーラー
	g.fillStyle = C.bg;
	g.fillRect(0, 0, w, RULER_H);
	g.strokeStyle = C.line;
	g.fillStyle = C.muted;
	g.font = '11px sans-serif';
	const step = niceStep(80 * st.view.spp / st.sampleRate);
	const t0 = Math.floor(st.view.start / st.sampleRate / step) * step;
	g.beginPath();
	for (let t = t0; t * st.sampleRate < st.view.start + w * st.view.spp; t += step) {
		const x = Math.round(xOf(t * st.sampleRate)) + 0.5;
		if (x < 0) continue;
		g.moveTo(x, RULER_H - 6); g.lineTo(x, RULER_H);
		g.fillText(step < 0.001 ? fmtTime(Math.round(t * st.sampleRate)) + '' : fmtTime(Math.round(t * st.sampleRate)), x + 3, 12);
	}
	g.moveTo(0, RULER_H + 0.5); g.lineTo(w, RULER_H + 0.5);
	g.moveTo(0, L.waveTop + 0.5); g.lineTo(w, L.waveTop + 0.5);
	g.moveTo(0, L.linkTop + 0.5); g.lineTo(w, L.linkTop + 0.5);
	g.stroke();

	// 波形
	const nch = st.channels.length;
	const laneH = L.waveH / nch;
	g.strokeStyle = C.accent;
	g.fillStyle = C.accent;
	g.lineWidth = 1;
	for (let c = 0; c < nch; c++) {
		drawWaveLane(g, st.channels[c], st.peaks[c], 0, w, L.waveTop + c * laneH, laneH);
		g.save();
		g.strokeStyle = C.line;
		g.beginPath();
		const mid = Math.round(L.waveTop + c * laneH + laneH / 2) + 0.5;
		g.moveTo(0, mid); g.lineTo(w, mid);
		if (c > 0) { g.moveTo(0, Math.round(L.waveTop + c * laneH) + 0.5); g.lineTo(w, Math.round(L.waveTop + c * laneH) + 0.5); }
		g.stroke();
		g.restore();
	}
	// 末尾
	const xe = xOf(st.frames);
	if (xe < w) { g.fillStyle = C.line + '88'; g.fillRect(xe, RULER_H, w - xe, h - RULER_H); }

	// ラベル
	g.font = '12px sans-serif';
	st.labels.forEach((lb, i) => {
		const x = Math.round(xOf(lb.position)) + 0.5;
		if (x < -200 || x > w + 2) return;
		const sel = st.sel?.kind === 'label' && st.sel.index === i;
		const col = sel ? C.accent : (lb.name.startsWith(':') ? C.warn : C.fg);
		g.strokeStyle = col;
		g.setLineDash([3, 3]);
		g.beginPath(); g.moveTo(x, L.waveTop); g.lineTo(x, L.linkTop); g.stroke();
		g.setLineDash([]);
		g.fillStyle = col;
		g.beginPath(); g.moveTo(x - 5, RULER_H + 2); g.lineTo(x + 5, RULER_H + 2); g.lineTo(x, RULER_H + 9); g.closePath(); g.fill();
		g.fillText(lb.name || '(名前なし)', x + 6, RULER_H + 16);
	});

	// リンク
	st.links.forEach((lk, i) => {
		const sel = st.sel?.kind === 'link' && st.sel.index === i;
		const conditional = lk.condition !== 'no';
		const col = sel ? C.accent : (conditional ? C.warn : C.ok);
		const y = L.linkTop + 6 + L.tiers.tier[i] * TIER_H + 4;
		const xf = Math.round(xOf(lk.from)) + 0.5, xt = Math.round(xOf(lk.to)) + 0.5;
		g.strokeStyle = col;
		g.fillStyle = col;
		g.lineWidth = sel ? 2 : 1;
		// 波形上の縦線 (From = 実線、To = 点線)
		g.globalAlpha = 0.7;
		g.beginPath(); g.moveTo(xf, L.waveTop); g.lineTo(xf, y); g.stroke();
		g.setLineDash([2, 3]);
		g.beginPath(); g.moveTo(xt, L.waveTop); g.lineTo(xt, y); g.stroke();
		g.setLineDash([]);
		g.globalAlpha = 1;
		// From → To の線と矢印
		g.beginPath(); g.moveTo(xf, y); g.lineTo(xt, y); g.stroke();
		const dir = xt >= xf ? 1 : -1;
		g.beginPath(); g.moveTo(xt, y); g.lineTo(xt - dir * 7, y - 4); g.lineTo(xt - dir * 7, y + 4); g.closePath(); g.fill();
		g.fillRect(xf - 3, y - 3, 6, 6);
		g.lineWidth = 1;
		const tag = (lk.smooth ? '~' : '') + (conditional ? `[${lk.condVar}]${COND_SYMBOL[lk.condition]}${lk.refValue}` : '');
		if (tag) { g.font = '10px sans-serif'; g.fillText(tag, Math.min(xf, xt) + 6, y - 3); }
	});

	// キャレットと再生位置
	const xc = Math.round(xOf(st.caret)) + 0.5;
	g.strokeStyle = C.fg;
	g.beginPath(); g.moveTo(xc, RULER_H); g.lineTo(xc, L.linkTop); g.stroke();
	if (st.playPos >= 0) {
		const xp = Math.round(xOf(st.playPos)) + 0.5;
		g.strokeStyle = C.bad;
		g.lineWidth = 2;
		g.beginPath(); g.moveTo(xp, 0); g.lineTo(xp, L.linkTop); g.stroke();
		g.lineWidth = 1;
	}
	drawOverview();
	refreshStatus();
}

let overviewCache = null;
function drawOverview() {
	const { g, w, h } = fitCanvas(ovCanvas);
	g.fillStyle = css('--panel');
	g.fillRect(0, 0, w, h);
	if (!st.frames || w <= 0 || h <= 0) return;
	if (!overviewCache || overviewCache.w !== w || overviewCache.h !== h) {
		// 全体の波形はキャッシュしておく (全チャンネルを重ねる)
		const off = document.createElement('canvas');
		const dpr = window.devicePixelRatio || 1;
		off.width = Math.round(w * dpr); off.height = Math.round(h * dpr);
		const og = off.getContext('2d');
		og.setTransform(dpr, 0, 0, dpr, 0, 0);
		og.strokeStyle = css('--muted');
		og.beginPath();
		const bpp = st.peaks[0].min.length / w;
		for (let x = 0; x < w; x++) {
			let lo = 1, hi = -1;
			const a = Math.floor(x * bpp), b = Math.max(a + 1, Math.floor((x + 1) * bpp));
			for (const p of st.peaks) for (let k = a; k < b && k < p.min.length; k++) { if (p.min[k] < lo) lo = p.min[k]; if (p.max[k] > hi) hi = p.max[k]; }
			if (hi < lo) continue;
			og.moveTo(x + 0.5, h / 2 - hi * (h / 2 - 2));
			og.lineTo(x + 0.5, h / 2 - lo * (h / 2 - 2) + 1);
		}
		og.stroke();
		overviewCache = { w, h, canvas: off };
	}
	g.drawImage(overviewCache.canvas, 0, 0, w, h);
	const sx = w / st.frames;
	// リンクとラベルの位置
	g.fillStyle = css('--ok');
	for (const l of st.links) { g.fillRect(l.from * sx, 0, 1, h); }
	g.fillStyle = css('--warn');
	for (const l of st.labels) { g.fillRect(l.position * sx, 0, 1, 6); }
	// 表示範囲
	const vx = st.view.start * sx, vw = Math.max(2, waveWidth() * st.view.spp * sx);
	g.fillStyle = css('--accent') + '33';
	g.fillRect(vx, 0, vw, h);
	g.strokeStyle = css('--accent');
	g.strokeRect(vx + 0.5, 0.5, vw - 1, h - 1);
	if (st.playPos >= 0) { g.fillStyle = css('--bad'); g.fillRect(st.playPos * sx, 0, 2, h); }
}

//---------------------------------------------------------------------------
// 一覧・状態表示
//---------------------------------------------------------------------------
function refreshLists() {
	const lk = $('links');
	lk.replaceChildren();
	const lorder = st.links.map((l, i) => i).sort((a, b) => st.links[a].from - st.links[b].from);
	for (const i of lorder) {
		const l = st.links[i];
		const cond = l.condition !== 'no' ? `  [${l.condVar}]${COND_SYMBOL[l.condition]}${l.refValue}` : '';
		const d = krt.el('div', { title: `${fmtPos(l.from)} → ${fmtPos(l.to)}` }, `${l.from} → ${l.to}${l.smooth ? '  ~' : ''}${cond}`);
		if (st.sel?.kind === 'link' && st.sel.index === i) d.classList.add('sel');
		d.addEventListener('click', () => { select('link', i); reveal(l.from); });
		d.addEventListener('dblclick', () => editLink(i));
		lk.append(d);
	}
	if (!st.links.length) lk.append(krt.el('div', { class: 'hint' }, '(なし)'));
	const lb = $('labels');
	lb.replaceChildren();
	const border = st.labels.map((l, i) => i).sort((a, b) => st.labels[a].position - st.labels[b].position);
	for (const i of border) {
		const l = st.labels[i];
		const d = krt.el('div', { title: fmtPos(l.position) }, `${l.position}  ${l.name}`);
		if (st.sel?.kind === 'label' && st.sel.index === i) d.classList.add('sel');
		d.addEventListener('click', () => { select('label', i); reveal(l.position); });
		d.addEventListener('dblclick', () => editLabel(i));
		lb.append(d);
	}
	if (!st.labels.length) lb.append(krt.el('div', { class: 'hint' }, '(なし)'));
}

function refreshFlags() {
	const box = $('flags');
	if (!box.children.length) {
		for (let i = 0; i < 16; i++) {
			const inp = krt.el('input', { type: 'text', value: '0', 'data-i': i });
			inp.addEventListener('change', () => {
				const v = Math.max(0, Math.min(9999, parseInt(inp.value, 10) || 0));
				st.flags[i] = v;
				inp.value = v;
				sendFlags();
				refreshFlags();
			});
			box.append(krt.el('label', {}, `[${i}]`, inp));
		}
	}
	for (const inp of box.querySelectorAll('input')) {
		const i = Number(inp.dataset.i);
		if (document.activeElement !== inp) inp.value = st.flags[i];
		inp.classList.toggle('changed', st.flags[i] !== 0);
	}
}

function refreshButtons() {
	const has = st.frames > 0;
	$('save').disabled = !has;
	$('undo').disabled = !st.undo.length;
	$('redo').disabled = !st.redo.length;
	for (const id of ['play', 'playTop', 'addLink', 'addLabel', 'addLink2', 'addLabel2']) $(id).disabled = !has;
	$('play').textContent = st.playing ? '■ 停止' : '▶ 再生';
	$('preview').disabled = !(st.sel?.kind === 'link');
	$('remove').disabled = !st.sel;
	$('save').textContent = st.modified ? '保存 *' : '保存';
	document.title = (st.modified ? '* ' : '') + 'ループチューナ' + (st.file ? ' - ' + st.file.replace(/^.*[\\/]/, '') : '');
}

function refreshStatus() {
	if (!st.frames) { $('stCaret').textContent = ''; return; }
	$('stCaret').textContent = 'キャレット ' + fmtPos(st.caret);
	const hasRange = st.rangeA >= 0 && st.rangeB >= 0 && st.rangeA !== st.rangeB;
	$('stSel').textContent = hasRange ? `選択 ${Math.min(st.rangeA, st.rangeB)}〜${Math.max(st.rangeA, st.rangeB)} (${Math.abs(st.rangeB - st.rangeA)} サンプル)` : '';
	$('stPlay').textContent = st.playPos >= 0 ? '再生位置 ' + fmtPos(st.playPos) : '';
	$('stZoom').textContent = st.view.spp >= 1 ? `1px = ${st.view.spp.toFixed(st.view.spp < 10 ? 1 : 0)} サンプル` : `1 サンプル = ${(1 / st.view.spp).toFixed(1)}px`;
}

function refreshAll() { refreshLists(); refreshFlags(); refreshButtons(); requestDraw(); }

function select(kind, index) { st.sel = kind ? { kind, index } : null; refreshAll(); }
function reveal(pos) {
	const w = waveWidth() * st.view.spp;
	if (pos < st.view.start || pos > st.view.start + w) setViewStart(pos - w / 2);
}

//---------------------------------------------------------------------------
// ゼロクロス
//---------------------------------------------------------------------------
function snapZero(pos) {
	if (!$('snap').checked || !st.frames) return pos;
	const range = Math.max(1, Math.min(4096, Math.round(st.view.spp * 8)));
	const ch = st.channels;
	const mono = (i) => { let v = 0; for (const c of ch) v += c[i]; return v; };
	for (let d = 0; d <= range; d++) {
		for (const p of [pos - d, pos + d]) {
			if (p <= 0 || p >= st.frames) continue;
			const a = mono(p - 1), b = mono(p);
			if ((a <= 0 && b > 0) || (a >= 0 && b < 0) || b === 0) return p;
		}
	}
	return pos;
}

//---------------------------------------------------------------------------
// ダイアログ
//---------------------------------------------------------------------------
function dialog(title, body, onOk) {
	const ok = krt.el('button', { class: 'primary' }, 'OK');
	const cancel = krt.el('button', {}, 'キャンセル');
	const err = krt.el('div', { class: 'krt-error' });
	const dlg = krt.el('div', { class: 'krt-dialog' }, krt.el('div', { class: 'krt-dialog-title' }, title), body, err,
		krt.el('div', { class: 'krt-dialog-buttons' }, cancel, ok));
	const modal = krt.el('div', { class: 'krt-modal' }, dlg);
	const close = () => { modal.remove(); waveCanvas.focus(); };
	ok.addEventListener('click', () => { const e = onOk(); if (e) err.textContent = e; else close(); });
	cancel.addEventListener('click', close);
	modal.addEventListener('keydown', (e) => {
		e.stopPropagation();
		if (e.key === 'Escape') close();
		if (e.key === 'Enter' && e.target.tagName !== 'SELECT') ok.click();
	});
	document.body.append(modal);
	dlg.querySelector('input,select')?.focus();
}

const parsePos = (s) => { const v = Number(String(s).trim()); return Number.isInteger(v) ? v : NaN; };

function editLink(index) {
	const isNew = index < 0;
	const l = isNew ? newLinkFromSelection() : { ...st.links[index] };
	const from = krt.el('input', { type: 'text', value: l.from });
	const to = krt.el('input', { type: 'text', value: l.to });
	const smooth = krt.el('input', { type: 'checkbox' });
	smooth.checked = !!l.smooth;
	const cond = krt.el('select');
	for (const [v, t] of CONDITIONS) { const o = krt.el('option', { value: v }, t); if (v === l.condition) o.selected = true; cond.append(o); }
	const condVar = krt.el('input', { type: 'text', value: l.condVar });
	const refValue = krt.el('input', { type: 'text', value: l.refValue });
	const body = krt.el('div', {},
		krt.el('div', { class: 'dlg-grid' },
			'From (飛ぶ位置)', from, 'To (飛び先)', to,
			'', krt.el('label', {}, smooth, ' Smooth (50ms でクロスフェードしてつなぐ)'),
			'条件', cond, 'フラグ番号', condVar, '値', refValue),
		krt.el('div', { class: 'hint' }, '位置はサンプル単位。条件付きのリンクは、フラグ [番号] と値を比べて満たすときだけ飛ぶ。\n同じ From に複数のリンクがあるときは条件付きのものが先に調べられる (本体と同じ)。'));
	dialog(isNew ? 'リンクを追加' : 'リンクを編集', body, () => {
		const f = parsePos(from.value), t = parsePos(to.value), cv = parsePos(condVar.value), rv = parsePos(refValue.value);
		if (!(f >= 0 && f <= st.frames) || !(t >= 0 && t <= st.frames)) return `From / To は 0〜${st.frames} の整数で指定してください`;
		if (cond.value !== 'no' && !(cv >= 0 && cv < 16)) return 'フラグ番号は 0〜15 です';
		if (!(rv >= 0 && rv <= 9999) && cond.value !== 'no') return '値は 0〜9999 です';
		if (f === t && cond.value === 'no') return '無条件のリンクで From と To が同じだと先へ進めません';
		beginEdit();
		const v = { from: f, to: t, smooth: smooth.checked, condition: cond.value, refValue: rv >= 0 ? rv : 0, condVar: cv >= 0 ? cv : 0 };
		if (isNew) { st.links.push(v); st.sel = { kind: 'link', index: st.links.length - 1 }; }
		else st.links[index] = v;
		endEdit();
		return null;
	});
}

function newLinkFromSelection() {
	const hasRange = st.rangeA >= 0 && st.rangeB >= 0 && st.rangeA !== st.rangeB;
	if (hasRange) return { from: Math.max(st.rangeA, st.rangeB), to: Math.min(st.rangeA, st.rangeB), smooth: false, condition: 'no', refValue: 0, condVar: 0 };
	return { from: st.caret || st.frames, to: 0, smooth: false, condition: 'no', refValue: 0, condVar: 0 };
}

/// ラベル名の式の説明 (本体と同じ文法か)
function describeLabel(name) {
	if (!name.startsWith(':')) return '通常のラベル (再生中に通過すると本体の onLabel イベントが起きる)';
	const m = /^:\s*\[\s*(\d+)\s*\]\s*(=|\+=|-=|\+\+|--)\s*(\[\s*(\d+)\s*\]|\d+)?\s*$/.exec(name);
	if (!m || Number(m[1]) > 15 || (m[4] !== undefined && Number(m[4]) > 15) ||
		((m[2] === '++' || m[2] === '--') ? m[3] !== undefined : m[3] === undefined)) return '⚠ フラグの式として正しくありません (本体は何もしません)';
	return `フラグの式: 通過するとフラグ [${m[1]}] を ${m[2]} ${m[3] ?? ''}`;
}

function editLabel(index) {
	const isNew = index < 0;
	const l = isNew ? { position: st.caret, name: '' } : { ...st.labels[index] };
	const pos = krt.el('input', { type: 'text', value: l.position });
	const name = krt.el('input', { type: 'text', value: l.name, spellcheck: 'false' });
	const desc = krt.el('div', { class: 'hint' });
	const upd = () => { desc.textContent = describeLabel(name.value); };
	name.addEventListener('input', upd);
	upd();
	const body = krt.el('div', {}, krt.el('div', { class: 'dlg-grid' }, '位置', pos, '名前', name), desc,
		krt.el('div', { class: 'hint' }, '«:[0]=1» «:[0]+=1» «:[0]++» «:[1]=[0]» のように «:» で始めると、通過したときにフラグを変える式になる。'));
	dialog(isNew ? 'ラベルを追加' : 'ラベルを編集', body, () => {
		const p = parsePos(pos.value);
		if (!(p >= 0 && p <= st.frames)) return `位置は 0〜${st.frames} の整数で指定してください`;
		if (name.value.includes("'")) return "名前に ' は使えません";
		beginEdit();
		const v = { position: p, name: name.value };
		if (isNew) { st.labels.push(v); st.sel = { kind: 'label', index: st.labels.length - 1 }; }
		else st.labels[index] = v;
		endEdit();
		return null;
	});
}

function removeSelected() {
	if (!st.sel) return;
	beginEdit();
	if (st.sel.kind === 'link') st.links.splice(st.sel.index, 1);
	else st.labels.splice(st.sel.index, 1);
	st.sel = null;
	endEdit();
}

//---------------------------------------------------------------------------
// マウス操作
//---------------------------------------------------------------------------
/// 位置 (x, y) にあるもの
function hitTest(x, y) {
	const L = layout(waveCanvas.clientHeight);
	if (y >= L.linkTop) {
		let best = null;
		st.links.forEach((lk, i) => {
			const ty = L.linkTop + 6 + L.tiers.tier[i] * TIER_H + 4;
			if (Math.abs(y - ty) > TIER_H / 2) return;
			for (const end of ['from', 'to']) {
				const d = Math.abs(xOf(lk[end]) - x);
				if (d <= 6 && (!best || d < best.d)) best = { kind: 'link', index: i, end, d };
			}
			const a = Math.min(xOf(lk.from), xOf(lk.to)), b = Math.max(xOf(lk.from), xOf(lk.to));
			if (!best && x >= a && x <= b) best = { kind: 'link', index: i, end: null, d: 99 };
		});
		return best;
	}
	if (y >= RULER_H && y < L.waveTop) {
		let best = null;
		st.labels.forEach((lb, i) => {
			const d = Math.abs(xOf(lb.position) - x);
			if (d <= 7 && (!best || d < best.d)) best = { kind: 'label', index: i, d };
		});
		return best;
	}
	if (y < RULER_H) return { kind: 'ruler' };
	return { kind: 'wave' };
}

let drag = null;
waveCanvas.addEventListener('mousedown', (e) => {
	if (!st.frames || e.button !== 0) return;
	waveCanvas.focus();
	const x = e.offsetX, y = e.offsetY;
	const h = hitTest(x, y);
	if (h?.kind === 'link' || h?.kind === 'label') {
		select(h.kind, h.index);
		if (h.kind === 'label' || h.end) drag = { ...h, before: snapshot(), moved: false };
		return;
	}
	const s = clampSample(sampleAt(x));
	st.caret = s;
	st.rangeA = s; st.rangeB = s;
	select(null);
	drag = { kind: 'range' };
	if (h?.kind === 'ruler' && st.playing) play(s);
});
window.addEventListener('mousemove', (e) => {
	if (!drag) return;
	const r = waveCanvas.getBoundingClientRect();
	const x = e.clientX - r.left;
	// 端に来たら少しずつ送る
	if (x < 0) setViewStart(st.view.start - st.view.spp * 10);
	if (x > r.width) setViewStart(st.view.start + st.view.spp * 10);
	const s = clampSample(sampleAt(Math.min(Math.max(x, 0), r.width)));
	if (drag.kind === 'range') { st.rangeB = s; st.caret = s; requestDraw(); return; }
	const pos = snapZero(s);
	if (drag.kind === 'label') st.labels[drag.index].position = pos;
	else st.links[drag.index][drag.end] = pos;
	drag.moved = true;
	requestDraw();
});
window.addEventListener('mouseup', () => {
	if (!drag) return;
	if ((drag.kind === 'link' || drag.kind === 'label') && drag.moved) {
		st.undo.push(drag.before);
		if (st.undo.length > UNDO_MAX) st.undo.shift();
		st.redo = [];
		endEdit();
	}
	if (drag.kind === 'range' && Math.abs(xOf(st.rangeB) - xOf(st.rangeA)) < 3) { st.rangeA = st.rangeB = -1; st.caret = clampSample(st.caret); }
	drag = null;
	refreshAll();
});
waveCanvas.addEventListener('dblclick', (e) => {
	const h = hitTest(e.offsetX, e.offsetY);
	if (h?.kind === 'link') editLink(h.index);
	else if (h?.kind === 'label') editLabel(h.index);
	else if (h?.kind === 'wave' && st.frames) play(clampSample(sampleAt(e.offsetX)));
});
waveCanvas.addEventListener('wheel', (e) => {
	if (!st.frames) return;
	e.preventDefault();
	if (e.ctrlKey) zoomAt(e.deltaY > 0 ? 1.25 : 0.8, e.offsetX);
	else setViewStart(st.view.start + (e.deltaY + e.deltaX) / 100 * waveWidth() * st.view.spp * 0.15);
}, { passive: false });
waveCanvas.addEventListener('mousemove', (e) => {
	if (drag) return;
	const h = hitTest(e.offsetX, e.offsetY);
	waveCanvas.style.cursor = (h?.kind === 'label' || h?.end) ? 'ew-resize' : (h?.kind === 'link' ? 'pointer' : 'crosshair');
});

let ovDrag = false;
const ovMove = (e) => {
	const r = ovCanvas.getBoundingClientRect();
	const s = (e.clientX - r.left) / r.width * st.frames;
	setViewStart(s - waveWidth() * st.view.spp / 2);
};
ovCanvas.addEventListener('mousedown', (e) => { if (!st.frames) return; ovDrag = true; ovMove(e); });
window.addEventListener('mousemove', (e) => { if (ovDrag) ovMove(e); });
window.addEventListener('mouseup', () => { ovDrag = false; });

//---------------------------------------------------------------------------
// ボタンとキー
//---------------------------------------------------------------------------
async function pickAndOpen() {
	const f = await krt.pickFiles(dirOf(st.file) || undefined, { multiple: false });
	if (f && f.length) openFile(f[0]);
}
function togglePlay() { if (st.playing) stop(); else play(st.caret); }
function previewLink() {
	if (st.sel?.kind !== 'link') return;
	const l = st.links[st.sel.index];
	play(Math.max(0, l.from - st.sampleRate * 2), st.sampleRate * 4);
}

$('open').addEventListener('click', pickAndOpen);
$('save').addEventListener('click', save);
$('undo').addEventListener('click', undo);
$('redo').addEventListener('click', redo);
$('play').addEventListener('click', togglePlay);
$('playTop').addEventListener('click', () => play(0));
$('preview').addEventListener('click', previewLink);
$('zoomIn').addEventListener('click', () => zoomAt(0.5, waveWidth() / 2));
$('zoomOut').addEventListener('click', () => zoomAt(2, waveWidth() / 2));
$('zoomAll').addEventListener('click', zoomAll);
for (const id of ['addLink', 'addLink2']) $(id).addEventListener('click', () => editLink(-1));
for (const id of ['addLabel', 'addLabel2']) $(id).addEventListener('click', () => editLabel(-1));
$('remove').addEventListener('click', removeSelected);
$('resetFlags').addEventListener('click', () => { st.flags.fill(0); sendFlags(); refreshFlags(); });
for (const id of ['looping', 'ignoreLinks']) $(id).addEventListener('change', sendOptions);

window.addEventListener('keydown', (e) => {
	if (document.querySelector('.krt-modal')) return;
	if (e.target.tagName === 'INPUT' && e.target.type === 'text') return;
	const ctrl = e.ctrlKey || e.metaKey;
	const k = e.key;
	let handled = true;
	if (ctrl && k.toLowerCase() === 's') save();
	else if (ctrl && k.toLowerCase() === 'o') pickAndOpen();
	else if (ctrl && k.toLowerCase() === 'z') (e.shiftKey ? redo : undo)();
	else if (ctrl && k.toLowerCase() === 'y') redo();
	else if (!st.frames) handled = false;
	else if (k === ' ') togglePlay();
	else if (k === 'Home') play(0);
	else if (k === 'Delete' || k === 'Backspace') removeSelected();
	else if (k === '+' || k === ';' || k === '=') zoomAt(0.5, xOf(st.caret));
	else if (k === '-') zoomAt(2, xOf(st.caret));
	else if (k === '0') zoomAll();
	else if (k === 'l' || k === 'L') editLink(-1);
	else if (k === 'b' || k === 'B') editLabel(-1);
	else if (k === 'p' || k === 'P') previewLink();
	else if (k === 'Enter' && st.sel) (st.sel.kind === 'link' ? editLink : editLabel)(st.sel.index);
	else if (k === 'ArrowLeft' || k === 'ArrowRight') {
		const step = e.shiftKey ? 1 : Math.max(1, Math.round(st.view.spp));
		st.caret = clampSample(st.caret + (k === 'ArrowLeft' ? -step : step));
		reveal(st.caret);
		refreshStatus();
		requestDraw();
	} else handled = false;
	if (handled) e.preventDefault();
});

window.addEventListener('resize', () => { clampView(); overviewCache = null; requestDraw(); });
window.addEventListener('beforeunload', (e) => { if (st.modified) { e.preventDefault(); e.returnValue = ''; } });

async function main() {
	await krt.init();
	refreshAll();
	const args = krt.info.args || [];
	if (args.length) openFile(args[0]);
}
main();
