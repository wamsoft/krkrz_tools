//---------------------------------------------------------------------------
// krkrimg — 画像フォーマットコンバータ (旧 krkrtpc の後継)
//
//   krkrimg                                    … 画面を開く
//   krkrimg --cli info    <ファイル>...
//   krkrimg --cli convert <ファイル>... [--opaque=tlg5|tlg6|png|bmp|jpg]
//                         [--alpha=tlg5|tlg6|png|bmp|sep] [--sep-main=jpg|png|bmp] [--sep-mask=jpg|png|bmp]
//                         [--quality=90] [--main-quality=90] [--mask-quality=90]
//                         [--transparent=remove|keep|expand1..8] [--input-addalpha] [--addalpha]
//                         [--out=DIR] [--force]
//   krkrimg --cli layers  <PSD / CLIP>... [--format=png|tlg5|tlg6] [--hidden] [--out=DIR] [--force]
//
// 入力: BMP / PNG / JPEG / TLG5 / TLG6 / PSD・CLIP STUDIO (.clip) (レイヤから合成)。
//       «foo_m.bmp/png/jpg» があればメイン/マスク分離形式として読む。
// 出力: 不透明な画像と透明部分のある画像で別々の形式を選ぶ (旧版と同じ)。
//       透明度を持つ形式でも全画素が不透明なら «不透明な画像» として扱う。
//
// 前処理 (旧版と同じ規則):
//   --transparent   完全透明部分の色: remove = 黒にする (既定) / keep = そのまま /
//                   expandN = N ピクセル以内の不透明な部分の色で埋める (JPEG のにじみ対策)
//   --input-addalpha 入力を ltAddAlpha 形式とみなす (--addalpha も付く)
//   --addalpha      ltAddAlpha 形式で出力する (色にアルファを掛ける)
//   TLG には mode タグ (opaque / alpha / addalpha) を書く。PNG の oFFs / vpAg / pHYs は
//   offs_* / vpag_* / reso_* タグとして TLG・PNG へ引き継ぐ。
//
// layers: レイヤを 1 枚ずつ «ファイル名/番号_レイヤ名.png» に書き出し、位置などを
//   «ファイル名/layers.json» にまとめる。各画像には文書上の位置 (offs_*) と文書の大きさ
//   (vpag_*) のタグを、TLG には加えてブレンドモードに対応する mode タグを書く。
//   PSD はレイヤー効果込み。非表示のレイヤは --hidden を付けたときだけ。
//
// --out は相対パスなら入力ファイルのフォルダから見た位置 (旧版と同じ)。
// CLI の終了コード: 0 = 成功 / 2 = 失敗
//---------------------------------------------------------------------------
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>

#include "krt/app/Text.h"
#include "krt/app/ToolApp.h"
#include "krt/image/Image.h"
#include "krt/image/Layers.h"

namespace fs = std::filesystem;
using appserve::Json;
namespace im = krt::image;

#ifndef KRT_VERSION
#define KRT_VERSION "0.1.0"
#endif

