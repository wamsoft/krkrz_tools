# krkrxp3 — xp3 アーカイブツール

xp3 アーカイブを作成・一覧・展開・検証します。

## 使い方

```
krkrxp3 [<フォルダ> | <xp3>]                    画面を開く
krkrxp3 --cli pack <フォルダ> [--out=FILE] [--rpf=FILE] [--protect]
                 [--no-compress-index] [--size-limit=KB | --no-size-limit] [--save-rpf=FILE]
krkrxp3 --cli list    <xp3> [--json]
krkrxp3 --cli extract <xp3> [--out=DIR]
krkrxp3 --cli verify  <xp3>
```

## 動作

- 吉里吉里2 のリリーサ (krkrrel) と同じ書式・同じ既定の分類 (拡張子ごとに 圧縮 / 格納のみ / 入れない) で
  作ります。中身が同じファイルは格納を共有します。フォルダに `default.rpf` (リリーサのプロファイル) があれば
  読みます
- `.` で始まる名前と CVS フォルダは入れません。圧縮しても小さくならないファイルは圧縮せずに格納します
- 展開プロテクト付きのファイルは展開しません (一覧と検証はできます)
- 吉里吉里2 の exe に結合された xp3 も読めます
- 本体 (krkrz) で読めること、旧リリーサの xp3 を読めることを確認済みです
- 暗号化 (旧 `xp3enc.dll`) と Ogg Vorbis のコードブック共有には対応していません
