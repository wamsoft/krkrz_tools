// PSD の読み込み (psdparse でレイヤから合成する)
//
// 旧版は «通常» と一部のブレンドモードしか扱えなかったが、psdparse は
// ブレンドモード・レイヤー効果・調整レイヤを含めて合成できる。
// 合成できない文書 (ビット数・カラーモードなど) は保存されている合成画像を使う。
#include "Codecs.h"

#include <psdfile.h>

namespace krt::image::detail {

bool loadPsd(const uint8_t* data, size_t size, Image& out, LoadInfo& info, std::string& error)
{
	psd::PSDFile psd;
	if (!psd.loadFromMemory(data, size)) { error = "PSD を読めません"; return false; }
	const int w = psd.header.width, h = psd.header.height;
	if (w <= 0 || h <= 0) { error = "PSD の大きさが不正です"; return false; }
	info.layers = (int)psd.layerList.size();

	std::vector<uint8_t> bgra;
	bool ok = !psd.layerList.empty() && psd.compositeImage(bgra) && bgra.size() == (size_t)w * h * 4;
	if (!ok) {
		// レイヤが無い / 合成できない: 保存されている合成画像
		if (!psd.canDecodeMergedImage()) { error = "PSD の画像を取り出せません (対応していないカラーモードかビット数です)"; return false; }
		bgra.assign((size_t)w * h * 4, 0);
		if (!psd.getMergedImage(bgra.data(), psd::BGRA_LE, w * 4)) { error = "PSD の合成画像を読めません"; return false; }
		if (!psd.mergedHasTransparency())
			for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 0xff;
	}
	out.width = w;
	out.height = h;
	out.bgra.swap(bgra);
	info.hasAlpha = true;   // 判定は呼び出し側 (hasTransparency) に任せる
	return true;
}

} // namespace krt::image::detail
