//---------------------------------------------------------------------------
// TLG6 エンコーダ
//
// 移植元: 本体 SaveTLG6.cpp
//   TLG6BitStream / CompressValuesGolomb / TryCompressGolomb /
//   ApplyColorFilter / DetectColorFilter / TLG6InitializeColorFilterCompressor /
//   SaveTLG6
// アルゴリズムの概要は SaveTLG6.cpp 冒頭の日本語解説を参照
// (MED/平均法予測 → 8x8 ブロック内ジグザグ並べ替え → 色相関フィルタ →
//  ゼロランレングス + ゴロム・ライス符号)。
//
// 本体との差分:
//   - tTVPBaseBitmap の代わりに BGRA top-down の生バッファ (stride=width*4)
//   - tTVPMemoryStream の代わりに std::vector
//   - 32bpp 前提 (本体の colors==1 = 8bpp 経路は対象外)
//   - デバッグ用 #ifdef (FILTER_TEST 等) と計時コード (reordertick) は削除
// 出力のバイト一致のため、型 (char / unsigned char の符号) や分岐順は本体のまま。
//---------------------------------------------------------------------------
#include "TlgInternal.h"

#include <cstdlib>
#include <cstring>
#include <memory>

namespace krt {
namespace tlg {
namespace detail {

namespace {

// 本体は char (MSVC では signed) を使っている。処理系依存を避けるため signed char に固定
using schar = signed char;

constexpr int MAX_COLOR_COMPONENTS = 4;
constexpr int FILTER_TRY_COUNT = 16;
constexpr int W_BLOCK_SIZE = 8;
constexpr int H_BLOCK_SIZE = 8;
constexpr int GOLOMB_GIVE_UP_BYTES = 4;

inline int golombK(int a, int n) { return tlg6Tables().golombBitLength[a][n]; }

//---------------------------------------------------------------------------
// TLG6.0 bitstream output implementation (SaveTLG6.cpp TLG6BitStream)
//---------------------------------------------------------------------------
class TLG6BitStream {
	int BufferBitPos = 0;   // bit position of output buffer
	long BufferBytePos = 0; // byte position of output buffer
	std::vector<uint8_t>& OutStream;
	std::vector<unsigned char> Buffer; // output buffer (容量 = size())

public:
	explicit TLG6BitStream(std::vector<uint8_t>& outstream) : OutStream(outstream) {}
	~TLG6BitStream() { Flush(); }

	int GetBitPos() const { return BufferBitPos; }
	long GetBytePos() const { return BufferBytePos; }

	void Flush() {
		if (!Buffer.empty() && (BufferBitPos || BufferBytePos)) {
			if (BufferBitPos) BufferBytePos++;
			OutStream.insert(OutStream.end(), Buffer.begin(), Buffer.begin() + BufferBytePos);
			BufferBytePos = 0;
			BufferBitPos = 0;
		}
		Buffer.clear();
	}

	long GetBitLength() const { return BufferBytePos * 8 + BufferBitPos; }

	void Put1Bit(bool b) {
		if (BufferBytePos == (long)Buffer.size()) {
			// need more bytes (本体は 0x1000 ずつ realloc + ゼロ埋め)
			Buffer.resize(Buffer.size() + 0x1000, 0);
		}

		if (b) Buffer[BufferBytePos] |= 1 << BufferBitPos;
		BufferBitPos++;
		if (BufferBitPos == 8) {
			BufferBitPos = 0;
			BufferBytePos++;
		}
	}

	void PutGamma(int v) {
		// Put a gamma code. v must be larger than 0.
		int t = v;
		t >>= 1;
		int cnt = 0;
		while (t) {
			Put1Bit(0);
			t >>= 1;
			cnt++;
		}
		Put1Bit(1);
		while (cnt--) {
			Put1Bit(v & 1);
			v >>= 1;
		}
	}

	static int GetGammaBitLengthGeneric(int v) {
		int needbits = 1;
		v >>= 1;
		while (v) {
			needbits += 2;
			v >>= 1;
		}
		return needbits;
	}

	static int GetGammaBitLength(int v) {
		// Get bit length where v is to be encoded as a gamma code.
		if (v <= 1) return 1;
		if (v <= 3) return 3;
		if (v <= 7) return 5;
		if (v <= 15) return 7;
		if (v <= 31) return 9;
		if (v <= 63) return 11;
		if (v <= 127) return 13;
		if (v <= 255) return 15;
		if (v <= 511) return 17;
		return GetGammaBitLengthGeneric(v);
	}

