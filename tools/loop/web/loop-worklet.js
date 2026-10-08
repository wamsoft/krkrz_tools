// ループ再生 (AudioWorklet)
//
// 本体 krkrz_dev/src/core/common/sound/WaveLoopManager.cpp の
// tTVPWaveLoopManager::Decode / GetNearestEvent / GetLabelAt / EvalLabelExpression を
// そのまま移植している。リンクのたどり方 (条件・同じ From のリンクの優先順・
// Smooth の 50ms クロスフェード・10 回続けてリンクだけが続いたら打ち切り) を
// 変えないこと。サンプル位置は音声ファイルのサンプル単位 (AudioContext は
// 音声と同じサンプルレートで作る)。

const MAX_FLAGS = 16;
const MAX_FLAG_VALUE = 9999;
const GIVE_UP_COUNT = 10;
const CONDITIONS = ['no', 'eq', 'ne', 'gt', 'ge', 'lt', 'le'];

// 本体の operator < (tTVPWaveLoopLink): From 昇順、同じ From なら条件付きを先、CondVar の大きい方を先
function linkLess(a, b) {
	if (a.from !== b.from) return a.from < b.from ? -1 : 1;
	if (a.cond !== b.cond) return b.cond - a.cond;
	return b.condVar - a.condVar;
}

// ラベルの式 (":[n]=v" ":[n]+=v" ":[n]-=v" ":[n]++" ":[n]--"、右辺は "[m]" も可)
function parseExpression(name) {
	let p = 0;
	const s = name;
	if (s[p] !== ':') return null;
	p++;
	const token = () => {
		while (p < s.length && s.charCodeAt(p) <= 0x20) p++;
		if (p >= s.length) return { t: 'eoe' };
		const c = s[p];
		if (c >= '0' && c <= '9') {
			let v = 0;
			while (p < s.length && s[p] >= '0' && s[p] <= '9') v = v * 10 + (s.charCodeAt(p++) - 48);
			return { t: 'int', v };
		}
		p++;
		if (c === '[') return { t: '[' };
		if (c === ']') return { t: ']' };
		if (c === '=') return { t: '=' };
		if (c === '+') { if (s[p] === '=') { p++; return { t: '+=' }; } if (s[p] === '+') { p++; return { t: '++' }; } return { t: '+' }; }
		if (c === '-') { if (s[p] === '=') { p++; return { t: '-=' }; } if (s[p] === '-') { p++; return { t: '--' }; } return { t: '+' }; }
		return { t: '?' };
	};
	let t = token(); if (t.t !== '[') return null;
	t = token(); if (t.t !== 'int') return null;
	const lv = t.v; if (lv < 0 || lv >= MAX_FLAGS) return null;
	t = token(); if (t.t !== ']') return null;
	const op = token().t;
	if (!['=', '+=', '++', '-=', '--'].includes(op)) return null;
	let rv = 0, indirect = false;
	t = token();
	if (t.t === '[') {
		t = token(); if (t.t !== 'int') return null;
		rv = t.v; if (rv < 0 || rv >= MAX_FLAGS) return null;
		t = token(); if (t.t !== ']') return null;
		indirect = true;
	} else if (t.t === 'int') {
		rv = t.v;
	} else if (t.t === 'eoe') {
		if (!(op === '++' || op === '--')) return null;
	} else {
		return null;
	}
	if (t.t !== 'eoe' && token().t !== 'eoe') return null;
	return { op, lv, rv, indirect };
}

class LoopProcessor extends AudioWorkletProcessor {
	constructor() {
		super();
		this.channels = [];
		this.frames = 0;
		this.sampleRate = sampleRate;
		this.links = [];
		this.labels = [];
		this.flags = new Int32Array(MAX_FLAGS);
		this.position = 0;
		this.playing = false;
		this.looping = false;
		this.ignoreLinks = false;
		this.remaining = -1;     // 残り再生サンプル数 (-1 = 制限なし)
		this.xfade = null;        // { data: Float32Array[], pos, len }
		this.quanta = 0;
		this.events = [];
		this.port.onmessage = (e) => this.onMessage(e.data);
	}

