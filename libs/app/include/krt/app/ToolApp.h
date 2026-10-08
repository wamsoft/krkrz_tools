//---------------------------------------------------------------------------
// ToolApp — ツール 1 本の入口 (GUI と CLI の振り分け)
//
//   int main(int argc, char** argv) {
//       krt::ToolApp tool({ "krkrcheck", "ファイル破損チェック", "0.1.0" });
//       tool.addOption({ "key", "FILE", "公開鍵ファイル", ... });
//       if (!tool.parseArgs(argc, argv)) return tool.exitCode();
//       if (tool.cli()) return runCli(...);           // 画面を出さずに処理して終わる
//       tool.addModule(std::make_unique<MyModule>(tool.jobs()));
//       return tool.runGui();
//   }
//
// - `--cli` を付けると CLI。付けなければブラウザの画面を開く
// - 画面側には標準で次の API がある
//     GET /api/app/info  … { id, title, version, args }
//     /api/fs/*          … フォルダ・ファイルの参照 (読み取りのみ。appserve 標準)
//     /api/job*          … JobRunner (長い処理の実行と進捗)
//---------------------------------------------------------------------------
#pragma once
#include <appserve/appserve.h>
#include <memory>
#include <string>
#include <vector>
#include "krt/app/JobRunner.h"

namespace krt {

struct ToolInfo {
	std::string id;        ///< 実行ファイル名と同じ短い名前 (krkrcheck)
	std::string title;     ///< 画面・ヘルプに出す名前
	std::string version;
};

class ToolApp {
public:
	explicit ToolApp(ToolInfo info);

	appserve::App&       app()  { return app_; }
	const ToolInfo&      info() const { return info_; }

	/// 独自オプションを足す (parseArgs より前に)
	void addOption(appserve::OptionSpec spec) { app_.addOption(std::move(spec)); }
	/// コマンドライン解析。false = 終了する (--help など)。終了コードは exitCode()
	bool parseArgs(int argc, char** argv);
	int  exitCode() const { return app_.exitCode(); }

	/// --cli が付いていたか
	bool cli() const { return cli_; }
	/// オプション以外の引数
	const std::vector<std::string>& args() const { return app_.options().args; }

	/// GUI 用のモジュールを足す
	void addModule(std::unique_ptr<appserve::IModule> m) { app_.addModule(std::move(m)); }
	/// 長い処理の実行役 (GUI のモジュールから使う)
	JobRunner& jobs() { return *jobs_; }

	/// 画面を開いて動かす。戻り値がプロセスの終了コード
	int runGui();

private:
	ToolInfo        info_;
	appserve::App   app_;
	bool            cli_ = false;
	JobRunner*      jobs_ = nullptr;   // app_ が所有
	std::unique_ptr<JobRunner> jobsOwned_;
};

} // namespace krt
