// CLIP STUDIO PAINT (.clip) の読み込み (clipparse でレイヤから合成する)
//
// 合成できないときは、ファイルに埋まっているプレビュー画像 (CanvasPreview) が
// 等倍であればそれを使う。
#include "Codecs.h"

#include <clipfile.h>

namespace krt::image::detail {

bool loadClip(const uint8_t* data, size_t size, Image& out, LoadInfo& info, std::string& error)
{
	clip::ClipFile cf;   // loadFromMemory はバイト列を所有しない (この関数の中だけで使う)
	if (!cf.loadFromMemory(data, (int64_t)size)) {
		error = "CLIP を読めません: " + cf.error();
		return false;
	}
	info.layers = (int)cf.layers().size();
	clip::Image merged;
	if (cf.mergedImage(merged) && merged.width > 0 && merged.height > 0) {
		out.width = (int)merged.width;
		out.height = (int)merged.height;
		out.bgra.resize(merged.rgba.size());
		for (size_t i = 0; i < merged.rgba.size(); i += 4) {
			out.bgra[i]     = merged.rgba[i + 2];
			out.bgra[i + 1] = merged.rgba[i + 1];
			out.bgra[i + 2] = merged.rgba[i];
			out.bgra[i + 3] = merged.rgba[i + 3];
		}
		info.hasAlpha = true;   // 判定は呼び出し側 (hasTransparency) に任せる
		return true;
	}
	std::vector<uint8_t> png;
	int w = 0, h = 0;
	if (cf.previewPng(png, w, h) && w == cf.canvasWidth() && h == cf.canvasHeight()) {
		LoadInfo pi;
		if (loadPng(png.data(), png.size(), out, pi, error)) {
			info.hasAlpha = pi.hasAlpha;
			return true;
		}
	}
	error = "CLIP の画像を合成できません" + (cf.error().empty() ? std::string() : ": " + cf.error());
	return false;
}

} // namespace krt::image::detail
