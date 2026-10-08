#include "krt/xp3/Xp3.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include <zlib.h>

#include "krt/app/Progress.h"
#include "krt/app/Text.h"

namespace fs = std::filesystem;

namespace krt::xp3 {

namespace {

// 先頭 11 バイトの目印。実行ファイルの中に同じ並びを作らないよう 2 つに分けて持つ
const unsigned char kMark1[8] = { 'X', 'P', '3', 0x0d, 0x0a, 0x20, 0x0a, 0x1a };
const unsigned char kMark2[3] = { 0x8b, 0x67, 0x01 };

bool isMark(const unsigned char* p)
{
	return std::memcmp(p, kMark1, 8) == 0 && std::memcmp(p + 8, kMark2, 3) == 0;
}

uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t rd64(const unsigned char* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

void put16(std::string& s, uint16_t v) { s += (char)(v & 0xff); s += (char)(v >> 8); }
void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += (char)((v >> (8 * i)) & 0xff); }
void put64(std::string& s, uint64_t v) { for (int i = 0; i < 8; ++i) s += (char)((v >> (8 * i)) & 0xff); }

std::u16string utf8ToUtf16(const std::string& s)
{
	std::u16string out;
	for (size_t i = 0; i < s.size();) {
		unsigned char c = (unsigned char)s[i];
		uint32_t cp;
		int n;
		if (c < 0x80) { cp = c; n = 0; }
		else if ((c >> 5) == 6) { cp = c & 0x1f; n = 1; }
		else if ((c >> 4) == 14) { cp = c & 0x0f; n = 2; }
		else { cp = c & 0x07; n = 3; }
		for (int k = 1; k <= n && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3f);
		i += n + 1;
		if (cp >= 0x10000) {
			cp -= 0x10000;
			out += (char16_t)(0xd800 + (cp >> 10));
			out += (char16_t)(0xdc00 + (cp & 0x3ff));
		} else {
			out += (char16_t)cp;
		}
	}
	return out;
}

std::string utf16ToUtf8(const unsigned char* p, size_t len)
{
	std::string out;
	for (size_t i = 0; i < len; ++i) {
		uint32_t cp = rd16(p + i * 2);
		if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < len) {
			const uint32_t lo = rd16(p + (i + 1) * 2);
			if (lo >= 0xdc00 && lo < 0xe000) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); ++i; }
		}
		if (cp < 0x80) out += (char)cp;
		else if (cp < 0x800) { out += (char)(0xc0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3f)); }
		else if (cp < 0x10000) { out += (char)(0xe0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3f)); out += (char)(0x80 | (cp & 0x3f)); }
		else { out += (char)(0xf0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3f)); out += (char)(0x80 | ((cp >> 6) & 0x3f)); out += (char)(0x80 | (cp & 0x3f)); }
	}
	return out;
}

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

// チャンク列からチャンクを探す (本体の FindChunk と同じ)
bool findChunk(const std::string& data, size_t from, size_t to, const char* name, size_t& start, size_t& size)
{
	size_t pos = from;
	while (pos + 12 <= to) {
		const auto* p = reinterpret_cast<const unsigned char*>(data.data() + pos);
		const uint64_t sz = rd64(p + 4);
		if (pos + 12 + sz > to) return false;
		if (std::memcmp(p, name, 4) == 0) {
			start = pos + 12;
			size = (size_t)sz;
			return true;
		}
		pos += 12 + (size_t)sz;
	}
	return false;
}

} // namespace