namespace {

struct ConvertSettings {
	std::string opaque = "tlg5";       ///< 不透明な画像の形式
	std::string alpha = "tlg5";        ///< 透明部分のある画像の形式 (sep = メイン/マスク分離)
	std::string sepMain = "jpg";
	std::string sepMask = "jpg";
	int         quality = 90;          ///< 不透明な画像の JPEG
	int         mainQuality = 90;      ///< 分離形式のメイン
	int         maskQuality = 90;      ///< 分離形式のマスク
	std::string transparent = "remove";
	bool        inputAddAlpha = false;
	bool        addAlpha = false;
	std::string outDir;
	bool        force = false;
};

struct LayerSettings {
	std::string format = "png";
	bool        hidden = false;
};

struct Settings {
	ConvertSettings conv;
	LayerSettings layer;
	bool json = false;
	bool quiet = false;
};

bool validSingle(const std::string& f) { return f == "png" || f == "bmp" || f == "jpg" || f == "jpeg"; }

std::string checkSettings(const ConvertSettings& c)
{
	const auto of = im::formatFromName(c.opaque);
	if (of == im::Format::Unknown || of == im::Format::Psd || of == im::Format::Clip) return "--opaque には tlg5 / tlg6 / png / bmp / jpg を指定してください";
	if (c.alpha != "sep") {
		const auto af = im::formatFromName(c.alpha);
		if (af == im::Format::Unknown || af == im::Format::Psd || af == im::Format::Clip || af == im::Format::Jpeg)
			return "--alpha には tlg5 / tlg6 / png / bmp / sep を指定してください";
	}
	if (!validSingle(c.sepMain) || !validSingle(c.sepMask)) return "--sep-main / --sep-mask には jpg / png / bmp を指定してください";
	if (c.transparent != "remove" && c.transparent != "keep" && c.transparent.rfind("expand", 0) != 0)
		return "--transparent には remove / keep / expand1〜expand8 を指定してください";
	return {};
}

Json infoJson(const std::string& path, const im::Image& img, const im::LoadInfo& li, const fs::path& mask)
{
	Json o = Json::object();
	o.set("path", Json(path));
	o.set("format", Json(im::formatName(li.format)));
	o.set("width", Json(img.width));
	o.set("height", Json(img.height));
	o.set("hasAlpha", Json(li.hasAlpha));
	o.set("transparent", Json(im::hasTransparency(img)));
	if (li.format == im::Format::Psd || li.format == im::Format::Clip) o.set("layers", Json(li.layers));
	if (!mask.empty()) o.set("mask", Json(krt::fromPath(mask)));
	Json tags = Json::object();
	for (const auto& [k, v] : img.tags) tags.set(k, Json(v));
	o.set("tags", std::move(tags));
	return o;
}

/// 1 ファイルを変換する。結果は JSON (path / outputs / kind / ok / message)
Json convertOne(const fs::path& src, const ConvertSettings& cs, krt::Progress& p)
{
	Json r = Json::object();
	r.set("path", Json(krt::fromPath(src)));
	auto fail = [&](const std::string& m) { r.set("ok", Json(false)); r.set("message", Json(m)); return r; };
	{
		const std::string e = checkSettings(cs);
		if (!e.empty()) return fail(e);
	}

	// 読み込み (+ 分離形式のマスク)
	p.progress(0.0, "読み込み: " + krt::fromPath(src.filename()));
	im::Image img;
	im::LoadInfo li;
	std::string err;
	if (!im::load(src, img, err, &li)) return fail(err);
	std::string inputType = cs.inputAddAlpha ? "addalpha" : "alpha";
	if (li.format == im::Format::Tlg5 || li.format == im::Format::Tlg6) {
		// TLG は自分の mode タグに従う
		const std::string m = im::tagValue(img.tags, "mode");
		if (m == "addalpha" || m == "alpha") inputType = m;
	}
	const bool canHaveMask = li.format == im::Format::Bmp || li.format == im::Format::Png || li.format == im::Format::Jpeg;
	const fs::path maskFile = canHaveMask ? im::findMaskFile(src) : fs::path();
	if (!maskFile.empty()) {
		im::Image mask;
		if (!im::load(maskFile, mask, err)) return fail("マスク画像 " + krt::fromPath(maskFile.filename()) + ": " + err);
		if (!im::bindMask(img, mask, err)) return fail(err);
		r.set("mask", Json(krt::fromPath(maskFile)));
	}
	const bool transparent = im::hasTransparency(img);
	if (!transparent) inputType = "opaque";
	const bool outAddAlpha = cs.addAlpha || cs.inputAddAlpha;
	const std::string outputType = outAddAlpha ? "addalpha" : inputType;

	// 出力先
	fs::path dir = src.parent_path();
	if (!cs.outDir.empty()) {
		const fs::path o = krt::toPath(cs.outDir);
		dir = o.is_absolute() ? o : (dir / o).lexically_normal();
	}
	fs::path base = dir / src.filename();
	base.replace_extension();
	struct Out { fs::path path; im::Format format; im::SaveOptions opt; bool mask; };
	std::vector<Out> outs;
	auto pathFor = [&](const fs::path& b, im::Format f) { fs::path x = b; x += im::formatExtension(f); return x; };
	if (!transparent) {
		const im::Format f = im::formatFromName(cs.opaque);
		im::SaveOptions o;
		o.withAlpha = false;
		o.jpegQuality = cs.quality;
		outs.push_back({ pathFor(base, f), f, o, false });
	} else if (cs.alpha == "sep") {
		im::SaveOptions om, ok;
		om.withAlpha = false;
		om.jpegQuality = cs.mainQuality;
		ok.withAlpha = false;
		ok.grayscale = true;
		ok.jpegQuality = cs.maskQuality;
		const im::Format fm = im::formatFromName(cs.sepMain), fk = im::formatFromName(cs.sepMask);
		fs::path mb = base;
		mb += "_m";
		outs.push_back({ pathFor(base, fm), fm, om, false });
		outs.push_back({ pathFor(mb, fk), fk, ok, true });
	} else {
		const im::Format f = im::formatFromName(cs.alpha);
		im::SaveOptions o;
		o.withAlpha = true;
		outs.push_back({ pathFor(base, f), f, o, false });
	}
	std::error_code ec;
	for (const auto& o : outs) {
		if (fs::equivalent(o.path, src, ec) || (!maskFile.empty() && fs::equivalent(o.path, maskFile, ec)))
			return fail("出力先が入力と同じファイルです (出力フォルダに別のフォルダを指定してください)");
		if (!cs.force && fs::exists(o.path, ec)) return fail("出力先が既にあります: " + krt::fromPath(o.path.filename()) + " (上書きを指定するか --force)");
	}
	fs::create_directories(dir, ec);

	// 前処理
	if (transparent && outputType == "alpha") {
		const std::string& t = cs.transparent;
		if (t == "remove") im::clearTransparentColor(img);
		else if (t.rfind("expand", 0) == 0) {
			int n = std::atoi(t.c_str() + 6);
			if (n <= 0) n = 1;
			if (n > 64) n = 64;
			p.progress(0.3, "完全透明部分の色を合成");
			im::expandOpaqueColor(img, n);
		}
	}
	if (transparent && inputType == "alpha" && outputType == "addalpha") im::toAddAlpha(img);
	im::setTag(img.tags, "mode", outputType);

	// 書き出し
	p.progress(0.5, "書き出し: " + krt::fromPath(src.filename()));
	Json written = Json::array();
	for (const auto& o : outs) {
		if (p.canceled()) return fail("中断しました");
		const im::Image* img2 = &img;
		im::Image tmp;
		if (o.mask) { tmp = im::extractMask(img); img2 = &tmp; }
		if (!im::save(o.path, *img2, o.format, o.opt, err)) {
			for (const auto& x : outs) fs::remove(x.path, ec);
			return fail(krt::fromPath(o.path.filename()) + ": " + err);
		}
		written.push(Json(krt::fromPath(o.path)));
	}
	r.set("outputs", std::move(written));
	r.set("kind", Json(transparent ? "transparent" : "opaque"));
	r.set("mode", Json(outputType));
	r.set("ok", Json(true));
	return r;
}

/// ファイル名に使えない文字を置き換える
std::string safeName(const std::string& s)
{
	std::string o;
	for (char c : s) o += ((unsigned char)c < 0x20 || std::strchr("\\/:*?\"<>|", c)) ? '_' : c;
	while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
	return o.empty() ? "layer" : o;
}

/// PSD / CLIP のレイヤを書き出す。結果は JSON (path / folder / count / ok / message)
Json exportLayers(const fs::path& src, const ConvertSettings& cs, const LayerSettings& ls, krt::Progress& p)
{
	Json r = Json::object();
	r.set("path", Json(krt::fromPath(src)));
	auto fail = [&](const std::string& m) { r.set("ok", Json(false)); r.set("message", Json(m)); return r; };
	const im::Format fmt = im::formatFromName(ls.format);
	if (fmt != im::Format::Png && fmt != im::Format::Tlg5 && fmt != im::Format::Tlg6) return fail("--format には png / tlg5 / tlg6 を指定してください");

	p.progress(0.0, "読み込み: " + krt::fromPath(src.filename()));
	im::LayerDocument doc;
	std::string err;
	if (!im::loadLayers(src, doc, err, ls.hidden)) return fail(err);

	fs::path dir = src.parent_path();
	if (!cs.outDir.empty()) {
		const fs::path o = krt::toPath(cs.outDir);
		dir = o.is_absolute() ? o : (dir / o).lexically_normal();
	}
	dir /= src.stem();
	std::error_code ec;
	const fs::path index = dir / "layers.json";
	if (!cs.force && fs::exists(index, ec)) return fail("出力先が既にあります: " + krt::fromPath(index) + " (上書きを指定するか --force)");
	fs::create_directories(dir, ec);

	Json list = Json::array();
	for (size_t i = 0; i < doc.layers.size(); ++i) {
		if (p.canceled()) return fail("中断しました");
		auto& e = doc.layers[i];
		p.progress((double)i / doc.layers.size(), e.name);
		char num[16];
		std::snprintf(num, sizeof(num), "%03d_", (int)i);
		const std::string file = num + safeName(e.name) + im::formatExtension(fmt);
		im::Image& img = e.image;
		if (cs.transparent == "remove") im::clearTransparentColor(img);
		else if (cs.transparent.rfind("expand", 0) == 0) im::expandOpaqueColor(img, std::max(1, std::atoi(cs.transparent.c_str() + 6)));
		im::setTag(img.tags, "offs_x", std::to_string(e.left));
		im::setTag(img.tags, "offs_y", std::to_string(e.top));
		im::setTag(img.tags, "offs_unit", "pixel");
		im::setTag(img.tags, "vpag_w", std::to_string(doc.width));
		im::setTag(img.tags, "vpag_h", std::to_string(doc.height));
		im::setTag(img.tags, "vpag_unit", "pixel");
		if (!e.mode.empty()) im::setTag(img.tags, "mode", e.mode);
		im::SaveOptions so;
		so.withAlpha = true;
		if (!im::save(dir / krt::toPath(file), img, fmt, so, err)) return fail(file + ": " + err);
		Json o = Json::object();
		o.set("file", Json(file));
		o.set("name", Json(e.name));
		o.set("folder", Json(e.folder));
		o.set("index", Json(e.index));
		o.set("visible", Json(e.visible));
		o.set("left", Json(e.left));
		o.set("top", Json(e.top));
		o.set("width", Json(img.width));
		o.set("height", Json(img.height));
		o.set("opacity", Json(e.opacity));
		o.set("blend", Json(e.blend));
		o.set("mode", Json(e.mode));
		list.push(std::move(o));
	}
	Json idx = Json::object();
	idx.set("source", Json(krt::fromPath(src.filename())));
	idx.set("width", Json(doc.width));
	idx.set("height", Json(doc.height));
	idx.set("layers", std::move(list));
	if (!krt::writeFile(index, idx.dump(1) + "\n")) return fail("書き込めません: " + krt::fromPath(index));
	r.set("folder", Json(krt::fromPath(dir)));
	r.set("count", Json((int)doc.layers.size()));
	r.set("ok", Json(true));
	return r;
}

/// 分離形式のマスク画像 (他の入力のマスク) を一覧から外す
std::vector<std::string> dropMaskInputs(const std::vector<std::string>& files, std::vector<std::string>* skipped)
{
	std::set<std::string> masks;
	for (const auto& f : files) {
		const fs::path m = im::findMaskFile(krt::toPath(f));
		if (!m.empty()) masks.insert(krt::fromPath(fs::absolute(m).lexically_normal()));
	}
	std::vector<std::string> out;
	for (const auto& f : files) {
		if (masks.count(krt::fromPath(fs::absolute(krt::toPath(f)).lexically_normal()))) {
			if (skipped) skipped->push_back(f);
			continue;
		}
		out.push_back(f);
	}
	return out;
}

int runCli(const Settings& s, const std::vector<std::string>& args)
{
	if (args.size() < 2) {
		std::fprintf(stderr, "usage: krkrimg --cli info|convert|layers <ファイル>... (--help 参照)\n");
		return 2;
	}
	const std::string cmd = args[0];
	const std::vector<std::string> files(args.begin() + 1, args.end());
	krt::ConsoleProgress progress(s.quiet);
	int rc = 0;
	Json arr = Json::array();

	if (cmd == "info") {
		for (const auto& f : files) {
			im::Image img;
			im::LoadInfo li;
			std::string err;
			if (!im::load(krt::toPath(f), img, err, &li)) { std::fprintf(stderr, "error: %s: %s\n", f.c_str(), err.c_str()); rc = 2; continue; }
			const fs::path mask = (li.format == im::Format::Bmp || li.format == im::Format::Png || li.format == im::Format::Jpeg)
				? im::findMaskFile(krt::toPath(f)) : fs::path();
			const Json o = infoJson(f, img, li, mask);
			if (s.json) { arr.push(o); continue; }
			std::printf("%s: %s %dx%d %s", f.c_str(), im::formatName(li.format), img.width, img.height,
			            im::hasTransparency(img) ? "透明部分あり" : "不透明");
			if (li.format == im::Format::Psd || li.format == im::Format::Clip) std::printf(" レイヤ %d 枚", li.layers);
			if (!mask.empty()) std::printf(" マスク %s", krt::fromPath(mask.filename()).c_str());
			std::printf("\n");
			for (const auto& [k, v] : img.tags) std::printf("    %s=%s\n", k.c_str(), v.c_str());
		}
	} else if (cmd == "convert") {
		const std::string e = checkSettings(s.conv);
		if (!e.empty()) { std::fprintf(stderr, "error: %s\n", e.c_str()); return 2; }
		std::vector<std::string> skipped;
		const auto list = dropMaskInputs(files, &skipped);
		for (const auto& f : skipped)
			if (!s.json) std::printf("省略\t%s\t(メイン画像のマスクとして読む)\n", f.c_str());
		for (const auto& f : list) {
			const Json r = convertOne(krt::toPath(f), s.conv, progress);
			progress.finish();
			if (!r["ok"].asBool()) rc = 2;
			if (s.json) { arr.push(r); continue; }
			if (r["ok"].asBool()) {
				std::printf("変換\t%s\t→", f.c_str());
				for (size_t i = 0; i < r["outputs"].size(); ++i) std::printf(" %s", r["outputs"][i].asStr().c_str());
				std::printf("\t(%s)\n", r["mode"].asStr().c_str());
			} else {
				std::printf("失敗\t%s\t%s\n", f.c_str(), r["message"].asStr().c_str());
			}
		}
	} else if (cmd == "layers") {
		for (const auto& f : files) {
			const Json r = exportLayers(krt::toPath(f), s.conv, s.layer, progress);
			progress.finish();
			if (!r["ok"].asBool()) rc = 2;
			if (s.json) { arr.push(r); continue; }
			if (r["ok"].asBool()) std::printf("書出\t%s\t→ %s (%d 枚)\n", f.c_str(), r["folder"].asStr().c_str(), (int)r["count"].asInt());
			else std::printf("失敗\t%s\t%s\n", f.c_str(), r["message"].asStr().c_str());
		}
	} else {
		std::fprintf(stderr, "error: 不明なコマンド: %s\n", cmd.c_str());
		return 2;
	}
	if (s.json) std::printf("%s\n", arr.dump(1).c_str());
	return rc;
}

ConvertSettings settingsFromJson(const Json& b)
{
	ConvertSettings cs;
	cs.opaque = b["opaque"].asStr(cs.opaque);
	cs.alpha = b["alpha"].asStr(cs.alpha);
	cs.sepMain = b["sepMain"].asStr(cs.sepMain);
	cs.sepMask = b["sepMask"].asStr(cs.sepMask);
	cs.quality = (int)b["quality"].asInt(cs.quality);
	cs.mainQuality = (int)b["mainQuality"].asInt(cs.mainQuality);
	cs.maskQuality = (int)b["maskQuality"].asInt(cs.maskQuality);
	cs.transparent = b["transparent"].asStr(cs.transparent);
	cs.inputAddAlpha = b["inputAddAlpha"].asBool();
	cs.addAlpha = b["addAlpha"].asBool();
	cs.outDir = b["outDir"].asStr();
	cs.force = b["force"].asBool();
	return cs;
}

// 画面向けの API
class ImageModule : public appserve::IModule {
public:
	explicit ImageModule(krt::JobRunner& jobs) : jobs_(jobs) {}
	const char* name() const override { return "krkrimg"; }

