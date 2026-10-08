#include "krt/app/JobRunner.h"

#include <chrono>
#include <exception>

using appserve::Json;

namespace krt {

namespace {
long long nowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

// 処理側へ渡す Progress の実装 (別スレッドから呼ばれる)
class JobRunner::Sink : public Progress {
public:
	explicit Sink(JobRunner& r) : r_(r) {}
	void progress(double ratio, const std::string& message) override
	{
		{
			std::lock_guard<std::mutex> lk(r_.mu_);
			r_.ratio_ = ratio;
			r_.message_ = message;
		}
		r_.publish(false);
	}
	void log(const std::string& line) override
	{
		if (r_.app_) r_.app_->browser().broadcast("joblog", line);
	}
	bool canceled() const override { return r_.cancel_; }

private:
	JobRunner& r_;
};

JobRunner::~JobRunner()
{
	cancel_ = true;
	join();
}

void JobRunner::onShutdown()
{
	cancel_ = true;
	join();
}

void JobRunner::join()
{
	if (thread_.joinable()) thread_.join();
}

void JobRunner::registerApi(appserve::ApiRegistry& reg)
{
	app_ = &reg.app();
	reg.route("/api/job/cancel", appserve::Affinity::Any, [this](const appserve::Request&) {
		cancel_ = true;
		return appserve::Response::json(status());
	});
	reg.route("/api/job", appserve::Affinity::Any, [this](const appserve::Request&) {
		return appserve::Response::json(status());
	});
}

Json JobRunner::status() const
{
	std::lock_guard<std::mutex> lk(mu_);
	Json j = Json::object();
	j.set("state", Json(state_));
	j.set("name", Json(name_));
	j.set("ratio", Json(ratio_));
	j.set("message", Json(message_));
	if (state_ == "done") j.set("result", result_);
	if (!error_.empty()) j.set("error", Json(error_));
	return j;
}

void JobRunner::publish(bool force)
{
	if (!app_) return;
	const long long t = nowMs();
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (!force && t - lastPublishMs_ < 50) return;
		lastPublishMs_ = t;
	}
	app_->browser().broadcastJson("job", status());
}

bool JobRunner::start(const std::string& name, Work work)
{
	if (running_) return false;
	join();   // 前回の終わったスレッドを回収
	{
		std::lock_guard<std::mutex> lk(mu_);
		state_ = "running";
		name_ = name;
		ratio_ = -1.0;
		message_.clear();
		result_ = Json();
		error_.clear();
	}
	cancel_ = false;
	running_ = true;
	publish(true);

	thread_ = std::thread([this, work = std::move(work)]() {
		Sink sink(*this);
		Json result;
		std::string err;
		bool ok = false;
		try {
			result = work(sink);
			ok = true;
		} catch (const std::exception& e) {
			err = e.what();
		} catch (...) {
			err = "unknown error";
		}
		{
			std::lock_guard<std::mutex> lk(mu_);
			if (ok && cancel_) state_ = "canceled";
			else if (ok)       state_ = "done";
			else               state_ = "failed";
			result_ = std::move(result);
			error_ = err;
		}
		running_ = false;
		publish(true);
	});
	return true;
}

} // namespace krt
