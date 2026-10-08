//---------------------------------------------------------------------------
// krt::image — 画像の読み書きと、吉里吉里向けの前処理
//
// 画素は常に 32bpp BGRA (メモリ上 B,G,R,A。本体の 0xAARRGGBB と同じ並び)、
// 上から下へ、1 行 width*4 バイトで持つ。アルファは «そのまま» (ストレート)。
// ltAddAlpha 形式へ変換したあとは、色にアルファが掛かった値になる。
//
// 読める形式: BMP / PNG / JPEG / TLG5 / TLG6 / PSD・CLIP (レイヤから合成)
// 書ける形式: BMP (24/32bit) / PNG / JPEG / TLG5 / TLG6
//
// 前処理は旧版の画像フォーマットコンバータ (krkrtpc) と同じ規則で行う:
//   - メイン/マスク分離形式 (foo.png + foo_m.png) の読み込み・書き出し
//   - 完全透明部分の色: 除去 / そのまま / 周囲の色で埋める (n ピクセル以内)
//   - ltAlpha → ltAddAlpha の変換
//   - タグ (mode=..., PNG の oFFs / vpAg / pHYs 由来の offs_* / vpag_* / reso_*)
//---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace krt::image {

/// 画像のタグ (TLG のタグ情報、PNG のチャンク由来の値)。順序を保つ
using Tags = std::vector<std::pair<std::string, std::string>>;

/// タグの値 (無ければ空)
std::string tagValue(const Tags& tags, const std::string& key);
/// タグを設定する (既にあれば置き換え)
void setTag(Tags& tags, const std::string& key, const std::string& value);

enum class Format { Unknown, Bmp, Png, Jpeg, Tlg5, Tlg6, Psd, Clip };

const char* formatName(Format f);
/// "bmp" / "png" / "jpg" / "jpeg" / "tlg5" / "tlg6" / "psd" / "clip"
Format formatFromName(const std::string& name);
/// 出力ファイルの拡張子 (".bmp" / ".png" / ".jpg" / ".tlg")
const char* formatExtension(Format f);
/// 先頭のバイト列から形式を判定する
Format detect(const uint8_t* data, size_t size);

struct Image {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> bgra;   ///< width*height*4
	Tags tags;

	bool empty() const { return width <= 0 || height <= 0; }
	uint8_t* row(int y) { return bgra.data() + (size_t)y * width * 4; }
	const uint8_t* row(int y) const { return bgra.data() + (size_t)y * width * 4; }
};

/// 読み込んだときの情報
struct LoadInfo {
	Format format = Format::Unknown;
	bool   hasAlpha = false;     ///< 元の形式が透明度を持っていたか
	bool   grayscale = false;    ///< グレイスケールの画像だった (JPEG / PNG / BMP 8bit)
	int    layers = 0;           ///< PSD / CLIP のレイヤ数
};

/// ファイル / メモリから読む。透明度の無い形式はアルファ 255 で埋める
bool load(const std::filesystem::path& path, Image& out, std::string& error, LoadInfo* info = nullptr);
bool loadMemory(const uint8_t* data, size_t size, Image& out, std::string& error, LoadInfo* info = nullptr);

struct SaveOptions {
	bool withAlpha = true;       ///< BMP は 32bit / PNG は RGBA / TLG は 4 色で書く
	bool grayscale = false;      ///< JPEG / PNG / BMP をグレイスケールで書く (マスク画像用)
	int  jpegQuality = 90;       ///< 1..100
	int  pngLevel = 9;           ///< zlib の圧縮レベル 0..9
};

/// 書き出す。タグは TLG ではタグ情報として、PNG では oFFs / vpAg / pHYs として書く
bool save(const std::filesystem::path& path, const Image& img, Format format,
          const SaveOptions& opt, std::string& error);
bool saveMemory(std::vector<uint8_t>& out, const Image& img, Format format,
                const SaveOptions& opt, std::string& error);

//---------------------------------------------------------------------------
// 前処理
//---------------------------------------------------------------------------

/// 透明な画素が 1 つでもあるか
bool hasTransparency(const Image& img);
/// アルファを 255 で埋める
void makeOpaque(Image& img);
/// 完全透明 (アルファ 0) の画素の色を 0 (黒) にする
void clearTransparentColor(Image& img);
/// 完全透明の画素の色を、n ピクセル以内で最も近い不透明な画素の色 (同距離は平均) にする。
/// 見つからない画素は黒にする (旧版の ExpandOpaqueColor と同じ結果)
void expandOpaqueColor(Image& img, int n);
/// ltAlpha → ltAddAlpha (色にアルファを掛ける。c * a / 255 の切り捨て)
void toAddAlpha(Image& img);
/// 本体と同じ式のグレイスケール値 ((b*19 + g*183 + r*54) >> 8)
inline uint8_t grayOf(const uint8_t* bgra) { return (uint8_t)((bgra[0] * 19 + bgra[1] * 183 + bgra[2] * 54) >> 8); }
/// mask の明るさを main のアルファにする (サイズが違えば false)
bool bindMask(Image& main, const Image& mask, std::string& error);
/// アルファを取り出したグレイスケール画像 (BGR とも同じ値、アルファ 255)
Image extractMask(const Image& img);

/// メイン/マスク分離形式のマスク画像を探す (foo.png に対し foo_m.bmp / .png / .jpg / .jpeg)。
/// 無ければ空のパス
std::filesystem::path findMaskFile(const std::filesystem::path& mainFile);

} // namespace krt::image
