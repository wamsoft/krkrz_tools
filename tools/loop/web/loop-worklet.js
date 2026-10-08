// ループ再生 (AudioWorklet)。処理本体は loop-core.js (本体 WaveLoopManager の移植)
import { LoopEngine } from './loop-core.js';

class LoopProcessor extends AudioWorkletProcessor {
	constructor() {
		super();
		this.engine = new LoopEngine(sampleRate);
		this.remaining = -1;     // 残り再生サンプル数 (-1 = 制限なし)
		this.quanta = 0;
		this.port.onmessage = (e) => this.onMessage(e.data);
	}

	onMessage(m) {
		const en = this.engine;
		switch (m.type) {
		case 'load': en.load(m.channels, m.frames); break;
		case 'data': en.setData(m.links, m.labels); break;
		case 'play':
			en.start(m.position);
			this.remaining = m.duration ?? -1;
			this.report(true);
			break;
		case 'stop':
			en.playing = false;
			en.xfade = null;
			this.report(true);
			break;
		case 'flags': en.setFlags(m.flags); break;
		case 'options':
			en.looping = !!m.looping;
			en.ignoreLinks = !!m.ignoreLinks;
			break;
		}
	}

	report(force) {
		const en = this.engine;
		this.port.postMessage({ type: 'state', playing: en.playing, position: en.position, flags: Array.from(en.flags), events: en.events, force });
		en.events = [];
	}

	process(inputs, outputs) {
		const out = outputs[0];
		const en = this.engine;
		if (en.playing && en.frames > 0) {
			const w = en.decode(out, out[0].length);
			for (let c = 0; c < out.length; c++) out[c].fill(0, w);
			if (this.remaining >= 0) { this.remaining -= w; if (this.remaining <= 0) en.playing = false; }
			if (!en.playing) this.report(true);
		}
		if (++this.quanta % 8 === 0 && (en.playing || en.events.length)) this.report(false);
		return true;
	}
}

registerProcessor('loop-processor', LoopProcessor);
