// PNG の読み書き (libpng)
//
// タグは本体 LoadPNG.cpp と同じ名前で出し入れする:
//   oFFs → offs_x / offs_y / offs_unit (pixel / micrometer)
//   vpAg → vpag_w / vpag_h / vpag_unit、caNv → vpag_* と offs_* (pixel)
//   pHYs → reso_x / reso_y / reso_unit (meter)
// 書き出しでは offs_* を oFFs、vpag_* を vpAg、reso_* を pHYs として書く (旧版と同じく
// vpAg は私的チャンク)。mode などその他のタグは PNG には書かない。
#include "Codecs.h"

#include <png.h>
#include <cstring>
#include <stdexcept>

namespace krt::image::detail {

namespace {

struct ReadSrc { const uint8_t* p; size_t n; size_t pos; };

void readFn(png_structp png, png_bytep out, png_size_t len)
{
	auto* s = static_cast<ReadSrc*>(png_get_io_ptr(png));
	if (s->pos + len > s->n) png_error(png, "PNG のデータが足りません");
	std::memcpy(out, s->p + s->pos, len);
	s->pos += len;
}

void errorFn(png_structp, png_const_charp msg) { throw std::runtime_error(msg); }
void warnFn(png_structp, png_const_charp) {}

uint32_t be32(const uint8_t* a) { return ((uint32_t)a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3]; }

const char* unitName(int u) { return u == PNG_OFFSET_PIXEL ? "pixel" : u == PNG_OFFSET_MICROMETER ? "micrometer" : "unknown"; }

int chunkFn(png_structp png, png_unknown_chunkp c)
{
	auto* tags = static_cast<Tags*>(png_get_user_chunk_ptr(png));
	auto is = [&](const char* nm) {
		for (int i = 0; i < 4; ++i) if ((c->name[i] | 0x20) != (nm[i] | 0x20)) return false;
		return true;
	};
	if (is("vpag") && c->size >= 9) {
		setTag(*tags, "vpag_w", std::to_string(be32(c->data)));
		setTag(*tags, "vpag_h", std::to_string(be32(c->data + 4)));
		setTag(*tags, "vpag_unit", unitName(c->data[8]));
		return 1;
	}
	if (std::memcmp(c->name, "caNv", 4) == 0 && c->size >= 16) {
		setTag(*tags, "vpag_w", std::to_string(be32(c->data)));
		setTag(*tags, "vpag_h", std::to_string(be32(c->data + 4)));
		setTag(*tags, "offs_x", std::to_string((int32_t)be32(c->data + 8)));
		setTag(*tags, "offs_y", std::to_string((int32_t)be32(c->data + 12)));
		setTag(*tags, "vpag_unit", "pixel");
		return 1;
	}
	return 0;
}

void writeFn(png_structp png, png_bytep data, png_size_t len)
{
	auto* v = static_cast<std::vector<uint8_t>*>(png_get_io_ptr(png));
	v->insert(v->end(), data, data + len);
}
void flushFn(png_structp) {}

bool toInt(const std::string& s, long long& v)
{
	if (s.empty()) return false;
	char* e;
	v = std::strtoll(s.c_str(), &e, 10);
	return *e == 0;
}

} // namespace

bool loadPng(const uint8_t* data, size_t size, Image& out, LoadInfo& info, std::string& error)
{
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, errorFn, warnFn);
	if (!png) { error = "PNG を読めません"; return false; }
	png_infop pi = png_create_info_struct(png);
	ReadSrc src{ data, size, 0 };
	std::vector<png_bytep> rows;
	try {
		png_set_read_fn(png, &src, readFn);
		png_set_read_user_chunk_fn(png, &out.tags, chunkFn);
		png_set_keep_unknown_chunks(png, PNG_HANDLE_CHUNK_ALWAYS, nullptr, 0);
		png_read_info(png, pi);
		png_uint_32 w, h;
		int depth, ctype, interlace;
		png_get_IHDR(png, pi, &w, &h, &depth, &ctype, &interlace, nullptr, nullptr);
		if (w == 0 || h == 0 || w > 65535 || h > 65535) throw std::runtime_error("PNG の大きさが不正です");

		png_int_32 ox, oy;
		int ounit;
		if (png_get_oFFs(png, pi, &ox, &oy, &ounit)) {
			setTag(out.tags, "offs_x", std::to_string(ox));
			setTag(out.tags, "offs_y", std::to_string(oy));
			setTag(out.tags, "offs_unit", unitName(ounit));
		}
		png_uint_32 rx, ry;
		int runit;
		if (png_get_pHYs(png, pi, &rx, &ry, &runit)) {
			setTag(out.tags, "reso_x", std::to_string(rx));
			setTag(out.tags, "reso_y", std::to_string(ry));
			setTag(out.tags, "reso_unit", runit == PNG_RESOLUTION_METER ? "meter" : "unknown");
		}

		info.hasAlpha = (ctype & PNG_COLOR_MASK_ALPHA) || png_get_valid(png, pi, PNG_INFO_tRNS);
		info.grayscale = !(ctype & PNG_COLOR_MASK_COLOR);
		if (depth == 16) png_set_strip_16(png);
		if (ctype == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
		if (!(ctype & PNG_COLOR_MASK_COLOR) && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
		if (png_get_valid(png, pi, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
		if (!(ctype & PNG_COLOR_MASK_COLOR)) png_set_gray_to_rgb(png);
		png_set_bgr(png);
		png_set_filler(png, 0xff, PNG_FILLER_AFTER);
		png_set_interlace_handling(png);
		png_read_update_info(png, pi);

		out.width = (int)w;
		out.height = (int)h;
		out.bgra.assign((size_t)w * h * 4, 0);
		rows.resize(h);
		for (png_uint_32 y = 0; y < h; ++y) rows[y] = out.row((int)y);
		png_read_image(png, rows.data());
		png_read_end(png, nullptr);
	} catch (const std::exception& e) {
		png_destroy_read_struct(&png, &pi, nullptr);
		error = std::string("PNG を読めません: ") + e.what();
		return false;
	}
	png_destroy_read_struct(&png, &pi, nullptr);
	return true;
}

bool savePng(std::vector<uint8_t>& out, const Image& img, const SaveOptions& opt, std::string& error)
{
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, errorFn, warnFn);
	if (!png) { error = "PNG を書けません"; return false; }
	png_infop pi = png_create_info_struct(png);
	out.clear();
	try {
		png_set_write_fn(png, &out, writeFn, flushFn);
		png_set_compression_level(png, opt.pngLevel);
		const int ctype = opt.grayscale ? PNG_COLOR_TYPE_GRAY : opt.withAlpha ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB;
		png_set_IHDR(png, pi, (png_uint_32)img.width, (png_uint_32)img.height, 8, ctype,
		             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

		long long x, y;
		const std::string ou = tagValue(img.tags, "offs_unit");
		if (toInt(tagValue(img.tags, "offs_x"), x) && toInt(tagValue(img.tags, "offs_y"), y) && (ou == "pixel" || ou == "micrometer"))
			png_set_oFFs(png, pi, (png_int_32)x, (png_int_32)y, ou == "pixel" ? PNG_OFFSET_PIXEL : PNG_OFFSET_MICROMETER);
		if (toInt(tagValue(img.tags, "reso_x"), x) && toInt(tagValue(img.tags, "reso_y"), y) && x > 0 && y > 0)
			png_set_pHYs(png, pi, (png_uint_32)x, (png_uint_32)y,
			             tagValue(img.tags, "reso_unit") == "meter" ? PNG_RESOLUTION_METER : PNG_RESOLUTION_UNKNOWN);
		png_write_info(png, pi);

		const std::string vu = tagValue(img.tags, "vpag_unit");
		if (toInt(tagValue(img.tags, "vpag_w"), x) && toInt(tagValue(img.tags, "vpag_h"), y) && (vu == "pixel" || vu == "micrometer")) {
			png_byte name[5] = { 'v', 'p', 'A', 'g', 0 };
			uint8_t d[9];
			for (int i = 0; i < 4; ++i) { d[i] = (uint8_t)(x >> (24 - 8 * i)); d[4 + i] = (uint8_t)(y >> (24 - 8 * i)); }
			d[8] = vu == "pixel" ? PNG_OFFSET_PIXEL : PNG_OFFSET_MICROMETER;
			png_write_chunk(png, name, d, 9);
		}

		const int ch = opt.grayscale ? 1 : opt.withAlpha ? 4 : 3;
		std::vector<uint8_t> line((size_t)img.width * ch);
		for (int yy = 0; yy < img.height; ++yy) {
			const uint8_t* s = img.row(yy);
			uint8_t* d = line.data();
			for (int xx = 0; xx < img.width; ++xx, s += 4) {
				if (ch == 1) { *d++ = grayOf(s); continue; }
				*d++ = s[2]; *d++ = s[1]; *d++ = s[0];
				if (ch == 4) *d++ = s[3];
			}
			png_write_row(png, line.data());
		}
		png_write_end(png, pi);
	} catch (const std::exception& e) {
		png_destroy_write_struct(&png, &pi);
		error = std::string("PNG を書けません: ") + e.what();
		return false;
	}
	png_destroy_write_struct(&png, &pi);
	return true;
}

} // namespace krt::image::detail
