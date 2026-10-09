//---------------------------------------------------------------------------
// krt::release — 配布用の実行可能ファイルを作る (Windows の吉里吉里Z の exe が対象)
//
// 本体に新しい目印は足さず、既にある仕組みだけを書き換える
// (設計: krkrz_dev/src/core/doc/ReleaseEmbedding.md)。
//
//   1. exe をコピーして改名する
//   2. リソースを書き換える (BeginUpdateResource / UpdateResource / EndUpdateResource)
//        埋め込みオプション : WINVER = TEXT / 139 (IDR_OPTION。中身が空だと exe に無いので足す)、
//                             SDL = BINARY / CONFIG.CF (resource/config.cf。本体の既定値に重ねる)。
//                             値に ASCII 以外を含む行は «name="\xNN..."» の形に直す
//        アイコン           : 既存のアイコングループ (WINVER は 107) を全部差し替え。無ければ 107 を足す
//        バージョン情報     : RT_VERSION 1 の文字列
//   3. セキュリティ設定 «-- TVPSystemSecurityOptions name(n):... --» の数字だけを
//      同じ長さで書き換える (PE の構造に触れない)
//   4. データ: exe の隣に xp3 を置く、または exe の後ろに 16 バイト境界で結合する
//      (フォルダを渡せば xp3 を作る)
//   5. 署名 (.sig) を作る — 必ず最後。exe を後から触ると検証に失敗する
//
// UpdateResource は exe の末尾に付いたデータ (結合した xp3 や Authenticode の署名) を
// 保持しないので、末尾にデータの付いた exe は元にできない。
//---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace krt {
class Progress;
}

namespace krt::release {

/// exe の種類 (オプションの置き場所で判別する)
enum class ExeKind { Unknown, Winver, Sdl };
const char* kindName(ExeKind k);

/// exe を調べた結果
struct ExeInfo {
	ExeKind     kind = ExeKind::Unknown;
	uint64_t    fileSize = 0;
	uint64_t    imageEnd = 0;          ///< PE のイメージの終わり (= 後ろに付いたデータの始まり)
	bool        hasOverlay = false;    ///< イメージの後ろにデータがある (結合済みの xp3・署名など)
	std::string options;               ///< 今の埋め込みオプション (UTF-8)
	bool        hasIcon = false;       ///< アイコングループがある
	std::vector<std::pair<std::string, std::string>> version;   ///< RT_VERSION の文字列
	std::vector<std::pair<std::string, int>> security;          ///< セキュリティ設定 (名前, 値)
};

bool inspect(const std::filesystem::path& exe, ExeInfo& out, std::string& error);

/// リリース設定 (release.json と同じ項目)
struct Settings {
	std::string exe;          ///< 元にする吉里吉里の exe
	std::string output;       ///< 作る exe (相対パスは設定ファイルのフォルダ基準)
	bool        setOptions = false;
	std::string options;      ///< 埋め込みオプション (.cf と同じ書式、1 行 1 オプション)
	std::string icon;         ///< .ico (空 = 変えない)
	std::vector<std::pair<std::string, std::string>> version;   ///< 変える文字列 (空の値は変えない)
	std::map<std::string, int> security;   ///< 変える項目だけ (forcedataxp3 など)

	enum class DataMode { None, Copy, Bind };
	DataMode    dataMode = DataMode::None;
	std::string data;         ///< xp3 ファイル、またはフォルダ (フォルダなら xp3 を作る)
	std::string rpf;          ///< フォルダから作るときのプロファイル (空 = フォルダの default.rpf か既定)
	std::string dataName = "data.xp3";   ///< Copy のときの名前

	std::string signKey;      ///< 秘密鍵 (空 = 署名しない)
	bool        force = false;   ///< 出力先を上書きする
};

bool loadSettings(const std::filesystem::path& json, Settings& out, std::string& error);
bool saveSettings(const std::filesystem::path& json, const Settings& s, std::string& error);
/// JSON 文字列 ⇔ 設定 (画面とのやりとり用)
bool settingsFromJsonText(const std::string& text, Settings& out, std::string& error);
std::string settingsToJsonText(const Settings& s);

struct Result {
	bool ok = false;
	std::string message;                ///< 失敗の理由
	std::vector<std::string> outputs;   ///< 作ったファイル
	std::vector<std::string> notes;     ///< 行ったこと (表示用)
};

/// リリースする。baseDir = 設定ファイルのフォルダ (相対パスの基準)
Result run(const Settings& s, const std::filesystem::path& baseDir, Progress& progress);

/// セキュリティ設定の項目名 (WINVER / SDL 共通の順)
const std::vector<std::string>& securityNames();

} // namespace krt::release
