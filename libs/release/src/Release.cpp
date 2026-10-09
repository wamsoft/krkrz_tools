// 配布用の実行可能ファイルを作る (Release.h 参照)
#include "krt/release/Release.h"
#include "PeResources.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>

#include <appserve/appserve.h>

#include "krt/app/Progress.h"
#include "krt/app/Text.h"
#include "krt/sig/Signature.h"
#include "krt/xp3/Xp3.h"

namespace fs = std::filesystem;
using appserve::Json;

namespace krt::release {

namespace {

// 本体の resource.h / tvpwin32.rc と同じ ID
constexpr WORD kIdrOption = 139;     // TEXT  : 埋め込みオプション (WINVER)
constexpr WORD kIdiMain = 107;       // ICON  : IDI_TVPWIN32
const wchar_t* const kSdlConfig = L"CONFIG.CF";   // BINARY: resource:// の config.cf (SDL)
const char kSecurityHead[] = "-- TVPSystemSecurityOptions ";

std::wstring widen(const std::string& s)
{
	if (s.empty()) return {};
	const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

bool readAll(const fs::path& p, std::vector<uint8_t>& out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

bool writeAll(const fs::path& p, const std::vector<uint8_t>& d)
{
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	if (!f) return false;
	f.write(reinterpret_cast<const char*>(d.data()), (std::streamsize)d.size());
	return (bool)f;
}

/// セキュリティ設定の文字列の位置 ([begin, end) は «-- TVPSystemSecurityOptions » の直後から « --» の手前まで)
bool findSecurity(const std::vector<uint8_t>& d, size_t& begin, size_t& end)
{
	const size_t hl = sizeof(kSecurityHead) - 1;
	for (size_t i = 0; i + hl < d.size(); ++i) {
		if (d[i] != '-' || std::memcmp(&d[i], kSecurityHead, hl) != 0) continue;
		size_t j = i + hl;
		while (j + 2 < d.size() && d[j] && !(d[j] == ' ' && d[j + 1] == '-' && d[j + 2] == '-')) ++j;
		if (j + 2 >= d.size() || !d[j]) continue;
		begin = i + hl;
		end = j;
		return true;
	}
	return false;
}

/// «name(n):name(n):...» を読む
std::vector<std::pair<std::string, int>> parseSecurity(const std::vector<uint8_t>& d, size_t b, size_t e)
{
	std::vector<std::pair<std::string, int>> out;
	const std::string s(d.begin() + b, d.begin() + e);
	size_t p = 0;
	while (p < s.size()) {
		const size_t lp = s.find('(', p), rp = s.find(')', p);
		if (lp == std::string::npos || rp == std::string::npos || rp < lp) break;
		out.emplace_back(s.substr(p, lp - p), std::atoi(s.substr(lp + 1, rp - lp - 1).c_str()));
		p = rp + 1;
		if (p < s.size() && s[p] == ':') ++p;
	}
	return out;
}

ExeKind detectKind(const pe::ResourceReader& r)
{
	// WINVER は tvpwin32.rc の TEXT リソース (起動スクリプト 143 / オプション説明 141) を持つ。
	// オプション 139 は中身が空だとリソースコンパイラが落とすので、無いこともある (書くときに足す)。
	// SDL は TEXT を持たず、resource/ を BINARY として持つ
	if (r.hasType(L"TEXT")) return ExeKind::Winver;
	if (r.hasType(L"BINARY")) return ExeKind::Sdl;
	return ExeKind::Unknown;
}

/// オプション行の名前 («name=value» / «name» の name。コメント・空行は空)
std::string optionName(const std::string& line)
{
	size_t b = 0;
	while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
	if (b >= line.size() || line[b] == ';') return {};
	const size_t e = line.find('=', b);
	std::string n = line.substr(b, e == std::string::npos ? std::string::npos : e - b);
	while (!n.empty() && (n.back() == ' ' || n.back() == '\t' || n.back() == '\r')) n.pop_back();
	return n;
}

/// 値に ASCII 以外が含まれる行は «name="\xNN..."» (TJS の文字列リテラル) に直す。
/// 本体は行を狭い文字列のまま読み、文字コードの解釈がビルドによって違うため
/// (本体同梱の config.cf と同じ書き方)
std::string escapeOptionLine(const std::string& line)
{
	const size_t eq = line.find('=');
	if (eq == std::string::npos || optionName(line).empty()) return line;
	std::string value = line.substr(eq + 1);
	if (!value.empty() && (value.front() == '"' || value.front() == '\'')) return line;   // 既に書式付き
	if (std::all_of(value.begin(), value.end(), [](char c) { return (unsigned char)c < 0x80; })) return line;
	const std::wstring w = widen(value);
	std::string out = line.substr(0, eq + 1) + "\"";
	char buf[16];
	for (wchar_t c : w) {
		std::snprintf(buf, sizeof(buf), "\\x%x", (unsigned)c);
		out += buf;
	}
	return out + "\"";
}

/// 埋め込みオプションの中身を作る。base (元の exe にあったもの) のうち、user で
/// 指定した名前の行は落とし、user の行を後ろに足す。改行は CRLF
std::string mergeOptions(const std::string& base, const std::string& user)
{
	auto lines = [](const std::string& s) {
		std::vector<std::string> v;
		std::string cur;
		for (char c : s) {
			if (c == '\r') continue;
			if (c == '\n') { v.push_back(cur); cur.clear(); } else cur += c;
		}
		if (!cur.empty()) v.push_back(cur);
		return v;
	};
	const auto ul = lines(user);
	std::set<std::string> names;
	for (const auto& l : ul) {
		const std::string n = optionName(l);
		if (!n.empty()) names.insert(n);
	}
	std::string out;
	for (const auto& l : lines(base)) {
		if (l.empty()) continue;
		const std::string n = optionName(l);
		if (!n.empty() && names.count(n)) continue;
		out += l + "\r\n";
	}
	if (!out.empty() && !ul.empty()) out += "; --- krkrrelease ---\r\n";
	for (const auto& l : ul) out += escapeOptionLine(l) + "\r\n";
	return out;
}

fs::path resolve(const fs::path& base, const std::string& p)
{
	if (p.empty()) return {};
	const fs::path x = krt::toPath(p);
	return x.is_absolute() ? x : (base / x).lexically_normal();
}

struct Sub : krt::Progress {
	krt::Progress& parent;
	double base, span;
	std::string label;
	Sub(krt::Progress& p, double b, double s, std::string l) : parent(p), base(b), span(s), label(std::move(l)) {}
	void progress(double r, const std::string& m) override { parent.progress(base + span * (r < 0 ? 0 : r), m.empty() ? label : m); }
	void log(const std::string& l) override { parent.log(l); }
	bool canceled() const override { return parent.canceled(); }
};

} // namespace

const char* kindName(ExeKind k)
{
	switch (k) {
	case ExeKind::Winver: return "winver";
	case ExeKind::Sdl:    return "sdl";
	default:              return "unknown";
	}
}

const std::vector<std::string>& securityNames()
{
	static const std::vector<std::string> names = {
		"forcedataxp3", "acceptfilenameargument", "disablemsgmap", "disableapplock", "disabled3d9" };
	return names;
}

//---------------------------------------------------------------------------
// exe を調べる
//---------------------------------------------------------------------------
bool inspect(const fs::path& exe, ExeInfo& out, std::string& error)
{
	out = ExeInfo();
	out.imageEnd = pe::imageEnd(exe, out.fileSize);
	if (!out.imageEnd) { error = "Windows の実行可能ファイル (PE) として読めません"; return false; }
	out.hasOverlay = out.fileSize > out.imageEnd;

	pe::ResourceReader r(exe);
	if (!r.ok()) { error = "リソースを読めません"; return false; }
	out.kind = detectKind(r);
	std::vector<uint8_t> d;
	if (out.kind == ExeKind::Winver) {
		const auto langs = r.languages(L"TEXT", MAKEINTRESOURCEW(kIdrOption));
		if (!langs.empty() && r.read(L"TEXT", MAKEINTRESOURCEW(kIdrOption), langs.front(), d)) out.options.assign(d.begin(), d.end());
	} else if (out.kind == ExeKind::Sdl) {
		const auto langs = r.languages(L"BINARY", kSdlConfig);
		if (!langs.empty() && r.read(L"BINARY", kSdlConfig, langs.front(), d)) out.options.assign(d.begin(), d.end());
	}
	while (!out.options.empty() && out.options.back() == '\0') out.options.pop_back();
	out.hasIcon = !r.names(RT_GROUP_ICON).empty();
	const auto vlangs = r.languages(RT_VERSION, MAKEINTRESOURCEW(1));
	pe::VersionInfo vi;
	if (!vlangs.empty() && r.read(RT_VERSION, MAKEINTRESOURCEW(1), vlangs.front(), d) && pe::parseVersion(d, vi))
		for (const auto& [k, v] : vi.strings) out.version.emplace_back(narrow(k), narrow(v));

	std::vector<uint8_t> file;
	size_t b = 0, e = 0;
	if (readAll(exe, file) && findSecurity(file, b, e)) out.security = parseSecurity(file, b, e);
	if (out.kind == ExeKind::Unknown) { error = "吉里吉里Z の実行可能ファイルではないようです (埋め込みオプションの置き場所がありません)"; return false; }
	return true;
}

//---------------------------------------------------------------------------
// 設定ファイル
//---------------------------------------------------------------------------
std::string settingsToJsonText(const Settings& s)
{
	Json j = Json::object();
	j.set("exe", Json(s.exe));
	j.set("output", Json(s.output));
	j.set("setOptions", Json(s.setOptions));
	j.set("options", Json(s.options));
	j.set("icon", Json(s.icon));
	Json v = Json::object();
	for (const auto& [k, val] : s.version) v.set(k, Json(val));
	j.set("version", std::move(v));
	Json sec = Json::object();
	for (const auto& [k, val] : s.security) sec.set(k, Json((long long)val));
	j.set("security", std::move(sec));
	j.set("dataMode", Json(std::string(s.dataMode == Settings::DataMode::Copy ? "copy" : s.dataMode == Settings::DataMode::Bind ? "bind" : "none")));
	j.set("data", Json(s.data));
	j.set("rpf", Json(s.rpf));
	j.set("dataName", Json(s.dataName));
	j.set("signKey", Json(s.signKey));
	return j.dump(1) + "\n";
}

bool settingsFromJsonText(const std::string& text, Settings& s, std::string& error)
{
	Json j;
	std::string err;
	if (!Json::parse(text, j, &err) || !j.isObj()) { error = "設定ファイルを読めません (JSON): " + err; return false; }
	s = Settings();
	s.exe = j["exe"].asStr();
	s.output = j["output"].asStr();
	s.setOptions = j["setOptions"].asBool();
	s.options = j["options"].asStr();
	s.icon = j["icon"].asStr();
	if (j["version"].isObj())
		for (const auto& [k, val] : j["version"].members()) s.version.emplace_back(k, val.asStr());
	if (j["security"].isObj())
		for (const auto& [k, val] : j["security"].members()) {
			if (std::find(securityNames().begin(), securityNames().end(), k) == securityNames().end()) {
				error = "セキュリティ設定の項目名が不正です: " + k;
				return false;
			}
			s.security[k] = (int)val.asInt(0);
		}
	const std::string mode = j["dataMode"].asStr("none");
	if (mode == "copy") s.dataMode = Settings::DataMode::Copy;
	else if (mode == "bind") s.dataMode = Settings::DataMode::Bind;
	else if (mode == "none") s.dataMode = Settings::DataMode::None;
	else { error = "dataMode は none / copy / bind です"; return false; }
	s.data = j["data"].asStr();
	s.rpf = j["rpf"].asStr();
	s.dataName = j["dataName"].asStr("data.xp3");
	s.signKey = j["signKey"].asStr();
	return true;
}

bool loadSettings(const fs::path& json, Settings& out, std::string& error)
{
	std::string text;
	if (!krt::readFile(json, text)) { error = "設定ファイルを開けません: " + krt::fromPath(json); return false; }
	return settingsFromJsonText(text, out, error);
}

bool saveSettings(const fs::path& json, const Settings& s, std::string& error)
{
	if (!krt::writeFile(json, settingsToJsonText(s))) { error = "設定ファイルを書けません: " + krt::fromPath(json); return false; }
	return true;
}

//---------------------------------------------------------------------------
// リリース
//---------------------------------------------------------------------------
Result run(const Settings& s, const fs::path& baseDir, krt::Progress& progress)
{
	Result r;
	auto fail = [&](const std::string& m) { r.ok = false; r.message = m; return r; };
	std::error_code ec;

	const fs::path src = resolve(baseDir, s.exe);
	const fs::path out = resolve(baseDir, s.output);
	if (src.empty() || out.empty()) return fail("元にする exe と出力先を指定してください");
	if (fs::equivalent(src, out, ec)) return fail("出力先が元の exe と同じです");

	// 1. 元の exe を調べる
	progress.progress(0.0, "exe を調べています");
	ExeInfo info;
	std::string err;
	if (!inspect(src, info, err)) return fail(krt::fromPath(src.filename()) + ": " + err);
	if (info.hasOverlay)
		return fail("元の exe の後ろにデータが付いています (xp3 の結合済み・署名済みなど)。何も付いていない吉里吉里Z の exe を指定してください");
	if (!s.force && fs::exists(out, ec)) return fail("出力先が既にあります: " + krt::fromPath(out) + " (上書きを指定するか --force)");
	for (const auto& [k, v] : s.security) {
		const bool known = std::any_of(info.security.begin(), info.security.end(), [&](const auto& x) { return x.first == k; });
		if (!known) return fail("この exe にはセキュリティ設定 «" + k + "» がありません");
		if (v < 0 || v > 9) return fail("セキュリティ設定の値は 0〜9 です: " + k);
	}

	// アイコン・データは先に確かめる (途中で失敗して半端な出力を残さないように)
	std::vector<pe::IconImage> icons;
	if (!s.icon.empty() && !pe::loadIco(resolve(baseDir, s.icon), icons, err)) return fail("アイコン: " + err);
	const fs::path data = resolve(baseDir, s.data);
	if (s.dataMode != Settings::DataMode::None && (data.empty() || !fs::exists(data, ec)))
		return fail("データ (xp3 ファイルまたはフォルダ) がありません: " + s.data);
	std::string privateKey;
	if (!s.signKey.empty() && !krt::readFile(resolve(baseDir, s.signKey), privateKey)) return fail("秘密鍵を読めません: " + s.signKey);

	// 2. コピー
	fs::create_directories(out.parent_path(), ec);
	if (!fs::copy_file(src, out, fs::copy_options::overwrite_existing, ec)) return fail("exe をコピーできません: " + ec.message());
	r.outputs.push_back(krt::fromPath(out));
	auto abort = [&](const std::string& m) {
		for (const auto& o : r.outputs) fs::remove(krt::toPath(o), ec);
		r.outputs.clear();
		return fail(m);
	};

	// 3. リソースの書き換え
	const bool doVersion = std::any_of(s.version.begin(), s.version.end(), [](const auto& x) { return !x.second.empty(); });
	if (s.setOptions || !icons.empty() || doVersion) {
		progress.progress(0.1, "リソースを書き換えています");
		pe::ResourceReader rd(src);
		HANDLE h = ::BeginUpdateResourceW(out.wstring().c_str(), FALSE);
		if (!h) return abort("リソースを書き換えられません (BeginUpdateResource)");
		bool okAll = true;
		std::string what;
		auto update = [&](LPCWSTR type, LPCWSTR name, WORD lang, const void* d, DWORD n, const char* label) {
			if (!::UpdateResourceW(h, type, name, lang, const_cast<void*>(d), n)) { okAll = false; what = label; }
		};

		if (s.setOptions) {
			// 元の exe にあったオプション (SDL の config.cf には本体の既定値が入っている) に、
			// 設定のオプションを重ねる。ASCII 以外の値は \xNN の形に直す
			const std::string text = mergeOptions(info.options, s.options);
			if (info.kind == ExeKind::Winver) {
				const auto langs = rd.languages(L"TEXT", MAKEINTRESOURCEW(kIdrOption));
				update(L"TEXT", MAKEINTRESOURCEW(kIdrOption), langs.empty() ? MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL) : langs.front(),
				       text.data(), (DWORD)text.size(), "埋め込みオプション");
			} else {
				const auto langs = rd.languages(L"BINARY", kSdlConfig);
				update(L"BINARY", kSdlConfig, langs.empty() ? MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL) : langs.front(),
				       text.data(), (DWORD)text.size(), "埋め込みオプション (config.cf)");
			}
			r.notes.push_back(std::string("埋め込みオプションを書き換えた (") + (info.kind == ExeKind::Winver ? "TEXT/139" : "BINARY/CONFIG.CF") + ")");
		}

		if (!icons.empty()) {
			// 既存のアイコングループ (WINVER は 107、SDL は MAINICON など) を全部、新しいアイコンに
			// 差し替える。古いグループが指していた RT_ICON は消す。グループが無ければ 107 として足す
			std::vector<std::wstring> groups = rd.names(RT_GROUP_ICON);
			const bool hadGroups = !groups.empty();
			if (!hadGroups) groups.push_back(L"#" + std::to_wstring(kIdiMain));
			WORD glang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
			std::set<uint16_t> used;
			for (uint16_t id : rd.iconIds()) used.insert(id);
			bool first = true;
			for (const auto& gname : groups) {
				for (WORD gl : rd.languages(RT_GROUP_ICON, pe::resName(gname))) {
					if (first) { glang = gl; first = false; }
					std::vector<uint8_t> g;
					if (!rd.read(RT_GROUP_ICON, pe::resName(gname), gl, g)) continue;
					for (uint16_t id : pe::groupIconIds(g)) {
						for (WORD il : rd.languages(RT_ICON, MAKEINTRESOURCEW(id))) update(RT_ICON, MAKEINTRESOURCEW(id), il, nullptr, 0, "古いアイコンの削除");
						used.erase(id);
					}
					update(RT_GROUP_ICON, pe::resName(gname), gl, nullptr, 0, "古いアイコンの削除");
				}
			}
			std::vector<uint16_t> ids;
			for (uint16_t id = 1; ids.size() < icons.size(); ++id)
				if (!used.count(id)) ids.push_back(id);
			for (size_t i = 0; i < icons.size(); ++i)
				update(RT_ICON, MAKEINTRESOURCEW(ids[i]), glang, icons[i].data.data(), (DWORD)icons[i].data.size(), "アイコン");
			const auto group = pe::buildGroupIcon(icons, ids);
			for (const auto& gname : groups)
				update(RT_GROUP_ICON, pe::resName(gname), glang, group.data(), (DWORD)group.size(), "アイコン");
			r.notes.push_back("アイコンを差し替えた (" + std::to_string(icons.size()) + " 枚" + (hadGroups ? "" : "、グループ 107 を追加") + ")");
		}

		if (doVersion) {
			const auto vlangs = rd.languages(RT_VERSION, MAKEINTRESOURCEW(1));
			std::vector<uint8_t> vd;
			pe::VersionInfo vi;
			if (vlangs.empty() || !rd.read(RT_VERSION, MAKEINTRESOURCEW(1), vlangs.front(), vd) || !pe::parseVersion(vd, vi)) {
				::EndUpdateResourceW(h, TRUE);
				return abort("元の exe のバージョン情報を読めません");
			}
			for (const auto& [k, val] : s.version) {
				if (val.empty()) continue;
				const std::wstring wk = widen(k), wv = widen(val);
				auto it = std::find_if(vi.strings.begin(), vi.strings.end(), [&](const auto& x) { return x.first == wk; });
				if (it != vi.strings.end()) it->second = wv; else vi.strings.emplace_back(wk, wv);
				// 数値の版 (VS_FIXEDFILEINFO) も文字列に合わせる
				uint32_t ms, ls;
				if ((k == "FileVersion" || k == "ProductVersion") && pe::parseVersionNumber(wv, ms, ls)) {
					const size_t at = k == "FileVersion" ? 8 : 16;
					for (int b = 0; b < 4; ++b) { vi.fixed[at + b] = (uint8_t)(ms >> (8 * b)); vi.fixed[at + 4 + b] = (uint8_t)(ls >> (8 * b)); }
				}
			}
			const auto nv = pe::buildVersion(vi);
			update(RT_VERSION, MAKEINTRESOURCEW(1), vlangs.front(), nv.data(), (DWORD)nv.size(), "バージョン情報");
			r.notes.push_back("バージョン情報を書き換えた");
		}

		if (!okAll) {
			::EndUpdateResourceW(h, TRUE);
			return abort("リソースを書き換えられません: " + what);
		}
		if (!::EndUpdateResourceW(h, FALSE)) return abort("リソースを書き込めません (EndUpdateResource)");
	}

