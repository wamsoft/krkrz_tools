// PE のリソースの読み書き (PeResources.h 参照)
#include "PeResources.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace krt::release::pe {

namespace {
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i))); }
void set16(std::vector<uint8_t>& v, size_t at, uint16_t x) { v[at] = (uint8_t)x; v[at + 1] = (uint8_t)(x >> 8); }
void align4(std::vector<uint8_t>& v) { while (v.size() % 4) v.push_back(0); }
} // namespace

uint64_t imageEnd(const fs::path& file, uint64_t& fileSize)
{
	std::ifstream f(file, std::ios::binary);
	fileSize = 0;
	if (!f) return 0;
	f.seekg(0, std::ios::end);
	fileSize = (uint64_t)f.tellg();
	auto readAt = [&](uint64_t pos, void* buf, size_t n) {
		f.clear();
		f.seekg((std::streamoff)pos);
		f.read(static_cast<char*>(buf), (std::streamsize)n);
		return (size_t)f.gcount() == n;
	};
	uint8_t dos[64];
	if (!readAt(0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') return 0;
	const uint32_t peOfs = le32(dos + 0x3c);
	uint8_t coff[24];
	if (!readAt(peOfs, coff, sizeof(coff)) || std::memcmp(coff, "PE\0\0", 4) != 0) return 0;
	const uint16_t n = le16(coff + 6), opt = le16(coff + 20);
	if (n == 0 || n > 96) return 0;
	std::vector<uint8_t> t((size_t)n * 40);
	const uint64_t tableOfs = (uint64_t)peOfs + 24 + opt;
	if (!readAt(tableOfs, t.data(), t.size())) return 0;
	uint64_t end = tableOfs + t.size();
	for (uint16_t i = 0; i < n; ++i) {
		const uint32_t size = le32(&t[i * 40 + 16]), ptr = le32(&t[i * 40 + 20]);
		if (size) end = std::max<uint64_t>(end, (uint64_t)ptr + size);
	}
	return end <= fileSize ? end : 0;
}

//---------------------------------------------------------------------------
// 読み取り
//---------------------------------------------------------------------------
ResourceReader::ResourceReader(const fs::path& file)
{
	module_ = ::LoadLibraryExW(file.wstring().c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
}

ResourceReader::~ResourceReader()
{
	if (module_) ::FreeLibrary(static_cast<HMODULE>(module_));
}

std::vector<uint16_t> ResourceReader::languages(const wchar_t* type, const wchar_t* name) const
{
	std::vector<uint16_t> langs;
	if (!module_) return langs;
	::EnumResourceLanguagesW(static_cast<HMODULE>(module_), type, name,
		[](HMODULE, LPCWSTR, LPCWSTR, WORD lang, LONG_PTR p) -> BOOL {
			reinterpret_cast<std::vector<uint16_t>*>(p)->push_back(lang);
			return TRUE;
		}, reinterpret_cast<LONG_PTR>(&langs));
	return langs;
}

bool ResourceReader::read(const wchar_t* type, const wchar_t* name, uint16_t lang, std::vector<uint8_t>& out) const
{
	if (!module_) return false;
	HMODULE m = static_cast<HMODULE>(module_);
	HRSRC r = ::FindResourceExW(m, type, name, lang);
	if (!r) return false;
	const DWORD size = ::SizeofResource(m, r);
	HGLOBAL g = ::LoadResource(m, r);
	const void* p = g ? ::LockResource(g) : nullptr;
	if (!p) return false;
	out.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + size);
	return true;
}

bool ResourceReader::hasType(const wchar_t* type) const
{
	if (!module_) return false;
	bool found = false;
	::EnumResourceNamesW(static_cast<HMODULE>(module_), type,
		[](HMODULE, LPCWSTR, LPWSTR, LONG_PTR p) -> BOOL { *reinterpret_cast<bool*>(p) = true; return FALSE; },
		reinterpret_cast<LONG_PTR>(&found));
	return found;
}

std::vector<uint16_t> ResourceReader::iconIds() const
{
	std::vector<uint16_t> ids;
	if (!module_) return ids;
	::EnumResourceNamesW(static_cast<HMODULE>(module_), RT_ICON,
		[](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR p) -> BOOL {
			if (IS_INTRESOURCE(name)) reinterpret_cast<std::vector<uint16_t>*>(p)->push_back((uint16_t)(uintptr_t)name);
			return TRUE;
		}, reinterpret_cast<LONG_PTR>(&ids));
	return ids;
}

std::vector<std::wstring> ResourceReader::names(const wchar_t* type) const
{
	std::vector<std::wstring> out;
	if (!module_) return out;
	::EnumResourceNamesW(static_cast<HMODULE>(module_), type,
		[](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR p) -> BOOL {
			auto* v = reinterpret_cast<std::vector<std::wstring>*>(p);
			if (IS_INTRESOURCE(name)) v->push_back(L"#" + std::to_wstring((uint16_t)(uintptr_t)name));
			else v->push_back(name);
			return TRUE;
		}, reinterpret_cast<LONG_PTR>(&out));
	return out;
}

const wchar_t* resName(const std::wstring& n)
{
	if (!n.empty() && n[0] == L'#') return MAKEINTRESOURCEW((WORD)std::wcstoul(n.c_str() + 1, nullptr, 10));
	return n.c_str();
}

//---------------------------------------------------------------------------
// アイコン
//---------------------------------------------------------------------------
bool loadIco(const fs::path& ico, std::vector<IconImage>& out, std::string& error)
{
	out.clear();
	std::ifstream f(ico, std::ios::binary);
	if (!f) { error = "アイコンファイルを開けません"; return false; }
	std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (d.size() < 6 || le16(&d[0]) != 0 || le16(&d[2]) != 1) { error = ".ico ファイルではありません"; return false; }
	const uint16_t count = le16(&d[4]);
	if (count == 0 || d.size() < 6 + (size_t)count * 16) { error = ".ico ファイルが壊れています"; return false; }
	for (uint16_t i = 0; i < count; ++i) {
		const uint8_t* e = &d[6 + i * 16];
		IconImage img;
		img.width = e[0];
		img.height = e[1];
		img.colors = e[2];
		img.planes = le16(e + 4);
		img.bitCount = le16(e + 6);
		const uint32_t size = le32(e + 8), ofs = le32(e + 12);
		if ((uint64_t)ofs + size > d.size() || size == 0) { error = ".ico ファイルが壊れています"; return false; }
		img.data.assign(d.begin() + ofs, d.begin() + ofs + size);
		// PNG の画像はヘッダの planes / bitCount が 0 のことがある。グループ側は 1 / 32 にしておく
		if (img.data.size() >= 8 && std::memcmp(img.data.data(), "\x89PNG", 4) == 0) {
			if (!img.planes) img.planes = 1;
			if (!img.bitCount) img.bitCount = 32;
		}
		out.push_back(std::move(img));
	}
	return true;
}

std::vector<uint16_t> groupIconIds(const std::vector<uint8_t>& g)
{
	std::vector<uint16_t> ids;
	if (g.size() < 6) return ids;
	const uint16_t count = le16(&g[4]);
	for (uint16_t i = 0; i < count && 6 + (size_t)(i + 1) * 14 <= g.size(); ++i) ids.push_back(le16(&g[6 + i * 14 + 12]));
	return ids;
}

std::vector<uint8_t> buildGroupIcon(const std::vector<IconImage>& images, const std::vector<uint16_t>& ids)
{
	std::vector<uint8_t> g;
	put16(g, 0);
	put16(g, 1);
	put16(g, (uint16_t)images.size());
	for (size_t i = 0; i < images.size(); ++i) {
		const IconImage& im = images[i];
		g.push_back(im.width);
		g.push_back(im.height);
		g.push_back(im.colors);
		g.push_back(0);
		put16(g, im.planes);
		put16(g, im.bitCount);
		put32(g, (uint32_t)im.data.size());
		put16(g, ids[i]);
	}
	return g;
}

//---------------------------------------------------------------------------
// バージョン情報 (VS_VERSIONINFO)
//   各ブロック: WORD wLength, WORD wValueLength, WORD wType, WCHAR szKey[], 4 バイト境界,
//              Value, 4 バイト境界, 子ブロック
//---------------------------------------------------------------------------
namespace {

struct Block {
	std::wstring key;
	uint16_t type = 0;            ///< 1 = 文字列, 0 = バイナリ
	std::vector<uint8_t> value;   ///< 文字列は UTF-16 (NUL 込み)
	std::vector<Block> children;
};

bool parseBlock(const uint8_t* p, size_t avail, Block& b, size_t& used)
{
	if (avail < 6) return false;
	const uint16_t len = le16(p), valueLen = le16(p + 2), type = le16(p + 4);
	if (len < 6 || len > avail) return false;
	size_t pos = 6;
	b.key.clear();
	while (pos + 2 <= len) {
		const wchar_t c = (wchar_t)le16(p + pos);
		pos += 2;
		if (!c) break;
		b.key += c;
	}
	pos = (pos + 3) & ~(size_t)3;
	b.type = type;
	const size_t vbytes = type == 1 ? (size_t)valueLen * 2 : valueLen;
	if (pos + vbytes > len) return false;
	b.value.assign(p + pos, p + pos + vbytes);
	pos = (pos + vbytes + 3) & ~(size_t)3;
	while (pos + 6 <= len) {
		Block c;
		size_t u = 0;
		if (!parseBlock(p + pos, len - pos, c, u)) break;
		b.children.push_back(std::move(c));
		pos = (pos + u + 3) & ~(size_t)3;
	}
	used = len;
	return true;
}

void writeBlock(std::vector<uint8_t>& out, const Block& b)
{
	align4(out);
	const size_t start = out.size();
	put16(out, 0);
	put16(out, (uint16_t)(b.type == 1 ? b.value.size() / 2 : b.value.size()));
	put16(out, b.type);
	for (wchar_t c : b.key) put16(out, (uint16_t)c);
	put16(out, 0);
	align4(out);
	out.insert(out.end(), b.value.begin(), b.value.end());
	for (const auto& c : b.children) writeBlock(out, c);
	set16(out, start, (uint16_t)(out.size() - start));
}

std::vector<uint8_t> utf16z(const std::wstring& s)
{
	std::vector<uint8_t> v;
	for (wchar_t c : s) put16(v, (uint16_t)c);
	put16(v, 0);
	return v;
}

std::wstring fromUtf16(const std::vector<uint8_t>& v)
{
	std::wstring s;
	for (size_t i = 0; i + 1 < v.size(); i += 2) {
		const wchar_t c = (wchar_t)le16(&v[i]);
		if (!c) break;
		s += c;
	}
	return s;
}

} // namespace

bool parseVersion(const std::vector<uint8_t>& data, VersionInfo& out)
{
	Block root;
	size_t used = 0;
	if (!parseBlock(data.data(), data.size(), root, used) || root.key != L"VS_VERSION_INFO") return false;
	out.fixed = root.value;
	out.strings.clear();
	for (const auto& c : root.children) {
		if (c.key != L"StringFileInfo" || c.children.empty()) continue;
		const Block& table = c.children.front();
		out.table = table.key;
		for (const auto& s : table.children) out.strings.emplace_back(s.key, fromUtf16(s.value));
	}
	return out.fixed.size() == 52;
}

std::vector<uint8_t> buildVersion(const VersionInfo& v)
{
	Block root;
	root.key = L"VS_VERSION_INFO";
	root.type = 0;
	root.value = v.fixed;
	Block sfi;
	sfi.key = L"StringFileInfo";
	sfi.type = 1;
	Block table;
	table.key = v.table;
	table.type = 1;
	for (const auto& [k, val] : v.strings) {
		Block s;
		s.key = k;
		s.type = 1;
		s.value = utf16z(val);
		table.children.push_back(std::move(s));
	}
	sfi.children.push_back(std::move(table));
	root.children.push_back(std::move(sfi));
	Block vfi;
	vfi.key = L"VarFileInfo";
	vfi.type = 1;
	Block tr;
	tr.key = L"Translation";
	tr.type = 0;
	const unsigned long code = std::wcstoul(v.table.c_str(), nullptr, 16);
	put16(tr.value, (uint16_t)(code >> 16));      // 言語
	put16(tr.value, (uint16_t)(code & 0xffff));   // コードページ
	vfi.children.push_back(std::move(tr));
	root.children.push_back(std::move(vfi));
	std::vector<uint8_t> out;
	writeBlock(out, root);
	return out;
}

bool parseVersionNumber(const std::wstring& s, uint32_t& ms, uint32_t& ls)
{
	unsigned v[4] = { 0, 0, 0, 0 };
	int n = 0;
	const wchar_t* p = s.c_str();
	while (n < 4) {
		if (*p < L'0' || *p > L'9') break;
		unsigned x = 0;
		while (*p >= L'0' && *p <= L'9') { x = x * 10 + (unsigned)(*p - L'0'); ++p; }
		if (x > 0xffff) return false;
		v[n++] = x;
		if (*p == L'.' || *p == L',') { ++p; continue; }
		break;
	}
	if (n == 0) return false;
	ms = (v[0] << 16) | v[1];
	ls = (v[2] << 16) | v[3];
	return true;
}

} // namespace krt::release::pe