//---------------------------------------------------------------------------
// 読み込み
//---------------------------------------------------------------------------
bool Archive::open(const fs::path& file, std::string& error)
{
	path_ = file;
	entries_.clear();
	std::ifstream f(file, std::ios::binary);
	if (!f) { error = "ファイルを開けません"; return false; }

	unsigned char head[11] = {};
	f.read(reinterpret_cast<char*>(head), 11);
	if (f.gcount() == 11 && isMark(head)) {
		offset_ = 0;
	} else if (head[0] == 'M' && head[1] == 'Z') {
		// 実行ファイルに結合された xp3。目印は 16 バイト境界にある
		bool found = false;
		std::vector<unsigned char> buf(1 << 20);
		uint64_t base = 16;
		f.clear();
		f.seekg(16);
		for (;;) {
			f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)buf.size());
			const size_t got = (size_t)f.gcount();
			if (got < 11) break;
			for (size_t i = 0; i + 11 <= got; i += 16) {
				if (isMark(buf.data() + i)) { offset_ = base + i; found = true; break; }
			}
			if (found || got < buf.size()) break;
			base += got;
		}
		if (!found) { error = "実行ファイルに xp3 が結合されていません"; return false; }
	} else {
		error = "xp3 ではありません";
		return false;
	}

	auto readAt = [&](uint64_t pos, void* dst, size_t n) -> bool {
		f.clear();
		f.seekg((std::streamoff)pos);
		f.read(reinterpret_cast<char*>(dst), (std::streamsize)n);
		return (size_t)f.gcount() == n;
	};

	unsigned char b8[8];
	uint64_t pointerPos = offset_ + 11;
	for (int guard = 0; guard < 1024; ++guard) {
		if (!readAt(pointerPos, b8, 8)) { error = "インデックスの位置を読めません"; return false; }
		const uint64_t indexOfs = rd64(b8);
		unsigned char flag;
		if (!readAt(offset_ + indexOfs, &flag, 1)) { error = "インデックスを読めません"; return false; }
		std::string index;
		uint64_t afterIndex;
		if ((flag & 0x07) == 1) {
			unsigned char sz[16];
			if (!readAt(offset_ + indexOfs + 1, sz, 16)) { error = "インデックスを読めません"; return false; }
			const uint64_t comp = rd64(sz), raw = rd64(sz + 8);
			std::string c((size_t)comp, '\0');
			if (!readAt(offset_ + indexOfs + 17, c.data(), (size_t)comp)) { error = "インデックスを読めません"; return false; }
			index.resize((size_t)raw);
			uLongf dl = (uLongf)raw;
			if (uncompress(reinterpret_cast<Bytef*>(index.data()), &dl,
			               reinterpret_cast<const Bytef*>(c.data()), (uLong)comp) != Z_OK || dl != raw) {
				error = "インデックスを展開できません";
				return false;
			}
			indexCompressed_ = true;
			afterIndex = offset_ + indexOfs + 17 + comp;
		} else if ((flag & 0x07) == 0) {
			if (!readAt(offset_ + indexOfs + 1, b8, 8)) { error = "インデックスを読めません"; return false; }
			const uint64_t raw = rd64(b8);
			index.resize((size_t)raw);
			if (raw && !readAt(offset_ + indexOfs + 9, index.data(), (size_t)raw)) { error = "インデックスを読めません"; return false; }
			afterIndex = offset_ + indexOfs + 9 + raw;
		} else {
			error = "未知のインデックス形式です";
			return false;
		}

		// "File" チャンクを順に読む
		size_t pos = 0;
		size_t fs_, fsz;
		while (findChunk(index, pos, index.size(), "File", fs_, fsz)) {
			const size_t fend = fs_ + fsz;
			size_t cs, csz;
			Entry e;
			if (!findChunk(index, fs_, fend, "info", cs, csz) || csz < 22) { error = "インデックスが壊れています (info)"; return false; }
			const auto* ip = reinterpret_cast<const unsigned char*>(index.data() + cs);
			e.flags = rd32(ip);
			e.orgSize = rd64(ip + 4);
			e.arcSize = rd64(ip + 12);
			const size_t nlen = rd16(ip + 20);
			if (22 + nlen * 2 > csz) { error = "インデックスが壊れています (名前)"; return false; }
			e.name = utf16ToUtf8(ip + 22, nlen);
			if (!findChunk(index, fs_, fend, "segm", cs, csz)) { error = "インデックスが壊れています (segm)"; return false; }
			for (size_t s = 0; s + 28 <= csz; s += 28) {
				const auto* sp = reinterpret_cast<const unsigned char*>(index.data() + cs + s);
				Segment seg;
				const uint32_t m = rd32(sp) & 0x07;
				if (m > 1) { error = "未知のセグメント形式です"; return false; }
				seg.compressed = m == 1;
				seg.start = rd64(sp + 4);
				seg.orgSize = rd64(sp + 12);
				seg.arcSize = rd64(sp + 20);
				e.segments.push_back(seg);
			}
			if (findChunk(index, fs_, fend, "adlr", cs, csz) && csz >= 4)
				e.adler32 = rd32(reinterpret_cast<const unsigned char*>(index.data() + cs));
			entries_.push_back(std::move(e));
			pos = fend;
		}
		if (!(flag & 0x80)) break;
		pointerPos = afterIndex;   // 続きあり: 次の I64 が次のインデックスの位置
	}
	return true;
}

