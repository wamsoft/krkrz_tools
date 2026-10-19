# krkrsign — キー生成・署名ツール

電子署名の鍵を作り、ファイルに署名し、署名を検証します。

## 使い方

```
krkrsign                                     画面を開く
krkrsign --cli keygen [--bits=1024] [--public=FILE] [--private=FILE] [--force]
krkrsign --cli sign   --key=<秘密鍵> <ファイル>...
krkrsign --cli verify --key=<公開鍵> <ファイル>...
```

## 動作

- 鍵は RSA (既定 1024bit = 旧ツールと同じです。2048 / 3072 / 4096 も使えます)、署名は SHA256 + RSA-PSS です
- 吉里吉里の exe (署名領域を持つもの) は exe の中へ、それ以外は «ファイル名.sig» へ署名を書きます
- 旧 krkrsign の鍵で署名でき、旧ツールで作った署名を検証できます。本体の sigcheck プラグインで検証できる
  ことも確認済みです
- 別の鍵で作った署名・壊れた署名は «破損» と表示します (本体の sigcheck はこれを «エラー (-2)» として返します)

配布先で署名を確かめるほうのツールは [krkrcheck](krkrcheck.md) です。
