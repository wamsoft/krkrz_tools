//---------------------------------------------------------------------------
// TLG5 / TLG6 デコーダ
//
// 移植元:
//   decodeTlg5      … 本体 LoadTLG.cpp TVPLoadTLG5
//   decodeTlg6      … 本体 LoadTLG.cpp TVPLoadTLG6
//   composeColors*  … 本体 tvpgl.c TVPTLG5ComposeColors3To4_c / 4To4_c
//   decodeGolomb*   … 本体 tvpgl.c TVPTLG6DecodeGolombValuesForFirst_c /
//                     TVPTLG6DecodeGolombValues_c
//   decodeLine*     … 本体 tvpgl.c TVPTLG6DecodeLineGeneric_c / TVPTLG6DecodeLine_c
//                     (med / avg / packed_bytes_add などのパック演算も同ファイル)
//   decodeTlg 本体  … 本体 LoadTLG.cpp TVPLoadTLG / TVPInternalLoadTLG
//                     ("TLG0.0 sds" ラッパーと "tags" チャンクの解析)
//
// 本体との差分:
//   - スキャンラインコールバックの代わりに BGRA top-down の vector へ出力
//   - 読み込みモードは glmNormal 相当のみ (BGRA)。RGBA 変換・ルール画像は対象外
//   - 不正データで範囲外アクセスしないよう、サイズ検査とバッファの余白を追加
//   - TLG6 の colors==1 (グレースケール) は B を G/R へ複製し A=0xff で返す
//     (本体は 32bpp に B だけ展開する)
//---------------------------------------------------------------------------
#include "TlgInternal.h"

#include "krt/tlg/Tlg.h"

#include <cstring>
#include <algorithm>
#include <limits>