	onMessage(m) {
		switch (m.type) {
		case 'load':
			this.channels = m.channels;
			this.frames = m.frames;
			this.playing = false;
			this.xfade = null;
			break;
		case 'data':
			this.links = m.links.map(l => ({
				from: l.from, to: l.to, smooth: !!l.smooth,
				cond: Math.max(0, CONDITIONS.indexOf(l.condition)), refValue: l.refValue | 0, condVar: l.condVar | 0,
			})).sort(linkLess);
			this.labels = m.labels.map(l => ({ position: l.position, name: l.name })).sort((a, b) => a.position - b.position);
			break;
		case 'play':
			this.position = Math.max(0, Math.min(this.frames, m.position | 0));
			this.remaining = m.duration ?? -1;
			this.xfade = null;
			this.playing = true;
			this.report(true);
			break;
		case 'stop':
			this.playing = false;
			this.xfade = null;
			this.report(true);
			break;
		case 'flags':
			for (let i = 0; i < MAX_FLAGS; i++) this.flags[i] = Math.max(0, Math.min(MAX_FLAG_VALUE, m.flags[i] | 0));
			break;
		case 'options':
			this.looping = !!m.looping;
			this.ignoreLinks = !!m.ignoreLinks;
			break;
		}
	}

	report(force) {
		this.port.postMessage({ type: 'state', playing: this.playing, position: this.position, flags: Array.from(this.flags), events: this.events, force });
		this.events = [];
	}

	matchCondition(l) {
		// 本体は CondVar != -1 のときに条件を見る (読み込みの既定値は 0)
		if (l.condVar === -1) return true;
		const f = this.flags[l.condVar] ?? 0;
		switch (l.cond) {
		case 0: return true;
		case 1: return l.refValue === f;
		case 2: return l.refValue !== f;
		case 3: return l.refValue < f;
		case 4: return l.refValue <= f;
		case 5: return l.refValue > f;
		case 6: return l.refValue >= f;
		default: return false;
		}
	}

	// GetNearestEvent: current 以降で最も近いリンク (条件を満たすもの)
	nearestEvent(current, ignoreConditions) {
		const L = this.links;
		if (!L.length) return null;
		let s = 0, e = L.length;
		while (e - s > 1) {
			const m = (s + e) >> 1;
			if (L[m].from <= current) s = m; else e = m;
		}
		if (s < L.length - 1 && L[s].from < current) s++;
		if (s >= L.length || L[s].from < current) return null;
		const from = L[s].from;
		while (s >= 1 && L[s - 1].from === from) s--;
		if (!ignoreConditions) {
			do {
				if (this.matchCondition(L[s])) break;
				s++;
			} while (s < L.length);
			if (s >= L.length || L[s].from < current) return null;
		}
		return L[s];
	}

	evalLabel(name) {
		const x = parseExpression(name);
		if (!x) return;
		const rv = x.indirect ? this.flags[x.rv] : x.rv;
		let v = this.flags[x.lv];
		switch (x.op) {
		case '=': v = rv; break;
		case '+=': v += rv; break;
		case '-=': v -= rv; break;
		case '++': v++; break;
		case '--': v--; break;
		}
		this.flags[x.lv] = Math.max(0, Math.min(MAX_FLAG_VALUE, v));
	}

	// [from, to) にあるラベル
	labelsIn(from, to) {
		const out = [];
		for (const l of this.labels) {
			if (l.position >= to) break;
			if (l.position >= from) out.push(l);
		}
		return out;
	}

	// デコーダ相当: pos から n サンプルを読む (範囲外は 0)
	read(out, offset, pos, n) {
		for (let c = 0; c < out.length; c++) {
			const src = this.channels[Math.min(c, this.channels.length - 1)];
			const dst = out[c];
			for (let i = 0; i < n; i++) {
				const p = pos + i;
				dst[offset + i] = p >= 0 && p < this.frames ? src[p] : 0;
			}
		}
	}

