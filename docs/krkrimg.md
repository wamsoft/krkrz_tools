# krkrimg — 画像フォーマットコンバータ

旧 krkrtpc の後継です。画像の形式を変換し、PSD / CLIP STUDIO のレイヤを合成・書き出します。

## 使い方

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

## 動作

- 入力は BMP / PNG / JPEG / TLG5 / TLG6 / PSD / CLIP STUDIO (.clip) です。PSD と CLIP はレイヤから合成します
  (PSD はブレンドモード・レイヤー効果・調整レイヤ込みです)。«ファイル_m.bmp/png/jpg» があればメイン/マスク
  分離形式として読みます
- 旧 krkrtpc と同じ規則で処理します: 不透明な画像と透明部分のある画像で別々の形式 (全画素が不透明なら
  «不透明»)、完全透明部分の色の処理 (除去 / そのまま / 周囲の色で合成)、ltAddAlpha への変換、
  TLG の `mode` タグ、PNG の oFFs / vpAg / pHYs を `offs_*` / `vpag_*` / `reso_*` タグとして引き継ぎます
- `layers`: レイヤを «ファイル名/番号_レイヤ名.png» に 1 枚ずつ書き出し、位置・不透明度・ブレンドモードを
  «ファイル名/layers.json» にまとめます。各画像には文書上の位置 (`offs_*`) と文書の大きさ (`vpag_*`)、
  TLG にはブレンドモードに対応する `mode` タグ (psmul など) を書きます。PSD はレイヤー効果込みです
- TLG の読み書きは本体のコードの移植です (`libs/tlg`)。**本体と同じ入力から同じバイト列を出す**ことと、
  本体で読めて画素・タグが一致することを確認済みです
