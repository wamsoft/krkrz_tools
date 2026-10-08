//---------------------------------------------------------------------------
// krkrsign — キー生成・署名ツール
//
//   krkrsign                                   … 画面を開く
//   krkrsign --cli keygen [--bits=1024] [--public=FILE] [--private=FILE] [--force]
//   krkrsign --cli sign   --key=<秘密鍵> <ファイル>...
//   krkrsign --cli verify --key=<公開鍵> <ファイル>...
//
// 吉里吉里2 の krkrsign と同じ書式の鍵・署名を作る。吉里吉里の exe (署名領域を
// 持つもの) は exe の中へ、それ以外は «ファイル名.sig» へ署名を書く。
//
// CLI の終了コード: 0 = 成功 / 1 = 検証で破損あり / 2 = 失敗
//---------------------------------------------------------------------------
#include <cstdio>
#include <memory>

#include "krt/app/Text.h"
#include "krt/app/ToolApp.h"
#include "krt/sig/Signature.h"

namespace fs = std::filesystem;
using appserve::Json;
namespace sig = krt::sig;

#ifndef KRT_VERSION
#define KRT_VERSION "0.1.0"
#endif

namespace {

struct Settings {
	std::string key;                       ///< --key
	int         bits = 1024;               ///< --bits
	std::string publicOut = "public.txt";  ///< --public
	std::string privateOut = "private.txt";///< --private
	bool        force = false;
	bool        quiet = false;
	bool        json = false;
};

const char* statusLabel(sig::Status s)
{
	switch (s) {
	case sig::Status::Ok:       return "正常";
	case sig::Status::Broken:   return "破損";
	case sig::Status::Canceled: return "中断";
	default:                    return "エラー";
	}
}

bool readKey(const std::string& file, std::string& text, std::string& err)
{
	if (file.empty()) { err = "--key=<鍵ファイル> を指定してください"; return false; }
	if (!krt::readFile(krt::toPath(file), text)) { err = "鍵ファイルを開けません: " + file; return false; }
	return true;
}

// 鍵を 2 つのファイルへ書く (既にあれば overwrite のときだけ上書き)
bool saveKeys(const sig::KeyPair& kp, const fs::path& pub, const fs::path& priv, bool overwrite, std::string& err)
{
	std::error_code ec;
	if (!overwrite && (fs::exists(pub, ec) || fs::exists(priv, ec))) {
		err = "鍵ファイルが既にあります (上書きするなら --force)";
		return false;
	}
	if (!krt::writeFile(pub, kp.publicKey) || !krt::writeFile(priv, kp.privateKey)) {
		err = "鍵ファイルを書き込めません";
		return false;
	}
	return true;
}

Json signEntryJson(const std::string& path, const sig::SignResult& r)
{
	Json o = Json::object();
	o.set("path", Json(path));
	o.set("ok", Json(r.ok));
	o.set("embedded", Json(r.embedded));
	o.set("written", Json(r.written));
	o.set("message", Json(r.message));
	return o;
}

Json verifyEntryJson(const std::string& path, const sig::VerifyResult& r)
{
	Json o = Json::object();
	o.set("path", Json(path));
	o.set("status", Json(sig::statusName(r.status)));
	o.set("label", Json(statusLabel(r.status)));
	o.set("message", Json(r.message));
	return o;
}

int runCli(const Settings& s, const std::vector<std::string>& args)
{
	if (args.empty()) {
		std::fprintf(stderr, "usage: krkrsign --cli keygen|sign|verify ... (--help 参照)\n");
		return 2;
	}
	const std::string cmd = args.front();
	const std::vector<std::string> files(args.begin() + 1, args.end());
	krt::ConsoleProgress progress(s.quiet);

	if (cmd == "keygen") {
		sig::KeyPair kp;
		std::string err;
		if (!sig::generateKeyPair(s.bits, kp, err) ||
		    !saveKeys(kp, krt::toPath(s.publicOut), krt::toPath(s.privateOut), s.force, err)) {
			std::fprintf(stderr, "error: %s\n", err.c_str());
			return 2;
		}
		std::printf("公開鍵: %s\n秘密鍵: %s\n", s.publicOut.c_str(), s.privateOut.c_str());
		std::printf("%s", kp.publicKey.c_str());
		return 0;
	}

	if (cmd != "sign" && cmd != "verify") {
		std::fprintf(stderr, "error: 不明なコマンド: %s (keygen / sign / verify)\n", cmd.c_str());
		return 2;
	}
	std::string key, err;
	if (!readKey(s.key, key, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
	if (files.empty()) { std::fprintf(stderr, "error: 対象のファイルを指定してください\n"); return 2; }

	int rc = 0;
	Json arr = Json::array();
	for (const auto& f : files) {
		if (cmd == "sign") {
			const auto r = sig::signFile(krt::toPath(f), key, &progress);
			progress.finish();
			if (!r.ok) rc = 2;
			if (s.json) arr.push(signEntryJson(f, r));
			else if (r.ok) std::printf("署名\t%s\t%s\n", f.c_str(), r.embedded ? "(exe に埋め込み)" : r.written.c_str());
			else std::printf("失敗\t%s\t%s\n", f.c_str(), r.message.c_str());
		} else {
			const auto r = sig::verifyFile(krt::toPath(f), key, &progress);
			progress.finish();
			if (r.status == sig::Status::Broken && rc == 0) rc = 1;
			if (r.status == sig::Status::Error || r.status == sig::Status::Canceled) rc = 2;
			if (s.json) arr.push(verifyEntryJson(f, r));
			else std::printf("%s\t%s%s%s\n", statusLabel(r.status), f.c_str(),
			                 r.message.empty() ? "" : "\t", r.message.c_str());
		}
	}
	if (s.json) std::printf("%s\n", arr.dump(1).c_str());
	return rc;
}

// 画面向けの API
class SignModule : public appserve::IModule {
public:
	explicit SignModule(krt::JobRunner& jobs) : jobs_(jobs) {}
	const char* name() const override { return "krkrsign"; }

	void registerApi(appserve::ApiRegistry& reg) override
	{
		// 鍵を作る (body: { bits })。数秒かかることがあるのでメインスレッド外で
		reg.route("/api/sign/keygen", appserve::Affinity::Any, [](const appserve::Request& req) {
			Json body;
			Json::parse(req.body, body);
			const int bits = (int)(body.isObj() && body.has("bits") ? body["bits"].asInt(1024) : 1024);
			sig::KeyPair kp;
			std::string err;
			if (!sig::generateKeyPair(bits, kp, err)) return appserve::Response::error(400, err);
			Json j = Json::object();
			j.set("publicKey", Json(kp.publicKey));
			j.set("privateKey", Json(kp.privateKey));
			return appserve::Response::json(j);
		});

		// 鍵を保存する (body: { publicPath, privatePath, publicKey, privateKey, overwrite })
		reg.route("/api/sign/savekeys", [](const appserve::Request& req) {
			Json b;
			Json::parse(req.body, b);
			sig::KeyPair kp{ b["publicKey"].asStr(), b["privateKey"].asStr() };
			std::string err;
			if (!saveKeys(kp, krt::toPath(b["publicPath"].asStr()), krt::toPath(b["privatePath"].asStr()),
			              b["overwrite"].asBool(), err))
				return appserve::Response::error(400, err);
			return appserve::Response::json(Json::object());
		});

		// 署名 / 検証 (body: { files: [...], keyFile })。結果は JobRunner の result
		reg.route("/api/sign/sign", [this](const appserve::Request& req) { return start(req, true); });
		reg.route("/api/sign/verify", [this](const appserve::Request& req) { return start(req, false); });
	}

private:
	appserve::Response start(const appserve::Request& req, bool doSign)
	{
		Json b;
		Json::parse(req.body, b);
		std::vector<std::string> files;
		if (b.isObj() && b.has("files"))
			for (size_t i = 0; i < b["files"].size(); ++i) files.push_back(b["files"][i].asStr());
		if (files.empty()) return appserve::Response::error(400, "対象のファイルがありません");
		std::string key, err;
		if (!readKey(b.isObj() ? b["keyFile"].asStr() : std::string(), key, err))
			return appserve::Response::error(400, err);

		const bool started = jobs_.start(doSign ? "sign" : "verify", [files, key, doSign](krt::Progress& p) {
			Json arr = Json::array();
			for (size_t i = 0; i < files.size(); ++i) {
				if (p.canceled()) break;
				p.progress((double)i / (double)files.size(), files[i]);
				if (doSign) {
					const auto r = sig::signFile(krt::toPath(files[i]), key);
					arr.push(signEntryJson(files[i], r));
				} else {
					const auto r = sig::verifyFile(krt::toPath(files[i]), key);
					arr.push(verifyEntryJson(files[i], r));
				}
			}
			p.progress(1.0, "完了");
			Json j = Json::object();
			j.set("mode", Json(doSign ? "sign" : "verify"));
			j.set("entries", std::move(arr));
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
	krt::ToolApp tool({ "krkrsign", "キー生成・署名ツール", KRT_VERSION });
	Settings s;
	tool.addOption({ "key", "FILE", "署名には秘密鍵、検証には公開鍵のファイル (--cli sign / verify)",
	                 [&s](const std::string& v) { s.key = v; return true; } });
	tool.addOption({ "bits", "N", "鍵の長さ 1024 (既定・旧ツール互換) / 2048 / 3072 / 4096 (--cli keygen)",
	                 [&s](const std::string& v) { s.bits = std::atoi(v.c_str()); return s.bits > 0; } });
	tool.addOption({ "public", "FILE", "公開鍵の出力先 (既定 public.txt)",
	                 [&s](const std::string& v) { s.publicOut = v; return true; } });
	tool.addOption({ "private", "FILE", "秘密鍵の出力先 (既定 private.txt)",
	                 [&s](const std::string& v) { s.privateOut = v; return true; } });
	tool.addOption({ "force", "", "鍵ファイルを上書きする",
	                 [&s](const std::string&) { s.force = true; return true; } });
	tool.addOption({ "json", "", "結果を JSON で出す",
	                 [&s](const std::string&) { s.json = true; return true; } });
	tool.addOption({ "quiet", "", "進捗を出さない",
	                 [&s](const std::string&) { s.quiet = true; return true; } });
	if (!tool.parseArgs(argc, argv)) return tool.exitCode();

	if (tool.cli()) return runCli(s, tool.args());

	tool.addModule(std::make_unique<SignModule>(tool.jobs()));
	return tool.runGui();
}