	void registerApi(appserve::ApiRegistry& reg) override
	{
		// 情報 (body: { files })
		reg.route("/api/image/info", appserve::Affinity::Any, [](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			std::vector<std::string> files;
			for (size_t i = 0; i < b["files"].size(); ++i) files.push_back(b["files"][i].asStr());
			std::vector<std::string> skipped;
			files = dropMaskInputs(files, &skipped);
			Json arr = Json::array();
			for (const auto& f : files) {
				im::Image img;
				im::LoadInfo li;
				std::string err;
				Json o;
				if (im::load(krt::toPath(f), img, err, &li)) {
					const fs::path mask = (li.format == im::Format::Bmp || li.format == im::Format::Png || li.format == im::Format::Jpeg)
						? im::findMaskFile(krt::toPath(f)) : fs::path();
					o = infoJson(f, img, li, mask);
				} else {
					o = Json::object();
					o.set("path", Json(f));
					o.set("error", Json(err));
				}
				arr.push(std::move(o));
			}
			Json j = Json::object();
			j.set("files", std::move(arr));
			Json sk = Json::array();
			for (const auto& f : skipped) sk.push(Json(f));
			j.set("skipped", std::move(sk));
			return appserve::Response::json(j);
		});

		// 変換 (body: { files, opaque, alpha, sepMain, sepMask, quality, mainQuality, maskQuality,
		//              transparent, inputAddAlpha, addAlpha, outDir, force })
		reg.route("/api/image/run", [this](const appserve::Request& req) {
			Json b; Json::parse(req.body, b);
			std::vector<std::string> files;
			for (size_t i = 0; i < b["files"].size(); ++i) files.push_back(b["files"][i].asStr());
			files = dropMaskInputs(files, nullptr);
			if (files.empty()) return appserve::Response::error(400, "ファイルがありません");
			const ConvertSettings cs = settingsFromJson(b);
			const std::string e = checkSettings(cs);
			if (!e.empty()) return appserve::Response::error(400, e);
			const std::string mode = b["mode"].asStr("convert");
			LayerSettings ls;
			ls.format = b["layerFormat"].asStr("png");
			ls.hidden = b["hidden"].asBool();
			const bool started = jobs_.start(mode, [files, cs, ls, mode](krt::Progress& p) {
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
					arr.push(mode == "layers" ? exportLayers(krt::toPath(files[i]), cs, ls, sub) : convertOne(krt::toPath(files[i]), cs, sub));
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
	}

private:
	krt::JobRunner& jobs_;
};

} // namespace

int main(int argc, char** argv)
{
	krt::ToolApp tool({ "krkrimg", "画像フォーマットコンバータ", KRT_VERSION });
	Settings s;
	auto q = [](const std::string& v, int& out) { out = std::atoi(v.c_str()); return out >= 1 && out <= 100; };
	tool.addOption({ "opaque", "FORMAT", "不透明な画像の形式 tlg5 / tlg6 / png / bmp / jpg (既定 tlg5)",
	                 [&s](const std::string& v) { s.conv.opaque = v; return true; } });
	tool.addOption({ "alpha", "FORMAT", "透明部分のある画像の形式 tlg5 / tlg6 / png / bmp / sep (既定 tlg5。sep = メイン/マスク分離)",
	                 [&s](const std::string& v) { s.conv.alpha = v; return true; } });
	tool.addOption({ "sep-main", "FORMAT", "分離形式のメイン jpg / png / bmp (既定 jpg)",
	                 [&s](const std::string& v) { s.conv.sepMain = v; return true; } });
	tool.addOption({ "sep-mask", "FORMAT", "分離形式のマスク jpg / png / bmp (既定 jpg)",
	                 [&s](const std::string& v) { s.conv.sepMask = v; return true; } });
	tool.addOption({ "quality", "Q", "不透明な画像を JPEG にするときの品質 1..100 (既定 90)",
	                 [&s, q](const std::string& v) { return q(v, s.conv.quality); } });
	tool.addOption({ "main-quality", "Q", "分離形式のメインの JPEG 品質 (既定 90)",
	                 [&s, q](const std::string& v) { return q(v, s.conv.mainQuality); } });
	tool.addOption({ "mask-quality", "Q", "分離形式のマスクの JPEG 品質 (既定 90)",
	                 [&s, q](const std::string& v) { return q(v, s.conv.maskQuality); } });
	tool.addOption({ "transparent", "METHOD", "完全透明部分の色 remove / keep / expand1〜expand8 (既定 remove)",
	                 [&s](const std::string& v) { s.conv.transparent = v; return true; } });
	tool.addOption({ "input-addalpha", "", "入力を ltAddAlpha 形式とみなす",
	                 [&s](const std::string&) { s.conv.inputAddAlpha = true; return true; } });
	tool.addOption({ "addalpha", "", "ltAddAlpha 形式で出力する",
	                 [&s](const std::string&) { s.conv.addAlpha = true; return true; } });
	tool.addOption({ "format", "FORMAT", "layers: 書き出す形式 png / tlg5 / tlg6 (既定 png)",
	                 [&s](const std::string& v) { s.layer.format = v; return true; } });
	tool.addOption({ "hidden", "", "layers: 非表示のレイヤも書き出す",
	                 [&s](const std::string&) { s.layer.hidden = true; return true; } });
	tool.addOption({ "out", "DIR", "出力フォルダ (相対パスは入力ファイルのフォルダから)",
	                 [&s](const std::string& v) { s.conv.outDir = v; return true; } });
	tool.addOption({ "force", "", "出力先を上書きする",
	                 [&s](const std::string&) { s.conv.force = true; return true; } });
	tool.addOption({ "json", "", "結果を JSON で出す",
	                 [&s](const std::string&) { s.json = true; return true; } });
	tool.addOption({ "quiet", "", "進捗を出さない",
	                 [&s](const std::string&) { s.quiet = true; return true; } });
	if (!tool.parseArgs(argc, argv)) return tool.exitCode();

	if (tool.cli()) return runCli(s, tool.args());

	tool.addModule(std::make_unique<ImageModule>(tool.jobs()));
	return tool.runGui();
}
