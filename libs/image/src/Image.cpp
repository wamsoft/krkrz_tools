// 形式の判定・振り分けと、旧版コンバータ (krkrtpc) と同じ規則の前処理
#include "krt/image/Image.h"
#include "Codecs.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "krt/tlg/Tlg.h"

namespace fs = std::filesystem;

namespace krt::image {

std::string tagValue(const Tags& tags, const std::string& key)
{
	for (const auto& [k, v] : tags)
		if (k == key) return v;
	return {};
}

void setTag(Tags& tags, const std::string& key, const std::string& value)
{
	for (auto& [k, v] : tags)
		if (k == key) { v = value; return; }
	tags.emplace_back(key, value);
}

const char* formatName(Format f)
{
	switch (f) {
	case Format::Bmp:  return "bmp";
	case Format::Png:  return "png";
	case Format::Jpeg: return "jpg";
	case Format::Tlg5: return "tlg5";
	case Format::Tlg6: return "tlg6";
	case Format::Psd:  return "psd";
	case Format::Clip: return "clip";
	default:           return "unknown";
	}
}

Format formatFromName(const std::string& name)
{
	std::string n = name;
	for (auto& c : n) c = (char)std::tolower((unsigned char)c);
	if (!n.empty() && n[0] == '.') n.erase(0, 1);
	if (n == "bmp") return Format::Bmp;
	if (n == "png") return Format::Png;
	if (n == "jpg" || n == "jpeg" || n == "jfif") return Format::Jpeg;
	if (n == "tlg5") return Format::Tlg5;
	if (n == "tlg6" || n == "tlg") return Format::Tlg6;
	if (n == "psd" || n == "psb" || n == "pdd") return Format::Psd;
	if (n == "clip") return Format::Clip;
	return Format::Unknown;
}

const char* formatExtension(Format f)
{
	switch (f) {
	case Format::Bmp:  return ".bmp";
	case Format::Png:  return ".png";
	case Format::Jpeg: return ".jpg";
	case Format::Tlg5:
	case Format::Tlg6: return ".tlg";
	case Format::Psd:  return ".psd";
	case Format::Clip: return ".clip";
	default:           return "";
	}
}

Format detect(const uint8_t* d, size_t n)
{
	if (n >= 8 && std::memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return Format::Png;
	if (n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) return Format::Jpeg;
	if (n >= 2 && d[0] == 'B' && d[1] == 'M') return Format::Bmp;
	if (n >= 4 && std::memcmp(d, "8BPS", 4) == 0) return Format::Psd;
	if (n >= 8 && std::memcmp(d, "CSFCHUNK", 8) == 0) return Format::Clip;
	if (n >= 11 && std::memcmp(d, "TLG5.0\x00raw\x1a", 11) == 0) return Format::Tlg5;
	if (n >= 11 && std::memcmp(d, "TLG6.0\x00raw\x1a", 11) == 0) return Format::Tlg6;
	if (n >= 11 && std::memcmp(d, "TLG0.0\x00sds\x1a", 11) == 0) {
		// タグ付き: 中身の形式を見る (11 + 4 バイトの長さの後ろ)
		if (n >= 15 + 11) {
			const Format inner = detect(d + 15, n - 15);
			if (inner == Format::Tlg5 || inner == Format::Tlg6) return inner;
		}
		return Format::Tlg6;
	}
	return Format::Unknown;
}

bool loadMemory(const uint8_t* data, size_t size, Image& out, std::string& error, LoadInfo* info)
{
	LoadInfo local;
	LoadInfo& li = info ? *info : local;
	li = LoadInfo();
	out = Image();
	li.format = detect(data, size);
	bool ok = false;
	switch (li.format) {
	case Format::Bmp:  ok = detail::loadBmp(data, size, out, li, error); break;
	case Format::Png:  ok = detail::loadPng(data, size, out, li, error); break;
	case Format::Jpeg: ok = detail::loadJpeg(data, size, out, li, error); break;
	case Format::Psd:  ok = detail::loadPsd(data, size, out, li, error); break;
	case Format::Clip: ok = detail::loadClip(data, size, out, li, error); break;
	case Format::Tlg5:
	case Format::Tlg6: {
		tlg::Tags tags;
		bool alpha = false;
		ok = tlg::decodeTlg(data, size, out.width, out.height, out.bgra, &alpha, &tags, error);
		if (ok) {
			li.hasAlpha = alpha;
			out.tags.assign(tags.begin(), tags.end());
			if (!alpha) makeOpaque(out);
		}
		break;
	}
	default:
		error = "対応していない画像形式です";
		return false;
	}
	if (ok && (out.width <= 0 || out.height <= 0 || out.bgra.size() != (size_t)out.width * out.height * 4)) {
		error = "画像を読めませんでした";
		ok = false;
	}
	return ok;
}

bool load(const fs::path& path, Image& out, std::string& error, LoadInfo* info)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) { error = "ファイルを開けません"; return false; }
	std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (buf.empty()) { error = "ファイルが空です"; return false; }
	return loadMemory(buf.data(), buf.size(), out, error, info);
}

