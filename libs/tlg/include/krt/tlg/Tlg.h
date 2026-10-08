//---------------------------------------------------------------------------
// krt_tlg — 吉里吉里Z の TLG5 / TLG6 画像コーデック (単体ライブラリ版)
//
// 吉里吉里Z 本体 (src/core/common/visual/) の
//   SaveTLG5.cpp / SaveTLG6.cpp / SaveTLG.h / LoadTLG.cpp / tvpgl.c (C 版のみ)
// を、エンジン型 (tjs_uint8 / iTJSBinaryStream / tTVPBaseBitmap / TJS 辞書) に
// 依存しない std 型だけの形へ移植したもの。エンコーダ出力は同じ入力に対して
// 本体の Bitmap.save(..., "tlg5"/"tlg6"/"tlg524"/"tlg624") とバイト一致する。
//
// 画像の形式 (入出力共通):
//   32bpp BGRA。メモリ上のバイト順は B,G,R,A (= 本体の 0xAARRGGBB を
//   リトルエンディアンで並べたもの)。上から下 (top-down)、stride = width*4。
//---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace krt {
namespace tlg {

// TLG メタ情報 ("tags" チャンク)。name / value とも UTF-8 文字列 (バイト列として扱う)。
// 本体の Bitmap.save の第 3 引数 (辞書) に相当する。順序はこのベクタの順で書き出す。
using Tags = std::vector<std::pair<std::string, std::string>>;

// TLG5 で保存する。
//   withAlpha = true  … colors=4 (本体の "tlg5")
//   withAlpha = false … colors=3、アルファは捨てる (本体の "tlg524")
// tags が空でなければ "TLG0.0 sds" ラッパー + "tags" チャンク付きで書く
// (本体 TVPSaveAsTLG と同じ)。空なら素の TLG5 ストリームのみ。
bool encodeTlg5(const uint8_t* bgra, int width, int height, bool withAlpha,
                const Tags& tags, std::vector<uint8_t>& out, std::string& error);

// TLG6 で保存する。withAlpha の意味は encodeTlg5 と同じ ("tlg6" / "tlg624")。
bool encodeTlg6(const uint8_t* bgra, int width, int height, bool withAlpha,
                const Tags& tags, std::vector<uint8_t>& out, std::string& error);

// TLG5 / TLG6、および "TLG0.0 sds" ラッパー (tags 付き) を読む。
// 出力は BGRA top-down。colors=3 の画像はアルファ 0xff で埋まる。
// hasAlpha (省略可) … 元データが colors==4 なら true。
// tags (省略可)     … "tags" チャンクの内容 (ファイル内の順序)。無ければ空。
bool decodeTlg(const uint8_t* data, size_t size, int& width, int& height,
               std::vector<uint8_t>& bgra, bool* hasAlpha, Tags* tags,
               std::string& error);

} // namespace tlg
} // namespace krt
