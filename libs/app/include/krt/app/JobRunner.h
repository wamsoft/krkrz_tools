//---------------------------------------------------------------------------
// JobRunner — GUI で長い処理を 1 本ずつ別スレッドで回す
//
// appserve のハンドラはメインスレッドで動くので、重い処理をそのまま
// 呼ぶと画面からの他のリクエストが止まる。ここで処理を別スレッドへ出し、
// 進捗とログを SSE で配信する。
//
//   API:
//     GET  /api/job          … 現在の状態 (state / name / ratio / message / result / error)
//     POST /api/job/cancel   … 中断を求める
//   SSE:
//     job  … 状態が変わるたび (進捗は 50ms 以上の間隔に間引く)
//     joblog … 処理が出したログ 1 行ずつ
//
// state は "idle" / "running" / "done" / "failed" / "canceled"。
//---------------------------------------------------------------------------
#pragma once
#include <appserve/appserve.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "krt/app/Progress.h"

namespace krt {

class JobRunner : public appserve::IModule {
public:
	/// 処理本体。結果は JSON で返す (画面がそのまま使う)。例外は失敗として扱う
	using Work = std::function<appserve::Json(Progress&)>;

	JobRunner() = default;
	~JobRunner() override;

	const char* name() const override { return "krt.job"; }
	void registerApi(appserve::ApiRegistry& reg) override;
	void onStart(appserve::App& app) override { app_ = &app; }
	void onShutdown() override;

	/// 処理を始める。既に動いていれば false
	bool start(const std::string& name, Work work);
	bool running() const { return running_; }

	/// 現在の状態 (GET /api/job と同じもの)
	appserve::Json status() const;

private:
	class Sink;
	void publish(bool force);
	void join();

	appserve::App*      app_ = nullptr;
	mutable std::mutex  mu_;
	std::thread         thread_;
	std::atomic<bool>   running_{false};
	std::atomic<bool>   cancel_{false};
	std::string         state_ = "idle";
	std::string         name_;
	double              ratio_ = -1.0;
	std::string         message_;
	appserve::Json      result_;
	std::string         error_;
	long long           lastPublishMs_ = 0;
};

} // namespace krt
