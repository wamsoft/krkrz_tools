//---------------------------------------------------------------------------
// krt_tlg 内部共通定義
//---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace krt {
namespace tlg {
namespace detail {

//---------------------------------------------------------------------------
// 定数 (本体 tvpgl.h / SaveTLG.h / SaveTLG6.cpp / tvpgl.c より)
//---------------------------------------------------------------------------
constexpr int TLG6_W_BLOCK_SIZE = 8;   // TVP_TLG6_W_BLOCK_SIZE
constexpr int TLG6_H_BLOCK_SIZE = 8;   // TVP_TLG6_H_BLOCK_SIZE
constexpr int TLG6_GOLOMB_N_COUNT = 4; // TVP_TLG6_GOLOMB_N_COUNT
constexpr int TLG6_LEADING_ZERO_TABLE_BITS = 12;
constexpr int TLG6_LEADING_ZERO_TABLE_SIZE = 1 << TLG6_LEADING_ZERO_TABLE_BITS;

constexpr int SLIDE_N = 4096;     // SaveTLG.h SLIDE_N
constexpr int SLIDE_M = 18 + 255; // SaveTLG.h SLIDE_M

//---------------------------------------------------------------------------
// TLG6 ゴロム符号のビット長テーブル / 先頭ゼロ数テーブル
// (tvpgl.c TVPTLG6InitGolombTable / TVPTLG6InitLeadingZeroTable)
//---------------------------------------------------------------------------
struct Tlg6Tables {
	// [a][n] : a = 誤差絶対値の和 (0..1023), n = カウンタ (0..3)
	char golombBitLength[TLG6_GOLOMB_N_COUNT * 2 * 128][TLG6_GOLOMB_N_COUNT];
	uint8_t leadingZero[TLG6_LEADING_ZERO_TABLE_SIZE];
};
// 初回呼び出し時に構築 (スレッドセーフ: 関数ローカル static)
const Tlg6Tables& tlg6Tables();

//---------------------------------------------------------------------------
// スライド辞書法 (LZSS 変形) 圧縮器
// 本体 SaveTLG.h / SaveTLG5.cpp の class SlideCompressor をそのまま移植。
// 状態 (辞書・ハッシュ鎖) が大きいのでヒープに置いて使うこと。
//---------------------------------------------------------------------------
class SlideCompressor {
	struct Chain {
		int Prev;
		int Next;
	};

	// 本体は Text[SLIDE_N + SLIDE_M - 1] だが、GetMatch が終端判定の直前に
	// 1 バイト先 (Text[SLIDE_N + SLIDE_M - 1]) を読むことがあるため 1 バイト余分に
	// 確保する (読んだ値は結果に影響しない)。
	unsigned char Text[SLIDE_N + SLIDE_M];
	int Map[256 * 256];
	Chain Chains[SLIDE_N];

	unsigned char Text2[SLIDE_N + SLIDE_M];
	int Map2[256 * 256];
	Chain Chains2[SLIDE_N];

	int S;
	int S2;

public:
	SlideCompressor();

private:
	int GetMatch(const unsigned char* cur, int curlen, int& pos, int s);
	void AddMap(int p);
	void DeleteMap(int p);

public:
	// in の inlen バイトを圧縮して out へ。outlen に出力バイト数。
	// (本体同様 inlen==0 のときは outlen を変更しない)
	void Encode(const unsigned char* in, long inlen, unsigned char* out, long& outlen);

	void Store();
	void Restore();
};

// TLG5 の LZSS 展開 (tvpgl.c TVPTLG5DecompressSlide_c)
// out の容量チェックのため outlimit を追加 (本体には無い。超えたら例外)。
int32_t tlg5DecompressSlide(uint8_t* out, const uint8_t* outlimit, const uint8_t* in,
                            int32_t insize, uint8_t* text, int32_t initialr);

//---------------------------------------------------------------------------
// リトルエンディアン 32bit 書き込み (SaveTLG5.cpp / SaveTLG6.cpp WriteInt32)
//---------------------------------------------------------------------------
inline void writeInt32(std::vector<uint8_t>& out, long num) {
	out.push_back(static_cast<uint8_t>(num & 0xff));
	out.push_back(static_cast<uint8_t>((num >> 8) & 0xff));
	out.push_back(static_cast<uint8_t>((num >> 16) & 0xff));
	out.push_back(static_cast<uint8_t>((num >> 24) & 0xff));
}
inline void patchInt32(std::vector<uint8_t>& out, size_t pos, uint32_t num) {
	out[pos + 0] = static_cast<uint8_t>(num & 0xff);
	out[pos + 1] = static_cast<uint8_t>((num >> 8) & 0xff);
	out[pos + 2] = static_cast<uint8_t>((num >> 16) & 0xff);
	out[pos + 3] = static_cast<uint8_t>((num >> 24) & 0xff);
}
inline void writeBytes(std::vector<uint8_t>& out, const void* p, size_t n) {
	const uint8_t* b = static_cast<const uint8_t*>(p);
	out.insert(out.end(), b, b + n);
}

// 生 TLG ストリーム (ラッパー無し) を out の末尾へ書く
// is24 = true で colors=3 (本体の "tlg524" / "tlg624")
void saveTlg5Raw(const uint8_t* bgra, int width, int height, bool is24, std::vector<uint8_t>& out);
void saveTlg6Raw(const uint8_t* bgra, int width, int height, bool is24, std::vector<uint8_t>& out);

// 例外 (内部用。公開 API で std::string error に変換する)
struct TlgError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

} // namespace detail
} // namespace tlg
} // namespace krt
