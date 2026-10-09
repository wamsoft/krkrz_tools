//---------------------------------------------------------------------------
// krkrcheck — ファイル破損チェックツール
//
//   krkrcheck                      … 画面を開く (対象 = 実行ファイルのフォルダ)
//   krkrcheck <フォルダ>            … 画面を開く (対象フォルダを指定)
//   krkrcheck --cli [<フォルダ>] [--key=<ファイル>] [--json|--tsv] [--quiet]
//
// 公開鍵と画面の文言は «実行ファイル名.ini» から読む (吉里吉里2 の
// ファイル破損チェックツールの ini をそのまま使える)。--key= で別の
// ファイル (ini でも PEM 単体でもよい) を指定できる。
//
// CLI の終了コード: 0 = すべて正常 / 1 = 破損あり / 2 = 検証できないファイルあり・失敗
//---------------------------------------------------------------------------
#include <cstdio>
#include <memory>
#include <mutex>

#include "FileCheck.h"
#include "krt/app/Text.h"
#include "krt/app/ToolApp.h"

namespace fs = std::filesystem;
using appserve::Json;

#ifndef KRT_VERSION
#define KRT_VERSION "0.1.0"
#endif

namespace {

struct Settings {
	std::string keyFile;     ///< --key
	bool        json = false;
	bool        tsv = false;
	bool        quiet = false;
};

fs::path resolveRoot(const std::vector<std::string>& args)
{
	if (!args.empty()) return krt::toPath(args.front());
	return krt::selfPath().parent_path();
}

bool loadSettingsConfig(const Settings& s, filecheck::Config& cfg, std::string& err)
{
	const fs::path ini = s.keyFile.empty() ? filecheck::defaultConfigPath() : krt::toPath(s.keyFile);
	return filecheck::loadConfig(ini, cfg, err);
}

int runCli(const Settings& s, const fs::path& root)
{
	filecheck::Config cfg;
	std::string err;
	std::error_code ec;
	if (s.keyFile.empty() && !fs::exists(filecheck::defaultConfigPath(), ec)) {
		std::fprintf(stderr,
			"error: 公開鍵がありません。«%s» (実行ファイルと同じフォルダ) を置くか、--key=<ini または PEM> を指定してください\n"
			"usage: krkrcheck --cli [<フォルダ>] [--key=<ファイル>] [--json|--tsv] [--quiet]\n",
			krt::fromPath(filecheck::defaultConfigPath()).c_str());
		return 2;
	}
	if (!loadSettingsConfig(s, cfg, err)) {
		std::fprintf(stderr, "error: %s\n", err.c_str());
		return 2;
	}
	krt::ConsoleProgress progress(s.quiet);
	filecheck::Report rep;
	try {
		rep = filecheck::run(root, cfg.publicKey, progress);
	} catch (const std::exception& e) {
		progress.finish();
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}
	progress.finish();

	if (s.json) {
		std::printf("%s\n", filecheck::toJson(rep).dump(1).c_str());
	} else if (s.tsv) {
		std::printf("%s", filecheck::toTsv(rep).c_str());
	} else {
		std::printf("正常 %d / 破損 %d / エラー %d (対象 %zu / 全ファイル %lld)\n",
		            rep.ok, rep.broken, rep.error, rep.entries.size(), (long long)rep.allFiles);
	}
	if (rep.broken) return 1;
	if (rep.error || rep.canceled) return 2;
	return 0;
}

// 画面向けの API
class CheckModule : public appserve::IModule {
public:
	CheckModule(krt::JobRunner& jobs, Settings s, fs::path root)
		: jobs_(jobs), settings_(std::move(s)), root_(std::move(root)) {}

	const char* name() const override { return "krkrcheck"; }

	void registerApi(appserve::ApiRegistry& reg) override
	{
		// 画面の文言・既定のフォルダ・鍵の状態
		reg.route("/api/check/config", [this](const appserve::Request&) {
			filecheck::Config cfg;
			std::string err;
			const bool ok = loadSettingsConfig(settings_, cfg, err);
			Json j = Json::object();
			j.set("caption", Json(cfg.caption));
			j.set("notice", Json(cfg.notice));
			j.set("root", Json(krt::fromPath(root_)));
			j.set("keySource", Json(cfg.source));
			j.set("keyLoaded", Json(ok));
			if (!ok) j.set("error", Json(err));
			return appserve::Response::json(j);
		});

		// 検証を始める (body: { root })
		reg.route("/api/check/start", [this](const appserve::Request& req) {
			Json body;
			Json::parse(req.body, body);
			fs::path root = root_;
			if (body.isObj() && body.has("root") && !body["root"].asStr().empty())
				root = krt::toPath(body["root"].asStr());

			filecheck::Config cfg;
			std::string err;
			if (!loadSettingsConfig(settings_, cfg, err))
				return appserve::Response::error(400, err);

			const bool started = jobs_.start("check", [this, root, key = cfg.publicKey](krt::Progress& p) {
				filecheck::Report rep = filecheck::run(root, key, p);
				Json j = filecheck::toJson(rep);
				std::lock_guard<std::mutex> lk(mu_);
				last_ = std::make_shared<filecheck::Report>(std::move(rep));
				return j;
			});
			if (!started) return appserve::Response::error(409, "実行中です");
			return appserve::Response::json(jobs_.status());
		});

		// 直近の結果を TSV で (「結果をコピー」用)
		reg.route("/api/check/report.tsv", [this](const appserve::Request&) {
			std::lock_guard<std::mutex> lk(mu_);
			if (!last_) return appserve::Response::error(404, "まだ結果がありません");
			return appserve::Response::text(filecheck::toTsv(*last_));
		});
	}

private:
	krt::JobRunner& jobs_;
	Settings        settings_;
	fs::path        root_;
	std::mutex      mu_;
	std::shared_ptr<filecheck::Report> last_;
};

} // namespace

int main(int argc, char** argv)
{
	krt::ToolApp tool({ "krkrcheck", "ファイル破損チェックツール", KRT_VERSION });
	Settings s;
	tool.addOption({ "key", "FILE", "公開鍵 (ini または PEM)。既定は «実行ファイル名.ini»",
	                 [&s](const std::string& v) { s.keyFile = v; return true; } });
	tool.addOption({ "json", "", "結果を JSON で出す (--cli)",
	                 [&s](const std::string&) { s.json = true; return true; } });
	tool.addOption({ "tsv", "", "結果を TSV で出す (--cli)",
	                 [&s](const std::string&) { s.tsv = true; return true; } });
	tool.addOption({ "quiet", "", "進捗を出さない (--cli)",
	                 [&s](const std::string&) { s.quiet = true; return true; } });
	if (!tool.parseArgs(argc, argv)) return tool.exitCode();

	const fs::path root = resolveRoot(tool.args());
	if (tool.cli()) return runCli(s, root);

	tool.addModule(std::make_unique<CheckModule>(tool.jobs(), s, root));
	return tool.runGui();
}
