// PSD / CLIP のレイヤ単位の取り出し
#include "krt/image/Layers.h"

#include <fstream>

#include <clipfile.h>
#include <psdfile.h>

namespace fs = std::filesystem;

namespace krt::image {

namespace {

std::string utf16ToUtf8(const std::u16string& s)
{
	std::string o;
	for (size_t i = 0; i < s.size(); ++i) {
		uint32_t c = s[i];
		if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
			c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
		if (c < 0x80) o += (char)c;
		else if (c < 0x800) { o += (char)(0xc0 | (c >> 6)); o += (char)(0x80 | (c & 0x3f)); }
		else if (c < 0x10000) { o += (char)(0xe0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3f)); o += (char)(0x80 | (c & 0x3f)); }
		else { o += (char)(0xf0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3f)); o += (char)(0x80 | ((c >> 6) & 0x3f)); o += (char)(0x80 | (c & 0x3f)); }
	}
	return o;
}

std::string keyString(int key)
{
	std::string s;
	for (int i = 3; i >= 0; --i) s += (char)((key >> (8 * i)) & 0xff);
	return s;
}

// PSD のブレンドキー → 吉里吉里の mode タグ (旧 krkrtpc と同じ対応。imageTagLayerType 参照)
std::string psdModeTag(const std::string& key)
{
	static const char* const table[][2] = {
		{ "norm", "alpha" },    { "lddg", "psadd" },     { "lbrn", "pssub" },   { "mul ", "psmul" },
		{ "scrn", "psscreen" }, { "over", "psoverlay" }, { "hLit", "pshlight" }, { "sLit", "psslight" },
		{ "div ", "psdodge" },  { "idiv", "psburn" },    { "lite", "pslighten" }, { "dark", "psdarken" },
		{ "diff", "psdiff" },   { "smud", "psexcl" },
	};
	for (const auto& t : table)
		if (key == t[0]) return t[1];
	return {};
}

bool readAll(const fs::path& p, std::vector<uint8_t>& buf)
{
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	buf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return !buf.empty();
}

bool loadPsdLayers(const std::vector<uint8_t>& buf, LayerDocument& out, std::string& error, bool withHidden)
{
	psd::PSDFile psd;
	if (!psd.loadFromMemory(buf.data(), buf.size())) { error = "PSD を読めません"; return false; }
	out.width = psd.header.width;
	out.height = psd.header.height;
	const auto& list = psd.layerList;
	auto nameOf = [&](int i) {
		const auto& l = list[i];
		return l.layerNameUnicode.empty() ? l.layerName : utf16ToUtf8(l.layerNameUnicode);
	};
	for (int i = 0; i < (int)list.size(); ++i) {
		const auto& l = list[i];
		if (l.layerType == psd::LAYER_TYPE_FOLDER || l.layerType == psd::LAYER_TYPE_HIDDEN || l.layerType == psd::LAYER_TYPE_ADJUST)
			continue;
		LayerEntry e;
		e.index = i;
		e.name = nameOf(i);
		e.visible = l.isVisible();
		for (int p = l.parentIndex; p >= 0; p = list[p].parentIndex) {
			e.folder = nameOf(p) + (e.folder.empty() ? "" : "/" + e.folder);
			if (!list[p].isVisible()) e.visible = false;
		}
		if (!e.visible && !withHidden) continue;
		e.opacity = l.opacity;
		e.blend = keyString(l.blendModeKey);
		e.mode = psdModeTag(e.blend);
		std::vector<uint8_t> bgra;
		int left = 0, top = 0, w = 0, h = 0;
		if (!psd.renderLayer(i, bgra, left, top, w, h) || w <= 0 || h <= 0 || bgra.size() != (size_t)w * h * 4)
			continue;   // 画素の無いレイヤ
		e.left = left;
		e.top = top;
		e.image.width = w;
		e.image.height = h;
		e.image.bgra.swap(bgra);
		out.layers.push_back(std::move(e));
	}
	return true;
}

bool loadClipLayers(const std::vector<uint8_t>& buf, LayerDocument& out, std::string& error, bool withHidden)
{
	clip::ClipFile cf;
	if (!cf.loadFromMemory(buf.data(), (int64_t)buf.size())) { error = "CLIP を読めません: " + cf.error(); return false; }
	out.width = (int)cf.canvasWidth();
	out.height = (int)cf.canvasHeight();
	const auto& list = cf.layers();
	for (int i = 0; i < (int)list.size(); ++i) {
		const auto& l = list[i];
		if (l.isGroup || l.isFilter || l.bounds.empty()) continue;
		LayerEntry e;
		e.index = i;
		e.name = l.name;
		e.visible = l.visibility != 0;
		for (int p = l.parent; p >= 0; p = list[p].parent) {
			e.folder = list[p].name + (e.folder.empty() ? "" : "/" + e.folder);
			if (!list[p].visibility) e.visible = false;
		}
		if (!e.visible && !withHidden) continue;
		e.opacity = (int)(l.opacity * 255 / 256);
		e.blend = std::to_string(l.composite);
		e.mode = l.composite == 0 ? "alpha" : "";
		clip::Image img;
		if (!cf.layerImage(i, clip::IMAGE_MODE_MASKED, img) || img.width == 0 || img.height == 0) continue;
		e.left = l.bounds.x;
		e.top = l.bounds.y;
		e.image.width = (int)img.width;
		e.image.height = (int)img.height;
		e.image.bgra.resize(img.rgba.size());
		for (size_t k = 0; k < img.rgba.size(); k += 4) {
			e.image.bgra[k] = img.rgba[k + 2];
			e.image.bgra[k + 1] = img.rgba[k + 1];
			e.image.bgra[k + 2] = img.rgba[k];
			e.image.bgra[k + 3] = img.rgba[k + 3];
		}
		out.layers.push_back(std::move(e));
	}
	return true;
}

} // namespace

bool loadLayers(const fs::path& path, LayerDocument& out, std::string& error, bool withHidden)
{
	out = LayerDocument();
	std::vector<uint8_t> buf;
	if (!readAll(path, buf)) { error = "ファイルを開けません"; return false; }
	out.format = detect(buf.data(), buf.size());
	if (out.format == Format::Psd) return loadPsdLayers(buf, out, error, withHidden);
	if (out.format == Format::Clip) return loadClipLayers(buf, out, error, withHidden);
	error = "レイヤを取り出せるのは PSD と CLIP だけです";
	return false;
}

} // namespace krt::image