namespace krt {
namespace tlg {
namespace detail {

namespace {

//---------------------------------------------------------------------------
// 入力読み取り (境界検査付き)
//---------------------------------------------------------------------------
class Reader {
	const uint8_t* data_;
	size_t size_;
	size_t pos_ = 0;

public:
	Reader(const uint8_t* d, size_t s) : data_(d), size_(s) {}
	size_t pos() const { return pos_; }
	size_t size() const { return size_; }
	size_t remain() const { return size_ - pos_; }
	void seek(size_t p) {
		if (p > size_) throw TlgError("TLG: unexpected end of data");
		pos_ = p;
	}
	void read(void* dst, size_t n) {
		if (n > remain()) throw TlgError("TLG: unexpected end of data");
		std::memcpy(dst, data_ + pos_, n);
		pos_ += n;
	}
	const uint8_t* ptr(size_t n) {
		if (n > remain()) throw TlgError("TLG: unexpected end of data");
		const uint8_t* p = data_ + pos_;
		pos_ += n;
		return p;
	}
	uint8_t u8() {
		uint8_t v;
		read(&v, 1);
		return v;
	}
	uint32_t u32() {
		uint8_t b[4];
		read(b, 4);
		return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	}
	int32_t i32() { return (int32_t)u32(); }
};

// 画像サイズの妥当性 (幅・高さとも正、総ピクセルが過大でない)
void checkSize(int32_t width, int32_t height) {
	if (width <= 0 || height <= 0) throw TlgError("TLG: invalid image size");
	if ((uint64_t)width * (uint64_t)height > (uint64_t)(1u << 30)) throw TlgError("TLG: image too large");
}

//---------------------------------------------------------------------------
// TLG5 色合成 (tvpgl.c TVPTLG5ComposeColors3To4_c / 4To4_c)
//---------------------------------------------------------------------------
void composeColors3To4(uint8_t* outp, const uint8_t* upper, uint8_t* const* buf, int width) {
	uint8_t pc[3] = {0, 0, 0};
	uint8_t c[3];
	for (int x = 0; x < width; x++) {
		c[0] = buf[0][x];
		c[1] = buf[1][x];
		c[2] = buf[2][x];
		c[0] += c[1];
		c[2] += c[1];
		outp[0] = (uint8_t)((pc[0] += c[0]) + upper[0]);
		outp[1] = (uint8_t)((pc[1] += c[1]) + upper[1]);
		outp[2] = (uint8_t)((pc[2] += c[2]) + upper[2]);
		outp[3] = 0xff;
		outp += 4;
		upper += 4;
	}
}
void composeColors4To4(uint8_t* outp, const uint8_t* upper, uint8_t* const* buf, int width) {
	uint8_t pc[4] = {0, 0, 0, 0};
	uint8_t c[4];
	for (int x = 0; x < width; x++) {
		c[0] = buf[0][x];
		c[1] = buf[1][x];
		c[2] = buf[2][x];
		c[3] = buf[3][x];
		c[0] += c[1];
		c[2] += c[1];
		outp[0] = (uint8_t)((pc[0] += c[0]) + upper[0]);
		outp[1] = (uint8_t)((pc[1] += c[1]) + upper[1]);
		outp[2] = (uint8_t)((pc[2] += c[2]) + upper[2]);
		outp[3] = (uint8_t)((pc[3] += c[3]) + upper[3]);
		outp += 4;
		upper += 4;
	}
}

//---------------------------------------------------------------------------
// TLG5 (LoadTLG.cpp TVPLoadTLG5)。r はマーク (11 バイト) の直後を指していること
//---------------------------------------------------------------------------
void decodeTlg5(Reader& r, int& width, int& height, std::vector<uint8_t>& bgra, int& colorsOut) {
	int32_t colors = r.u8();
	int32_t w = r.i32();
	int32_t h = r.i32();
	int32_t blockheight = r.i32();

	if (colors != 3 && colors != 4) throw TlgError("TLG5: unsupported color type");
	checkSize(w, h);
	if (blockheight <= 0) throw TlgError("TLG5: invalid block height");

	int blockcount = (int)((h - 1) / blockheight) + 1;

	// skip block size section
	r.seek(r.pos() + (size_t)blockcount * 4);

	// 不正ヘッダで巨大な確保をしないための概算チェック:
	// LZSS は 25 バイトで最大 8*273 バイトに展開される (約 88 倍) ので、
	// 残りデータ量から展開可能な最大量を超える画像サイズは不正とみなす。
	if ((uint64_t)w * h * colors > (uint64_t)r.remain() * 88 + 64) throw TlgError("TLG5: data is too short for the image size");

	width = w;
	height = h;
	colorsOut = colors;
	const size_t pitch = (size_t)w * 4;
	bgra.assign(pitch * h, 0);

	std::vector<uint8_t> text(4096 + 16, 0);
	const size_t bufsize = (size_t)blockheight * w;
	std::vector<uint8_t> outbuf[4];
	for (int i = 0; i < colors; i++) outbuf[i].assign(bufsize + 16, 0);
	int32_t rpos = 0;

	const uint8_t* prevline = nullptr;
	for (int32_t y_blk = 0; y_blk < h; y_blk += blockheight) {
		// 1 ブロックに必要な量 (最終ブロックは行数が少ない)
		int32_t y_lim = y_blk + blockheight;
		if (y_lim > h) y_lim = h;
		const size_t need = (size_t)(y_lim - y_blk) * w;

		// read file and decompress
		for (int c = 0; c < colors; c++) {
			uint8_t mark = r.u8();
			int32_t size = r.i32();
			if (size < 0) throw TlgError("TLG5: invalid block size");
			const uint8_t* inbuf = r.ptr((size_t)size);
			if (mark == 0) {
				// modified LZSS compressed data
				rpos = tlg5DecompressSlide(outbuf[c].data(), outbuf[c].data() + bufsize, inbuf, size, text.data(), rpos);
			} else {
				// raw data
				if ((size_t)size > bufsize) throw TlgError("TLG5: raw block too large");
				std::memcpy(outbuf[c].data(), inbuf, size);
			}
		}
		(void)need;

		// compose colors and store
		uint8_t* outbufp[4];
		for (int c = 0; c < colors; c++) outbufp[c] = outbuf[c].data();
		for (int32_t y = y_blk; y < y_lim; y++) {
			uint8_t* current = bgra.data() + pitch * y;
			uint8_t* current_org = current;
			if (prevline) {
				// not first line
				if (colors == 3)
					composeColors3To4(current, prevline, outbufp, w);
				else
					composeColors4To4(current, prevline, outbufp, w);
			} else {
				// first line
				int pr = 0, pg = 0, pb = 0, pa = 0;
				for (int x = 0; x < w; x++) {
					int b = outbufp[0][x];
					int g = outbufp[1][x];
					int rr = outbufp[2][x];
					b += g;
					rr += g;
					*current++ = (uint8_t)(pb += b);
					*current++ = (uint8_t)(pg += g);
					*current++ = (uint8_t)(pr += rr);
					if (colors == 3)
						*current++ = 0xff;
					else
						*current++ = (uint8_t)(pa += outbufp[3][x]);
				}
			}
			for (int c = 0; c < colors; c++) outbufp[c] += w;
			prevline = current_org;
		}
	}
}

//---------------------------------------------------------------------------
// TLG6 ゴロム復号 (tvpgl.c TVPTLG6DecodeGolombValues(ForFirst)_c)
// forFirst=true なら 32bit 単位でゼロクリアしつつ最下位バイト (B) に書く。
// false なら pixelbuf の該当バイト (pixelbuf はあらかじめ +c 済み) のみ書く。
// 本体からの差分: pixelbuf の上限と bit_pool の読み越しを検査する。
//---------------------------------------------------------------------------
inline uint32_t fetch32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void decodeGolombValues(int8_t* pixelbuf, int pixel_count, const uint8_t* bit_pool, const uint8_t* bit_pool_limit,
                        bool forFirst) {
	const Tlg6Tables& T = tlg6Tables();
	const int LZBITS = TLG6_LEADING_ZERO_TABLE_BITS;
	const int LZMASK = TLG6_LEADING_ZERO_TABLE_SIZE - 1;

	int n = TLG6_GOLOMB_N_COUNT - 1; // output counter
	int a = 0;                       // summary of absolute values of errors

	int bit_pos = 1;
	uint8_t zero = (*bit_pool & 1) ? 0 : 1;

	int8_t* limit = pixelbuf + (size_t)pixel_count * 4;

	auto check = [&]() {
		if (bit_pool >= bit_pool_limit) throw TlgError("TLG6: bit stream overrun");
	};

	while (pixelbuf < limit) {
		// get running count
		int count;

		{
			uint32_t t = fetch32(bit_pool) >> bit_pos;
			int b = T.leadingZero[t & LZMASK];
			int bit_count = b;
			while (!b) {
				bit_count += LZBITS;
				bit_pos += LZBITS;
				bit_pool += bit_pos >> 3;
				bit_pos &= 7;
				check();
				t = fetch32(bit_pool) >> bit_pos;
				b = T.leadingZero[t & LZMASK];
				bit_count += b;
			}

			bit_pos += b;
			bit_pool += bit_pos >> 3;
			bit_pos &= 7;

			bit_count--;
			if (bit_count > 30) throw TlgError("TLG6: invalid run length");
			count = 1 << bit_count;
			count += ((fetch32(bit_pool) >> (bit_pos)) & (count - 1));

			bit_pos += bit_count;
			bit_pool += bit_pos >> 3;
			bit_pos &= 7;
			check();
		}

		if ((limit - pixelbuf) / 4 < count) throw TlgError("TLG6: run length exceeds block");

		if (zero) {
			// zero values
			// fill distination with zero
			do {
				if (forFirst)
					std::memset(pixelbuf, 0, 4);
				else
					*pixelbuf = 0;
				pixelbuf += 4;
			} while (--count);

			zero ^= 1;
		} else {
			// non-zero values
			// fill distination with glomb code
			do {
				int k = T.golombBitLength[a][n], v, sign;

				uint32_t t = fetch32(bit_pool) >> bit_pos;
				int bit_count;
				int b;
				if (t) {
					b = T.leadingZero[t & LZMASK];
					bit_count = b;
					while (!b) {
						bit_count += LZBITS;
						bit_pos += LZBITS;
						bit_pool += bit_pos >> 3;
						bit_pos &= 7;
						check();
						t = fetch32(bit_pool) >> bit_pos;
						b = T.leadingZero[t & LZMASK];
						bit_count += b;
					}
					bit_count--;
				} else {
					// 符号化側で打ち切られた (GOLOMB_GIVE_UP_BYTES) 値: 次の 8bit に m>>k がある
					bit_pool += 5;
					check();
					bit_count = bit_pool[-1];
					bit_pos = 0;
					t = fetch32(bit_pool);
					b = 0;
				}

				v = (bit_count << k) + ((t >> b) & ((1 << k) - 1));
				sign = (v & 1) - 1;
				v >>= 1;
				a += v;
				if (forFirst) {
					uint32_t u = (unsigned char)((v ^ sign) + sign + 1);
					std::memcpy(pixelbuf, &u, 4); // リトルエンディアン前提 (下位バイト = B)
				} else {
					*pixelbuf = (int8_t)((v ^ sign) + sign + 1);
				}
				pixelbuf += 4;

				bit_pos += b;
				bit_pos += k;
				bit_pool += bit_pos >> 3;
				bit_pos &= 7;
				check();

				if (--n < 0) {
					a >>= 1;
					n = TLG6_GOLOMB_N_COUNT - 1;
				}
				// 不正データで a がテーブル範囲を超えないように
				if (a >= TLG6_GOLOMB_N_COUNT * 2 * 128) throw TlgError("TLG6: invalid golomb data");
			} while (--count);
			zero ^= 1;
		}
	}
}

//---------------------------------------------------------------------------
// TLG6 ライン復号 (tvpgl.c の packed 演算と TVPTLG6DecodeLineGeneric_c)
//---------------------------------------------------------------------------
inline uint32_t make_gt_mask(uint32_t a, uint32_t b) {
	uint32_t tmp2 = ~b;
	uint32_t tmp = ((a & tmp2) + (((a ^ tmp2) >> 1) & 0x7f7f7f7f)) & 0x80808080;
	tmp = ((tmp >> 7) + 0x7f7f7f7f) ^ 0x7f7f7f7f;
	return tmp;
}
inline uint32_t packed_bytes_add(uint32_t a, uint32_t b) {
	uint32_t tmp = (((a & b) << 1) + ((a ^ b) & 0xfefefefe)) & 0x01010100;
	return a + b - tmp;
}
inline uint32_t med2(uint32_t a, uint32_t b, uint32_t c) {
	// do Median Edge Detector   thx, Mr. sugi  at    kirikiri.info
	uint32_t aa_gt_bb = make_gt_mask(a, b);
	uint32_t a_xor_b_and_aa_gt_bb = ((a ^ b) & aa_gt_bb);
	uint32_t aa = a_xor_b_and_aa_gt_bb ^ a;
	uint32_t bb = a_xor_b_and_aa_gt_bb ^ b;
	uint32_t n = make_gt_mask(c, bb);
	uint32_t nn = make_gt_mask(aa, c);
	uint32_t m = ~(n | nn);
	return (n & aa) | (nn & bb) | ((bb & m) - (c & m) + (aa & m));
}
inline uint32_t med(uint32_t a, uint32_t b, uint32_t c, uint32_t v) { return packed_bytes_add(med2(a, b, c), v); }
inline uint32_t avg(uint32_t a, uint32_t b, uint32_t /*c*/, uint32_t v) {
	uint32_t pk = ((a & b) + (((a ^ b) & 0xfefefefe) >> 1)) + ((a ^ b) & 0x01010101); // TLG6_AVG_PACKED
	return packed_bytes_add(pk, v);
}

// 本体の TVP_TLG6_DO_CHROMA_DECODE_PROTO(2) マクロをそのまま移植
#define KRT_TLG6_DO_CHROMA_DECODE_PROTO(FN, B, G, R, A)                                                        \
	do {                                                                                                       \
		uint32_t u = *prevline;                                                                                \
		p = FN(p, u, up,                                                                                       \
		       (0xff0000 & ((uint32_t)(R) << 16)) + (0xff00 & ((uint32_t)(G) << 8)) + (0xff & (uint32_t)(B)) + \
		           ((uint32_t)(A) << 24));                                                                     \
		up = u;                                                                                                \
		*curline = p;                                                                                          \
		curline++;                                                                                             \
		prevline++;                                                                                            \
		in += step;                                                                                            \
	} while (--w);
#define KRT_TLG6_DO_CHROMA_DECODE(N, B, G, R)                    \
	case (N << 1): KRT_TLG6_DO_CHROMA_DECODE_PROTO(med, B, G, R, IA) break; \
	case (N << 1) + 1: KRT_TLG6_DO_CHROMA_DECODE_PROTO(avg, B, G, R, IA) break;

void decodeLineGeneric(const uint32_t* prevline, uint32_t* curline, int width, int start_block, int block_limit,
                       const uint8_t* filtertypes, int skipblockbytes, const uint32_t* in, uint32_t initialp,
                       int oddskip, int dir) {
	// chroma/luminosity decoding
	// (this does reordering, color correlation filter, MED/AVG  at a time)
	uint32_t p, up;
	int step, i;

	if (start_block) {
		prevline += start_block * TLG6_W_BLOCK_SIZE;
		curline += start_block * TLG6_W_BLOCK_SIZE;
		p = curline[-1];
		up = prevline[-1];
	} else {
		p = up = initialp;
	}

	in += skipblockbytes * start_block;
	step = (dir & 1) ? 1 : -1;

	for (i = start_block; i < block_limit; i++) {
		int w = width - i * TLG6_W_BLOCK_SIZE, ww;
		if (w > TLG6_W_BLOCK_SIZE) w = TLG6_W_BLOCK_SIZE;
		ww = w;
		if (step == -1) in += ww - 1;
		if (i & 1) in += oddskip * ww;
		// 本体の IA/IR/IG/IB は (char) キャスト。signed char に固定
#define IA (signed char)((*in >> 24) & 0xff)
#define IR (signed char)((*in >> 16) & 0xff)
#define IG (signed char)((*in >> 8) & 0xff)
#define IB (signed char)((*in) & 0xff)
		switch (filtertypes[i]) {
			KRT_TLG6_DO_CHROMA_DECODE(0, IB, IG, IR);
			KRT_TLG6_DO_CHROMA_DECODE(1, IB + IG, IG, IR + IG);
			KRT_TLG6_DO_CHROMA_DECODE(2, IB, IG + IB, IR + IB + IG);
			KRT_TLG6_DO_CHROMA_DECODE(3, IB + IR + IG, IG + IR, IR);
			KRT_TLG6_DO_CHROMA_DECODE(4, IB + IR, IG + IB + IR, IR + IB + IR + IG);
			KRT_TLG6_DO_CHROMA_DECODE(5, IB + IR, IG + IB + IR, IR);
			KRT_TLG6_DO_CHROMA_DECODE(6, IB + IG, IG, IR);
			KRT_TLG6_DO_CHROMA_DECODE(7, IB, IG + IB, IR);
			KRT_TLG6_DO_CHROMA_DECODE(8, IB, IG, IR + IG);
			KRT_TLG6_DO_CHROMA_DECODE(9, IB + IG + IR + IB, IG + IR + IB, IR + IB);
			KRT_TLG6_DO_CHROMA_DECODE(10, IB + IR, IG + IR, IR);
			KRT_TLG6_DO_CHROMA_DECODE(11, IB, IG + IB, IR + IB);
			KRT_TLG6_DO_CHROMA_DECODE(12, IB, IG + IR + IB, IR + IB);
			KRT_TLG6_DO_CHROMA_DECODE(13, IB + IG, IG + IR + IB + IG, IR + IB + IG);
			KRT_TLG6_DO_CHROMA_DECODE(14, IB + IG + IR, IG + IR, IR + IB + IG + IR);
			KRT_TLG6_DO_CHROMA_DECODE(15, IB, IG + (IB << 1), IR + (IB << 1));

		default:
			// 本体は黙って return するが、未知のフィルタは不正データとして扱う
			throw TlgError("TLG6: unsupported filter type");
		}
#undef IA
#undef IR
#undef IG
#undef IB
		if (step == 1)
			in += skipblockbytes - ww;
		else
			in += skipblockbytes + 1;
		if (i & 1) in -= oddskip * ww;
	}
}
#undef KRT_TLG6_DO_CHROMA_DECODE
#undef KRT_TLG6_DO_CHROMA_DECODE_PROTO

//---------------------------------------------------------------------------
// TLG6 (LoadTLG.cpp TVPLoadTLG6)。r はマーク (11 バイト) の直後を指していること
//---------------------------------------------------------------------------
void decodeTlg6(Reader& r, int& width, int& height, std::vector<uint8_t>& bgra, int& colorsOut) {
	uint8_t hdr[4];
	r.read(hdr, 4);

	int colors = hdr[0]; // color component count
	if (colors != 1 && colors != 4 && colors != 3) throw TlgError("TLG6: unsupported color count");
	if (hdr[1] != 0) throw TlgError("TLG6: data flag must be zero");
	if (hdr[2] != 0) throw TlgError("TLG6: unsupported color type");
	if (hdr[3] != 0) throw TlgError("TLG6: external golomb bit length table is not supported");

	int32_t w = r.i32();
	int32_t h = r.i32();
	checkSize(w, h);
	int32_t max_bit_length = r.i32();
	if (max_bit_length < 0) throw TlgError("TLG6: invalid max bit length");

	// 不正ヘッダで巨大な確保をしないための概算チェック:
	// 8 行ごと・色ごとに最低 4 バイト (ビット長) + 1 バイトのデータが必要
	if ((uint64_t)((h - 1) / TLG6_H_BLOCK_SIZE + 1) * colors * 5 > r.remain())
		throw TlgError("TLG6: data is too short for the image size");

	width = w;
	height = h;
	colorsOut = colors;
	bgra.assign((size_t)w * h * 4, 0);

	// compute some values
	const int x_block_count = (int)((w - 1) / TLG6_W_BLOCK_SIZE) + 1;
	const int y_block_count = (int)((h - 1) / TLG6_H_BLOCK_SIZE) + 1;
	const int main_count = w / TLG6_W_BLOCK_SIZE;
	const int fraction = w - main_count * TLG6_W_BLOCK_SIZE;

	// allocate memories (ゴロム復号は 4 バイト先読みするので余白を足す)
	const size_t bit_pool_size = (size_t)max_bit_length / 8 + 5;
	std::vector<uint8_t> bit_pool(bit_pool_size + 16, 0);
	std::vector<uint32_t> pixelbuf((size_t)w * TLG6_H_BLOCK_SIZE + 1, 0);
	std::vector<uint8_t> filter_types((size_t)x_block_count * y_block_count + 16, 0);
	std::vector<uint32_t> zeroline(w, colors == 3 ? 0xff000000u : 0x00000000u);
	// 0xff000000 for colors=3 makes alpha value opaque
	std::vector<uint8_t> LZSS_text(4096 + 16, 0);

	// initialize LZSS text (used by chroma filter type codes)
	{
		uint8_t* p = LZSS_text.data();
		for (int i = 0; i < 32; i++) {
			for (int j = 0; j < 16; j++) {
				p[0] = p[1] = p[2] = p[3] = (uint8_t)i;
				p += 4;
				p[0] = p[1] = p[2] = p[3] = (uint8_t)j;
				p += 4;
			}
		}
	}

	// read chroma filter types.
	// chroma filter types are compressed via LZSS as used by TLG5.
	{
		int32_t inbuf_size = r.i32();
		if (inbuf_size < 0) throw TlgError("TLG6: invalid filter type size");
		const uint8_t* inbuf = r.ptr((size_t)inbuf_size);
		tlg5DecompressSlide(filter_types.data(), filter_types.data() + (size_t)x_block_count * y_block_count, inbuf,
		                    inbuf_size, LZSS_text.data(), 0);
	}

	// for each horizontal block group ...
	const uint32_t* prevline = zeroline.data();
	uint32_t* image = reinterpret_cast<uint32_t*>(bgra.data()); // vector<uint8_t> の確保は 4 バイト境界以上
	for (int y = 0; y < h; y += TLG6_H_BLOCK_SIZE) {
		int ylim = y + TLG6_H_BLOCK_SIZE;
		if (ylim >= h) ylim = h;

		int pixel_count = (ylim - y) * w;

		// colors==1 のとき ForFirst を通らないので残りのバイトを明示的にクリア
		if (colors == 1) std::fill(pixelbuf.begin(), pixelbuf.end(), 0u);

		// decode values
		for (int c = 0; c < colors; c++) {
			// read bit length
			int32_t bit_length = r.i32();

			// get compress method
			int method = (bit_length >> 30) & 3;
			bit_length &= 0x3fffffff;

			// compute byte length
			int32_t byte_length = bit_length / 8;
			if (bit_length % 8) byte_length++;
			if ((size_t)byte_length > bit_pool_size) throw TlgError("TLG6: bit length exceeds max bit length");

			// read source from input
			r.read(bit_pool.data(), (size_t)byte_length);
			std::memset(bit_pool.data() + byte_length, 0, bit_pool.size() - byte_length);

			// two most significant bits of bitlength are entropy coding method;
			// 00 means Golomb method (others are not supported)
			switch (method) {
			case 0:
				if (c == 0 && colors != 1)
					decodeGolombValues(reinterpret_cast<int8_t*>(pixelbuf.data()), pixel_count, bit_pool.data(),
					                   bit_pool.data() + bit_pool.size() - 4, true);
				else
					decodeGolombValues(reinterpret_cast<int8_t*>(pixelbuf.data()) + c, pixel_count, bit_pool.data(),
					                   bit_pool.data() + bit_pool.size() - 4, false);
				break;
			default: throw TlgError("TLG6: unsupported entropy coding method");
			}
		}

		// for each line
		const uint8_t* ft = filter_types.data() + (y / TLG6_H_BLOCK_SIZE) * x_block_count;
		int skipbytes = (ylim - y) * TLG6_W_BLOCK_SIZE;

		for (int yy = y; yy < ylim; yy++) {
			uint32_t* curline = image + (size_t)yy * w;

			int dir = (yy & 1) ^ 1;
			int oddskip = ((ylim - yy - 1) - (yy - y));
			if (main_count) {
				int start = ((w < TLG6_W_BLOCK_SIZE) ? w : TLG6_W_BLOCK_SIZE) * (yy - y);
				decodeLineGeneric(prevline, curline, w, 0, main_count, ft, skipbytes, pixelbuf.data() + start,
				                  colors == 3 ? 0xff000000 : 0, oddskip, dir);
			}

			if (main_count != x_block_count) {
				int ww = fraction;
				if (ww > TLG6_W_BLOCK_SIZE) ww = TLG6_W_BLOCK_SIZE;
				int start = ww * (yy - y);
				decodeLineGeneric(prevline, curline, w, main_count, x_block_count, ft, skipbytes,
				                  pixelbuf.data() + start, colors == 3 ? 0xff000000 : 0, oddskip, dir);
			}

			prevline = curline;
		}
	}

	if (colors == 1) {
		// グレースケール: B を G/R へ複製し不透明にする
		for (size_t i = 0; i < (size_t)w * h; i++) {
			uint8_t* px = bgra.data() + i * 4;
			px[1] = px[2] = px[0];
			px[3] = 0xff;
		}
	}
}

//---------------------------------------------------------------------------
// TVPInternalLoadTLG: マーク 11 バイトを読んで TLG5 / TLG6 へ振り分け
//---------------------------------------------------------------------------
void internalLoad(Reader& r, int& width, int& height, std::vector<uint8_t>& bgra, int& colors) {
	uint8_t mark[11];
	r.read(mark, 11);
	if (!std::memcmp("TLG5.0\x00raw\x1a\x00", mark, 11))
		decodeTlg5(r, width, height, bgra, colors);
	else if (!std::memcmp("TLG6.0\x00raw\x1a\x00", mark, 11))
		decodeTlg6(r, width, height, bgra, colors);
	else
		throw TlgError("TLG: invalid TLG header or unsupported TLG version");
}

// "tags" チャンクの解析 (LoadTLG.cpp TVPLoadTLG 内)
// 形式: "<namelen>:<name>=<valuelen>:<value>," の繰り返し
void parseTags(const char* tag, size_t chunksize, Tags* tags) {
	const char* tagp = tag;
	const char* tagp_lim = tag + chunksize;
	auto readLen = [&](const char* what) {
		size_t len = 0;
		bool any = false;
		while (tagp < tagp_lim && *tagp >= '0' && *tagp <= '9') {
			len = len * 10 + (size_t)(*tagp - '0'), tagp++, any = true;
			if (len > chunksize) throw TlgError(std::string("TLG: malformed tag (too long ") + what + ")");
		}
		if (!any || tagp >= tagp_lim || *tagp != ':')
			throw TlgError(std::string("TLG: malformed tag (missing colon after ") + what + " length)");
		tagp++;
		return len;
	};
	while (tagp < tagp_lim) {
		size_t namelen = readLen("name");
		if ((size_t)(tagp_lim - tagp) < namelen) throw TlgError("TLG: malformed tag (name is truncated)");
		std::string name(tagp, namelen);
		tagp += namelen;
		if (tagp >= tagp_lim || *tagp != '=') throw TlgError("TLG: malformed tag (missing equals after name)");
		tagp++;
		size_t valuelen = readLen("value");
		if ((size_t)(tagp_lim - tagp) < valuelen) throw TlgError("TLG: malformed tag (value is truncated)");
		std::string value(tagp, valuelen);
		tagp += valuelen;
		if (tagp >= tagp_lim || *tagp != ',') throw TlgError("TLG: malformed tag (missing comma after tag)");
		tagp++;
		if (tags) tags->emplace_back(std::move(name), std::move(value));
	}
}

} // namespace
} // namespace detail

//---------------------------------------------------------------------------
// 公開 API: decodeTlg (LoadTLG.cpp TVPLoadTLG)
//---------------------------------------------------------------------------
bool decodeTlg(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& bgra,
               bool* hasAlpha, Tags* tags, std::string& error) {
	using namespace detail;
	try {
		if (!data) throw TlgError("TLG: no data");
		if (tags) tags->clear();
		Reader r(data, size);
		uint8_t mark[11];
		r.read(mark, 11);
		int colors = 0;

		if (!std::memcmp("TLG0.0\x00sds\x1a\x00", mark, 11)) {
			// read TLG0.0 Structured Data Stream
			// TLG0.0 SDS tagged data is simple "NAME=VALUE," string;
			// Each NAME and VALUE have length:content expression.
			// eg: 4:LEFT=2:20,3:TOP=3:120,4:TYPE=1:3,
			// The last ',' cannot be ommited.
			// Each string (name and value) must be encoded in utf-8.

			// read raw data size
			uint32_t rawlen = r.u32();

			// try to load TLG raw data
			internalLoad(r, width, height, bgra, colors);

			// seek to meta info data point
			if ((uint64_t)rawlen + 11 + 4 > size) throw TlgError("TLG: invalid raw data length in TLG0.0 header");
			r.seek((size_t)rawlen + 11 + 4);

			// read tag data
			while (r.remain() >= 4) {
				char chunkname[4];
				r.read(chunkname, 4);
				uint32_t chunksize = r.u32();
				const uint8_t* chunk = r.ptr(chunksize);
				if (!std::memcmp(chunkname, "tags", 4)) {
					parseTags(reinterpret_cast<const char*>(chunk), chunksize, tags);
				}
				// それ以外のチャンクは読み飛ばす
			}
		} else {
			r.seek(0); // rewind
			internalLoad(r, width, height, bgra, colors);
		}
		if (hasAlpha) *hasAlpha = (colors == 4);
		return true;
	} catch (const std::exception& e) {
		error = e.what();
		return false;
	}
}

} // namespace tlg
} // namespace krt
