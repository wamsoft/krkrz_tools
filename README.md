# krkrz_tools

吉里吉里Z の標準ツール群。吉里吉里2 時代の標準ツール (C++Builder 製・Windows 専用) を置き換える。

- **Windows / Linux / macOS** で動く (macOS は未検証)
- 各ツールは**ブラウザを画面にするネイティブアプリ** ([appserve](https://github.com/wamsoft/appserve))。
  起動するとローカルの HTTP サーバを立て、ブラウザのアプリ窓で画面を開く
- **全ツールがコマンドラインでも動く** (`--cli`)。GUI と CLI は同じ処理を呼ぶ
- 旧ツールの書式 (公開鍵・`.sig`・exe 埋め込み署名・ini など) と互換

計画と進捗は krkrz_dev の [TODO-tools.md](https://github.com/wamsoft/krkrz_dev/blob/develop/TODO-tools.md)。

## ツール

| 実行ファイル | ツール | 状態 |
|---|---|---|
| `krkrcheck` | ファイル破損チェックツール | 動作確認済み (Windows) |
| `krkrsign` | キー生成・署名ツール | 動作確認済み (Windows) |
| `krkrxp3` | xp3 アーカイブツール (作成・一覧・展開・検証) | 動作確認済み (Windows) |
| `krkraudio` | 音声フォーマットコンバータ (変換・音量・口パク用の音量) | 動作確認済み (Windows) |
| `krkrimg` | 画像フォーマットコンバータ (旧 krkrtpc の後継。PSD / CLIP の合成・レイヤ書き出し) | 動作確認済み (Windows) |

### krkrcheck — ファイル破損チェックツール

フォルダ以下を走査し、`.sig` の付いたファイルと、署名が埋め込まれた吉里吉里の exe を公開鍵で検証する。
公開鍵と画面の文言は **«実行ファイル名.ini»** から読む (吉里吉里2 の «ファイル破損チェックツール.ini» をそのまま使える。Shift_JIS / UTF-8 どちらでもよい)。

```
krkrcheck                          画面を開く (対象 = 実行ファイルのフォルダ)
krkrcheck <フォルダ>                画面を開く (対象フォルダを指定)
krkrcheck --cli [<フォルダ>] [--key=<ini / 公開鍵>] [--json | --tsv] [--quiet]
```

終了コード (CLI): 0 = すべて正常 / 1 = 破損あり / 2 = 検証できないファイルあり・失敗

### krkrsign — キー生成・署名ツール

```
krkrsign                                     画面を開く
krkrsign --cli keygen [--bits=1024] [--public=FILE] [--private=FILE] [--force]
krkrsign --cli sign   --key=<秘密鍵> <ファイル>...
krkrsign --cli verify --key=<公開鍵> <ファイル>...
```

- 鍵は RSA (既定 1024bit = 旧ツールと同じ。2048 / 3072 / 4096 も可)、署名は SHA256 + RSA-PSS
- 吉里吉里の exe (署名領域を持つもの) は exe の中へ、それ以外は «ファイル名.sig» へ署名を書く
- 旧 krkrsign の鍵で署名でき、旧ツールで作った署名を検証できる。本体の sigcheck プラグインで検証できることも確認済み
- 別の鍵で作った署名・壊れた署名は «破損» と表示する (本体の sigcheck はこれを «エラー (-2)» として返す)

### krkrxp3 — xp3 アーカイブツール

```
krkrxp3 [<フォルダ> | <xp3>]                    画面を開く
krkrxp3 --cli pack <フォルダ> [--out=FILE] [--rpf=FILE] [--protect]
                 [--no-compress-index] [--size-limit=KB | --no-size-limit] [--save-rpf=FILE]
krkrxp3 --cli list    <xp3> [--json]
krkrxp3 --cli extract <xp3> [--out=DIR]
krkrxp3 --cli verify  <xp3>
```

- 吉里吉里2 のリリーサ (krkrrel) と同じ書式・同じ既定の分類 (拡張子ごとに 圧縮 / 格納のみ / 入れない) で作る。
  中身が同じファイルは格納を共有する。フォルダに `default.rpf` (リリーサのプロファイル) があれば読む
- `.` で始まる名前と CVS フォルダは入れない。圧縮しても小さくならないファイルは圧縮せずに格納する
- 展開プロテクト付きのファイルは展開しない (一覧と検証はできる)
- 吉里吉里2 の exe に結合された xp3 も読める
- 本体 (krkrz) で読めること、旧リリーサの xp3 を読めることを確認済み
- 暗号化 (旧 `xp3enc.dll`) と Ogg Vorbis のコードブック共有は未対応

### krkraudio — 音声フォーマットコンバータ

```
krkraudio [<ファイル>...]                       画面を開く
krkraudio --cli info     <ファイル>...
krkraudio --cli convert  <ファイル>... --to=ogg|opus|wav [--out=DIR]
                         [--quality=Q] [--bitrate=KBPS] [--bits=16|24|32]
                         [--gain=DB] [--normalize=LUFS] [--replaygain] [--force]
krkraudio --cli loudness <ファイル>...
krkraudio --cli lipsync  <ファイル> [--fps=30] [--format=json|csv] [--out=FILE]
```

- 入力は WAV (PCM 8/16/24/32bit・float)、Ogg Vorbis、Ogg Opus。出力は Ogg Vorbis / Ogg Opus / WAV
- «ファイル.sli» (ループ情報) があれば変換先にも書き出す。Opus は 48kHz になるので位置を換算する
- 音量: `--gain` は Opus ならヘッダゲイン (波形は変えない。本体は常に適用)、Vorbis / WAV は波形に焼き込む。
  `--normalize=-18` で統合ラウドネス (EBU R128) を揃える。`--replaygain` は Vorbis に
  `REPLAYGAIN_TRACK_GAIN` / `PEAK` を書く (本体を `-ogg_rg=track` で起動したときに効く)。
  Opus を Opus 以外へ変換するときは元のヘッダゲインを焼き込む
- `lipsync`: 一定間隔ごとの音量 (RMS とピーク、0〜1)。口パクの開き具合に使う (出力形式は暫定。母音の推定は今後)
- 本体 (krkrz) で、変換した音声と .sli を開けることを確認済み

### krkrimg — 画像フォーマットコンバータ

```
krkrimg [<ファイル>...]                         画面を開く
krkrimg --cli info    <ファイル>...
krkrimg --cli convert <ファイル>... [--opaque=tlg5|tlg6|png|bmp|jpg]
                      [--alpha=tlg5|tlg6|png|bmp|sep] [--sep-main=jpg|png|bmp] [--sep-mask=jpg|png|bmp]
                      [--quality=90] [--main-quality=90] [--mask-quality=90]
                      [--transparent=remove|keep|expand1..8] [--input-addalpha] [--addalpha]
                      [--out=DIR] [--force]
krkrimg --cli layers  <PSD / CLIP>... [--format=png|tlg5|tlg6] [--hidden] [--out=DIR] [--force]
```

- 入力は BMP / PNG / JPEG / TLG5 / TLG6 / PSD / CLIP STUDIO (.clip)。PSD と CLIP はレイヤから合成する
  (PSD はブレンドモード・レイヤー効果・調整レイヤ込み)。«ファイル_m.bmp/png/jpg» があればメイン/マスク分離形式として読む
- 旧 krkrtpc と同じ規則: 不透明な画像と透明部分のある画像で別々の形式 (全画素が不透明なら «不透明»)、
  完全透明部分の色の処理 (除去 / そのまま / 周囲の色で合成)、ltAddAlpha への変換、
  TLG の `mode` タグ、PNG の oFFs / vpAg / pHYs を `offs_*` / `vpag_*` / `reso_*` タグとして引き継ぐ
- `layers`: レイヤを «ファイル名/番号_レイヤ名.png» に 1 枚ずつ書き出し、位置・不透明度・ブレンドモードを
  «ファイル名/layers.json» にまとめる。各画像には文書上の位置 (`offs_*`) と文書の大きさ (`vpag_*`)、
  TLG にはブレンドモードに対応する `mode` タグ (psmul など) を書く。PSD はレイヤー効果込み
- TLG の読み書きは本体のコードの移植 (`libs/tlg`)。**本体と同じ入力から同じバイト列を出す**ことと、
  本体で読めて画素・タグが一致することを確認済み

### 共通のオプション

- `--cli` … 画面を開かずに処理して終わる
- 値を取るオプションは **`--name=値`** の形で書く (`--key=public.txt`)
- `--help` で一覧、`--port=N` / `--browser=none` などは appserve の起動オプション (`--help` 参照)

## ビルド

前提: CMake 3.21 以上、Ninja、vcpkg (`VCPKG_ROOT` を設定)。Windows は Visual Studio 2022 の開発者コマンドプロンプトから。

```bash
git submodule update --init
cmake --preset windows          # linux / macos
cmake --build --preset windows-rel
```

成果物は `build/<preset>/tools/<ツール>/Release/<ツール>.exe`。画面 (web/) は exe に埋め込まれるので exe 1 本で動く。

吉里吉里Z 本体のソースを使うツール (今後の xp3 / 画像 / 音声など) は、環境変数 `KRKRZ_BASE` に
krkrz_dev を置いているフォルダの親を設定する (krkrz_android / krkrz_linux と同じ)。

## 構成

```
external/appserve   ブラウザ UI のフレームワーク (submodule)
external/psdparse   PSD の読み込みと合成 (submodule)
external/clipparse  CLIP STUDIO (.clip) の読み込みと合成 (submodule。sqlite を FetchContent で取る)
cmake/KrtTool.cmake krt_add_tool(): ツールの exe + 画面の埋め込み
libs/app            共通の枠 (GUI / CLI の振り分け、長い処理の実行と進捗、パス変換)
libs/sig            電子署名 (検証・鍵生成・署名)
libs/xp3            xp3 アーカイブの読み書き
libs/loop           ループ情報 (.sli) の読み書き
libs/audio          音声の読み書き (WAV / Vorbis / Opus) とラウドネス・音量
libs/tlg            TLG5 / TLG6 の読み書き (本体 SaveTLG5/6・LoadTLG の移植)
libs/image          画像の読み書き (BMP / PNG / JPEG / TLG / PSD / CLIP)、旧 krkrtpc と同じ前処理、レイヤ書き出し
web/common          全ツール共通の画面部品 (krt.js / krt.css: フォルダ・ファイル選択ほか)
tools/<ツール>/      main.cpp (CLI + API) と web/ (画面)
```

## ライセンス

未定 (吉里吉里Z 本体に合わせる予定)。
