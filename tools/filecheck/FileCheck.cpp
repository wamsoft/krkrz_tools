#include "FileCheck.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>

#include "krt/app/Progress.h"
#include "krt/app/Text.h"
#include "krt/sig/Signature.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using appserve::Json;

namespace filecheck {

namespace {

bool isValidUtf8(const std::string& s)
{
	size_t i = 0;
	while (i < s.size()) {
		const unsigned char c = (unsigned char)s[i];
		int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
		if (n < 0 || i + n >= s.size() + (n == 0 ? 1 : 0)) return false;
		for (int k = 1; k <= n; ++k)
			if (((unsigned char)s[i + k] >> 6) != 2) return false;
		i += n + 1;
	}
	return true;
}

// 旧ツールの ini は Shift_JIS。UTF-8 として読めなければ CP932 として読む
std::string toUtf8(const std::string& raw)
{
	std::string s = raw;
	if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
		s.erase(0, 3);
	if (isValidUtf8(s)) return s;
#ifdef _WIN32
	const int wn = MultiByteToWideChar(932, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(wn, L'\0');
	MultiByteToWideChar(932, 0, s.data(), (int)s.size(), w.data(), wn);
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
	std::string out(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, out.data(), n, nullptr, nullptr);
	return out;
#else
	return s;   // TODO: Windows 以外での CP932 の ini (必要になったら iconv)
#endif
}

void replaceAll(std::string& s, const std::string& from, const std::string& to)
{
	for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size())
		s.replace(p, from.size(), to);
}

int64_t toUnixMs(fs::file_time_type t)
{
	using namespace std::chrono;
	const auto sys = time_point_cast<milliseconds>(t - fs::file_time_type::clock::now() + system_clock::now());
	return sys.time_since_epoch().count();
}

std::string formatTime(int64_t unixMs)
{
	const std::time_t tt = (std::time_t)(unixMs / 1000);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &tt);
#else
	localtime_r(&tt, &tm);
#endif
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M:%S", &tm);
	return buf;
}

bool iequalsExt(const fs::path& p, const char* ext)
{
	std::string e = krt::fromPath(p.extension());
	std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return e == ext;
}

// 1 ファイルぶんの進捗を全体の進捗へ写す
class SubProgress : public krt::Progress {
public:
	SubProgress(krt::Progress& parent, double base, double span, std::string msg)
		: parent_(parent), base_(base), span_(span), msg_(std::move(msg)) {}
	void progress(double ratio, const std::string&) override
	{
		parent_.progress(base_ + span_ * (ratio < 0 ? 0 : ratio), msg_);
	}
	void log(const std::string& line) override { parent_.log(line); }
	bool canceled() const override { return parent_.canceled(); }

private:
	krt::Progress& parent_;
	double base_, span_;
	std::string msg_;
};

} // namespace

fs::path defaultConfigPath()
{
	fs::path p = krt::selfPath();
	p.replace_extension(".ini");
	return p;
}

bool loadConfig(const fs::path& ini, Config& cfg, std::string& error)
{
	std::string raw;
	if (!krt::readFile(ini, raw)) {
		error = "設定ファイルを開けません: " + krt::fromPath(ini);
		return false;
	}
	const std::string text = toUtf8(raw);
	cfg.source = krt::fromPath(ini);
	cfg.publicKey = text;   // 公開鍵は ini 全体から探す (旧ツールと同じ)

	std::istringstream in(text);
	std::string line, section;
	while (std::getline(in, line)) {
		line = krt::trim(line);
		if (line.empty() || line[0] == ';') continue;
		if (line.front() == '[' && line.back() == ']') {
			section = line.substr(1, line.size() - 2);
			continue;
		}
		if (section != "message") continue;
		const auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = krt::trim(line.substr(0, eq));
		std::string value = krt::trim(line.substr(eq + 1));
		if (key == "caption") cfg.caption = value;
		else if (key == "notice") {
			replaceAll(value, "[cr]", "\n");
			cfg.notice = value;
		}
	}
	return true;
}

