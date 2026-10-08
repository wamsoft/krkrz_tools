//---------------------------------------------------------------------------
// krkrxp3 — xp3 アーカイブの作成・一覧・展開・検証
//
//   krkrxp3                                          … 画面を開く
//   krkrxp3 --cli pack <フォルダ> [--out=FILE] [--rpf=FILE] [--protect]
//                    [--no-compress-index] [--size-limit=KB | --no-size-limit] [--save-rpf=FILE]
//   krkrxp3 --cli list    <xp3> [--json]
//   krkrxp3 --cli extract <xp3> [--out=DIR]
//   krkrxp3 --cli verify  <xp3>
//
// pack は吉里吉里2 のリリーサ (krkrrel) と同じ書式・同じ既定の分類で xp3 を作る。
// フォルダに default.rpf があれば (--rpf 未指定時) それを読む。展開プロテクト付きの
// ファイルは展開しない。
//
// CLI の終了コード: 0 = 成功 / 1 = verify で壊れたファイルあり / 2 = 失敗
//---------------------------------------------------------------------------
#include <cstdio>
#include <map>
#include <memory>

#include "krt/app/Text.h"
#include "krt/app/ToolApp.h"
#include "krt/xp3/Xp3.h"

namespace fs = std::filesystem;
using appserve::Json;
namespace xp3 = krt::xp3;

#ifndef KRT_VERSION
#define KRT_VERSION "0.1.0"
#endif