bool saveMemory(std::vector<uint8_t>& out, const Image& img, Format format, const SaveOptions& opt, std::string& error)
{
	out.clear();
	if (img.empty() || img.bgra.size() != (size_t)img.width * img.height * 4) { error = "画像が空です"; return false; }
	switch (format) {
	case Format::Bmp:  return detail::saveBmp(out, img, opt, error);
	case Format::Png:  return detail::savePng(out, img, opt, error);
	case Format::Jpeg: return detail::saveJpeg(out, img, opt, error);
	case Format::Tlg5:
	case Format::Tlg6: {
		tlg::Tags tags(img.tags.begin(), img.tags.end());
		return format == Format::Tlg5
			? tlg::encodeTlg5(img.bgra.data(), img.width, img.height, opt.withAlpha, tags, out, error)
			: tlg::encodeTlg6(img.bgra.data(), img.width, img.height, opt.withAlpha, tags, out, error);
	}
	default:
		error = "この形式では書き出せません";
		return false;
	}
}

bool save(const fs::path& path, const Image& img, Format format, const SaveOptions& opt, std::string& error)
{
	std::vector<uint8_t> buf;
	if (!saveMemory(buf, img, format, opt, error)) return false;
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) { error = "書き込めません"; return false; }
	f.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
	if (!f) { error = "書き込めません"; return false; }
	return true;
}

//---------------------------------------------------------------------------
// 前処理
//---------------------------------------------------------------------------

bool hasTransparency(const Image& img)
{
	for (size_t i = 3; i < img.bgra.size(); i += 4)
		if (img.bgra[i] != 0xff) return true;
	return false;
}

void makeOpaque(Image& img)
{
	for (size_t i = 3; i < img.bgra.size(); i += 4) img.bgra[i] = 0xff;
}

void clearTransparentColor(Image& img)
{
	for (size_t i = 0; i < img.bgra.size(); i += 4)
		if (img.bgra[i + 3] == 0) img.bgra[i] = img.bgra[i + 1] = img.bgra[i + 2] = 0;
}

void expandOpaqueColor(Image& img, int n)
{
	// 旧版 TPCMainUnit.cpp の ExpandOpaqueColor と同じ。距離は二乗距離で、
	// 探す範囲は (2n+1) 四方。同じ距離の画素が複数あれば平均 (切り捨て)。
	// 埋めた画素は «不透明» とは見なさない (元画像で判定する)
	if (n <= 0) { clearTransparentColor(img); return; }
	const int w = img.width, h = img.height;
	const std::vector<uint8_t> src = img.bgra;
	const int ndStart = n * n * 2;
	for (int y = 0; y < h; ++y) {
		const int y0 = std::max(0, y - n), y1 = std::min(h - 1, y + n);
		for (int x = 0; x < w; ++x) {
			uint8_t* d = img.bgra.data() + ((size_t)y * w + x) * 4;
			if (src[((size_t)y * w + x) * 4 + 3] != 0) continue;
			int nearest = ndStart, npix = 0, r = 0, g = 0, b = 0;
			const int x0 = std::max(0, x - n), x1 = std::min(w - 1, x + n);
			for (int yy = y0; yy <= y1; ++yy) {
				const int dy = (yy - y) * (yy - y);
				if (dy > nearest) continue;
				const uint8_t* line = src.data() + (size_t)yy * w * 4;
				for (int xx = x0; xx <= x1; ++xx) {
					const uint8_t* p = line + (size_t)xx * 4;
					if (!p[3]) continue;
					const int dist = (xx - x) * (xx - x) + dy;
					if (nearest > dist) {
						npix = 1; b = p[0]; g = p[1]; r = p[2]; nearest = dist;
					} else if (nearest == dist) {
						++npix; b += p[0]; g += p[1]; r += p[2];
					}
				}
			}
			if (nearest != ndStart) {
				d[0] = (uint8_t)(b / npix); d[1] = (uint8_t)(g / npix); d[2] = (uint8_t)(r / npix);
			} else {
				d[0] = d[1] = d[2] = 0;
			}
			d[3] = 0;
		}
	}
}

void toAddAlpha(Image& img)
{
	for (size_t i = 0; i < img.bgra.size(); i += 4) {
		const int a = img.bgra[i + 3];
		for (int c = 0; c < 3; ++c) img.bgra[i + c] = (uint8_t)(img.bgra[i + c] * a / 255);
	}
}

bool bindMask(Image& main, const Image& mask, std::string& error)
{
	if (main.width != mask.width || main.height != mask.height) {
		error = "メイン画像とマスク画像のサイズが違います";
		return false;
	}
	for (size_t i = 0; i < main.bgra.size(); i += 4) main.bgra[i + 3] = grayOf(&mask.bgra[i]);
	return true;
}

Image extractMask(const Image& img)
{
	Image m;
	m.width = img.width;
	m.height = img.height;
	m.bgra.resize(img.bgra.size());
	for (size_t i = 0; i < img.bgra.size(); i += 4) {
		const uint8_t a = img.bgra[i + 3];
		m.bgra[i] = m.bgra[i + 1] = m.bgra[i + 2] = a;
		m.bgra[i + 3] = 0xff;
	}
	return m;
}

fs::path findMaskFile(const fs::path& mainFile)
{
	fs::path base = mainFile;
	base.replace_extension();
	base += "_m";
	std::error_code ec;
	for (const char* ext : { ".bmp", ".png", ".jpg", ".jpeg" }) {
		fs::path p = base;
		p += ext;
		if (fs::exists(p, ec)) return p;
	}
	return {};
}

} // namespace krt::image
