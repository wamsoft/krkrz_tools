//---------------------------------------------------------------------------
// ファイル破損チェック — 処理本体 (GUI / CLI 共通)
//
// 吉里吉里2 の «ファイル破損チェックツール» (sigchk) と同じ規則で動く:
//   - 対象フォルダ以下を再帰的に走査する ('.' で始まる名前は除く)
//   - 拡張子 .sig のファイルは署名そのものなので対象にしない
//   - .exe は署名が埋め込まれた吉里吉里の exe なら対象
//   - それ以外は «ファイル名.sig» があれば対象
//   - 設定は «実行ファイル名.ini» ([message] caption / notice、公開鍵の PEM)
//---------------------------------------------------------------------------
#pragma once
#include <appserve/json.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace krt {
class Progress;
}

namespace filecheck {

/// 画面の文言と公開鍵 (旧ツールの ini と同じ内容)
struct Config {
	std::string caption = "ファイル破損チェックツール";
	std::string notice;
	std::string publicKey;   ///< PEM を含むテキスト (ini 全体でもよい)
	std::string source;      ///< 読んだファイル
};

/// ini を読む。公開鍵は ini 全体から PEM を探す (旧ツールと同じ)
bool loadConfig(const std::filesystem::path& ini, Config& cfg, std::string& error);

/// 実行ファイルの隣の «実行ファイル名.ini»
std::filesystem::path defaultConfigPath();

struct Entry {
	std::string path;      ///< 対象フォルダからの相対パス ('/' 区切り)
	std::string status;    ///< "ok" / "broken" / "error" / "canceled"
	std::string message;
	int64_t     size = 0;
	int64_t     mtime = 0; ///< UNIX 時刻 (ミリ秒)
	bool        embedded = false;   ///< exe 埋め込み署名で検証した
};

struct Report {
	std::string        root;
	std::vector<Entry> entries;       ///< 検証した (しようとした) ファイル
	int64_t            allFiles = 0;  ///< 走査したファイルの総数
	int                ok = 0, broken = 0, error = 0;
	bool               canceled = false;
	/// 走査した全ファイルの一覧 (結果のコピー用。旧ツールと同じ «日時 \t サイズ \t パス»)
	std::vector<std::string> allList;
};

/// 対象フォルダを走査して検証する
Report run(const std::filesystem::path& root, const std::string& publicKey, krt::Progress& progress);

appserve::Json toJson(const Report& r);
/// 結果の TSV (見出し行 + 対象ファイル。続けて全ファイル一覧)
std::string toTsv(const Report& r);

/// 結果の日本語表記
const char* statusLabel(const std::string& status);

} // namespace filecheck
