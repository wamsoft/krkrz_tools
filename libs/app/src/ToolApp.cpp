#include "krt/app/ToolApp.h"

#include "krt/app/Text.h"

#ifdef _WIN32
#include <windows.h>
#endif

using appserve::Json;

namespace krt {

ToolApp::ToolApp(ToolInfo info) : info_(std::move(info))
{
	app_.options().appName    = info_.id;
	app_.options().appVersion = info_.version;
	jobsOwned_ = std::make_unique<JobRunner>();
	jobs_ = jobsOwned_.get();

	app_.addOption({
		"cli", "", "画面を開かずにコマンドラインで処理して終了する",
		[this](const std::string&) { cli_ = true; return true; }
	});
}

bool ToolApp::parseArgs(int argc, char** argv)
{
#ifdef _WIN32
	// 結果やログは UTF-8 で出す
	SetConsoleOutputCP(CP_UTF8);
#endif
	return app_.parseArgs(argc, argv);
}

int ToolApp::runGui()
{
	hideConsoleIfOwned();

	// 画面がフォルダやファイルを選ぶための参照 API (読み取りのみ)
	app_.addModule(appserve::makeFsModule());
	if (jobsOwned_) app_.addModule(std::move(jobsOwned_));

	app_.registry().route("/api/app/info", appserve::Affinity::Any,
		[this](const appserve::Request&) {
			Json j = Json::object();
			j.set("id", Json(info_.id));
			j.set("title", Json(info_.title));
			j.set("version", Json(info_.version));
			j.set("self", Json(fromPath(selfPath())));
			j.set("args", Json::array(app_.options().args));
			return appserve::Response::json(j);
		});
	return app_.run();
}

} // namespace krt
