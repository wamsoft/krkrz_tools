//---------------------------------------------------------------------------
// TLG5 エンコーダ
//
// 移植元: 本体 SaveTLG5.cpp の Compress() / SaveTLG5()
// 本体との差分:
//   - tTVPBaseBitmap の代わりに BGRA top-down の生バッファを受け取る
//     (本体の GetScanLine(y) も上から y 行目を返すので並びは同じ)
//   - iTJSBinaryStream の代わりに std::vector へ追記する。ブロックサイズ表は
//     本体同様に仮書き → 後から書き戻す
//   - 32bpp 前提 (本体の colors==1 = 8bpp 経路は対象外)
//---------------------------------------------------------------------------
#include "TlgInternal.h"

#include <memory>

namespace krt {
namespace tlg {
namespace detail {

static constexpr int TLG5_BLOCK_HEIGHT = 4; // SaveTLG5.cpp BLOCK_HEIGHT

void saveTlg5Raw(const uint8_t* bgra, int width, int height, bool is24, std::vector<uint8_t>& out) {
	const int colors = is24 ? 3 : 4;
	const size_t pitch = (size_t)width * 4;

	// header
	writeBytes(out, "TLG5.0\x00raw\x1a\x00", 11);
	out.push_back((uint8_t)colors);
	writeInt32(out, width);
	writeInt32(out, height);
	writeInt32(out, TLG5_BLOCK_HEIGHT);

	const int blockcount = (int)((height - 1) / TLG5_BLOCK_HEIGHT) + 1;

	// buffers/compressors
	// 圧縮器は全ブロック・全色で 1 つを使い回す (辞書を引き継ぐ)。本体と同じ
	auto compressor = std::make_unique<SlideCompressor>();
	std::vector<unsigned char> cmpinbuf[4];
	std::vector<unsigned char> cmpoutbuf[4];
	for (int i = 0; i < colors; i++) {
		cmpinbuf[i].resize((size_t)width * TLG5_BLOCK_HEIGHT);
		cmpoutbuf[i].resize((size_t)width * TLG5_BLOCK_HEIGHT * 9 / 4 + 64);
	}
	std::vector<int> blocksizes(blockcount);

	// write block size header (later fill this)
	const size_t blocksizepos = out.size();
	for (int i = 0; i < blockcount; i++) writeBytes(out, "    ", 4);

	int block = 0;
	for (int blk_y = 0; blk_y < height; blk_y += TLG5_BLOCK_HEIGHT, block++) {
		int ylim = blk_y + TLG5_BLOCK_HEIGHT;
		if (ylim > height) ylim = height;

		int inp = 0;

		for (int y = blk_y; y < ylim; y++) {
			// retrieve scan lines
			const unsigned char* upper = (y != 0) ? bgra + pitch * (y - 1) : nullptr;
			const unsigned char* current = bgra + pitch * y;

			// prepare buffer
			int prevcl[4];
			int val[4];

			for (int c = 0; c < colors; c++) prevcl[c] = 0;

			for (int x = 0; x < width; x++) {
				for (int c = 0; c < colors; c++) {
					int cl;
					if (upper)
						cl = *current++ - *upper++;
					else
						cl = *current++;
					val[c] = cl - prevcl[c];
					prevcl[c] = cl;
				}
				// composite colors
				switch (colors) {
				case 3:
					cmpinbuf[0][inp] = (unsigned char)(val[0] - val[1]);
					cmpinbuf[1][inp] = (unsigned char)(val[1]);
					cmpinbuf[2][inp] = (unsigned char)(val[2] - val[1]);
					// skip alpha
					current++;
					if (upper) upper++;
					break;
				case 4:
					cmpinbuf[0][inp] = (unsigned char)(val[0] - val[1]);
					cmpinbuf[1][inp] = (unsigned char)(val[1]);
					cmpinbuf[2][inp] = (unsigned char)(val[2] - val[1]);
					cmpinbuf[3][inp] = (unsigned char)(val[3]);
					break;
				}

				inp++;
			}
		}

		// compress buffer and write to the file

		// LZSS
		int blocksize = 0;
		for (int c = 0; c < colors; c++) {
			long wrote = 0;
			compressor->Store();
			compressor->Encode(cmpinbuf[c].data(), inp, cmpoutbuf[c].data(), wrote);
			if (wrote < inp) {
				out.push_back(0x00);
				writeInt32(out, wrote);
				writeBytes(out, cmpoutbuf[c].data(), (size_t)wrote);
				blocksize += wrote + 4 + 1;
			} else {
				// 圧縮で縮まなければ辞書を巻き戻して生データで書く
				compressor->Restore();
				out.push_back(0x01);
				writeInt32(out, inp);
				writeBytes(out, cmpinbuf[c].data(), (size_t)inp);
				blocksize += inp + 4 + 1;
			}
		}

		blocksizes[block] = blocksize;
	}

	// write block sizes
	for (int i = 0; i < blockcount; i++) patchInt32(out, blocksizepos + (size_t)i * 4, (uint32_t)blocksizes[i]);
}

} // namespace detail
} // namespace tlg
} // namespace krt
