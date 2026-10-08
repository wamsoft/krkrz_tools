//---------------------------------------------------------------------------
// krkrloop — ループチューナ (旧 looptune / LoopTuner2 の後継)
//
//   krkrloop [<音声ファイル>]                 … 画面を開く
//   krkrloop --cli info  <音声ファイル>...    … 長さと «ファイル.sli» の内容を表示
//   krkrloop --cli check <音声ファイル>...    … .sli が本体で読めて、位置が音声の範囲内か調べる
//
// 音声は WAV / Ogg Vorbis / Ogg Opus。ループ情報は «音声ファイル.sli» (本体の
// WaveLoopManager と同じ書式。読み込みは libs/loop の移植版)。
//
// 画面: 波形の表示・拡大縮小、リンク (From → To、Smooth、条件) とラベルの追加・
// 移動・編集、元に戻す / やり直し、保存。再生はブラウザの AudioWorklet で、本体の
// WaveLoopManager::Decode と同じ規則でリンクをたどる (サンプル単位で正確、
// Smooth は 50ms のクロスフェード、«:» で始まるラベルでフラグを操作)。
// WAV 書き出し: 選択範囲 / 全体、またはリンクをたどって再生した音 (ループを展開した音)。
//
// CLI の終了コード: 0 = 成功 / 1 = check で問題あり / 2 = 失敗
//---------------------------------------------------------------------------
#include <cstdio>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>

#include "krt/app/Text.h"
#include "krt/app/ToolApp.h"
#include "krt/audio/Audio.h"
#include "krt/loop/Sli.h"

namespace fs = std::filesystem;
using appserve::Json;
namespace au = krt::audio;
namespace lp = krt::loop;

#ifndef KRT_VERSION
#define KRT_VERSION "0.1.0"
#endif

