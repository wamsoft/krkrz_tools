//---------------------------------------------------------------------------
// krkraudio — 音声フォーマットコンバータ
//
//   krkraudio                                         … 画面を開く
//   krkraudio --cli info     <ファイル>...
//   krkraudio --cli convert  <ファイル>... --to=ogg|opus|wav [--out=DIR]
//                            [--quality=Q] [--bitrate=KBPS] [--bits=16|24|32]
//                            [--gain=DB] [--normalize=LUFS] [--replaygain] [--force]
//   krkraudio --cli loudness <ファイル>...
//   krkraudio --cli lipsync  <ファイル> [--fps=30] [--format=json|csv] [--out=FILE]
//
// 入力は WAV / Ogg Vorbis / Ogg Opus。«ファイル.sli» (ループ情報) があれば変換先にも
// 書き出す (Opus は 48kHz なので、サンプルレートが変わるときは位置を換算する)。
//
// 音量の扱い:
//   --gain=DB       音量を変える。Opus はヘッダゲイン (波形は変えない。本体は常に適用)、
//                   Vorbis / WAV は波形に焼き込む
//   --normalize=L   統合ラウドネスが L LUFS になるよう --gain に加算する
//   --replaygain    Vorbis に REPLAYGAIN_TRACK_GAIN / PEAK を書く
//                   (本体を -ogg_rg=track で起動したときだけ効く)
//   Opus を Opus 以外へ変換するときは、元のヘッダゲインを波形に焼き込む。
//
// CLI の終了コード: 0 = 成功 / 2 = 失敗
//---------------------------------------------------------------------------
#include <cmath>
#include <cstdio>
#include <memory>

#include "krt/app/Text.h"
#include "krt/app/ToolApp.h"
#include "krt/audio/Audio.h"
#include "krt/loop/Sli.h"

namespace fs = std::filesystem;
using appserve::Json;
namespace au = krt::audio;

#ifndef KRT_VERSION
#define KRT_VERSION "0.1.0"
#endif

