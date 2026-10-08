//---------------------------------------------------------------------------
// krt::image — PSD / CLIP のレイヤを 1 枚ずつ取り出す
//
// 画像は «そのレイヤが占める矩形» の大きさで、文書上の位置 (left / top) を持つ。
// PSD はレイヤー効果込み (psdparse の renderLayer。はみ出す効果は矩形も広がる)、
// CLIP はマスクをアルファに繰り込んだ画素 (不透明度は掛けない)。
// フォルダ・調整レイヤ・画素の無いレイヤは含めない。並びは下から上。
//---------------------------------------------------------------------------
#pragma once
#include "krt/image/Image.h"

namespace krt::image {

struct LayerEntry {
	int         index = 0;      ///< 元のファイルでのレイヤ番号
	std::string name;           ///< UTF-8
	std::string folder;         ///< 親フォルダの名前を '/' でつないだもの (最上位は空)
	bool        visible = true; ///< 自分と親フォルダがすべて表示
	int         left = 0, top = 0;
	int         opacity = 255;  ///< 0..255
	std::string blend;          ///< 元のブレンドモード (PSD は 4 文字のキー、CLIP は番号)
	std::string mode;           ///< 吉里吉里の mode タグ (alpha / psmul など。対応が無ければ空)
	Image       image;
};

struct LayerDocument {
	Format format = Format::Unknown;
	int    width = 0, height = 0;
	std::vector<LayerEntry> layers;
};

/// PSD / CLIP のレイヤを取り出す。withHidden = false なら非表示のレイヤを除く
bool loadLayers(const std::filesystem::path& path, LayerDocument& out, std::string& error, bool withHidden = false);

} // namespace krt::image