	// 本体の Decode。out = チャンネルごとの Float32Array、samples = 欲しいサンプル数
	decode(out, samples) {
		let written = 0;
		let giveUp = 0;
		const half = Math.floor(this.sampleRate * 25 / 1000);   // TVP_WL_SMOOTH_TIME_HALF
		const total = this.frames;
		while (written !== samples) {
			let nextEvent = 0;
			let notFound = false;
			const link = this.ignoreLinks ? null : this.nearestEvent(this.position, false);
			if (link) {
				if (link.from === this.position) {
					giveUp++;
					if (giveUp >= GIVE_UP_COUNT) break;
					this.events.push({ kind: 'jump', from: link.from, to: link.to });
					this.position = link.to;
					continue;
				}
				if (link.smooth) {
					let before = half;
					if (link.from - before < 0) before = link.from;
					if (link.to - before < 0) before = link.to;
					if (link.from - before > this.position) {
						nextEvent = link.from - before;
					} else if (!this.xfade) {
						nextEvent = link.from;
						before = link.from - this.position;
						let after = half;
						if (total - link.from < after) after = total - link.from;
						if (total - link.to < after) after = total - link.to;
						const over = this.nearestEvent(link.to, true);
						if (over && over.from - link.to < after) after = over.from - link.to;
						const len = before + after;
						const ch = this.channels.length;
						const src1 = [], src2 = [], data = [];
						for (let c = 0; c < ch; c++) { src1.push(new Float32Array(len)); src2.push(new Float32Array(len)); data.push(new Float32Array(len)); }
						this.read(src1, 0, this.position, len);
						this.read(src2, 0, link.to - before, len);
						// DoCrossFade (float): before は 0→50%、after は 50→100%
						const fade = (offset, n, rs, re) => {
							if (!n) return;
							const step = (re - rs) / 100 / n;
							for (let c = 0; c < ch; c++) {
								let r = rs / 100;
								for (let i = 0; i < n; i++, r += step) {
									const a = src1[c][offset + i], b = src2[c][offset + i];
									data[c][offset + i] = a + (b - a) * r;
								}
							}
						};
						fade(0, before, 0, 50);
						fade(before, after, 50, 100);
						this.xfade = { data, pos: 0, len };
					} else {
						nextEvent = link.from;
					}
				} else {
					nextEvent = link.from;
				}
			} else {
				notFound = true;
			}

			let unit = (notFound || nextEvent - this.position > samples - written) ? samples - written : nextEvent - this.position;
			if (this.xfade && unit > this.xfade.len - this.xfade.pos) unit = this.xfade.len - this.xfade.pos;
			if (unit > 0) giveUp = 0;

			for (const l of this.labelsIn(this.position, this.position + unit)) {
				this.events.push({ kind: 'label', position: l.position, name: l.name });
				if (l.name[0] === ':') this.evalLabel(l.name);
			}

			if (!this.xfade) {
				const decoded = Math.max(0, Math.min(unit, total - this.position));
				this.read(out, written, this.position, decoded);
				this.position += decoded;
				written += decoded;
				if (decoded !== unit) {
					if (!this.looping) { this.playing = false; break; }
					if (this.position === 0) break;
					this.events.push({ kind: 'rewind' });
					this.position = 0;
				}
			} else {
				const x = this.xfade;
				for (let c = 0; c < out.length; c++) {
					const src = x.data[Math.min(c, x.data.length - 1)];
					out[c].set(src.subarray(x.pos, x.pos + unit), written);
				}
				x.pos += unit;
				this.position += unit;
				written += unit;
				if (x.pos === x.len) this.xfade = null;
			}
		}
		return written;
	}

	process(inputs, outputs) {
		const out = outputs[0];
		if (this.playing && this.frames > 0) {
			const n = out[0].length;
			const w = this.decode(out, n);
			for (let c = 0; c < out.length; c++) out[c].fill(0, w);
			if (this.remaining >= 0) { this.remaining -= w; if (this.remaining <= 0) this.playing = false; }
			if (!this.playing) this.report(true);
		}
		if (++this.quanta % 8 === 0 && (this.playing || this.events.length)) this.report(false);
		return true;
	}
}

registerProcessor('loop-processor', LoopProcessor);