namespace {

struct ConvertSettings {
	std::string to;              ///< ogg / opus / wav
	std::string outDir;
	double      quality = 0.4;   ///< Vorbis
	int         bitrate = 0;     ///< Opus kbps
	int         bits = 16;       ///< WAV
	double      gain = 0.0;
	bool        hasNormalize = false;
	double      normalize = -18.0;
	bool        replayGain = false;
	bool        force = false;
};

struct Settings {
	ConvertSettings conv;
	double      fps = 30.0;
	std::string lipFormat = "json";
	std::string out;
	bool        json = false;
	bool        quiet = false;
};

Json infoJson(const std::string& path, const au::Info& in)
{
	Json o = Json::object();
	o.set("path", Json(path));
	o.set("format", Json(au::formatName(in.format)));
	o.set("sampleRate", Json(in.sampleRate));
	o.set("channels", Json(in.channels));
	o.set("frames", Json((long long)in.frames));
	o.set("seconds", Json(in.seconds()));
	if (in.format == au::Format::Wav) {
		o.set("bits", Json(in.bitsPerSample));
		o.set("float", Json(in.floatSamples));
	}
	if (in.format == au::Format::Opus) o.set("headerGainDb", Json(in.opusHeaderGainQ8 / 256.0));
	if (in.format == au::Format::Vorbis) o.set("nominalBitrate", Json(in.nominalBitrate));
	Json tags = Json::object();
	for (const auto& [k, v] : in.tags) tags.set(k, Json(v));
	o.set("tags", std::move(tags));
	return o;
}

/// 1 ファイルを変換する。結果は JSON (path / output / ok / message ほか)
Json convertOne(const fs::path& src, const ConvertSettings& cs, krt::Progress& p)
{
	Json r = Json::object();
	r.set("path", Json(krt::fromPath(src)));
	auto fail = [&](const std::string& m) { r.set("ok", Json(false)); r.set("message", Json(m)); return r; };

	const au::Format to = au::formatFromName(cs.to);
	if (to == au::Format::Unknown) return fail("--to には ogg / opus / wav を指定してください");

	au::Pcm pcm;
	au::Info info;
	std::string err;
	p.progress(0.0, "読み込み: " + krt::fromPath(src.filename()));
	if (!au::decode(src, pcm, info, err)) return fail(err);
	const int srcRate = pcm.sampleRate;

	fs::path dir = cs.outDir.empty() ? src.parent_path() : krt::toPath(cs.outDir);
	fs::path out = dir / src.filename();
	out.replace_extension(au::formatExtension(to));
	std::error_code ec;
	if (fs::equivalent(out, src, ec)) return fail("出力先が入力と同じファイルです (--out で別のフォルダを指定)");
	if (!cs.force && fs::exists(out, ec)) return fail("出力先が既にあります (上書きを指定するか --force)");
	fs::create_directories(dir, ec);

	// Opus 以外へ出すときは、元の Opus のヘッダゲインを波形に入れる
	if (info.format == au::Format::Opus && to != au::Format::Opus) au::applyGain(pcm, info.opusHeaderGainQ8 / 256.0);
	const double carriedGainDb = (info.format == au::Format::Opus && to == au::Format::Opus) ? info.opusHeaderGainQ8 / 256.0 : 0.0;

	double gainDb = cs.gain;
	au::Loudness loud;
	if (cs.hasNormalize || cs.replayGain) {
		p.progress(0.1, "ラウドネスを測定");
		if (!au::measureLoudness(pcm, loud, err)) return fail(err);
		if (cs.hasNormalize) gainDb += cs.normalize - (loud.integratedLufs + carriedGainDb);
		r.set("loudnessLufs", Json(loud.integratedLufs + carriedGainDb));
	}

	au::EncodeOptions eo;
	eo.format = to;
	eo.wavBits = cs.bits;
	eo.vorbisQuality = (float)cs.quality;
	eo.opusBitrateKbps = cs.bitrate;
	if (to == au::Format::Opus) {
		const double total = carriedGainDb + gainDb;
		eo.opusHeaderGainQ8 = (int)std::lround(total * 256.0);
	} else {
		au::applyGain(pcm, gainDb);
	}
	if (to == au::Format::Vorbis && cs.replayGain) {
		// ゲインを焼き込んだ後の音量で計り直す
		au::Loudness after;
		if (!au::measureLoudness(pcm, after, err)) return fail(err);
		char g[32], pk[32];
		std::snprintf(g, sizeof(g), "%+.2f dB", after.replayGainDb());
		std::snprintf(pk, sizeof(pk), "%.6f", after.samplePeak);
		eo.tags.emplace_back("REPLAYGAIN_TRACK_GAIN", g);
		eo.tags.emplace_back("REPLAYGAIN_TRACK_PEAK", pk);
		r.set("replayGain", Json(std::string(g)));
	}
	// 元のコメント (ENCODER と ReplayGain 以外) は引き継ぐ
	if (to != au::Format::Wav) {
		for (const auto& [k, v] : info.tags) {
			std::string uk = k;
			for (auto& c : uk) c = (char)std::toupper((unsigned char)c);
			if (uk == "ENCODER" || uk.rfind("REPLAYGAIN_", 0) == 0 || uk.rfind("R128_", 0) == 0) continue;
			eo.tags.emplace_back(k, v);
		}
	}

	struct Sub : krt::Progress {
		krt::Progress& parent;
		explicit Sub(krt::Progress& pp) : parent(pp) {}
		void progress(double ratio, const std::string&) override { parent.progress(0.2 + 0.8 * ratio, "書き出し"); }
		void log(const std::string& l) override { parent.log(l); }
		bool canceled() const override { return parent.canceled(); }
	} sub(p);
	if (!au::encode(pcm, out, eo, &sub, err)) {
		fs::remove(out, ec);
		return fail(err);
	}
	r.set("output", Json(krt::fromPath(out)));
	r.set("gainDb", Json(gainDb));

	// ループ情報 (.sli)
	fs::path sli = src;
	sli += ".sli";
	if (fs::exists(sli, ec)) {
		std::string text;
		krt::loop::Sli data;
		if (krt::readFile(sli, text) && krt::loop::parse(text, data, err)) {
			const int dstRate = to == au::Format::Opus ? 48000 : srcRate;
			krt::loop::rescale(data, srcRate, dstRate);
			fs::path outSli = out;
			outSli += ".sli";
			krt::writeFile(outSli, krt::loop::write(data));
			r.set("sli", Json(krt::fromPath(outSli)));
			if (dstRate != srcRate) r.set("sliRescaled", Json(std::to_string(srcRate) + " → " + std::to_string(dstRate) + " Hz"));
		} else {
			r.set("sliError", Json(err));
		}
	}
	r.set("ok", Json(true));
	return r;
}

Json loudnessOne(const fs::path& src)
{
	Json r = Json::object();
	r.set("path", Json(krt::fromPath(src)));
	au::Pcm pcm;
	au::Info info;
	au::Loudness l;
	std::string err;
	if (!au::decode(src, pcm, info, err) || !au::measureLoudness(pcm, l, err)) {
		r.set("ok", Json(false));
		r.set("message", Json(err));
		return r;
	}
	// 本体で鳴る音量で表す (Opus はヘッダゲイン込み)
	const double hg = info.format == au::Format::Opus ? info.opusHeaderGainQ8 / 256.0 : 0.0;
	r.set("ok", Json(true));
	r.set("lufs", Json(l.integratedLufs + hg));
	r.set("peak", Json(l.samplePeak * std::pow(10.0, hg / 20.0)));
	r.set("replayGainDb", Json(l.replayGainDb() - hg));
	return r;
}

/// 口パク用の音量 (JSON / CSV の文字列)
bool lipsyncText(const fs::path& src, double fps, const std::string& format, std::string& out, std::string& err)
{
	au::Pcm pcm;
	au::Info info;
	if (!au::decode(src, pcm, info, err)) return false;
	if (info.format == au::Format::Opus) au::applyGain(pcm, info.opusHeaderGainQ8 / 256.0);
	const auto lv = au::levels(pcm, fps);
	if (format == "csv") {
		out = "frame,time,rms,peak\n";
		char line[96];
		for (size_t i = 0; i < lv.size(); ++i) {
			std::snprintf(line, sizeof(line), "%zu,%.4f,%.5f,%.5f\n", i, i / fps, lv[i].rms, lv[i].peak);
			out += line;
		}
	} else {
		// 値は 5 桁に丸めて書く (Json の既定の書き出しは 17 桁になり、長い音声では大きくなる)
		auto series = [&](bool rms) {
			std::string a = "[";
			char v[24];
			for (size_t i = 0; i < lv.size(); ++i) {
				std::snprintf(v, sizeof(v), "%s%.5f", i ? "," : "", rms ? lv[i].rms : lv[i].peak);
				a += v;
			}
			return a + "]";
		};
		char head[64];
		std::snprintf(head, sizeof(head), "\"fps\":%g,\"frames\":%zu,", fps, lv.size());
		out = "{\"source\":" + Json::quote(krt::fromPath(src.filename())) + "," + head +
		      "\"rms\":" + series(true) + ",\"peak\":" + series(false) + "}\n";
	}
	return true;
}

int runCli(const Settings& s, const std::vector<std::string>& args)
{
	if (args.size() < 2) {
		std::fprintf(stderr, "usage: krkraudio --cli info|convert|loudness|lipsync <ファイル>... (--help 参照)\n");
		return 2;
	}
	const std::string cmd = args[0];
	const std::vector<std::string> files(args.begin() + 1, args.end());
	krt::ConsoleProgress progress(s.quiet);
	int rc = 0;
	Json arr = Json::array();

	if (cmd == "info") {
		for (const auto& f : files) {
			au::Pcm pcm;
			au::Info info;
			std::string err;
			if (!au::decode(krt::toPath(f), pcm, info, err)) { std::fprintf(stderr, "error: %s: %s\n", f.c_str(), err.c_str()); rc = 2; continue; }
			const Json o = infoJson(f, info);
			if (s.json) { arr.push(o); continue; }
			std::printf("%s: %s %d Hz %d ch %.3f 秒 (%llu サンプル)", f.c_str(), au::formatName(info.format),
			            info.sampleRate, info.channels, info.seconds(), (unsigned long long)info.frames);
			if (info.format == au::Format::Opus) std::printf(" ヘッダゲイン %+.2f dB", info.opusHeaderGainQ8 / 256.0);
			if (info.format == au::Format::Wav) std::printf(" %dbit%s", info.bitsPerSample, info.floatSamples ? " float" : "");
			std::printf("\n");
			for (const auto& [k, v] : info.tags) std::printf("    %s=%s\n", k.c_str(), v.c_str());
		}
	} else if (cmd == "convert") {
		for (const auto& f : files) {
			const Json r = convertOne(krt::toPath(f), s.conv, progress);
			progress.finish();
			if (!r["ok"].asBool()) rc = 2;
			if (s.json) { arr.push(r); continue; }
			if (r["ok"].asBool()) {
				std::printf("変換\t%s\t→ %s", f.c_str(), r["output"].asStr().c_str());
				if (r.has("sli")) std::printf("\t(.sli%s)", r.has("sliRescaled") ? (" " + r["sliRescaled"].asStr()).c_str() : "");
				std::printf("\n");
			} else {
				std::printf("失敗\t%s\t%s\n", f.c_str(), r["message"].asStr().c_str());
			}
		}
	} else if (cmd == "loudness") {
		for (const auto& f : files) {
			const Json r = loudnessOne(krt::toPath(f));
			if (!r["ok"].asBool()) rc = 2;
			if (s.json) { arr.push(r); continue; }
			if (r["ok"].asBool())
				std::printf("%s: %.2f LUFS / ピーク %.4f / ReplayGain %+.2f dB\n", f.c_str(),
				            r["lufs"].asReal(), r["peak"].asReal(), r["replayGainDb"].asReal());
			else std::printf("%s: %s\n", f.c_str(), r["message"].asStr().c_str());
		}
	} else if (cmd == "lipsync") {
		std::string text, err;
		if (!lipsyncText(krt::toPath(files[0]), s.fps, s.lipFormat, text, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
		if (s.out.empty()) std::printf("%s", text.c_str());
		else if (!krt::writeFile(krt::toPath(s.out), text)) { std::fprintf(stderr, "error: 書き込めません\n"); return 2; }
		return 0;
	} else {
		std::fprintf(stderr, "error: 不明なコマンド: %s\n", cmd.c_str());
		return 2;
	}
	if (s.json) std::printf("%s\n", arr.dump(1).c_str());
	return rc;
}

// 画面向けの API
class AudioModule : public appserve::IModule {
public:
	explicit AudioModule(krt::JobRunner& jobs) : jobs_(jobs) {}
	const char* name() const override { return "krkraudio"; }

	void registerApi(appserve::ApiRegistry& reg) override
	{
		// 情報 (body: { files })
		reg.route("/api/audio/info", appserve::Affinity::Any, [](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			Json arr = Json::array();
			for (size_t i = 0; i < b["files"].size(); ++i) {
				const std::string f = b["files"][i].asStr();
				au::Pcm pcm;
				au::Info info;
				std::string err;
				Json o;
				if (au::decode(krt::toPath(f), pcm, info, err)) {
					o = infoJson(f, info);
					std::error_code ec;
					o.set("hasSli", Json(fs::exists(krt::toPath(f + ".sli"), ec)));
				} else {
					o = Json::object();
					o.set("path", Json(f));
					o.set("error", Json(err));
				}
				arr.push(std::move(o));
			}
			return appserve::Response::json(arr);
		});

		// 変換 / ラウドネス (body: { files, mode, to, outDir, quality, bitrate, bits, gain, normalize, replaygain, force })
		reg.route("/api/audio/run", [this](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			std::vector<std::string> files;
			for (size_t i = 0; i < b["files"].size(); ++i) files.push_back(b["files"][i].asStr());
			if (files.empty()) return appserve::Response::error(400, "ファイルがありません");
			const std::string mode = b["mode"].asStr("convert");
			ConvertSettings cs;
			cs.to = b["to"].asStr("ogg");
			cs.outDir = b["outDir"].asStr();
			cs.quality = b["quality"].asReal(0.4);
			cs.bitrate = (int)b["bitrate"].asInt(0);
			cs.bits = (int)b["bits"].asInt(16);
			cs.gain = b["gain"].asReal(0.0);
			cs.hasNormalize = b.has("normalize") && b["normalize"].isNum();
			cs.normalize = b["normalize"].asReal(-18.0);
			cs.replayGain = b["replaygain"].asBool();
			cs.force = b["force"].asBool();
			const bool started = jobs_.start(mode, [files, mode, cs](krt::Progress& p) {
				Json arr = Json::array();
				for (size_t i = 0; i < files.size(); ++i) {
					if (p.canceled()) break;
					struct Sub : krt::Progress {
						krt::Progress& parent; double base, span; std::string name;
						Sub(krt::Progress& pp, double b, double s, std::string n) : parent(pp), base(b), span(s), name(std::move(n)) {}
						void progress(double r, const std::string&) override { parent.progress(base + span * (r < 0 ? 0 : r), name); }
						void log(const std::string& l) override { parent.log(l); }
						bool canceled() const override { return parent.canceled(); }
					} sub(p, (double)i / files.size(), 1.0 / files.size(), files[i]);
					arr.push(mode == "loudness" ? loudnessOne(krt::toPath(files[i])) : convertOne(krt::toPath(files[i]), cs, sub));
				}
				p.progress(1.0, "完了");
				Json j = Json::object();
				j.set("mode", Json(mode));
				j.set("entries", std::move(arr));
				return j;
			});
			if (!started) return appserve::Response::error(409, "実行中です");
			return appserve::Response::json(jobs_.status());
		});

		// 口パク用の音量 (body: { file, fps, format, out })
		reg.route("/api/audio/lipsync", appserve::Affinity::Any, [](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			std::string text, err;
			const std::string fmt = b["format"].asStr("json");
			if (!lipsyncText(krt::toPath(b["file"].asStr()), b["fps"].asReal(30.0), fmt, text, err))
				return appserve::Response::error(400, err);
			const std::string out = b["out"].asStr();
			if (!out.empty() && !krt::writeFile(krt::toPath(out), text)) return appserve::Response::error(400, "書き込めません");
			Json j = Json::object();
			j.set("text", Json(text.size() > 200000 ? text.substr(0, 200000) : text));
			j.set("written", Json(out));
			return appserve::Response::json(j);
		});
	}

private:
	krt::JobRunner& jobs_;
};

} // namespace

int main(int argc, char** argv)
{
	krt::ToolApp tool({ "krkraudio", "音声フォーマットコンバータ", KRT_VERSION });
	Settings s;
	auto num = [](const std::string& v, double& d) { char* e; d = std::strtod(v.c_str(), &e); return e != v.c_str(); };
	tool.addOption({ "to", "FORMAT", "変換先 ogg / opus / wav (convert)",
	                 [&s](const std::string& v) { s.conv.to = v; return true; } });
	tool.addOption({ "out", "PATH", "convert: 出力フォルダ / lipsync: 出力ファイル",
	                 [&s](const std::string& v) { s.conv.outDir = v; s.out = v; return true; } });
	tool.addOption({ "quality", "Q", "Vorbis の品質 -0.1 .. 1.0 (既定 0.4)",
	                 [&s, num](const std::string& v) { return num(v, s.conv.quality); } });
	tool.addOption({ "bitrate", "KBPS", "Opus のビットレート (既定: 自動)",
	                 [&s](const std::string& v) { s.conv.bitrate = std::atoi(v.c_str()); return s.conv.bitrate >= 0; } });
	tool.addOption({ "bits", "N", "WAV のビット数 16 / 24 / 32 (float)",
	                 [&s](const std::string& v) { s.conv.bits = std::atoi(v.c_str()); return true; } });
	tool.addOption({ "gain", "DB", "音量を変える (Opus はヘッダゲイン、ほかは波形に焼き込み)",
	                 [&s, num](const std::string& v) { return num(v, s.conv.gain); } });
	tool.addOption({ "normalize", "LUFS", "統合ラウドネスをこの値に揃える (例 -18)",
	                 [&s, num](const std::string& v) { s.conv.hasNormalize = true; return num(v, s.conv.normalize); } });
	tool.addOption({ "replaygain", "", "Vorbis に REPLAYGAIN_TRACK_GAIN / PEAK を書く",
	                 [&s](const std::string&) { s.conv.replayGain = true; return true; } });
	tool.addOption({ "force", "", "出力先を上書きする",
	                 [&s](const std::string&) { s.conv.force = true; return true; } });
	tool.addOption({ "fps", "N", "lipsync: 1 秒あたりの値の数 (既定 30)",
	                 [&s, num](const std::string& v) { return num(v, s.fps) && s.fps > 0; } });
	tool.addOption({ "format", "json|csv", "lipsync: 出力形式",
	                 [&s](const std::string& v) { s.lipFormat = v; return v == "json" || v == "csv"; } });
	tool.addOption({ "json", "", "結果を JSON で出す",
	                 [&s](const std::string&) { s.json = true; return true; } });
	tool.addOption({ "quiet", "", "進捗を出さない",
	                 [&s](const std::string&) { s.quiet = true; return true; } });
	if (!tool.parseArgs(argc, argv)) return tool.exitCode();

	if (tool.cli()) return runCli(s, tool.args());

	tool.addModule(std::make_unique<AudioModule>(tool.jobs()));
	return tool.runGui();
}