namespace {

struct Settings {
	bool json = false;
};

fs::path sliPathOf(const fs::path& audio)
{
	fs::path p = audio;
	p += ".sli";
	return p;
}

Json sliToJson(const lp::Sli& s)
{
	Json links = Json::array();
	for (const auto& k : s.links) {
		Json o = Json::object();
		o.set("from", Json((long long)k.from));
		o.set("to", Json((long long)k.to));
		o.set("smooth", Json(k.smooth));
		o.set("condition", Json(lp::conditionName(k.condition)));
		o.set("refValue", Json(k.refValue));
		o.set("condVar", Json(k.condVar));
		links.push(std::move(o));
	}
	Json labels = Json::array();
	for (const auto& b : s.labels) {
		Json o = Json::object();
		o.set("position", Json((long long)b.position));
		o.set("name", Json(b.name));
		labels.push(std::move(o));
	}
	Json j = Json::object();
	j.set("links", std::move(links));
	j.set("labels", std::move(labels));
	return j;
}

bool sliFromJson(const Json& j, lp::Sli& s, std::string& err)
{
	s = lp::Sli();
	for (size_t i = 0; i < j["links"].size(); ++i) {
		const Json& o = j["links"][i];
		lp::Link k;
		k.from = o["from"].asInt(0);
		k.to = o["to"].asInt(0);
		k.smooth = o["smooth"].asBool();
		if (!lp::conditionFromName(o["condition"].asStr("no"), k.condition)) { err = "条件の指定が不正です"; return false; }
		k.refValue = (int)o["refValue"].asInt(0);
		k.condVar = (int)o["condVar"].asInt(0);
		s.links.push_back(k);
	}
	for (size_t i = 0; i < j["labels"].size(); ++i) {
		const Json& o = j["labels"][i];
		lp::Label b;
		b.position = o["position"].asInt(0);
		b.name = o["name"].asStr();
		if (b.name.find('\'') != std::string::npos) { err = "ラベル名に ' は使えません"; return false; }
		s.labels.push_back(b);
	}
	return true;
}

/// .sli の内容を音声の長さに照らして調べる。問題の一覧を返す
std::vector<std::string> checkSli(const lp::Sli& s, int64_t frames)
{
	std::vector<std::string> out;
	char buf[256];
	for (size_t i = 0; i < s.links.size(); ++i) {
		const auto& k = s.links[i];
		if (k.from < 0 || k.from > frames || k.to < 0 || k.to > frames) {
			std::snprintf(buf, sizeof(buf), "リンク %zu (%lld → %lld) が音声の範囲 (0〜%lld) の外です", i,
			              (long long)k.from, (long long)k.to, (long long)frames);
			out.push_back(buf);
		}
		if (k.condition != lp::Condition::None && (k.condVar < 0 || k.condVar >= 16)) {
			std::snprintf(buf, sizeof(buf), "リンク %zu のフラグ番号 %d が 0〜15 の外です", i, k.condVar);
			out.push_back(buf);
		}
		if (k.from == k.to && k.condition == lp::Condition::None) {
			std::snprintf(buf, sizeof(buf), "リンク %zu は From と To が同じ (%lld) です。無条件だと先へ進めません", i, (long long)k.from);
			out.push_back(buf);
		}
	}
	for (size_t i = 0; i < s.labels.size(); ++i) {
		const auto& b = s.labels[i];
		if (b.position < 0 || b.position > frames) {
			std::snprintf(buf, sizeof(buf), "ラベル %zu «%s» (%lld) が音声の範囲の外です", i, b.name.c_str(), (long long)b.position);
			out.push_back(buf);
		}
	}
	return out;
}

int runCli(const Settings& s, const std::vector<std::string>& args)
{
	if (args.size() < 2) {
		std::fprintf(stderr, "usage: krkrloop --cli info|check <音声ファイル>... (--help 参照)\n");
		return 2;
	}
	const std::string cmd = args[0];
	if (cmd != "info" && cmd != "check") { std::fprintf(stderr, "error: 不明なコマンド: %s\n", cmd.c_str()); return 2; }
	int rc = 0;
	Json arr = Json::array();
	for (size_t i = 1; i < args.size(); ++i) {
		const std::string& f = args[i];
		au::Pcm pcm;
		au::Info info;
		std::string err;
		Json o = Json::object();
		o.set("path", Json(f));
		if (!au::decode(krt::toPath(f), pcm, info, err)) {
			std::fprintf(stderr, "error: %s: %s\n", f.c_str(), err.c_str());
			rc = 2;
			continue;
		}
		const int64_t frames = (int64_t)pcm.frames();
		o.set("sampleRate", Json(pcm.sampleRate));
		o.set("frames", Json((long long)frames));
		const fs::path sp = sliPathOf(krt::toPath(f));
		std::error_code ec;
		lp::Sli sli;
		bool hasSli = false;
		std::vector<std::string> problems;
		if (fs::exists(sp, ec)) {
			std::string text;
			if (!krt::readFile(sp, text) || !lp::parse(text, sli, err)) problems.push_back(".sli を本体が読めません: " + err);
			else { hasSli = true; problems = checkSli(sli, frames); }
		}
		if (hasSli) o.set("sli", sliToJson(sli));
		Json pj = Json::array();
		for (const auto& p : problems) pj.push(Json(p));
		o.set("problems", std::move(pj));
		if (!problems.empty() && rc == 0) rc = 1;
		if (s.json) { arr.push(std::move(o)); continue; }

		std::printf("%s: %d Hz %lld サンプル (%.3f 秒)%s\n", f.c_str(), pcm.sampleRate, (long long)frames,
		            (double)frames / pcm.sampleRate, hasSli ? "" : (fs::exists(sp, ec) ? "" : "  .sli なし"));
		if (cmd == "info" && hasSli) {
			for (const auto& k : sli.links)
				std::printf("    Link  %lld → %lld%s%s\n", (long long)k.from, (long long)k.to, k.smooth ? " Smooth" : "",
				            k.condition == lp::Condition::None ? "" :
				            (std::string(" [") + std::to_string(k.condVar) + "] " + lp::conditionName(k.condition) + " " + std::to_string(k.refValue)).c_str());
			for (const auto& b : sli.labels)
				std::printf("    Label %lld «%s»\n", (long long)b.position, b.name.c_str());
		}
		for (const auto& p : problems) std::printf("    問題: %s\n", p.c_str());
		if (cmd == "check" && problems.empty()) std::printf("    問題なし\n");
	}
	if (s.json) std::printf("%s\n", arr.dump(1).c_str());
	return rc;
}

// 画面向けの API。開いている音声は 1 つ
class LoopModule : public appserve::IModule {
public:
	const char* name() const override { return "krkrloop"; }

