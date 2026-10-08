// BMP の読み書き (自前。Windows の BITMAPINFOHEADER 系)
//
// 読み: 1 / 4 / 8 bit (パレット)、16 bit (555 / BITFIELDS)、24 bit、32 bit (BI_RGB / BITFIELDS)。
//       RLE 圧縮は扱わない。32 bit は透明度付きとみなす (旧版と同じ) が、
//       アルファが全部 0 のものは «アルファを使っていない» 32 bit とみなして不透明にする。
// 書き: 24 bit (不透明) / 32 bit (透明度付き) / 8 bit グレイスケール。下から上へ格納。
#include "Codecs.h"

#include <algorithm>
#include <cstring>

namespace krt::image::detail {

namespace {

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xff); v.push_back(x >> 8); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xff); }

// マスクから 8bit 値を取り出す
struct Field {
	uint32_t mask = 0;
	int shift = 0, bits = 0;
	explicit Field(uint32_t m = 0) : mask(m)
	{
		if (!m) return;
		while (!(m & 1)) { m >>= 1; ++shift; }
		while (m & 1) { m >>= 1; ++bits; }
	}
	uint8_t get(uint32_t v) const
	{
		if (!mask) return 0;
		const uint32_t x = (v & mask) >> shift;
		const uint32_t maxv = (1u << bits) - 1;
		return (uint8_t)((x * 255 + maxv / 2) / maxv);
	}
};

} // namespace

