# krkrz_tools — 吉里吉里Z 標準ツール

日本語 | [English](README_en.md)

吉里吉里Z の標準ツール群です。吉里吉里2 時代の標準ツール (C++Builder 製・Windows 専用) を置き換えます。

- **Windows / Linux / macOS** で動きます (macOS は Intel のみ確認済み。Apple Silicon は未確認です)
- 各ツールは**ブラウザを画面にするネイティブアプリ** ([appserve](https://github.com/wamsoft/appserve)) です。
  起動するとローカルの HTTP サーバを立て、ブラウザのアプリ窓で画面を開きます
- **全ツールがコマンドラインでも動きます** (`--cli`)。GUI と CLI は同じ処理を呼びます
- 旧ツールの書式 (公開鍵・`.sig`・exe 埋め込み署名・ini など) と互換です

計画と進捗は krkrz_dev の [TODO-tools.md](https://github.com/wamsoft/krkrz_dev/blob/develop/TODO-tools.md) にあります。

## ツール一覧

| 実行ファイル | ツール | 状態 | 説明 |
|---|---|---|---|
| `krkrcheck` | ファイル破損チェックツール | 動作確認済み (Windows) | [docs/krkrcheck.md](docs/krkrcheck.md) |
| `krkrsign` | キー生成・署名ツール | 動作確認済み (Windows) | [docs/krkrsign.md](docs/krkrsign.md) |
| `krkrxp3` | xp3 アーカイブツール (作成・一覧・展開・検証) | 動作確認済み (Windows) | [docs/krkrxp3.md](docs/krkrxp3.md) |
| `krkraudio` | 音声フォーマットコンバータ (変換・音量・口パク用の音量) | 動作確認済み (Windows) | [docs/krkraudio.md](docs/krkraudio.md) |
| `krkrloop` | ループチューナ (波形を見ながら .sli のリンク・ラベルを編集、本体と同じ規則のループ再生) | 動作確認済み (Windows) | [docs/krkrloop.md](docs/krkrloop.md) |
| `krkrrelease` | リリーサ (配布用の exe を作る: 埋め込みオプション・アイコン・バージョン情報・セキュリティ設定・xp3・署名。**Windows 専用**) | 動作確認済み (Windows) | [docs/krkrrelease.md](docs/krkrrelease.md) |
| `krkrimg` | 画像フォーマットコンバータ (旧 krkrtpc の後継。PSD / CLIP の合成・レイヤ書き出し) | 動作確認済み (Windows) | [docs/krkrimg.md](docs/krkrimg.md) |

どのツールも、引数なし (またはファイル・フォルダを指定) で起動すると画面を開き、`--cli` を付けると画面を
開かずに処理します。各ツールのコマンドとオプションの詳細は、上の表の docs/ のページにあります。

### krkrcheck — ファイル破損チェックツール

フォルダ以下の `.sig` の付いたファイルと、署名が埋め込まれた吉里吉里の exe を公開鍵で検証します。公開鍵と
画面の文言は «実行ファイル名.ini» から読みます (吉里吉里2 の «ファイル破損チェックツール.ini» をそのまま
使えます)。CLI の終了コードは 0 = すべて正常 / 1 = 破損あり / 2 = 検証できないファイルあり・失敗 です。
→ [docs/krkrcheck.md](docs/krkrcheck.md)

### krkrsign — キー生成・署名ツール

RSA の鍵を作り (`keygen`)、署名し (`sign`)、検証します (`verify`)。署名は SHA256 + RSA-PSS で、吉里吉里の
exe は exe の中へ、それ以外は «ファイル名.sig» へ書きます。旧 krkrsign の鍵・署名と互換です。
→ [docs/krkrsign.md](docs/krkrsign.md)

### krkrxp3 — xp3 アーカイブツール

xp3 を作成 (`pack`)・一覧 (`list`)・展開 (`extract`)・検証 (`verify`) します。吉里吉里2 のリリーサ
(krkrrel) と同じ書式・同じ既定の分類で作り、`default.rpf` (リリーサのプロファイル) を読みます。
暗号化と Ogg Vorbis のコードブック共有には対応していません。
→ [docs/krkrxp3.md](docs/krkrxp3.md)

### krkraudio — 音声フォーマットコンバータ

WAV / Ogg Vorbis / Ogg Opus を相互に変換し (`convert`)、音量 (ゲイン・ラウドネスの正規化・ReplayGain) を
扱い、口パク用の音量を書き出します (`lipsync`)。«ファイル.sli» (ループ情報) も変換先へ引き継ぎます。
→ [docs/krkraudio.md](docs/krkraudio.md)

### krkrloop — ループチューナ

波形を見ながら «音声ファイル.sli» のリンクとラベルを編集し、本体の `WaveLoopManager` と同じ規則で
ループ再生します。CLI では .sli の内容の表示 (`info`) と、本体が読めるかの確認 (`check`) ができます。
→ [docs/krkrloop.md](docs/krkrloop.md)

### krkrrelease — リリーサ (Windows 専用)

吉里吉里Z の exe (WINVER 版 / SDL 版の Windows) から配布用の exe を作ります。設定は `release.json` に保存し、
埋め込みオプション・アイコン・バージョン情報・セキュリティ設定・データ (xp3 の隣置き / 結合)・署名を
この順に処理します。
→ [docs/krkrrelease.md](docs/krkrrelease.md)

### krkrimg — 画像フォーマットコンバータ

BMP / PNG / JPEG / TLG5 / TLG6 / PSD / CLIP STUDIO (.clip) を読み、旧 krkrtpc と同じ規則で変換します
(`convert`)。PSD / CLIP のレイヤを 1 枚ずつ書き出すこともできます (`layers`)。TLG の出力は本体と
同じバイト列です。
→ [docs/krkrimg.md](docs/krkrimg.md)

## 共通のオプション

- `--cli` … 画面を開かずに処理して終わります
- 値を取るオプションは **`--name=値`** の形で書きます (`--key=public.txt`)
- `--help` で一覧を表示します。`--port=N` / `--browser=none` などは appserve の起動オプションです (`--help` を参照してください)

## 必要なもの

- CMake 3.21 以上
- Ninja
- [vcpkg](https://github.com/microsoft/vcpkg) — 環境変数 `VCPKG_ROOT` に導入先を指定
- Windows: Visual Studio 2022 (開発者コマンドプロンプトから実行します)

## ビルド

```bash
git submodule update --init
cmake --preset windows          # linux / macos (Apple Silicon) / macos-x64 (Intel)
cmake --build --preset windows-rel
```

成果物は `build/<preset>/tools/<ツール>/Release/<ツール>.exe` です。画面 (web/) は exe に埋め込まれるので、
exe 1 本で動きます。Windows では MSVC のランタイムと vcpkg の依存ライブラリを静的リンクします
(triplet `x64-windows-static`)。

確認済みの環境 (2026-10-09): Windows x64 (VS 2022)、Linux x64 (Steam Linux Runtime 3.0 «sniper» SDK で
GLIBC 2.30 以下・libstdc++ 依存なし。ホストの直接ビルドもできます)、macOS x64 (Intel、`macos-x64`)。
Apple Silicon (`macos`) は未確認です。

環境変数 `KRKRZ_BASE` に krkrz_dev を置いているフォルダの親を設定すると、吉里吉里Z 本体のソースを参照
できます (krkrz_android / krkrz_linux と同じ作法です)。いまのところ本体のソースを直接使うツールは無いので、
設定しなくてもビルドできます。

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
libs/release        配布用の exe を作る (PE のリソース書き換え・セキュリティ設定・結合・署名。Windows 専用)
web/common          全ツール共通の画面部品 (krt.js / krt.css: フォルダ・ファイル選択ほか)
tools/<ツール>/      main.cpp (CLI + API) と web/ (画面)
docs/               各ツールの説明
```

## ライセンス

このリポジトリは吉里吉里Z 本体と同じ条件です ([LICENSE](LICENSE))。TLG・.sli・xp3 の読み書きには
本体のコードを移植した部分があります。submodule と vcpkg で取得するライブラリは、それぞれのライセンスに
従います。