Report run(const fs::path& root, const std::string& publicKey, krt::Progress& progress)
{
	Report rep;
	rep.root = krt::fromPath(root);

	// 1. 走査
	struct Target { fs::path path; std::string rel; int64_t size; int64_t mtime; bool exe; };
	std::vector<Target> targets;
	std::error_code ec;
	auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
	if (ec) throw std::runtime_error("フォルダを開けません: " + rep.root);
	for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (ec) break;
		if (progress.canceled()) { rep.canceled = true; return rep; }
		const fs::path& p = it->path();
		const std::string name = krt::fromPath(p.filename());
		if (!name.empty() && name[0] == '.') {
			if (it->is_directory(ec)) it.disable_recursion_pending();
			continue;
		}
		const int64_t mtime = toUnixMs(it->last_write_time(ec));
		if (it->is_directory(ec)) {
			rep.allList.push_back(formatTime(mtime) + "\t[directory]\t" + krt::fromPath(p));
			continue;
		}
		const int64_t size = (int64_t)it->file_size(ec);
		char sizeBuf[32];
		std::snprintf(sizeBuf, sizeof(sizeBuf), "%11lld", (long long)size);
		rep.allList.push_back(formatTime(mtime) + "\t" + sizeBuf + "\t" + krt::fromPath(p));
		++rep.allFiles;
		if ((rep.allFiles % 200) == 0)
			progress.progress(-1.0, "走査中 (" + std::to_string(rep.allFiles) + " ファイル)");

		if (iequalsExt(p, ".sig")) continue;
		bool add = false, exe = false;
		if (iequalsExt(p, ".exe") && krt::sig::hasEmbeddedSignature(p)) {
			add = exe = true;
		} else {
			fs::path s = p;
			s += ".sig";
			add = fs::exists(s, ec);
		}
		if (add) {
			targets.push_back({ p, krt::fromPath(fs::relative(p, root, ec)), size, mtime, exe });
		}
	}

	// 2. 検証 (進捗はファイルサイズで按分)
	int64_t total = 0;
	for (const auto& t : targets) total += std::max<int64_t>(t.size, 1);
	int64_t done = 0;
	for (const auto& t : targets) {
		if (progress.canceled()) { rep.canceled = true; break; }
		const double base = total ? (double)done / (double)total : 0.0;
		const double span = total ? (double)std::max<int64_t>(t.size, 1) / (double)total : 0.0;
		SubProgress sub(progress, base, span, t.rel);
		sub.progress(0.0, std::string());
		const auto v = krt::sig::verifyFile(t.path, publicKey, &sub);

		Entry e;
		e.path = t.rel;
		e.size = t.size;
		e.mtime = t.mtime;
		e.embedded = t.exe;
		e.status = krt::sig::statusName(v.status);
		e.message = v.message;
		if (v.status == krt::sig::Status::Canceled) { rep.canceled = true; break; }
		if (v.status == krt::sig::Status::Ok) ++rep.ok;
		else if (v.status == krt::sig::Status::Broken) ++rep.broken;
		else ++rep.error;
		progress.log(std::string(statusLabel(e.status)) + "\t" + e.path + (e.message.empty() ? "" : "\t" + e.message));
		rep.entries.push_back(std::move(e));
		done += std::max<int64_t>(t.size, 1);
	}
	if (!rep.canceled) progress.progress(1.0, "完了");
	return rep;
}

const char* statusLabel(const std::string& s)
{
	if (s == "ok") return "正常";
	if (s == "broken") return "破損";
	if (s == "canceled") return "中断";
	return "エラー";
}

Json toJson(const Report& r)
{
	Json j = Json::object();
	j.set("root", Json(r.root));
	j.set("allFiles", Json((long long)r.allFiles));
	j.set("ok", Json(r.ok));
	j.set("broken", Json(r.broken));
	j.set("error", Json(r.error));
	j.set("canceled", Json(r.canceled));
	Json arr = Json::array();
	for (const auto& e : r.entries) {
		Json o = Json::object();
		o.set("path", Json(e.path));
		o.set("status", Json(e.status));
		o.set("label", Json(statusLabel(e.status)));
		o.set("message", Json(e.message));
		o.set("size", Json((long long)e.size));
		o.set("mtime", Json((long long)e.mtime));
		o.set("embedded", Json(e.embedded));
		arr.push(std::move(o));
	}
	j.set("entries", std::move(arr));
	return j;
}

std::string toTsv(const Report& r)
{
	std::string s = "名前\t日付\tサイズ\t結果\n";
	for (const auto& e : r.entries) {
		s += e.path + "\t" + formatTime(e.mtime) + "\t" + std::to_string(e.size) + "\t" + statusLabel(e.status);
		if (!e.message.empty()) s += "\t" + e.message;
		s += "\n";
	}
	s += "\n全ファイル (" + std::to_string(r.allFiles) + ")\n";
	for (const auto& l : r.allList) s += l + "\n";
	return s;
}

} // namespace filecheck