	void PutValue(long v, int len) {
		// put value "v" as length of "len"
		while (len--) {
			Put1Bit(v & 1);
			v >>= 1;
		}
	}
};

//---------------------------------------------------------------------------
// ゴロム符号化 (SaveTLG6.cpp CompressValuesGolomb)
//---------------------------------------------------------------------------
void CompressValuesGolomb(TLG6BitStream& bs, schar* buf, int size) {
	// run-length golomb method
	bs.PutValue(buf[0] ? 1 : 0, 1); // initial value state

	int count;

	int n = TLG6_GOLOMB_N_COUNT - 1; // 個数のカウンタ
	int a = 0;                       // 予測誤差の絶対値の和

	count = 0;
	for (int i = 0; i < size; i++) {
		if (buf[i]) {
			// write zero count
			if (count) bs.PutGamma(count);

			// count non-zero values
			count = 0;
			int ii;
			for (ii = i; ii < size; ii++) {
				if (buf[ii])
					count++;
				else
					break;
			}

			// write non-zero count
			bs.PutGamma(count);

			// write non-zero values
			for (; i < ii; i++) {
				int e = buf[i];
				int k = golombK(a, n);
				int m = ((e >= 0) ? 2 * e : -2 * e - 1) - 1;
				int store_limit = (int)bs.GetBytePos() + GOLOMB_GIVE_UP_BYTES;
				bool put1 = true;
				for (int c = (m >> k); c > 0; c--) {
					if (store_limit == bs.GetBytePos()) {
						// 0 が長くなりすぎる場合は打ち切って m>>k を 8bit で直接書く
						bs.PutValue(m >> k, 8);
						put1 = false;
						break;
					}
					bs.Put1Bit(0);
				}
				if (store_limit == bs.GetBytePos()) {
					bs.PutValue(m >> k, 8);
					put1 = false;
				}
				if (put1) bs.Put1Bit(1);
				bs.PutValue(m, k);
				a += (m >> 1);
				if (--n < 0) {
					a >>= 1;
					n = TLG6_GOLOMB_N_COUNT - 1;
				}
			}

			i = ii - 1;
			count = 0;
		} else {
			// zero
			count++;
		}
	}

	if (count) bs.PutGamma(count);
}

//---------------------------------------------------------------------------
// 符号長の試算 (SaveTLG6.cpp TryCompressGolomb)
//---------------------------------------------------------------------------
class TryCompressGolomb {
	int TotalBits; // total bit count
	int Count;     // running count
	int N;
	int A;
	bool LastNonZero;

public:
	TryCompressGolomb() { Reset(); }

	void Reset() {
		TotalBits = 1;
		Count = 0;
		N = TLG6_GOLOMB_N_COUNT - 1;
		A = 0;
		LastNonZero = false;
	}

	int Try(schar* buf, int size) {
		for (int i = 0; i < size; i++) {
			if (buf[i]) {
				// write zero count
				if (!LastNonZero) {
					if (Count) TotalBits += TLG6BitStream::GetGammaBitLength(Count);

					// count non-zero values
					Count = 0;
				}

				// write non-zero values
				for (; i < size; i++) {
					int e = buf[i];
					if (!e) break;
					Count++;
					int k = golombK(A, N);
					int m = ((e >= 0) ? 2 * e : -2 * e - 1) - 1;
					int unexp_bits = (m >> k);
					if (unexp_bits >= (GOLOMB_GIVE_UP_BYTES * 8 - 8 / 2))
						unexp_bits = (GOLOMB_GIVE_UP_BYTES * 8 - 8 / 2) + 8;
					TotalBits += unexp_bits + 1 + k;
					A += (m >> 1);
					if (--N < 0) {
						A >>= 1;
						N = TLG6_GOLOMB_N_COUNT - 1;
					}
				}

				// write non-zero count

				i--;
				LastNonZero = true;
			} else {
				// zero
				if (LastNonZero) {
					if (Count) {
						TotalBits += TLG6BitStream::GetGammaBitLength(Count);
						Count = 0;
					}
				}

				Count++;
				LastNonZero = false;
			}
		}
		return TotalBits;
	}

