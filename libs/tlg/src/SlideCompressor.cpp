//---------------------------------------------------------------------------
// スライド辞書法 (LZSS 変形) の圧縮 / 展開と TLG6 用テーブル
//
// 移植元:
//   SlideCompressor          … 本体 SaveTLG5.cpp (class SlideCompressor)
//   tlg5DecompressSlide      … 本体 tvpgl.c TVPTLG5DecompressSlide_c
//   tlg6Tables               … 本体 tvpgl.c TVPTLG6InitLeadingZeroTable /
//                              TVPTLG6InitGolombTable (SaveTLG6.cpp にも同一の
//                              テーブルがある。圧縮側・展開側で共用する)
// 出力のバイト一致のため、アルゴリズム・分岐順は本体のまま変えていない。
//---------------------------------------------------------------------------
#include "TlgInternal.h"

namespace krt {
namespace tlg {
namespace detail {

//---------------------------------------------------------------------------
// SlideCompressor (SaveTLG5.cpp)
//---------------------------------------------------------------------------
SlideCompressor::SlideCompressor() {
	S = 0;
	S2 = 0;
	for (int i = 0; i < SLIDE_N + SLIDE_M; i++) Text[i] = 0, Text2[i] = 0;
	for (int i = 0; i < 256 * 256; i++) Map[i] = -1;
	for (int i = 0; i < SLIDE_N; i++) Chains[i].Prev = Chains[i].Next = -1;
	// 初期辞書 (全ゼロ) を末尾側から登録する。本体と同じ順序であることが必要
	for (int i = SLIDE_N - 1; i >= 0; i--) AddMap(i);
}
//---------------------------------------------------------------------------
int SlideCompressor::GetMatch(const unsigned char* cur, int curlen, int& pos, int s) {
	// get match length
	if (curlen < 3) return 0;

	int place = cur[0] + ((int)cur[1] << 8);

	int maxlen = 0;
	if ((place = Map[place]) != -1) {
		int place_org;
		curlen -= 1;
		do {
			place_org = place;
			if (s == place || s == ((place + 1) & (SLIDE_N - 1))) continue;
			place += 2;
			int lim = (SLIDE_M < curlen ? SLIDE_M : curlen) + place_org;
			const unsigned char* c = cur + 2;
			if (lim >= SLIDE_N) {
				if (place_org <= s && s < SLIDE_N)
					lim = s;
				else if (s < (lim & (SLIDE_N - 1)))
					lim = s + SLIDE_N;
			} else {
				if (place_org <= s && s < lim) lim = s;
			}
			while (Text[place] == *(c++) && place < lim) place++;
			int matchlen = place - place_org;
			if (matchlen > maxlen) pos = place_org, maxlen = matchlen;
			if (matchlen == SLIDE_M) return maxlen;

		} while ((place = Chains[place_org].Next) != -1);
	}
	return maxlen;
}
//---------------------------------------------------------------------------
void SlideCompressor::AddMap(int p) {
	int place = Text[p] + ((int)Text[(p + 1) & (SLIDE_N - 1)] << 8);

	if (Map[place] == -1) {
		// first insertion
		Map[place] = p;
	} else {
		// not first insertion
		int old = Map[place];
		Map[place] = p;
		Chains[old].Prev = p;
		Chains[p].Next = old;
		Chains[p].Prev = -1;
	}
}
//---------------------------------------------------------------------------
void SlideCompressor::DeleteMap(int p) {
	int n;
	if ((n = Chains[p].Next) != -1) Chains[n].Prev = Chains[p].Prev;

	if ((n = Chains[p].Prev) != -1) {
		Chains[n].Next = Chains[p].Next;
	} else if (Chains[p].Next != -1) {
		int place = Text[p] + ((int)Text[(p + 1) & (SLIDE_N - 1)] << 8);
		Map[place] = Chains[p].Next;
	} else {
		int place = Text[p] + ((int)Text[(p + 1) & (SLIDE_N - 1)] << 8);
		Map[place] = -1;
	}

	Chains[p].Prev = -1;
	Chains[p].Next = -1;
}
//---------------------------------------------------------------------------
void SlideCompressor::Encode(const unsigned char* in, long inlen, unsigned char* out, long& outlen) {
	unsigned char code[40], codeptr, mask;

	if (inlen == 0) return;

	outlen = 0;
	code[0] = 0;
	codeptr = mask = 1;

	int s = S;
	while (inlen > 0) {
		int pos = 0;
		int len = GetMatch(in, (int)inlen, pos, s);
		if (len >= 3) {
			code[0] |= mask;
			if (len >= 18) {
				code[codeptr++] = pos & 0xff;
				code[codeptr++] = ((pos & 0xf00) >> 8) | 0xf0;
				code[codeptr++] = len - 18;
			} else {
				code[codeptr++] = pos & 0xff;
				code[codeptr++] = ((pos & 0xf00) >> 8) | ((len - 3) << 4);
			}
			while (len--) {
				unsigned char c = *in++;
				DeleteMap((s - 1) & (SLIDE_N - 1));
				DeleteMap(s);
				if (s < SLIDE_M - 1) Text[s + SLIDE_N] = c;
				Text[s] = c;
				AddMap((s - 1) & (SLIDE_N - 1));
				AddMap(s);
				s++;
				inlen--;
				s &= (SLIDE_N - 1);
			}
		} else {
			unsigned char c = *in++;
			DeleteMap((s - 1) & (SLIDE_N - 1));
			DeleteMap(s);
			if (s < SLIDE_M - 1) Text[s + SLIDE_N] = c;
			Text[s] = c;
			AddMap((s - 1) & (SLIDE_N - 1));
			AddMap(s);
			s++;
			inlen--;
			s &= (SLIDE_N - 1);
			code[codeptr++] = c;
		}
		mask <<= 1;

		if (mask == 0) {
			for (int i = 0; i < codeptr; i++) out[outlen++] = code[i];
			mask = codeptr = 1;
			code[0] = 0;
		}
	}

	if (mask != 1) {
		for (int i = 0; i < codeptr; i++) out[outlen++] = code[i];
	}

	S = s;
}
//---------------------------------------------------------------------------
void SlideCompressor::Store() {
	S2 = S;
	int i;
	for (i = 0; i < SLIDE_N + SLIDE_M - 1; i++) Text2[i] = Text[i];
	for (i = 0; i < 256 * 256; i++) Map2[i] = Map[i];
	for (i = 0; i < SLIDE_N; i++) Chains2[i] = Chains[i];
}
//---------------------------------------------------------------------------
void SlideCompressor::Restore() {
	S = S2;
	int i;
	for (i = 0; i < SLIDE_N + SLIDE_M - 1; i++) Text[i] = Text2[i];
	for (i = 0; i < 256 * 256; i++) Map[i] = Map2[i];
	for (i = 0; i < SLIDE_N; i++) Chains[i] = Chains2[i];
}

//---------------------------------------------------------------------------
// TLG5 LZSS 展開 (tvpgl.c TVPTLG5DecompressSlide_c)
// 本体からの差分: 不正データ対策として in の読み越し / out の書き越しを検査する。
//---------------------------------------------------------------------------
int32_t tlg5DecompressSlide(uint8_t* out, const uint8_t* outlimit, const uint8_t* in,
                            int32_t insize, uint8_t* text, int32_t initialr) {
	int32_t r = initialr;
	unsigned int flags = 0;
	const uint8_t* inlim = in + insize;
	while (in < inlim) {
		if (((flags >>= 1) & 256) == 0) {
			flags = *in++ | 0xff00;
			if (in >= inlim) break; // フラグバイトだけで終わっている
		}
		if (flags & 1) {
			if (inlim - in < 2) throw TlgError("TLG: LZSS data is truncated");
			int32_t mpos = in[0] | ((in[1] & 0xf) << 8);
			int32_t mlen = (in[1] & 0xf0) >> 4;
			in += 2;
			mlen += 3;
			if (mlen == 18) {
				if (in >= inlim) throw TlgError("TLG: LZSS data is truncated");
				mlen += *in++;
			}
			if (outlimit - out < mlen) throw TlgError("TLG: LZSS output overflow");

			while (mlen--) {
				*out++ = text[r++] = text[mpos++];
				mpos &= (4096 - 1);
				r &= (4096 - 1);
			}
		} else {
			if (out >= outlimit) throw TlgError("TLG: LZSS output overflow");
			unsigned char c = *in++;
			*out++ = c;
			text[r++] = c;
			r &= (4096 - 1);
		}
	}
	return r;
}

//---------------------------------------------------------------------------
// TLG6 テーブル (tvpgl.c)
//---------------------------------------------------------------------------
static const short TLG6GolombCompressed[TLG6_GOLOMB_N_COUNT][9] = {
	{3, 7, 15, 27, 63, 108, 223, 448, 130},
	{3, 5, 13, 24, 51, 95, 192, 384, 257},
	{2, 5, 12, 21, 39, 86, 155, 320, 384},
	{2, 3, 9, 18, 33, 61, 129, 258, 511},
	// Tuned by W.Dee, 2004/03/25
};

static Tlg6Tables buildTables() {
	Tlg6Tables t{};
	// TVPTLG6InitLeadingZeroTable: 最下位の立っているビット位置 + 1 (無ければ 0)
	for (int i = 0; i < TLG6_LEADING_ZERO_TABLE_SIZE; i++) {
		int cnt = 0;
		int j;
		for (j = 1; j != TLG6_LEADING_ZERO_TABLE_SIZE && !(i & j); j <<= 1, cnt++)
			;
		cnt++;
		if (j == TLG6_LEADING_ZERO_TABLE_SIZE) cnt = 0;
		t.leadingZero[i] = (uint8_t)cnt;
	}
	// TVPTLG6InitGolombTable: 圧縮表現を展開
	for (int n = 0; n < TLG6_GOLOMB_N_COUNT; n++) {
		int a = 0;
		for (int i = 0; i < 9; i++) {
			for (int j = 0; j < TLG6GolombCompressed[n][i]; j++) t.golombBitLength[a++][n] = (char)i;
		}
		if (a != TLG6_GOLOMB_N_COUNT * 2 * 128) throw std::logic_error("TLG6 golomb table is broken");
	}
	return t;
}

const Tlg6Tables& tlg6Tables() {
	static const Tlg6Tables tables = buildTables();
	return tables;
}

} // namespace detail
} // namespace tlg
} // namespace krt
