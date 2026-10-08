//---------------------------------------------------------------------------
// krt_tlg 単体テスト / 変換 CLI
//
//   tlgtest encode5|encode6|encode524|encode624 <in.raw> <w> <h> <out.tlg> [name=value ...]
//       in.raw = BGRA (w*h*4 バイト, top-down)。末尾の name=value はタグとして付ける
//   tlgtest decode <in.tlg> <out.raw>
//       BGRA を書き出し、サイズ / アルファ有無 / タグを標準出力に表示
//   tlgtest selftest
//       さまざまなサイズ・絵柄で encode → decode の可逆性を確認
//---------------------------------------------------------------------------
#include "krt/tlg/Tlg.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using krt::tlg::Tags;

static bool readFile(const char* path, std::vector<uint8_t>& out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}
static bool writeFile(const char* path, const std::vector<uint8_t>& data) {
	std::ofstream f(path, std::ios::binary);
	if (!f) return false;
	f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
	return (bool)f;
}

//---------------------------------------------------------------------------
// selftest
//---------------------------------------------------------------------------
struct Rng {
	uint32_t s;
	explicit Rng(uint32_t seed) : s(seed) {}
	uint32_t next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
};

static std::vector<uint8_t> makeImage(int w, int h, int pattern, uint32_t seed) {
	std::vector<uint8_t> img((size_t)w * h * 4);
	Rng r(seed * 2654435761u + 12345u);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint8_t* p = &img[((size_t)y * w + x) * 4];
			switch (pattern) {
			case 0: // 完全ノイズ
			{
				uint32_t v = r.next();
				p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
				break;
			}
			case 1: // グラデーション + 弱いノイズ
			{
				uint32_t v = r.next();
				p[0] = (uint8_t)(x * 3 + (v & 3));
				p[1] = (uint8_t)(y * 5 + ((v >> 2) & 1));
				p[2] = (uint8_t)(x + y);
				p[3] = (uint8_t)(255 - x * 2);
				break;
			}
			case 2: // ほぼ平坦 (疎な点) → ゼロラン / LZSS 一致を多く通す
			{
				uint32_t v = r.next();
				bool dot = (v % 97) == 0;
				p[0] = dot ? (uint8_t)(v >> 8) : 0x40;
				p[1] = dot ? (uint8_t)(v >> 16) : 0x80;
				p[2] = 0xc0;
				p[3] = dot ? (uint8_t)(v >> 24) : 0xff;
				break;
			}
			default: // 大きな段差 (ゴロム打ち切り経路)
			{
				bool on = ((x / 3) + (y / 2)) & 1;
				p[0] = on ? 0xff : 0x00;
				p[1] = on ? 0x00 : 0xff;
				p[2] = (uint8_t)(on ? 0x80 : 0x7f);
				p[3] = on ? 0x00 : 0xff;
				break;
			}
			}
		}
	}
	return img;
}

static int selftest() {
	const int sizes[][2] = {{1, 1}, {1, 7}, {7, 1}, {2, 2},  {3, 5},   {8, 8},   {9, 9},   {15, 17}, {16, 16},
	                        {17, 9}, {31, 4}, {33, 3}, {64, 64}, {65, 1}, {100, 37}, {257, 131}, {300, 200}};
	int failures = 0, cases = 0;
	for (auto& sz : sizes) {
		const int w = sz[0], h = sz[1];
		for (int pattern = 0; pattern < 4; pattern++) {
			auto img = makeImage(w, h, pattern, (uint32_t)(w * 1000 + h * 10 + pattern));
			for (int tlg6 = 0; tlg6 < 2; tlg6++) {
				for (int alpha = 0; alpha < 2; alpha++) {
					for (int withTags = 0; withTags < 2; withTags++) {
						cases++;
						Tags tags;
						if (withTags) tags = {{"mode", "alpha"}, {"offs_x", "12"}, {"名前", "値=,:"}};
						std::vector<uint8_t> enc, dec;
						std::string err;
						bool ok = tlg6 ? krt::tlg::encodeTlg6(img.data(), w, h, alpha != 0, tags, enc, err)
						               : krt::tlg::encodeTlg5(img.data(), w, h, alpha != 0, tags, enc, err);
						int dw = 0, dh = 0;
						bool hasAlpha = false;
						Tags dtags;
						if (ok) ok = krt::tlg::decodeTlg(enc.data(), enc.size(), dw, dh, dec, &hasAlpha, &dtags, err);
						if (ok && (dw != w || dh != h)) ok = false, err = "size mismatch";
						if (ok && hasAlpha != (alpha != 0)) ok = false, err = "hasAlpha mismatch";
						if (ok && dtags != tags) ok = false, err = "tags mismatch";
						if (ok) {
							for (size_t i = 0; i < (size_t)w * h && ok; i++) {
								for (int c = 0; c < 4; c++) {
									uint8_t expect = (c == 3 && !alpha) ? 0xff : img[i * 4 + c];
									if (dec[i * 4 + c] != expect) {
										ok = false;
										err = "pixel mismatch at " + std::to_string(i) + " ch " + std::to_string(c);
										break;
									}
								}
							}
						}
						if (!ok) {
							failures++;
							std::printf("FAIL %dx%d pattern=%d %s alpha=%d tags=%d : %s\n", w, h, pattern,
							            tlg6 ? "TLG6" : "TLG5", alpha, withTags, err.c_str());
						}
					}
				}
			}
		}
	}

	// 異常系: 壊れたデータで落ちないこと (結果は失敗で良い)
	{
		auto img = makeImage(37, 23, 1, 7);
		for (int tlg6 = 0; tlg6 < 2; tlg6++) {
			std::vector<uint8_t> enc, dec;
			std::string err;
			if (tlg6)
				krt::tlg::encodeTlg6(img.data(), 37, 23, true, {}, enc, err);
			else
				krt::tlg::encodeTlg5(img.data(), 37, 23, true, {}, enc, err);
			Rng r(99 + tlg6);
			for (int t = 0; t < 2000; t++) {
				auto bad = enc;
				int nflip = 1 + (int)(r.next() % 4);
				// 幅・高さ (先頭 11+12 バイト付近) を壊すと最大 4GB 確保になり遅いので、
				// 大半はヘッダ以降を壊す (ヘッダ破壊は 1/10 だけ)
				const size_t from = (t % 10 == 0) ? 0 : 24;
				for (int k = 0; k < nflip; k++)
					bad[from + r.next() % (bad.size() - from)] ^= (uint8_t)(1u << (r.next() % 8));
				if (t % 5 == 0) bad.resize(r.next() % bad.size());
				int dw, dh;
				krt::tlg::decodeTlg(bad.data(), bad.size(), dw, dh, dec, nullptr, nullptr, err);
			}
		}
	}

	std::printf("selftest: %d cases, %d failures\n", cases, failures);
	std::printf(failures ? "SELFTEST FAILED\n" : "SELFTEST OK\n");
	return failures ? 1 : 0;
}

