//---------------------------------------------------------------------------
// krkrrelease — リリーサ (配布用の実行可能ファイルを作る。Windows 専用)
//
//   krkrrelease [<release.json>]                     … 画面を開く
//   krkrrelease --cli run      <release.json> [--force]
//   krkrrelease --cli info     <exe>                 … 埋め込みオプション・セキュリティ設定・版情報を表示
//   krkrrelease --cli template <release.json> [--exe=<吉里吉里の exe>]   … 設定ファイルのひな形を書く
//
// 吉里吉里Z の exe (WINVER / SDL の Windows 版) をコピーして、埋め込みオプション・
// アイコン・バージョン情報・セキュリティ設定を書き換え、xp3 を隣に置くか結合し、
// 最後に署名 (.sig) を作る。手順と設定の意味は libs/release/include/krt/release/Release.h。
// 設定ファイル (JSON) の相対パスは、設定ファイルのフォルダ基準。
//
// CLI の終了コード: 0 = 成功 / 2 = 失敗
//---------------------------------------------------------------------------
#include <cstdio>
#include <memory>

#include "krt/app/Text.h"
#include "krt/app/ToolApp.h"
#include "krt/release/Release.h"

namespace fs = std::filesystem;
using appserve::Json;
namespace rl = krt::release;

#ifndef KRT_VERSION
#define KRT_VERSION "0.1.0"
#endif