bool loadBmp(const uint8_t* d, size_t n, Image& out, LoadInfo& info, std::string& error)
{
	if (n < 14 + 12) { error = "BMP が壊れています"; return false; }
	const uint32_t dataOfs = le32(d + 10);
	const uint8_t* ih = d + 14;
	const uint32_t ihSize = le32(ih);
	int32_t w, h;
	int bpp;
	uint32_t comp = 0, clrUsed = 0;
	uint32_t masks[4] = { 0, 0, 0, 0 };
	size_t palOfs, palEntry;
	if (ihSize == 12) {   // OS/2 BITMAPCOREHEADER
		w = le16(ih + 4); h = (int16_t)le16(ih + 6); bpp = le16(ih + 10);
		palOfs = 14 + 12; palEntry = 3;
	} else {
		if (ihSize < 40 || n < 14 + (size_t)ihSize) { error = "BMP が壊れています"; return false; }
		w = (int32_t)le32(ih + 4); h = (int32_t)le32(ih + 8); bpp = le16(ih + 14);
		comp = le32(ih + 16); clrUsed = le32(ih + 32);
		palOfs = 14 + ihSize; palEntry = 4;
		if (comp == 3 || comp == 6) {  // BI_BITFIELDS / BI_ALPHABITFIELDS
			const uint8_t* mp = ihSize >= 52 ? ih + 40 : d + 14 + 40;
			if (mp + 12 > d + n) { error = "BMP が壊れています"; return false; }
			masks[0] = le32(mp); masks[1] = le32(mp + 4); masks[2] = le32(mp + 8);
			if (ihSize >= 56 || comp == 6) masks[3] = le32(mp + 12);
			if (ihSize == 40) palOfs += comp == 6 ? 16 : 12;
		} else if (comp != 0) {
			error = "圧縮された BMP には対応していません";
			return false;
		}
	}
	const bool topDown = h < 0;
	if (h < 0) h = -h;
	if (w <= 0 || h <= 0 || w > 65535 || h > 65535) { error = "BMP の大きさが不正です"; return false; }
	if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) { error = "対応していない BMP のビット数です"; return false; }

	const size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
	if ((size_t)dataOfs + stride * h > n) { error = "BMP のデータが足りません"; return false; }

	// パレット
	uint8_t pal[256][4] = {};
	bool grayPal = bpp <= 8;
	if (bpp <= 8) {
		const size_t count = clrUsed ? std::min<size_t>(clrUsed, 256) : (size_t)1 << bpp;
		if (palOfs + count * palEntry > n) { error = "BMP が壊れています"; return false; }
		for (size_t i = 0; i < count; ++i) {
			const uint8_t* p = d + palOfs + i * palEntry;
			pal[i][0] = p[0]; pal[i][1] = p[1]; pal[i][2] = p[2]; pal[i][3] = 0xff;
			if (p[0] != p[1] || p[1] != p[2]) grayPal = false;
		}
	}
	if (bpp == 16 && !masks[0]) { masks[0] = 0x7c00; masks[1] = 0x03e0; masks[2] = 0x001f; }
	if (bpp == 32 && !masks[0]) { masks[0] = 0xff0000; masks[1] = 0xff00; masks[2] = 0xff; masks[3] = 0xff000000; }
	const Field fr(masks[0]), fg(masks[1]), fb(masks[2]), fa(masks[3]);

	out.width = w;
	out.height = h;
	out.bgra.assign((size_t)w * h * 4, 0xff);
	for (int y = 0; y < h; ++y) {
		const uint8_t* s = d + dataOfs + stride * (topDown ? y : h - 1 - y);
		uint8_t* o = out.row(y);
		for (int x = 0; x < w; ++x, o += 4) {
			switch (bpp) {
			case 1: case 4: case 8: {
				const int idx = bpp == 8 ? s[x] : bpp == 4 ? (s[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xf : (s[x >> 3] >> (7 - (x & 7))) & 1;
				std::memcpy(o, pal[idx], 4);
				break;
			}
			case 16: {
				const uint32_t v = le16(s + x * 2);
				o[0] = fb.get(v); o[1] = fg.get(v); o[2] = fr.get(v); o[3] = fa.mask ? fa.get(v) : 0xff;
				break;
			}
			case 24:
				o[0] = s[x * 3]; o[1] = s[x * 3 + 1]; o[2] = s[x * 3 + 2];
				break;
			case 32: {
				const uint32_t v = le32(s + x * 4);
				o[0] = fb.get(v); o[1] = fg.get(v); o[2] = fr.get(v); o[3] = fa.mask ? fa.get(v) : 0xff;
				break;
			}
			}
		}
	}
	info.grayscale = grayPal;
	info.hasAlpha = (bpp == 32 || bpp == 16) && fa.mask;
	if (info.hasAlpha) {
		// アルファを使っていない 32 bit BMP (全部 0) は不透明として扱う
		bool allZero = true;
		for (size_t i = 3; i < out.bgra.size() && allZero; i += 4) allZero = out.bgra[i] == 0;
		if (allZero) {
			info.hasAlpha = false;
			for (size_t i = 3; i < out.bgra.size(); i += 4) out.bgra[i] = 0xff;
		}
	}
	return true;
}

bool saveBmp(std::vector<uint8_t>& out, const Image& img, const SaveOptions& opt, std::string&)
{
	const int w = img.width, h = img.height;
	const int bpp = opt.grayscale ? 8 : opt.withAlpha ? 32 : 24;
	const size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
	const uint32_t palSize = bpp == 8 ? 256 * 4 : 0;
	const uint32_t dataOfs = 14 + 40 + palSize;
	const size_t total = dataOfs + stride * h;
	out.clear();
	out.reserve(total);
	out.push_back('B'); out.push_back('M');
	put32(out, (uint32_t)total);
	put32(out, 0);
	put32(out, dataOfs);
	put32(out, 40);
	put32(out, (uint32_t)w);
	put32(out, (uint32_t)h);
	put16(out, 1);
	put16(out, (uint16_t)bpp);
	put32(out, 0);                         // BI_RGB
	put32(out, (uint32_t)(stride * h));
	put32(out, 3780); put32(out, 3780);    // 96 dpi
	put32(out, bpp == 8 ? 256 : 0);
	put32(out, 0);
	if (bpp == 8)
		for (int i = 0; i < 256; ++i) { out.push_back((uint8_t)i); out.push_back((uint8_t)i); out.push_back((uint8_t)i); out.push_back(0); }
	std::vector<uint8_t> line(stride, 0);   // 詰め物は 0
	for (int y = h - 1; y >= 0; --y) {
		const uint8_t* s = img.row(y);
		for (int x = 0; x < w; ++x, s += 4) {
			if (bpp == 8) line[x] = grayOf(s);
			else if (bpp == 24) { line[x * 3] = s[0]; line[x * 3 + 1] = s[1]; line[x * 3 + 2] = s[2]; }
			else std::memcpy(&line[x * 4], s, 4);
		}
		out.insert(out.end(), line.begin(), line.end());
	}
	return true;
}

} // namespace krt::image::detail