//---------------------------------------------------------------------------
int main(int argc, char** argv) {
	if (argc >= 2 && !std::strcmp(argv[1], "selftest")) return selftest();

	if (argc >= 6 && !std::strncmp(argv[1], "encode", 6)) {
		std::string mode = argv[1] + 6;
		bool tlg6, alpha;
		if (mode == "5")
			tlg6 = false, alpha = true;
		else if (mode == "6")
			tlg6 = true, alpha = true;
		else if (mode == "524")
			tlg6 = false, alpha = false;
		else if (mode == "624")
			tlg6 = true, alpha = false;
		else {
			std::fprintf(stderr, "unknown mode: %s\n", argv[1]);
			return 2;
		}
		std::vector<uint8_t> raw;
		if (!readFile(argv[2], raw)) {
			std::fprintf(stderr, "cannot read %s\n", argv[2]);
			return 1;
		}
		int w = std::atoi(argv[3]), h = std::atoi(argv[4]);
		if (w <= 0 || h <= 0 || raw.size() != (size_t)w * h * 4) {
			std::fprintf(stderr, "raw size mismatch (%zu bytes for %dx%d)\n", raw.size(), w, h);
			return 1;
		}
		Tags tags;
		for (int i = 6; i < argc; i++) {
			const char* eq = std::strchr(argv[i], '=');
			if (!eq) continue;
			tags.emplace_back(std::string(argv[i], eq - argv[i]), std::string(eq + 1));
		}
		std::vector<uint8_t> out;
		std::string err;
		bool ok = tlg6 ? krt::tlg::encodeTlg6(raw.data(), w, h, alpha, tags, out, err)
		               : krt::tlg::encodeTlg5(raw.data(), w, h, alpha, tags, out, err);
		if (!ok) {
			std::fprintf(stderr, "encode failed: %s\n", err.c_str());
			return 1;
		}
		if (!writeFile(argv[5], out)) {
			std::fprintf(stderr, "cannot write %s\n", argv[5]);
			return 1;
		}
		std::printf("wrote %s (%zu bytes)\n", argv[5], out.size());
		return 0;
	}

	if (argc == 4 && !std::strcmp(argv[1], "decode")) {
		std::vector<uint8_t> data;
		if (!readFile(argv[2], data)) {
			std::fprintf(stderr, "cannot read %s\n", argv[2]);
			return 1;
		}
		int w = 0, h = 0;
		bool hasAlpha = false;
		Tags tags;
		std::vector<uint8_t> bgra;
		std::string err;
		if (!krt::tlg::decodeTlg(data.data(), data.size(), w, h, bgra, &hasAlpha, &tags, err)) {
			std::fprintf(stderr, "decode failed: %s\n", err.c_str());
			return 1;
		}
		if (!writeFile(argv[3], bgra)) {
			std::fprintf(stderr, "cannot write %s\n", argv[3]);
			return 1;
		}
		std::printf("size %dx%d alpha=%d tags=%zu\n", w, h, hasAlpha ? 1 : 0, tags.size());
		for (auto& kv : tags) std::printf("tag %s=%s\n", kv.first.c_str(), kv.second.c_str());
		return 0;
	}

	std::fprintf(stderr,
	             "usage:\n"
	             "  tlgtest encode5|encode6|encode524|encode624 <in.raw> <w> <h> <out.tlg> [name=value ...]\n"
	             "  tlgtest decode <in.tlg> <out.raw>\n"
	             "  tlgtest selftest\n");
	return 2;
}
