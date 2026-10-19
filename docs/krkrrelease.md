# krkrrelease — リリーサ (Windows 専用)

吉里吉里Z の exe (WINVER 版 / SDL 版の Windows) から配布用の exe を作ります。埋め込みオプション・アイコン・
バージョン情報・セキュリティ設定・xp3・署名を扱います。

## 使い方

```
krkrrelease [<release.json>]                      画面を開く
krkrrelease --cli run      <release.json> [--force]
krkrrelease --cli info     <exe>                  埋め込みオプション・セキュリティ設定・版情報を表示
krkrrelease --cli template <release.json> [--exe=<吉里吉里の exe>]   設定ファイルのひな形を書く
```

設定は JSON (`release.json`) に保存し、画面でも CLI でも同じものを使います (相対パスは設定ファイルのフォルダ
基準です)。

## 処理の順序

1. exe をコピーして改名します (元の exe は書き換えません。後ろにデータが付いた exe は元にできません)
2. リソースを書き換えます
   - 埋め込みオプション (`.cf` と同じ書式、`;` はコメント): WINVER は TEXT/139、SDL は BINARY/CONFIG.CF
     (本体同梱の既定値に重ねます)。値に ASCII 以外を含む行は `name="\xNN..."` に直して書きます
   - アイコン (.ico、複数サイズ): 既存のアイコングループを全部差し替えます (無ければ WINVER は 107、SDL は
     MAINICON として足します)
   - バージョン情報: FileDescription / ProductName / CompanyName / LegalCopyright / FileVersion / ProductVersion
     (FileVersion・ProductVersion は数値の版も合わせます)
3. セキュリティ設定 (`forcedataxp3` / `acceptfilenameargument` / `disablemsgmap` / `disableapplock` /
   WINVER は `disabled3d9`) の数字を同じ長さで書き換えます
4. データ: `none` / `copy` (exe の隣に data.xp3) / `bind` (exe の後ろに 16 バイト境界で結合) から選びます。
   フォルダを指定すると xp3 を作ります (フォルダの default.rpf か `rpf` のプロファイルを使います)
5. 署名 (秘密鍵を指定したとき): exe と、隣に置いた xp3 の `.sig` を作ります。**必ず最後**に行います

## 設定ファイル

例 (`krkrrelease --cli template` が書くもの):

```json
{
 "exe": "krkrz64.exe",
 "output": "release/game.exe",
 "setOptions": true,
 "options": "; 起動オプション\n",
 "icon": "game.ico",
 "version": { "FileDescription": "", "ProductName": "", "CompanyName": "", "LegalCopyright": "", "FileVersion": "", "ProductVersion": "" },
 "security": { "acceptfilenameargument": 0, "forcedataxp3": 1 },
 "dataMode": "copy",
 "data": "data",
 "rpf": "",
 "dataName": "data.xp3",
 "signKey": "private.txt"
}
```

- 空の値の項目は変えません。`security` には変える項目だけを書きます
- 作った exe を後から書き換える (Authenticode・DRM など) と署名が合わなくなるので、その後に `.sig` を
  作り直してください (`krkrsign --cli sign`)

## 確認済みのこと

WINVER / SDL とも、作った exe が埋め込みオプション (日本語の値を含む) を読み、data.xp3 / 結合した xp3 から
起動します。SDL は本体の既定値 (padinterval) が残ります。バージョン情報・アイコンが Windows に表示され、
`forcedataxp3=1` で data フォルダから起動しなくなります。署名は krkrsign で検証できます。