bool Archive::anyProtected() const
{
	return std::any_of(entries_.begin(), entries_.end(), [](const Entry& e) { return e.isProtected(); });
}

bool Archive::read(const Entry& e, std::string& out, std::string& error) const
{
	std::ifstream f(path_, std::ios::binary);
	if (!f) { error = "ファイルを開けません"; return false; }
	out.clear();
	out.reserve((size_t)e.orgSize);
	for (const auto& s : e.segments) {
		std::string buf((size_t)s.arcSize, '\0');
		f.clear();
		f.seekg((std::streamoff)(offset_ + s.start));
		f.read(buf.data(), (std::streamsize)buf.size());
		if ((uint64_t)f.gcount() != s.arcSize) { error = "データが途中で切れています"; return false; }
		if (s.compressed) {
			std::string raw((size_t)s.orgSize, '\0');
			uLongf dl = (uLongf)s.orgSize;
			if (uncompress(reinterpret_cast<Bytef*>(raw.data()), &dl,
			               reinterpret_cast<const Bytef*>(buf.data()), (uLong)buf.size()) != Z_OK || dl != s.orgSize) {
				error = "展開できません (データが壊れています)";
				return false;
			}
			out += raw;
		} else {
			out += buf;
		}
	}
	const uLong a = adler32(adler32(0L, Z_NULL, 0), reinterpret_cast<const Bytef*>(out.data()), (uInt)out.size());
	if (a != e.adler32) { error = "チェックサムが一致しません (データが壊れています)"; return false; }
	return true;
}

//---------------------------------------------------------------------------
// 作成のオプション
//---------------------------------------------------------------------------
PackOptions PackOptions::defaults()
{
	PackOptions o;
	// 旧リリーサ (RelSettingsUnit.cpp) の既定の分類
	for (const char* e : { ".xpk", ".xp3", ".exe", ".bat", ".tmp", ".db", ".sue", ".vix", ".ico", ".aul",
	                       ".aue", ".rpf", ".bak", ".log", ".kep", ".cf", ".cfu", "" })
		o.discard.insert(e);
	for (const char* e : { ".wav", ".dll", ".tpi", ".spi", ".txt", ".mid", ".smf", ".swf", ".ks", ".tjs",
	                       ".ma", ".asq", ".asd", ".ttf", ".ttc", ".bmp", ".tft", ".cks" })
		o.compress.insert(e);
	return o;
}

Action PackOptions::classify(const std::string& extIn) const
{
	const std::string ext = lower(extIn);
	if (discard.count(ext)) return Action::Discard;
	if (compress.count(ext)) return Action::Compress;
	if (store.count(ext)) return Action::Store;
	if (ext.rfind(".~", 0) == 0) return Action::Discard;   // エディタのバックアップ
	const PackOptions d = defaults();
	if (d.discard.count(ext)) return Action::Discard;
	if (d.compress.count(ext)) return Action::Compress;
	return Action::Store;
}

