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
cmake/KrtTool.cmake krt_add_tool(): ツールの exe + 画面の埋め込み
libs/app            共通の枠 (GUI / CLI の振り分け、長い処理の実行と進捗、パス変換)
libs/sig            電子署名 (検証・鍵生成・署名)
libs/xp3            xp3 アーカイブの読み書き
web/common          全ツール共通の画面部品 (krt.js / krt.css: フォルダ・ファイル選択ほか)
tools/<ツール>/      main.cpp (CLI + API) と web/ (画面)
```

## ライセンス

未定 (吉里吉里Z 本体に合わせる予定)。