namespace {

struct Settings {
	std::string out, rpf, saveRpf;
	bool protect = false, noCompressIndex = false, noSizeLimit = false, json = false, quiet = false;
	int64_t sizeLimitKB = -1;
};

fs::path defaultOutput(const fs::path& dir)
{
	fs::path d = dir;
	if (!d.has_filename()) d = d.parent_path();   // 末尾の区切りを落とす
	return d.parent_path() / (d.filename().native() + fs::path(".xp3").native());
}

// 展開して書き出す。protected は飛ばす
Json extractAll(const xp3::Archive& arc, const fs::path& dir, krt::Progress& p)
{
	Json arr = Json::array();
	const auto& es = arc.entries();
	int skipped = 0, failed = 0, ok = 0;
	for (size_t i = 0; i < es.size(); ++i) {
		if (p.canceled()) break;
		const auto& e = es[i];
		p.progress((double)i / (double)es.size(), e.name);
		Json o = Json::object();
		o.set("name", Json(e.name));
		// 展開プロテクトの付いたアーカイブの先頭にある警告文 ("$$$ ..." で始まる名前) も展開しない
		if (e.isProtected() || (arc.anyProtected() && e.name.rfind("$$$ ", 0) == 0)) {
			o.set("status", Json("protected"));
			++skipped;
		} else {
			std::string data, err;
			const fs::path dst = dir / krt::toPath(e.name);
			std::error_code ec;
			fs::create_directories(dst.parent_path(), ec);
			if (!arc.read(e, data, err) || !krt::writeFile(dst, data)) {
				o.set("status", Json("error"));
				o.set("message", Json(err.empty() ? "書き込めません" : err));
				++failed;
			} else {
				o.set("status", Json("ok"));
				++ok;
			}
		}
		arr.push(std::move(o));
	}
	p.progress(1.0, "完了");
	Json j = Json::object();
	j.set("entries", std::move(arr));
	j.set("ok", Json(ok));
	j.set("protected", Json(skipped));
	j.set("failed", Json(failed));
	return j;
}

Json verifyAll(const xp3::Archive& arc, krt::Progress& p)
{
	Json arr = Json::array();
	const auto& es = arc.entries();
	int ok = 0, bad = 0;
	for (size_t i = 0; i < es.size(); ++i) {
		if (p.canceled()) break;
		p.progress((double)i / (double)es.size(), es[i].name);
		std::string data, err;
		const bool good = arc.read(es[i], data, err);
		good ? ++ok : ++bad;
		if (!good) {
			Json o = Json::object();
			o.set("name", Json(es[i].name));
			o.set("message", Json(err));
			arr.push(std::move(o));
		}
	}
	p.progress(1.0, "完了");
	Json j = Json::object();
	j.set("ok", Json(ok));
	j.set("broken", Json(bad));
	j.set("errors", std::move(arr));
	return j;
}

Json listJson(const xp3::Archive& arc)
{
	Json arr = Json::array();
	for (const auto& e : arc.entries()) {
		Json o = Json::object();
		o.set("name", Json(e.name));
		o.set("size", Json((long long)e.orgSize));
		o.set("arcSize", Json((long long)e.arcSize));
		bool comp = false;
		for (const auto& s : e.segments) comp = comp || s.compressed;
		o.set("compressed", Json(comp));
		o.set("segments", Json((long long)e.segments.size()));
		o.set("protected", Json(e.isProtected()));
		arr.push(std::move(o));
	}
	Json j = Json::object();
	j.set("entries", std::move(arr));
	j.set("offset", Json((long long)arc.baseOffset()));
	j.set("indexCompressed", Json(arc.indexCompressed()));
	return j;
}

Json packResultJson(const xp3::PackResult& r, const fs::path& out)
{
	Json j = Json::object();
	j.set("output", Json(krt::fromPath(out)));
	j.set("files", Json(r.files));
	j.set("compressed", Json(r.compressed));
	j.set("deduplicated", Json(r.deduplicated));
	j.set("discarded", Json(r.discarded));
	j.set("orgBytes", Json((long long)r.orgBytes));
	j.set("arcBytes", Json((long long)r.arcBytes));
	j.set("canceled", Json(r.canceled));
	return j;
}

int runCli(const Settings& s, const std::vector<std::string>& args)
{
	if (args.size() < 2) {
		std::fprintf(stderr, "usage: krkrxp3 --cli pack|list|extract|verify <path> ... (--help 参照)\n");
		return 2;
	}
	const std::string cmd = args[0];
	const fs::path target = krt::toPath(args[1]);
	krt::ConsoleProgress progress(s.quiet);
	try {
		if (cmd == "pack") {
			xp3::PackOptions opt = xp3::PackOptions::defaults();
			std::string rpfOut, err;
			fs::path rpf = s.rpf.empty() ? target / "default.rpf" : krt::toPath(s.rpf);
			std::error_code ec;
			if (fs::exists(rpf, ec)) {
				if (!xp3::loadRpf(rpf, opt, &rpfOut, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
				if (!s.quiet) std::fprintf(stderr, "プロファイル: %s\n", krt::fromPath(rpf).c_str());
			} else if (!s.rpf.empty()) {
				std::fprintf(stderr, "error: プロファイルがありません: %s\n", s.rpf.c_str());
				return 2;
			}
			if (s.protect) opt.protect = true;
			if (s.noCompressIndex) opt.compressIndex = false;
			if (s.noSizeLimit) opt.doCompressSizeLimit = false;
			if (s.sizeLimitKB >= 0) { opt.doCompressSizeLimit = true; opt.compressSizeLimitKB = (uint64_t)s.sizeLimitKB; }
			// 出力: --out > プロファイルの OutputFileName (.xp3 のとき) > «フォルダ名.xp3»
			fs::path out = !s.out.empty() ? krt::toPath(s.out)
				: (!rpfOut.empty() && krt::toPath(rpfOut).extension() == ".xp3") ? krt::toPath(rpfOut)
				: defaultOutput(target);
			if (!s.saveRpf.empty() && !xp3::saveRpf(krt::toPath(s.saveRpf), opt, krt::fromPath(out), err)) {
				std::fprintf(stderr, "error: %s\n", err.c_str());
				return 2;
			}
			const auto r = xp3::pack(target, out, opt, progress);
			progress.finish();
			if (s.json) std::printf("%s\n", packResultJson(r, out).dump(1).c_str());
			else std::printf("%s: %d ファイル (圧縮 %d / 共有 %d / 除外 %d) %llu → %llu bytes\n",
			                 krt::fromPath(out).c_str(), r.files, r.compressed, r.deduplicated, r.discarded,
			                 (unsigned long long)r.orgBytes, (unsigned long long)r.arcBytes);
			return r.canceled ? 2 : 0;
		}

		xp3::Archive arc;
		std::string err;
		if (!arc.open(target, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }

		if (cmd == "list") {
			if (s.json) { std::printf("%s\n", listJson(arc).dump(1).c_str()); return 0; }
			for (const auto& e : arc.entries()) {
				bool comp = false;
				for (const auto& sg : e.segments) comp = comp || sg.compressed;
				std::printf("%12llu %12llu %s%s %s\n", (unsigned long long)e.orgSize, (unsigned long long)e.arcSize,
				            comp ? "Z" : "-", e.isProtected() ? "P" : "-", e.name.c_str());
			}
			std::printf("%zu ファイル%s\n", arc.entries().size(), arc.anyProtected() ? " (展開プロテクトあり)" : "");
			return 0;
		}
		if (cmd == "extract") {
			const fs::path dir = s.out.empty() ? target.parent_path() / target.stem() : krt::toPath(s.out);
			const Json j = extractAll(arc, dir, progress);
			progress.finish();
			if (s.json) std::printf("%s\n", j.dump(1).c_str());
			else std::printf("%s: 展開 %lld / プロテクトで除外 %lld / 失敗 %lld\n", krt::fromPath(dir).c_str(),
			                 (long long)j["ok"].asInt(), (long long)j["protected"].asInt(), (long long)j["failed"].asInt());
			return j["failed"].asInt() ? 2 : 0;
		}
		if (cmd == "verify") {
			const Json j = verifyAll(arc, progress);
			progress.finish();
			if (s.json) std::printf("%s\n", j.dump(1).c_str());
			else {
				for (size_t i = 0; i < j["errors"].size(); ++i)
					std::printf("破損\t%s\t%s\n", j["errors"][i]["name"].asStr().c_str(), j["errors"][i]["message"].asStr().c_str());
				std::printf("正常 %lld / 破損 %lld\n", (long long)j["ok"].asInt(), (long long)j["broken"].asInt());
			}
			return j["broken"].asInt() ? 1 : 0;
		}
		std::fprintf(stderr, "error: 不明なコマンド: %s\n", cmd.c_str());
		return 2;
	} catch (const std::exception& e) {
		progress.finish();
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}
}

// 画面向けの API
class Xp3Module : public appserve::IModule {
public:
	explicit Xp3Module(krt::JobRunner& jobs) : jobs_(jobs) {}
	const char* name() const override { return "krkrxp3"; }

	void registerApi(appserve::ApiRegistry& reg) override
	{
		// フォルダの中の拡張子と既定の扱い + プロファイル (body: { dir })
		reg.route("/api/xp3/scan", appserve::Affinity::Any, [](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			const fs::path dir = krt::toPath(b["dir"].asStr());
			xp3::PackOptions opt = xp3::PackOptions::defaults();
			std::string rpfOut, err;
			const fs::path rpf = dir / "default.rpf";
			std::error_code ec;
			const bool hasRpf = fs::exists(rpf, ec) && xp3::loadRpf(rpf, opt, &rpfOut, err);
			struct Stat { long long count = 0, bytes = 0; };
			std::map<std::string, Stat> exts;
			auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
			if (ec) return appserve::Response::error(400, "フォルダを開けません");
			for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
				if (ec) break;
				const std::string fname = krt::fromPath(it->path().filename());
				if (it->is_directory(ec)) {
					if ((!fname.empty() && fname[0] == '.') || fname == "CVS") it.disable_recursion_pending();
					continue;
				}
				if (!fname.empty() && fname[0] == '.') continue;
				std::string ext = krt::fromPath(it->path().extension());
				for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
				auto& st = exts[ext];
				++st.count;
				st.bytes += (long long)it->file_size(ec);
			}
			Json arr = Json::array();
			for (const auto& [ext, st] : exts) {
				Json o = Json::object();
				o.set("ext", Json(ext));
				o.set("count", Json(st.count));
				o.set("bytes", Json(st.bytes));
				const auto a = opt.classify(ext);
				o.set("action", Json(a == xp3::Action::Compress ? "compress" : a == xp3::Action::Store ? "store" : "discard"));
				arr.push(std::move(o));
			}
			Json j = Json::object();
			j.set("exts", std::move(arr));
			j.set("rpf", Json(hasRpf ? krt::fromPath(rpf) : std::string()));
			j.set("output", Json(!rpfOut.empty() && krt::toPath(rpfOut).extension() == ".xp3" ? rpfOut : krt::fromPath(defaultOutput(dir))));
			j.set("protect", Json(opt.protect));
			j.set("compressIndex", Json(opt.compressIndex));
			j.set("doLimit", Json(opt.doCompressSizeLimit));
			j.set("limitKB", Json((long long)opt.compressSizeLimitKB));
			return appserve::Response::json(j);
		});

		// 作成 (body: { dir, out, actions: {ext: action}, protect, compressIndex, doLimit, limitKB, saveRpf })
		reg.route("/api/xp3/pack", [this](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			const fs::path dir = krt::toPath(b["dir"].asStr());
			const fs::path out = krt::toPath(b["out"].asStr());
			xp3::PackOptions opt;
			const Json& acts = b["actions"];
			if (acts.isObj()) {
				for (const auto& [k, v] : acts.members()) {
					const std::string a = v.asStr();
					(a == "compress" ? opt.compress : a == "discard" ? opt.discard : opt.store).insert(k);
				}
			}
			opt.protect = b["protect"].asBool();
			opt.compressIndex = b["compressIndex"].asBool(true);
			opt.doCompressSizeLimit = b["doLimit"].asBool(true);
			opt.compressSizeLimitKB = (uint64_t)b["limitKB"].asInt(1024);
			if (b["saveRpf"].asBool()) {
				std::string err;
				if (!xp3::saveRpf(dir / "default.rpf", opt, krt::fromPath(out), err))
					return appserve::Response::error(400, err);
			}
			const bool started = jobs_.start("pack", [dir, out, opt](krt::Progress& p) {
				return packResultJson(xp3::pack(dir, out, opt, p), out);
			});
			if (!started) return appserve::Response::error(409, "実行中です");
			return appserve::Response::json(jobs_.status());
		});

		// 一覧 (body: { file })
		reg.route("/api/xp3/list", appserve::Affinity::Any, [](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			xp3::Archive arc;
			std::string err;
			if (!arc.open(krt::toPath(b["file"].asStr()), err)) return appserve::Response::error(400, err);
			return appserve::Response::json(listJson(arc));
		});

		// 検証 / 展開 (body: { file, dir })
		reg.route("/api/xp3/verify", [this](const appserve::Request& req) { return startArc(req, false); });
		reg.route("/api/xp3/extract", [this](const appserve::Request& req) { return startArc(req, true); });
	}

private:
	appserve::Response startArc(const appserve::Request& req, bool extract)
	{
		Json b; Json::parse(req.body, b);
		auto arc = std::make_shared<xp3::Archive>();
		std::string err;
		if (!arc->open(krt::toPath(b["file"].asStr()), err)) return appserve::Response::error(400, err);
		const fs::path dir = krt::toPath(b["dir"].asStr());
		const bool started = jobs_.start(extract ? "extract" : "verify", [arc, dir, extract](krt::Progress& p) {
			Json j = extract ? extractAll(*arc, dir, p) : verifyAll(*arc, p);
			j.set("mode", Json(extract ? "extract" : "verify"));
			return j;
		});
		if (!started) return appserve::Response::error(409, "実行中です");
		return appserve::Response::json(jobs_.status());
	}

	krt::JobRunner& jobs_;
};

} // namespace

int main(int argc, char** argv)
{
	krt::ToolApp tool({ "krkrxp3", "xp3 アーカイブツール", KRT_VERSION });
	Settings s;
	tool.addOption({ "out", "PATH", "出力先 (pack: xp3 ファイル / extract: フォルダ)",
	                 [&s](const std::string& v) { s.out = v; return true; } });
	tool.addOption({ "rpf", "FILE", "リリーサのプロファイル (既定: フォルダの default.rpf)",
	                 [&s](const std::string& v) { s.rpf = v; return true; } });
	tool.addOption({ "save-rpf", "FILE", "使った設定をプロファイルとして保存する",
	                 [&s](const std::string& v) { s.saveRpf = v; return true; } });
	tool.addOption({ "protect", "", "展開プロテクトを付ける",
	                 [&s](const std::string&) { s.protect = true; return true; } });
	tool.addOption({ "no-compress-index", "", "インデックスを圧縮しない",
	                 [&s](const std::string&) { s.noCompressIndex = true; return true; } });
	tool.addOption({ "size-limit", "KB", "このサイズ以上のファイルは圧縮しない (既定 1024)",
	                 [&s](const std::string& v) { s.sizeLimitKB = std::atoll(v.c_str()); return s.sizeLimitKB >= 0; } });
	tool.addOption({ "no-size-limit", "", "サイズに関わらず圧縮対象の拡張子は圧縮する",
	                 [&s](const std::string&) { s.noSizeLimit = true; return true; } });
	tool.addOption({ "json", "", "結果を JSON で出す",
	                 [&s](const std::string&) { s.json = true; return true; } });
	tool.addOption({ "quiet", "", "進捗を出さない",
	                 [&s](const std::string&) { s.quiet = true; return true; } });
	if (!tool.parseArgs(argc, argv)) return tool.exitCode();

	if (tool.cli()) return runCli(s, tool.args());

	tool.addModule(std::make_unique<Xp3Module>(tool.jobs()));
	return tool.runGui();
}