namespace {

// INI のカンマ区切り (TStrings::CommaText) を分解する
std::set<std::string> splitComma(const std::string& v)
{
	std::set<std::string> r;
	std::string cur;
	bool q = false;
	for (char c : v) {
		if (c == '"') { q = !q; continue; }
		if (c == ',' && !q) { r.insert(lower(trim(cur))); cur.clear(); continue; }
		cur += c;
	}
	if (!trim(cur).empty()) r.insert(lower(trim(cur)));
	return r;
}

std::string joinComma(const std::set<std::string>& s)
{
	std::string r;
	for (const auto& e : s) {
		if (!r.empty()) r += ',';
		r += e.empty() ? "\"\"" : e;
	}
	return r;
}

bool iniBool(const std::string& v) { return v == "1" || lower(v) == "true"; }

} // namespace

bool loadRpf(const fs::path& rpf, PackOptions& opt, std::string* outputName, std::string& error)
{
	std::string text;
	if (!readFile(rpf, text)) { error = "プロファイルを開けません"; return false; }
	std::istringstream in(text);
	std::string line, section;
	while (std::getline(in, line)) {
		line = trim(line);
		if (line.empty() || line[0] == ';') continue;
		if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
		const auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
		if (section == "Output" && k == "OutputFileName" && outputName) *outputName = v;
		else if (section == "Extensions" && k == "Compress") opt.compress = splitComma(v);
		else if (section == "Extensions" && k == "Store") opt.store = splitComma(v);
		else if (section == "Extensions" && k == "Discard") opt.discard = splitComma(v);
		else if (section == "Options" && k == "DoCompressSizeLimit") opt.doCompressSizeLimit = iniBool(v);
		else if (section == "Options" && k == "CompressSizeLimit") opt.compressSizeLimitKB = std::strtoull(v.c_str(), nullptr, 10);
		else if (section == "Options" && k == "Protect") opt.protect = iniBool(v);
		else if (section == "Options" && k == "CompressIndex") opt.compressIndex = iniBool(v);
	}
	// 旧リリーサの «拡張子なし» の表示名 (日本語の見出し) が残っていれば空文字として扱う
	for (auto* s : { &opt.compress, &opt.store, &opt.discard }) {
		for (auto it = s->begin(); it != s->end();) {
			if (!it->empty() && (*it)[0] != '.') { it = s->erase(it); s->insert(""); }
			else ++it;
		}
	}
	return true;
}

bool saveRpf(const fs::path& rpf, const PackOptions& opt, const std::string& outputName, std::string& error)
{
	std::string s;
	s += "[Output]\r\nExecutable=0\r\nOutputFileName=" + outputName + "\r\n";
	s += "[Extensions]\r\nCompress=" + joinComma(opt.compress) + "\r\nStore=" + joinComma(opt.store) +
	     "\r\nDiscard=" + joinComma(opt.discard) + "\r\n";
	s += "[Options]\r\nDoCompressSizeLimit=" + std::string(opt.doCompressSizeLimit ? "1" : "0") +
	     "\r\nCompressSizeLimit=" + std::to_string(opt.compressSizeLimitKB) +
	     "\r\nProtect=" + std::string(opt.protect ? "1" : "0") +
	     "\r\nOVBookShare=0\r\nCompressIndex=" + std::string(opt.compressIndex ? "1" : "0") + "\r\n";
	if (!writeFile(rpf, s)) { error = "プロファイルを書けません"; return false; }
	return true;
}