	void registerApi(appserve::ApiRegistry& reg) override
	{
		// 開く (body: { file }) → { path, sampleRate, channels, frames, format, sli?, sliError? }
		reg.route("/api/loop/open", appserve::Affinity::Any, [this](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			const std::string f = b["file"].asStr();
			auto pcm = std::make_shared<au::Pcm>();
			au::Info info;
			std::string err;
			if (!au::decode(krt::toPath(f), *pcm, info, err)) return appserve::Response::error(400, err);
			// Opus のヘッダゲインは本体が常に掛けるので、聞く音もそれに合わせる
			if (info.format == au::Format::Opus) au::applyGain(*pcm, info.opusHeaderGainQ8 / 256.0);
			{
				std::lock_guard<std::mutex> lk(mu_);
				pcm_ = pcm;
			}
			Json j = Json::object();
			j.set("path", Json(f));
			j.set("format", Json(au::formatName(info.format)));
			j.set("sampleRate", Json(pcm->sampleRate));
			j.set("channels", Json(pcm->channels));
			j.set("frames", Json((long long)pcm->frames()));
			const fs::path sp = sliPathOf(krt::toPath(f));
			j.set("sliPath", Json(krt::fromPath(sp)));
			std::error_code ec;
			if (fs::exists(sp, ec)) {
				std::string text;
				lp::Sli sli;
				if (krt::readFile(sp, text) && lp::parse(text, sli, err)) j.set("sli", sliToJson(sli));
				else j.set("sliError", Json(".sli を読めません (本体も読めない書式です): " + err));
			}
			return appserve::Response::json(j);
		});

		// 開いている音声の PCM (32bit float、チャンネル交互、リトルエンディアン)。
		// 書き出しで元の精度を保つため、間引かずにそのまま渡す
		reg.route("/api/loop/pcm", appserve::Affinity::Any, [this](const appserve::Request&) {
			std::shared_ptr<au::Pcm> pcm;
			{
				std::lock_guard<std::mutex> lk(mu_);
				pcm = pcm_;
			}
			if (!pcm) return appserve::Response::error(404, "音声を開いていません");
			std::string out(pcm->data.size() * sizeof(float), '\0');
			std::memcpy(out.data(), pcm->data.data(), out.size());
			return appserve::Response::bytes(std::move(out), "application/octet-stream");
		});

		// WAV 書き出し (クエリ: out, rate, channels, bits, force。本文: 32bit float のチャンネル交互)
		reg.route("/api/loop/wav", appserve::Affinity::Any, [](const appserve::Request& req) {
			const fs::path out = krt::toPath(req.param("out"));
			const int rate = std::atoi(req.param("rate").c_str());
			const int ch = std::atoi(req.param("channels").c_str());
			const int bits = std::atoi(req.param("bits", "16").c_str());
			const bool force = req.param("force") == "1";
			if (out.empty() || rate <= 0 || ch <= 0 || (bits != 16 && bits != 24 && bits != 32))
				return appserve::Response::error(400, "指定が不正です");
			if (req.body.size() % (sizeof(float) * ch) != 0) return appserve::Response::error(400, "データの長さが不正です");
			std::error_code ec;
			if (!force && fs::exists(out, ec)) return appserve::Response::error(409, "出力先が既にあります: " + krt::fromPath(out));
			au::Pcm pcm;
			pcm.sampleRate = rate;
			pcm.channels = ch;
			pcm.data.resize(req.body.size() / sizeof(float));
			std::memcpy(pcm.data.data(), req.body.data(), req.body.size());
			au::EncodeOptions eo;
			eo.format = au::Format::Wav;
			eo.wavBits = bits;
			std::string err;
			if (!au::encode(pcm, out, eo, nullptr, err)) return appserve::Response::error(400, err);
			Json j = Json::object();
			j.set("path", Json(krt::fromPath(out)));
			j.set("frames", Json((long long)pcm.frames()));
			return appserve::Response::json(j);
		});

		// 保存 (body: { file, links, labels, frames }) → { sliPath, problems }
		reg.route("/api/loop/save", appserve::Affinity::Any, [](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			lp::Sli sli;
			std::string err;
			if (!sliFromJson(b, sli, err)) return appserve::Response::error(400, err);
			const std::string text = lp::write(sli);
			// 書いたものを本体と同じ規則で読み直せることを確かめる
			lp::Sli back;
			if (!lp::parse(text, back, err)) return appserve::Response::error(400, "本体が読めない内容になります: " + err);
			const fs::path sp = sliPathOf(krt::toPath(b["file"].asStr()));
			if (!krt::writeFile(sp, text)) return appserve::Response::error(400, "書き込めません: " + krt::fromPath(sp));
			Json j = Json::object();
			j.set("sliPath", Json(krt::fromPath(sp)));
			Json pj = Json::array();
			for (const auto& p : checkSli(sli, b["frames"].asInt(INT64_MAX))) pj.push(Json(p));
			j.set("problems", std::move(pj));
			return appserve::Response::json(j);
		});
	}

private:
	std::mutex mu_;
	std::shared_ptr<au::Pcm> pcm_;
};

} // namespace

int main(int argc, char** argv)
{
	krt::ToolApp tool({ "krkrloop", "ループチューナ", KRT_VERSION });
	Settings s;
	tool.addOption({ "json", "", "結果を JSON で出す",
	                 [&s](const std::string&) { s.json = true; return true; } });
	if (!tool.parseArgs(argc, argv)) return tool.exitCode();

	if (tool.cli()) return runCli(s, tool.args());

	tool.addModule(std::make_unique<LoopModule>());
	return tool.runGui();
}
