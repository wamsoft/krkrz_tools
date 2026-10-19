# krkraudio — 音声フォーマットコンバータ

音声の形式の変換、音量の調整、口パク用の音量の書き出しを行います。

## 使い方

```
krkraudio [<ファイル>...]                       画面を開く
krkraudio --cli info     <ファイル>...
krkraudio --cli convert  <ファイル>... --to=ogg|opus|wav [--out=DIR]
                         [--quality=Q] [--bitrate=KBPS] [--bits=16|24|32]
                         [--gain=DB] [--normalize=LUFS] [--replaygain] [--force]
krkraudio --cli loudness <ファイル>...
krkraudio --cli lipsync  <ファイル> [--fps=30] [--format=json|csv] [--out=FILE]
```

## 動作

- 入力は WAV (PCM 8/16/24/32bit・float)、Ogg Vorbis、Ogg Opus です。出力は Ogg Vorbis / Ogg Opus / WAV です
- «ファイル.sli» (ループ情報) があれば変換先にも書き出します。Opus は 48kHz になるので位置を換算します
- 音量: `--gain` は Opus ならヘッダゲイン (波形は変えません。本体は常に適用します)、Vorbis / WAV は波形に
  焼き込みます。`--normalize=-18` で統合ラウドネス (EBU R128) を揃えます。`--replaygain` は Vorbis に
  `REPLAYGAIN_TRACK_GAIN` / `PEAK` を書きます (本体を `-ogg_rg=track` で起動したときに効きます)。
  Opus を Opus 以外へ変換するときは元のヘッダゲインを焼き込みます
- `lipsync`: 一定間隔ごとの音量 (RMS とピーク、0〜1) を書き出します。口パクの開き具合に使います
  (出力形式は暫定です。母音の推定は今後の予定です)
- 本体 (krkrz) で、変換した音声と .sli を開けることを確認済みです