//---------------------------------------------------------------------------
// 作成
//---------------------------------------------------------------------------
namespace {

struct PackedFile {
	std::string          name;    // 格納名
	fs::path             local;   // 元ファイル (重複排除の比較用。空 = 警告エントリ)
	uint32_t             flags = 0;
	uint64_t             orgSize = 0, arcSize = 0;
	uint32_t             adler = 0;
	std::vector<Segment> segments;
};

bool sameContents(const fs::path& a, const fs::path& b)
{
	std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
	std::vector<char> ba(1 << 16), bb(1 << 16);
	for (;;) {
		fa.read(ba.data(), (std::streamsize)ba.size());
		fb.read(bb.data(), (std::streamsize)bb.size());
		const auto na = fa.gcount(), nb = fb.gcount();
		if (na != nb || std::memcmp(ba.data(), bb.data(), (size_t)na) != 0) return false;
		if (na == 0) return true;
	}
}

// 展開プロテクト時に先頭に入れる警告 (旧リリーサと同じ PNG。中のテキストを
// 警告文のファイルとして登録する)
// 旧リリーサ (RelSettingsUnit.cpp) の dummy_png の 1 つ目の PNG (警告文の tEXt を含む部分)
const unsigned char kProtectPng[] =
	"\x89\x50\x4e\x47\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00"
	"\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90\x77\x53\xde"
	"\x00\x00\x00\xa5\x74\x45\x58\x74\x57\x61\x72\x6e\x69\x6e\x67\x00"
	"\x57\x61\x72\x6e\x69\x6e\x67\x3a\x20\x45\x78\x74\x72\x61\x63\x74"
	"\x69\x6e\x67\x20\x74\x68\x69\x73\x20\x61\x72\x63\x68\x69\x76\x65"
	"\x20\x6d\x61\x79\x20\x69\x6e\x66\x72\x69\x6e\x67\x65\x20\x6f\x6e"
	"\x20\x61\x75\x74\x68\x6f\x72\x27\x73\x20\x72\x69\x67\x68\x74\x73"
	"\x2e\x20\x8c\x78\x8d\x90\x20\x3a\x20\x82\xb1\x82\xcc\x83\x41\x81"
	"\x5b\x83\x4a\x83\x43\x83\x75\x82\xf0\x93\x57\x8a\x4a\x82\xb7\x82"
	"\xe9\x82\xb1\x82\xc6\x82\xc9\x82\xe6\x82\xe8\x81\x41\x82\xa0\x82"
	"\xc8\x82\xbd\x82\xcd\x92\x98\x8d\xec\x8e\xd2\x82\xcc\x8c\xa0\x97"
	"\x98\x82\xf0\x90\x4e\x8a\x51\x82\xb7\x82\xe9\x82\xa8\x82\xbb\x82"
	"\xea\x82\xaa\x82\xa0\x82\xe8\x82\xdc\x82\xb7\x81\x42\x4b\x49\x44"
	"\x27\x00\x00\x00\x0c\x49\x44\x41\x54\x78\x9c\x63\xf8\xff\xff\x3f"
	"\x00\x05\xfe\x02\xfe\x0d\xef\x46\xb8\x00\x00\x00\x00\x49\x45\x4e"
	"\x44\xae\x42\x60\x82";
const size_t kProtectPngSize = sizeof(kProtectPng) - 1;   // 文字列リテラルの終端を除く
constexpr size_t kProtectTextStart = 0x30;
constexpr size_t kProtectTextLength = 157;
const char kProtectName1[] = u8"$$$ This is a protected archive. $$$ 著作者はこのアーカイブが正規の利用方法以外の方法で展開されることを望んでいません。 ";
const char kProtectName2[] = u8"$$$ Warning! Extracting this archive may infringe on author's rights. 警告 このアーカイブを展開することにより、あなたは著作者の権利を侵害するおそれがあります。.txt";

} // namespace