	int Flush() {
		if (Count) {
			TotalBits += TLG6BitStream::GetGammaBitLength(Count);
			Count = 0;
		}
		return TotalBits;
	}
};

//---------------------------------------------------------------------------
// 色相関フィルタ (SaveTLG6.cpp ApplyColorFilter)
// 実際に使われるのは 0..15 (FILTER_TRY_COUNT)。16..39 は本体にある実験用の
// フィルタで、互換のため一緒に移植している (展開側は 0..15 のみ対応)。
// 本体は 4 個ずつ展開したループ (DO_FILTER マクロ) を使う箇所があるが、
// 要素ごとに独立した演算なので単純ループと結果は同じ。
//---------------------------------------------------------------------------
void ApplyColorFilter(schar* bufb, schar* bufg, schar* bufr, int len, int code) {
	int d;
	unsigned char t;
	switch (code) {
	case 0: break;
	case 1:
		for (d = 0; d < len; d++) bufr[d] -= bufg[d], bufb[d] -= bufg[d];
		break;
	case 2:
		for (d = 0; d < len; d++) bufr[d] -= bufg[d], bufg[d] -= bufb[d];
		break;
	case 3:
		for (d = 0; d < len; d++) bufb[d] -= bufg[d], bufg[d] -= bufr[d];
		break;
	case 4:
		for (d = 0; d < len; d++) bufr[d] -= bufg[d], bufg[d] -= bufb[d], bufb[d] -= bufr[d];
		break;
	case 5:
		for (d = 0; d < len; d++) bufg[d] -= bufb[d], bufb[d] -= bufr[d];
		break;
	case 6:
		for (d = 0; d < len; d++) bufb[d] -= bufg[d];
		break;
	case 7:
		for (d = 0; d < len; d++) bufg[d] -= bufb[d];
		break;
	case 8:
		for (d = 0; d < len; d++) bufr[d] -= bufg[d];
		break;
	case 9:
		for (d = 0; d < len; d++) bufb[d] -= bufg[d], bufg[d] -= bufr[d], bufr[d] -= bufb[d];
		break;
	case 10:
		for (d = 0; d < len; d++) bufg[d] -= bufr[d], bufb[d] -= bufr[d];
		break;
	case 11:
		for (d = 0; d < len; d++) bufr[d] -= bufb[d], bufg[d] -= bufb[d];
		break;
	case 12:
		for (d = 0; d < len; d++) bufg[d] -= bufr[d], bufr[d] -= bufb[d];
		break;
	case 13:
		for (d = 0; d < len; d++) bufg[d] -= bufr[d], bufr[d] -= bufb[d], bufb[d] -= bufg[d];
		break;
	case 14:
		for (d = 0; d < len; d++) bufr[d] -= bufb[d], bufb[d] -= bufg[d], bufg[d] -= bufr[d];
		break;
	case 15:
		for (d = 0; d < len; d++) {
			t = (unsigned char)(bufb[d] << 1);
			bufr[d] -= t, bufg[d] -= t;
		}
		break;
	case 16:
		for (d = 0; d < len; d++) bufg[d] -= bufr[d];
		break;
	case 17:
		for (d = 0; d < len; d++) bufr[d] -= bufb[d], bufb[d] -= bufg[d];
		break;
	case 18:
		for (d = 0; d < len; d++) bufr[d] -= bufb[d];
		break;
	case 19:
		for (d = 0; d < len; d++) bufb[d] -= bufr[d], bufr[d] -= bufg[d];
		break;
	case 20:
		for (d = 0; d < len; d++) bufb[d] -= bufr[d];
		break;
	case 21:
		for (d = 0; d < len; d++) bufb[d] -= bufg[d] >> 1;
		break;
	case 22:
		for (d = 0; d < len; d++) bufg[d] -= bufb[d] >> 1;
		break;
	case 23:
		for (d = 0; d < len; d++) bufg[d] -= bufb[d], bufb[d] -= bufr[d], bufr[d] -= bufg[d];
		break;
	case 24:
		for (d = 0; d < len; d++) bufb[d] -= bufr[d], bufr[d] -= bufg[d], bufg[d] -= bufb[d];
		break;
	case 25:
		for (d = 0; d < len; d++) bufg[d] -= bufr[d] >> 1;
		break;
	case 26:
		for (d = 0; d < len; d++) bufr[d] -= bufg[d] >> 1;
		break;
	case 27:
		for (d = 0; d < len; d++) {
			t = (unsigned char)(bufr[d] >> 1);
			bufg[d] -= t, bufb[d] -= t;
		}
		break;
	case 28:
		for (d = 0; d < len; d++) bufr[d] -= bufb[d] >> 1;
		break;
	case 29:
		for (d = 0; d < len; d++) {
			t = (unsigned char)(bufg[d] >> 1);
			bufr[d] -= t, bufb[d] -= t;
		}
		break;
	case 30:
		for (d = 0; d < len; d++) {
			t = (unsigned char)(bufb[d] >> 1);
			bufr[d] -= t, bufg[d] -= t;
		}
		break;
	case 31:
		for (d = 0; d < len; d++) bufb[d] -= bufr[d] >> 1;
		break;
	case 32:
		for (d = 0; d < len; d++) bufr[d] -= bufb[d] << 1;
		break;
	case 33:
		for (d = 0; d < len; d++) bufb[d] -= bufg[d] << 1;
		break;
	case 34:
		for (d = 0; d < len; d++) {
			t = (unsigned char)(bufr[d] << 1);
			bufg[d] -= t, bufb[d] -= t;
		}
		break;
	case 35:
		for (d = 0; d < len; d++) bufg[d] -= bufb[d] << 1;
		break;
	case 36:
		for (d = 0; d < len; d++) bufr[d] -= bufg[d] << 1;
		break;
	case 37:
		for (d = 0; d < len; d++) bufr[d] -= bufg[d] << 1, bufb[d] -= bufg[d] << 1;
		break;
	case 38:
		for (d = 0; d < len; d++) bufg[d] -= bufr[d] << 1;
		break;
	case 39:
		for (d = 0; d < len; d++) bufb[d] -= bufr[d] << 1;
		break;
	}
}

//---------------------------------------------------------------------------
// 最良の色相関フィルタを探す (SaveTLG6.cpp DetectColorFilter)
//---------------------------------------------------------------------------
int DetectColorFilter(schar* b, schar* g, schar* r, int size, int& outsize) {
	int minbits = -1;
	int mincode = -1;

	schar bbuf[H_BLOCK_SIZE * W_BLOCK_SIZE];
	schar gbuf[H_BLOCK_SIZE * W_BLOCK_SIZE];
	schar rbuf[H_BLOCK_SIZE * W_BLOCK_SIZE];
	TryCompressGolomb bc, gc, rc;

	for (int code = 0; code < FILTER_TRY_COUNT; code++) // 17..27 are currently not used
	{
		// copy bbuf, gbuf, rbuf into b, g, r.
		std::memcpy(bbuf, b, size);
		std::memcpy(gbuf, g, size);
		std::memcpy(rbuf, r, size);

		// copy compressor
		bc.Reset();
		gc.Reset();
		rc.Reset();

		// Apply color filter
		ApplyColorFilter(bbuf, gbuf, rbuf, size, code);

		// try to compress
		int bits;
		bits = (bc.Try(bbuf, size), bc.Flush());
		if (minbits != -1 && minbits < bits) continue;
		bits += (gc.Try(gbuf, size), gc.Flush());
		if (minbits != -1 && minbits < bits) continue;
		bits += (rc.Try(rbuf, size), rc.Flush());

		if (minbits == -1 || minbits > bits) {
			minbits = bits, mincode = code;
		}
	}

	outsize = minbits;

	return mincode;
}

//---------------------------------------------------------------------------
// フィルタ種別を圧縮する LZSS 辞書の初期化
// (SaveTLG6.cpp TLG6InitializeColorFilterCompressor。展開側 LoadTLG.cpp の
//  LZSS_text 初期化と対になる)
//---------------------------------------------------------------------------
void TLG6InitializeColorFilterCompressor(SlideCompressor& c) {
	unsigned char code[4096];
	std::vector<unsigned char> dum(4096 * 2); // 出力は捨てる (本体は 4096 バイト)
	unsigned char* p = code;
	for (int i = 0; i < 32; i++) {
		for (int j = 0; j < 16; j++) {
			p[0] = p[1] = p[2] = p[3] = (unsigned char)i;
			p += 4;
			p[0] = p[1] = p[2] = p[3] = (unsigned char)j;
			p += 4;
		}
	}

	long dumlen;
	c.Encode(code, 4096, dum.data(), dumlen);
}

} // namespace

//---------------------------------------------------------------------------
// SaveTLG6 (SaveTLG6.cpp)
//---------------------------------------------------------------------------
void saveTlg6Raw(const uint8_t* bgra, int width, int height, bool is24, std::vector<uint8_t>& out) {
	(void)tlg6Tables(); // TVPTLG6InitGolombTable 相当

	// check pixel format
	const int colors = is24 ? 3 : 4;
	const int stride = 4; // 32bpp (colors==3 でも 4 バイト単位)
	const size_t pitch = (size_t)width * 4;
	auto scanLine = [&](int y) { return bgra + pitch * y; };

	// output stream header
	writeBytes(out, "TLG6.0\x00raw\x1a\x00", 11);
	out.push_back((uint8_t)colors);
	out.push_back(0); // data flag (0)
	out.push_back(0); // color type (0)
	out.push_back(0); // external golomb table (0)
	writeInt32(out, width);
	writeInt32(out, height);

	// compress
	long max_bit_length = 0;

	std::vector<unsigned char> bufv[MAX_COLOR_COMPONENTS];
	std::vector<schar> block_bufv[MAX_COLOR_COMPONENTS];
	unsigned char* buf[MAX_COLOR_COMPONENTS] = {};
	schar* block_buf[MAX_COLOR_COMPONENTS] = {};

	// ビットストリームは一旦メモリへ (本体の tTVPMemoryStream)
	std::vector<uint8_t> memstream;

	int fc = 0;
	std::vector<unsigned char> filtertypes;
	{
		TLG6BitStream bs(memstream);

		// allocate buffer
		for (int c = 0; c < colors; c++) {
			bufv[c].assign(W_BLOCK_SIZE * H_BLOCK_SIZE * 3, 0);
			block_bufv[c].assign((size_t)H_BLOCK_SIZE * width, 0);
			buf[c] = bufv[c].data();
			block_buf[c] = block_bufv[c].data();
		}
		int w_block_count = (int)((width - 1) / W_BLOCK_SIZE) + 1;
		int h_block_count = (int)((height - 1) / H_BLOCK_SIZE) + 1;
		filtertypes.assign((size_t)w_block_count * h_block_count, 0);

		for (int y = 0; y < height; y += H_BLOCK_SIZE) {
			int ylim = y + H_BLOCK_SIZE;
			if (ylim > height) ylim = height;
			int gwp = 0;
			int xp = 0;
			for (int x = 0; x < width; x += W_BLOCK_SIZE, xp++) {
				int xlim = x + W_BLOCK_SIZE;
				if (xlim > width) xlim = width;
				int bw = xlim - x;

				int p0size = 0; // size of MED method (p=0)
				int minp = 0;   // most efficient method (0:MED, 1:AVG)
				int ft = 0;     // filter type
				int wp = 0;     // write point
				for (int p = 0; p < 2; p++) {
					int dbofs = (p + 1) * (H_BLOCK_SIZE * W_BLOCK_SIZE);

					// do med(when p=0) or take average of upper and left pixel(p=1)
					for (int c = 0; c < colors; c++) {
						int wp2 = 0; // 本体ではブロック内で同名の wp を再宣言している
						for (int yy = y; yy < ylim; yy++) {
							const unsigned char* sl = x * stride + c + scanLine(yy);
							const unsigned char* usl;
							if (yy >= 1)
								usl = x * stride + c + scanLine(yy - 1);
							else
								usl = nullptr;
							for (int xx = x; xx < xlim; xx++) {
								unsigned char pa = xx > 0 ? sl[-stride] : 0;
								unsigned char pb = usl ? *usl : 0;
								unsigned char px = *sl;

								unsigned char py;

								if (p == 0) {
									unsigned char pc = (xx > 0 && usl) ? usl[-stride] : 0;
									unsigned char min_a_b = pa > pb ? pb : pa;
									unsigned char max_a_b = pa < pb ? pb : pa;

									if (pc >= max_a_b)
										py = min_a_b;
									else if (pc < min_a_b)
										py = max_a_b;
									else
										py = (unsigned char)(pa + pb - pc);
								} else {
									py = (unsigned char)((pa + pb + 1) >> 1);
								}

								buf[c][wp2] = (unsigned char)(px - py);

								wp2++;
								sl += stride;
								if (usl) usl += stride;
							}
						}
					}

					// reordering
					// Transfer the data into block_buf (block buffer).
					// Even lines are stored forward (left to right),
					// Odd lines are stored backward (right to left).

					wp = 0;
					for (int yy = y; yy < ylim; yy++) {
						int ofs;
						if (!(xp & 1))
							ofs = (yy - y) * bw;
						else
							ofs = (ylim - yy - 1) * bw;
						bool dir; // false for forward, true for backward
						if (!((ylim - y) & 1)) {
							// vertical line count per block is even
							dir = ((yy & 1) ^ (xp & 1)) ? true : false;
						} else {
							// otherwise;
							if (xp & 1) {
								dir = (yy & 1) != 0;
							} else {
								dir = ((yy & 1) ^ (xp & 1)) ? true : false;
							}
						}

						if (!dir) {
							// forward
							for (int xx = 0; xx < bw; xx++) {
								for (int c = 0; c < colors; c++) buf[c][wp + dbofs] = buf[c][ofs + xx];
								wp++;
							}
						} else {
							// backward
							for (int xx = bw - 1; xx >= 0; xx--) {
								for (int c = 0; c < colors; c++) buf[c][wp + dbofs] = buf[c][ofs + xx];
								wp++;
							}
						}
					}
				}

				for (int p = 0; p < 2; p++) {
					int dbofs = (p + 1) * (H_BLOCK_SIZE * W_BLOCK_SIZE);
					// detect color filter
					int size = 0;
					int ft_;
					if (colors >= 3)
						ft_ = DetectColorFilter(reinterpret_cast<schar*>(buf[0] + dbofs),
						                        reinterpret_cast<schar*>(buf[1] + dbofs),
						                        reinterpret_cast<schar*>(buf[2] + dbofs), wp, size);
					else
						ft_ = 0;

					// select efficient mode of p (MED or average)
					if (p == 0) {
						p0size = size;
						ft = ft_;
					} else {
						if (p0size >= size) minp = 1, ft = ft_;
					}
				}

				// Apply most efficient color filter / prediction method
				wp = 0;
				int dbofs = (minp + 1) * (H_BLOCK_SIZE * W_BLOCK_SIZE);
				for (int yy = y; yy < ylim; yy++) {
					for (int xx = 0; xx < bw; xx++) {
						for (int c = 0; c < colors; c++) block_buf[c][gwp + wp] = (schar)buf[c][wp + dbofs];
						wp++;
					}
				}

				ApplyColorFilter(block_buf[0] + gwp, block_buf[1] + gwp, block_buf[2] + gwp, wp, ft);

				filtertypes[fc++] = (unsigned char)((ft << 1) + minp);
				gwp += wp;
			}

			// compress values (entropy coding)
			for (int c = 0; c < colors; c++) {
				int method;
				CompressValuesGolomb(bs, block_buf[c], gwp);
				method = 0;
				long bitlength = bs.GetBitLength();
				if (bitlength & 0xc0000000) throw TlgError("TLG6: too large bit length (given image may be too large)");
				// two most significant bits of bitlength are
				// entropy coding method;
				// 00 means Golomb method,
				// 01 means Gamma method (implemented but not used),
				// 10 means modified LZSS method (not yet implemented),
				// 11 means raw (uncompressed) data (not yet implemented).
				if (max_bit_length < bitlength) max_bit_length = bitlength;
				bitlength |= ((long)method << 30);
				// 本体同様、ビット長を先に書いてからビット列を書き出す
				writeInt32(memstream, bitlength);
				bs.Flush();
			}
		}
	}

	// write max bit length
	writeInt32(out, max_bit_length);

	// output filter types
	{
		auto comp = std::make_unique<SlideCompressor>();
		TLG6InitializeColorFilterCompressor(*comp);
		std::vector<unsigned char> outbuf((size_t)fc * 2 + 64);
		long outlen = 0;
		comp->Encode(filtertypes.data(), fc, outbuf.data(), outlen);
		writeInt32(out, outlen);
		writeBytes(out, outbuf.data(), (size_t)outlen);
	}

	// copy memory stream to output stream
	writeBytes(out, memstream.data(), memstream.size());
}

} // namespace detail
} // namespace tlg
} // namespace krt
