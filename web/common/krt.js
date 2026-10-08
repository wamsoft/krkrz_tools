//---------------------------------------------------------------------------
// krkrz_tools 共通の画面部品
//
//   import { app, krt } from './common/krt.js';
//   const info = await krt.init();               // 接続 + /api/app/info
//   krt.watchJob(st => ...);                     // 長い処理の状態 (JobRunner)
//   const dir = await krt.pickFolder(initial);   // フォルダ選択 (null = 取消)
//---------------------------------------------------------------------------
import { app } from '../lib/appserve.js';

export { app };

function el(tag, attrs = {}, ...children) {
	const e = document.createElement(tag);
	for (const [k, v] of Object.entries(attrs)) {
		if (k === 'class') e.className = v;
		else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
		else e.setAttribute(k, v);
	}
	for (const c of children) e.append(c instanceof Node ? c : document.createTextNode(String(c)));
	return e;
}

export const krt = {
	info: null,
	el,

	/// サーバへ接続し、ツールの情報を読む
	async init() {
		await app.ready();
		this.info = await app.get('/api/app/info');
		document.title = this.info.title;
		return this.info;
	},

	//-----------------------------------------------------------------------
	// 長い処理 (JobRunner)
	//-----------------------------------------------------------------------
	/// 状態が変わるたびに fn(status) を呼ぶ。今の状態でも 1 回呼ぶ。
	async watchJob(fn) {
		app.on('job', fn);
		fn(await app.get('/api/job'));
	},
	/// 処理が出したログ 1 行ずつ
	watchJobLog(fn) { app.on('joblog', fn); },
	cancelJob() { return app.post('/api/job/cancel', {}); },

	//-----------------------------------------------------------------------
	// フォルダ選択 (ブラウザにはネイティブのダイアログが無いので、
	// サーバのファイル参照 API (/api/fs) で一覧を出す)
	//-----------------------------------------------------------------------
	pickFolder(initial) {
		return this._picker({ initial, files: false });
	},

	/// ファイルを選ぶ。multiple なら配列、そうでなければ 1 つのパス (取消は null)
	pickFiles(initial, { multiple = false } = {}) {
		return this._picker({ initial, files: true, multiple });
	},

	_picker({ initial, files, multiple = false }) {
		return new Promise(async (resolve) => {
			const pathInput = el('input', { type: 'text', class: 'krt-picker-path', spellcheck: 'false' });
			const list = el('div', { class: 'krt-picker-list' });
			const roots = el('div', { class: 'krt-picker-roots' });
			const ok = el('button', { class: 'primary' }, files ? '選ぶ' : 'このフォルダを選ぶ');
			const cancel = el('button', {}, '取消');
			const hint = el('div', { class: 'krt-note' },
				files ? (multiple ? 'クリックで選択 (Ctrl / Shift で複数)。フォルダはダブルクリックで開きます。'
				                  : 'クリックで選択。フォルダはダブルクリックで開きます。')
				      : 'フォルダはダブルクリックで開きます。');
			const dlg = el('div', { class: 'krt-modal' },
				el('div', { class: 'krt-dialog krt-picker' },
					el('div', { class: 'krt-dialog-title' }, files ? 'ファイルの選択' : 'フォルダの選択'),
					roots, pathInput, list, hint,
					el('div', { class: 'krt-dialog-buttons' }, cancel, ok)));
			document.body.append(dlg);

			let selected = [];      // 選択中のファイル (path)
			let lastIndex = -1;
			const close = (v) => { dlg.remove(); resolve(v); };
			cancel.addEventListener('click', () => close(null));
			ok.addEventListener('click', () => {
				if (!files) return close(pathInput.value);
				if (!selected.length) return;
				close(multiple ? selected.slice() : selected[0]);
			});

			const open = async (path) => {
				let r;
				try { r = await app.get('/api/fs/list', { path }); }
				catch (e) { list.textContent = String(e.message || e); return; }
				pathInput.value = r.path;
				selected = [];
				lastIndex = -1;
				list.replaceChildren();
				if (r.parent) {
					list.append(el('div', { class: 'krt-picker-item dir', ondblclick: () => open(r.parent) }, '..'));
				}
				const sorted = r.entries.slice().sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
				const fileItems = [];
				for (const e of sorted) {
					if (e.dir) {
						list.append(el('div', { class: 'krt-picker-item dir', ondblclick: () => open(e.path) }, e.name));
					} else if (files) {
						const idx = fileItems.length;
						const item = el('div', { class: 'krt-picker-item file' }, e.name);
						item.addEventListener('click', (ev) => {
							if (multiple && ev.shiftKey && lastIndex >= 0) {
								const [a, b] = [Math.min(lastIndex, idx), Math.max(lastIndex, idx)];
								selected = fileItems.slice(a, b + 1).map(f => f.path);
							} else if (multiple && (ev.ctrlKey || ev.metaKey)) {
								selected = selected.includes(e.path) ? selected.filter(p => p !== e.path) : selected.concat(e.path);
								lastIndex = idx;
							} else {
								selected = [e.path];
								lastIndex = idx;
							}
							fileItems.forEach(f => f.el.classList.toggle('selected', selected.includes(f.path)));
						});
						item.addEventListener('dblclick', () => { selected = [e.path]; ok.click(); });
						fileItems.push({ path: e.path, el: item });
						list.append(item);
					}
				}
				if (r.error) list.append(el('div', { class: 'krt-note' }, r.error));
			};
			pathInput.addEventListener('keydown', (ev) => { if (ev.key === 'Enter') open(pathInput.value); });

			try {
				const rr = await app.get('/api/fs/roots');
				for (const root of rr.roots) {
					roots.append(el('button', { onclick: () => open(root.path) }, root.label || root.path));
				}
			} catch (e) { /* 一覧が出せなくても入力欄で指定できる */ }
			open(initial || '.');
		});
	},

	//-----------------------------------------------------------------------
	// 表示用の整形
	//-----------------------------------------------------------------------
	formatSize(n) {
		if (n < 1024) return n + ' B';
		if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
		if (n < 1024 * 1024 * 1024) return (n / 1024 / 1024).toFixed(1) + ' MB';
		return (n / 1024 / 1024 / 1024).toFixed(2) + ' GB';
	},
	formatTime(ms) {
		const d = new Date(ms);
		const p = (v) => String(v).padStart(2, '0');
		return `${d.getFullYear()}/${p(d.getMonth() + 1)}/${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
	},
};