namespace {

struct CliSettings {
	bool force = false;
	bool json = false;
	bool quiet = false;
	std::string exe;
};

Json infoToJson(const std::string& path, const rl::ExeInfo& i)
{
	Json o = Json::object();
	o.set("path", Json(path));
	o.set("kind", Json(std::string(rl::kindName(i.kind))));
	o.set("fileSize", Json((long long)i.fileSize));
	o.set("imageEnd", Json((long long)i.imageEnd));
	o.set("hasOverlay", Json(i.hasOverlay));
	o.set("options", Json(i.options));
	o.set("hasIcon", Json(i.hasIcon));
	Json v = Json::object();
	for (const auto& [k, val] : i.version) v.set(k, Json(val));
	o.set("version", std::move(v));
	Json s = Json::object();
	for (const auto& [k, val] : i.security) s.set(k, Json((long long)val));
	o.set("security", std::move(s));
	return o;
}

Json resultToJson(const rl::Result& r)
{
	Json o = Json::object();
	o.set("ok", Json(r.ok));
	o.set("message", Json(r.message));
	Json outs = Json::array();
	for (const auto& x : r.outputs) outs.push(Json(x));
	o.set("outputs", std::move(outs));
	Json notes = Json::array();
	for (const auto& x : r.notes) notes.push(Json(x));
	o.set("notes", std::move(notes));
	return o;
}

int runCli(const CliSettings& cs, const std::vector<std::string>& args)
{
	if (args.size() < 2) {
		std::fprintf(stderr, "usage: krkrrelease --cli run|info|template <ファイル> (--help 参照)\n");
		return 2;
	}
	const std::string cmd = args[0];
	const fs::path file = krt::toPath(args[1]);
	std::string err;
	if (cmd == "info") {
		rl::ExeInfo info;
		if (!rl::inspect(file, info, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
		if (cs.json) { std::printf("%s\n", infoToJson(args[1], info).dump(1).c_str()); return 0; }
		std::printf("%s: %s 版", args[1].c_str(), info.kind == rl::ExeKind::Winver ? "WINVER" : "SDL");
		if (info.hasOverlay) std::printf("  (後ろにデータあり: %llu バイト)", (unsigned long long)(info.fileSize - info.imageEnd));
		std::printf("\n  セキュリティ設定:");
		for (const auto& [k, v] : info.security) std::printf(" %s(%d)", k.c_str(), v);
		std::printf("\n  アイコン: %s\n  バージョン情報:\n", info.hasIcon ? "あり" : "なし");
		for (const auto& [k, v] : info.version) std::printf("    %s = %s\n", k.c_str(), v.c_str());
		std::printf("  埋め込みオプション:%s\n", info.options.empty() ? " (なし)" : "");
		if (!info.options.empty()) std::printf("%s\n", info.options.c_str());
		return 0;
	}
	if (cmd == "template") {
		std::error_code ec;
		if (!cs.force && fs::exists(file, ec)) { std::fprintf(stderr, "error: 既にあります (--force で上書き)\n"); return 2; }
		rl::Settings s;
		s.exe = cs.exe.empty() ? "krkrz64.exe" : cs.exe;
		s.output = "release/game.exe";
		s.setOptions = true;
		s.options = "; 起動オプション (.cf と同じ書式。1 行 1 オプション、; で始まる行はコメント)\n";
		s.icon = "";
		s.version = { { "FileDescription", "" }, { "ProductName", "" }, { "CompanyName", "" },
		              { "LegalCopyright", "" }, { "FileVersion", "" }, { "ProductVersion", "" } };
		s.security = { { "forcedataxp3", 1 }, { "acceptfilenameargument", 0 } };
		s.dataMode = rl::Settings::DataMode::Copy;
		s.data = "data";
		if (!rl::saveSettings(file, s, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
		std::printf("書き出しました: %s\n", args[1].c_str());
		return 0;
	}
	if (cmd == "run") {
		rl::Settings s;
		if (!rl::loadSettings(file, s, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
		if (cs.force) s.force = true;
		krt::ConsoleProgress progress(cs.quiet);
		const rl::Result r = rl::run(s, fs::absolute(file).parent_path(), progress);
		progress.finish();
		if (cs.json) { std::printf("%s\n", resultToJson(r).dump(1).c_str()); return r.ok ? 0 : 2; }
		if (!r.ok) { std::fprintf(stderr, "error: %s\n", r.message.c_str()); return 2; }
		for (const auto& n : r.notes) std::printf("  %s\n", n.c_str());
		std::printf("作成しました:\n");
		for (const auto& o : r.outputs) std::printf("  %s\n", o.c_str());
		return 0;
	}
	std::fprintf(stderr, "error: 不明なコマンド: %s\n", cmd.c_str());
	return 2;
}

// 画面向けの API
class ReleaseModule : public appserve::IModule {
public:
	explicit ReleaseModule(krt::JobRunner& jobs) : jobs_(jobs) {}
	const char* name() const override { return "krkrrelease"; }

	void registerApi(appserve::ApiRegistry& reg) override
	{
		// exe を調べる (body: { exe, base })
		reg.route("/api/release/inspect", appserve::Affinity::Any, [](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			fs::path p = krt::toPath(b["exe"].asStr());
			if (p.is_relative() && !b["base"].asStr().empty()) p = krt::toPath(b["base"].asStr()) / p;
			rl::ExeInfo info;
			std::string err;
			if (!rl::inspect(p, info, err)) return appserve::Response::error(400, err);
			return appserve::Response::json(infoToJson(krt::fromPath(p), info));
		});
		// 設定ファイルを読む (body: { file })
		reg.route("/api/release/load", appserve::Affinity::Any, [](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			std::string text;
			if (!krt::readFile(krt::toPath(b["file"].asStr()), text)) return appserve::Response::error(400, "設定ファイルを開けません");
			rl::Settings s;
			std::string err;
			if (!rl::settingsFromJsonText(text, s, err)) return appserve::Response::error(400, err);
			return appserve::Response::json(Json::parse(rl::settingsToJsonText(s)));
		});
		// 設定ファイルに書く (body: { file, settings })
		reg.route("/api/release/save", appserve::Affinity::Any, [](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			rl::Settings s;
			std::string err;
			if (!rl::settingsFromJsonText(b["settings"].dump(), s, err)) return appserve::Response::error(400, err);
			if (!rl::saveSettings(krt::toPath(b["file"].asStr()), s, err)) return appserve::Response::error(400, err);
			return appserve::Response::json(Json::object());
		});
		// リリース (body: { settings, base, force })
		reg.route("/api/release/run", [this](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			rl::Settings s;
			std::string err;
			if (!rl::settingsFromJsonText(b["settings"].dump(), s, err)) return appserve::Response::error(400, err);
			s.force = b["force"].asBool();
			const fs::path base = krt::toPath(b["base"].asStr());
			const bool started = jobs_.start("release", [s, base](krt::Progress& p) {
				return resultToJson(rl::run(s, base, p));
			});
			if (!started) return appserve::Response::error(409, "実行中です");
			return appserve::Response::json(jobs_.status());
		});
	}

private:
	krt::JobRunner& jobs_;
};

} // namespace

int main(int argc, char** argv)
{
	krt::ToolApp tool({ "krkrrelease", "リリーサ", KRT_VERSION });
	CliSettings cs;
	tool.addOption({ "force", "", "出力先・設定ファイルを上書きする",
	                 [&cs](const std::string&) { cs.force = true; return true; } });
	tool.addOption({ "exe", "FILE", "template: 元にする吉里吉里の exe",
	                 [&cs](const std::string& v) { cs.exe = v; return true; } });
	tool.addOption({ "json", "", "結果を JSON で出す",
	                 [&cs](const std::string&) { cs.json = true; return true; } });
	tool.addOption({ "quiet", "", "進捗を出さない",
	                 [&cs](const std::string&) { cs.quiet = true; return true; } });
	if (!tool.parseArgs(argc, argv)) return tool.exitCode();

	if (tool.cli()) return runCli(cs, tool.args());

	tool.addModule(std::make_unique<ReleaseModule>(tool.jobs()));
	return tool.runGui();
}
