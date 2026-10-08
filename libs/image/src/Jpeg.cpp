// JPEG の読み書き (libjpeg-turbo)
//
// 読み: フルカラー / グレイスケール / CMYK (Adobe の反転 CMYK を含む)。
// 書き: フルカラー / グレイスケール (マスク画像用)。透明度は持てない。
#include "Codecs.h"

#include <cstdio>
#include <csetjmp>
#include <jpeglib.h>

namespace krt::image::detail {

namespace {

struct ErrMgr {
	jpeg_error_mgr pub;
	std::jmp_buf jump;
	char message[JMSG_LENGTH_MAX];
};

void onError(j_common_ptr c)
{
	auto* e = reinterpret_cast<ErrMgr*>(c->err);
	(*c->err->format_message)(c, e->message);
	std::longjmp(e->jump, 1);
}

} // namespace

bool loadJpeg(const uint8_t* data, size_t size, Image& out, LoadInfo& info, std::string& error)
{
	jpeg_decompress_struct cinfo;
	ErrMgr err;
	cinfo.err = jpeg_std_error(&err.pub);
	err.pub.error_exit = onError;
	if (setjmp(err.jump)) {
		jpeg_destroy_decompress(&cinfo);
		error = std::string("JPEG を読めません: ") + err.message;
		return false;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, data, (unsigned long)size);
	jpeg_read_header(&cinfo, TRUE);
	const bool cmyk = cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK;
	info.grayscale = cinfo.jpeg_color_space == JCS_GRAYSCALE;
	if (cmyk) cinfo.out_color_space = JCS_CMYK;
	else cinfo.out_color_space = JCS_EXT_BGRA;
	cinfo.dct_method = JDCT_ISLOW;
	jpeg_start_decompress(&cinfo);
	out.width = (int)cinfo.output_width;
	out.height = (int)cinfo.output_height;
	out.bgra.assign((size_t)out.width * out.height * 4, 0xff);
	std::vector<uint8_t> line((size_t)out.width * 4);
	// Adobe の CMYK は値が反転している (saw_Adobe_marker)
	const bool inverted = cmyk && cinfo.saw_Adobe_marker;
	while (cinfo.output_scanline < cinfo.output_height) {
		uint8_t* dst = out.row((int)cinfo.output_scanline);
		JSAMPROW row = cmyk ? line.data() : dst;
		jpeg_read_scanlines(&cinfo, &row, 1);
		if (cmyk) {
			for (int x = 0; x < out.width; ++x) {
				int c = line[x * 4], m = line[x * 4 + 1], y = line[x * 4 + 2], k = line[x * 4 + 3];
				if (!inverted) { c = 255 - c; m = 255 - m; y = 255 - y; k = 255 - k; }
				dst[x * 4]     = (uint8_t)(y * k / 255);
				dst[x * 4 + 1] = (uint8_t)(m * k / 255);
				dst[x * 4 + 2] = (uint8_t)(c * k / 255);
				dst[x * 4 + 3] = 0xff;
			}
		}
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	for (size_t i = 3; i < out.bgra.size(); i += 4) out.bgra[i] = 0xff;
	return true;
}

bool saveJpeg(std::vector<uint8_t>& out, const Image& img, const SaveOptions& opt, std::string& error)
{
	jpeg_compress_struct cinfo;
	ErrMgr err;
	cinfo.err = jpeg_std_error(&err.pub);
	err.pub.error_exit = onError;
	unsigned char* mem = nullptr;
	unsigned long memSize = 0;
	std::vector<uint8_t> line;
	if (setjmp(err.jump)) {
		jpeg_destroy_compress(&cinfo);
		if (mem) std::free(mem);
		error = std::string("JPEG を書けません: ") + err.message;
		return false;
	}
	jpeg_create_compress(&cinfo);
	jpeg_mem_dest(&cinfo, &mem, &memSize);
	cinfo.image_width = (JDIMENSION)img.width;
	cinfo.image_height = (JDIMENSION)img.height;
	if (opt.grayscale) {
		cinfo.input_components = 1;
		cinfo.in_color_space = JCS_GRAYSCALE;
	} else {
		cinfo.input_components = 4;
		cinfo.in_color_space = JCS_EXT_BGRX;
	}
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, opt.jpegQuality < 1 ? 1 : opt.jpegQuality > 100 ? 100 : opt.jpegQuality, TRUE);
	cinfo.optimize_coding = TRUE;
	jpeg_start_compress(&cinfo, TRUE);
	line.resize((size_t)img.width);
	while (cinfo.next_scanline < cinfo.image_height) {
		const uint8_t* src = img.row((int)cinfo.next_scanline);
		JSAMPROW row;
		if (opt.grayscale) {
			for (int x = 0; x < img.width; ++x) line[x] = grayOf(src + x * 4);
			row = line.data();
		} else {
			row = const_cast<JSAMPROW>(src);
		}
		jpeg_write_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_compress(&cinfo);
	out.assign(mem, mem + memSize);
	jpeg_destroy_compress(&cinfo);
	std::free(mem);
	return true;
}

} // namespace krt::image::detail