PackResult pack(const fs::path& sourceDir, const fs::path& output, const PackOptions& opt, Progress& progress)
{
	PackResult res;
	std::error_code ec;

	// 1. 対象ファイルを集める ('.' で始まる名前と CVS フォルダは除く)
	struct Src { fs::path path; std::string name; uint64_t size; bool compress; };
	std::vector<Src> files;
	const fs::path outAbs = fs::weakly_canonical(output, ec);
	auto it = fs::recursive_directory_iterator(sourceDir, fs::directory_options::skip_permission_denied, ec);
	if (ec) throw std::runtime_error("フォルダを開けません: " + fromPath(sourceDir));
	for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (ec) break;
		const std::string fname = fromPath(it->path().filename());
		if (it->is_directory(ec)) {
			if ((!fname.empty() && fname[0] == '.') || fname == "CVS") it.disable_recursion_pending();
			continue;
		}
		if (!fname.empty() && fname[0] == '.') continue;
		if (fs::weakly_canonical(it->path(), ec) == outAbs) continue;   // 出力先自身
		const Action a = opt.classify(fromPath(it->path().extension()));
		if (a == Action::Discard) { ++res.discarded; continue; }
		files.push_back({ it->path(), fromPath(fs::relative(it->path(), sourceDir, ec)),
		                  (uint64_t)it->file_size(ec), a == Action::Compress });
	}
	std::sort(files.begin(), files.end(), [](const Src& a, const Src& b) { return a.name < b.name; });

	// 2. 書き出し
	std::fstream out(output, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
	if (!out) throw std::runtime_error("出力ファイルを作れません: " + fromPath(output));
	auto pos = [&]() { return (uint64_t)out.tellp(); };
	auto write = [&](const void* p, size_t n) {
		out.write(reinterpret_cast<const char*>(p), (std::streamsize)n);
		if (!out) throw std::runtime_error("書き込みに失敗しました");
	};

	// ヘッダ (クッション形式)
	{
		std::string h(reinterpret_cast<const char*>(kMark1), 8);
		h.append(reinterpret_cast<const char*>(kMark2), 3);
		put64(h, 11 + 4 + 8);            // クッションの位置 (0x17)
		put32(h, 1);                     // ヘッダの版
		h += (char)0x80;                 // 続きあり
		put64(h, 0);                     // サイズ 0 のインデックス
		write(h.data(), h.size());
	}
	const uint64_t indexPointerPos = pos();
	write("        ", 8);                // 本当のインデックス位置 (後で書く)

	std::vector<PackedFile> packed;
	if (opt.protect) {
		const uint64_t start = pos() + kProtectTextStart;
		write(kProtectPng, kProtectPngSize);
		PackedFile pf;
		pf.name = std::string(kProtectName1) + kProtectName1 + kProtectName1 + kProtectName2;
		pf.orgSize = pf.arcSize = kProtectTextLength;
		pf.adler = (uint32_t)adler32(adler32(0L, Z_NULL, 0), kProtectPng + kProtectTextStart, (uInt)kProtectTextLength);
		pf.segments.push_back({ false, start, kProtectTextLength, kProtectTextLength });
		packed.push_back(std::move(pf));
	}

	uint64_t total = 0, done = 0;
	for (const auto& f : files) total += std::max<uint64_t>(f.size, 1);
	const uint64_t limit = opt.doCompressSizeLimit ? opt.compressSizeLimitKB * 1024 : UINT64_MAX;
	std::vector<char> buf(1 << 20);

	for (const auto& f : files) {
		if (progress.canceled()) { res.canceled = true; break; }
		progress.progress(total ? (double)done / (double)total : 0.0, f.name);

		// チェックサム (重複排除の判定にも使う)
		uLong adler = adler32(0L, Z_NULL, 0);
		{
			std::ifstream in(f.path, std::ios::binary);
			if (!in) throw std::runtime_error("ファイルを開けません: " + f.name);
			for (;;) {
				in.read(buf.data(), (std::streamsize)buf.size());
				const auto n = in.gcount();
				if (n <= 0) break;
				adler = adler32(adler, reinterpret_cast<const Bytef*>(buf.data()), (uInt)n);
			}
		}

		PackedFile pf;
		pf.name = f.name;
		pf.local = f.path;
		pf.flags = opt.protect ? kFileProtected : 0;
		pf.orgSize = f.size;
		pf.adler = (uint32_t)adler;

		// 同じ中身のファイルがあればそのセグメントを共有する
		bool dedup = false;
		for (const auto& p : packed) {
			if (!p.local.empty() && p.adler == pf.adler && p.orgSize == pf.orgSize && sameContents(p.local, f.path)) {
				pf.segments = p.segments;
				pf.arcSize = p.arcSize;
				dedup = true;
				break;
			}
		}

		if (!dedup) {
			const uint64_t segStart = pos();
			bool compressed = false;
			if (f.compress && f.size > 0 && f.size < limit && f.size < 0x7fffffffULL) {
				// 1 ファイル = 1 セグメントで zlib 圧縮 (旧リリーサと同じ)
				std::string src;
				if (!readFile(f.path, src)) throw std::runtime_error("ファイルを読めません: " + f.name);
				uLongf clen = compressBound((uLong)src.size());
				std::string comp(clen, '\0');
				if (compress2(reinterpret_cast<Bytef*>(comp.data()), &clen,
				              reinterpret_cast<const Bytef*>(src.data()), (uLong)src.size(), Z_BEST_COMPRESSION) == Z_OK &&
				    clen < src.size()) {
					write(comp.data(), clen);
					pf.segments.push_back({ true, segStart, f.size, (uint64_t)clen });
					pf.arcSize = clen;
					compressed = true;
					++res.compressed;
				}
			}
			if (!compressed) {
				std::ifstream in(f.path, std::ios::binary);
				uint64_t copied = 0;
				for (;;) {
					in.read(buf.data(), (std::streamsize)buf.size());
					const auto n = in.gcount();
					if (n <= 0) break;
					write(buf.data(), (size_t)n);
					copied += (uint64_t)n;
					if (progress.canceled()) break;
				}
				pf.segments.push_back({ false, segStart, copied, copied });
				pf.arcSize = copied;
			}
		} else {
			++res.deduplicated;
		}
		res.orgBytes += f.size;
		++res.files;
		done += std::max<uint64_t>(f.size, 1);
		packed.push_back(std::move(pf));
	}

	// 3. インデックス
	progress.progress(1.0, "インデックスを書いています");
	std::string index;
	for (const auto& p : packed) {
		const std::u16string n16 = utf8ToUtf16(p.name);
		std::string info;
		put32(info, p.flags);
		put64(info, p.orgSize);
		put64(info, p.arcSize);
		put16(info, (uint16_t)n16.size());
		for (char16_t c : n16) put16(info, (uint16_t)c);
		std::string segm;
		for (const auto& s : p.segments) {
			put32(segm, s.compressed ? 1 : 0);
			put64(segm, s.start);
			put64(segm, s.orgSize);
			put64(segm, s.arcSize);
		}
		std::string adlr;
		put32(adlr, p.adler);
		std::string body;
		body += "info"; put64(body, info.size()); body += info;
		body += "segm"; put64(body, segm.size()); body += segm;
		body += "adlr"; put64(body, adlr.size()); body += adlr;
		index += "File"; put64(index, body.size()); index += body;
	}
	const uint64_t indexPos = pos();
	std::string ih;
	bool wroteCompressed = false;
	if (opt.compressIndex) {
		uLongf clen = compressBound((uLong)index.size());
		std::string comp(clen, '\0');
		if (compress2(reinterpret_cast<Bytef*>(comp.data()), &clen,
		              reinterpret_cast<const Bytef*>(index.data()), (uLong)index.size(), Z_BEST_COMPRESSION) == Z_OK) {
			ih += (char)1;
			put64(ih, clen);
			put64(ih, index.size());
			write(ih.data(), ih.size());
			write(comp.data(), clen);
			wroteCompressed = true;
		}
	}
	if (!wroteCompressed) {
		ih += (char)0;
		put64(ih, index.size());
		write(ih.data(), ih.size());
		write(index.data(), index.size());
	}
	const uint64_t endPos = pos();
	out.seekp((std::streamoff)indexPointerPos);
	std::string ptr;
	put64(ptr, indexPos);
	write(ptr.data(), ptr.size());
	out.close();
	res.arcBytes = endPos;
	return res;
}

} // namespace krt::xp3