	// 4. セキュリティ設定 (リソースの書き換えの後。同じ長さで数字だけ置き換える)
	if (!s.security.empty()) {
		progress.progress(0.2, "セキュリティ設定を書き換えています");
		std::vector<uint8_t> file;
		size_t b = 0, e = 0;
		if (!readAll(out, file) || !findSecurity(file, b, e)) return abort("セキュリティ設定の文字列が見つかりません");
		std::string sec(file.begin() + b, file.begin() + e);
		for (const auto& [k, v] : s.security) {
			const size_t at = sec.find(k + "(");
			const size_t vpos = at + k.size() + 1;
			if (at == std::string::npos || vpos >= sec.size() || sec[vpos + 1] != ')') return abort("セキュリティ設定を書き換えられません: " + k);
			sec[vpos] = (char)('0' + v);
		}
		std::memcpy(&file[b], sec.data(), sec.size());
		if (!writeAll(out, file)) return abort("書き込めません: " + krt::fromPath(out));
		r.notes.push_back("セキュリティ設定: " + sec);
	}

	// 5. データ
	fs::path dataFile;   // 署名する xp3 (Copy のとき)
	if (s.dataMode != Settings::DataMode::None) {
		fs::path xp3 = data;
		fs::path temp;
		if (fs::is_directory(data, ec)) {
			xp3::PackOptions po = xp3::PackOptions::defaults();
			fs::path rpf = resolve(baseDir, s.rpf);
			if (rpf.empty() && fs::exists(data / "default.rpf", ec)) rpf = data / "default.rpf";
			if (!rpf.empty() && !xp3::loadRpf(rpf, po, nullptr, err)) return abort("プロファイル: " + err);
			temp = out.parent_path() / (s.dataMode == Settings::DataMode::Copy ? krt::toPath(s.dataName) : fs::path(out.stem().wstring() + L".bind.tmp.xp3"));
			if (s.dataMode == Settings::DataMode::Copy && !s.force && fs::exists(temp, ec)) return abort("出力先が既にあります: " + krt::fromPath(temp));
			Sub sub(progress, 0.3, 0.5, "xp3 を作っています");
			try {
				const auto pr = xp3::pack(data, temp, po, sub);
				if (pr.canceled) { fs::remove(temp, ec); return abort("中断しました"); }
				r.notes.push_back("xp3 を作った: " + std::to_string(pr.files) + " ファイル");
			} catch (const std::exception& ex) {
				fs::remove(temp, ec);
				return abort(std::string("xp3 を作れません: ") + ex.what());
			}
			xp3 = temp;
		}
		if (s.dataMode == Settings::DataMode::Copy) {
			const fs::path dst = out.parent_path() / krt::toPath(s.dataName);
			if (xp3 != dst) {
				if (!s.force && fs::exists(dst, ec)) return abort("出力先が既にあります: " + krt::fromPath(dst));
				if (!fs::copy_file(xp3, dst, fs::copy_options::overwrite_existing, ec)) return abort("xp3 をコピーできません: " + ec.message());
			}
			r.outputs.push_back(krt::fromPath(dst));
			dataFile = dst;
			r.notes.push_back("xp3 を exe の隣に置いた: " + krt::fromPath(dst.filename()));
		} else {
			// exe の後ろに 16 バイト境界で付け足す
			std::vector<uint8_t> body;
			if (!readAll(xp3, body)) return abort("xp3 を読めません");
			if (!temp.empty()) fs::remove(temp, ec);
			std::ofstream f(out, std::ios::binary | std::ios::app);
			const uint64_t size = fs::file_size(out, ec);
			const std::vector<char> pad((size_t)((16 - size % 16) % 16), 0);
			f.write(pad.data(), (std::streamsize)pad.size());
			f.write(reinterpret_cast<const char*>(body.data()), (std::streamsize)body.size());
			if (!f) return abort("xp3 を結合できません");
			r.notes.push_back("xp3 を exe に結合した (" + std::to_string(body.size()) + " バイト、位置 " + std::to_string(size + pad.size()) + ")");
		}
	}

	// 6. 署名 (必ず最後)
	if (!privateKey.empty()) {
		progress.progress(0.9, "署名しています");
		std::vector<fs::path> targets = { out };
		if (!dataFile.empty()) targets.push_back(dataFile);
		for (const auto& t : targets) {
			const auto sr = sig::signFile(t, privateKey);
			if (!sr.ok) return abort("署名できません: " + krt::fromPath(t.filename()) + ": " + sr.message);
			if (!sr.embedded) r.outputs.push_back(sr.written);
			r.notes.push_back("署名した: " + krt::fromPath(fs::path(krt::toPath(sr.written)).filename()));
		}
	}

	progress.progress(1.0, "完了");
	r.ok = true;
	return r;
}

} // namespace krt::release
