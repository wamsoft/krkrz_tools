//---------------------------------------------------------------------------
// 文字列とパスの変換
//
// ツール内の文字列は UTF-8 で持つ。Windows でも非 ASCII のパスを
// 正しく扱うため、std::filesystem::path との変換は必ずここを通す。
//---------------------------------------------------------------------------
#pragma once
#include <filesystem>
#include <string>

namespace krt {

/// UTF-8 文字列 → パス
std::filesystem::path toPath(const std::string& utf8);
/// パス → UTF-8 文字列 (区切りは '/' に揃える)
std::string fromPath(const std::filesystem::path& p);

/// 前後の空白を落とす
std::string trim(const std::string& s);

/// ファイル全体を読む (失敗時は false)
bool readFile(const std::filesystem::path& p, std::string& out);
/// ファイルへ書く (失敗時は false)
bool writeFile(const std::filesystem::path& p, const std::string& data);

/// 実行ファイル自身のパス
std::filesystem::path selfPath();

/// Windows でダブルクリック起動されたとき (自分しかコンソールに居ない) だけ
/// コンソール窓を隠す。コマンドプロンプトから起動した場合は何もしない。
void hideConsoleIfOwned();

} // namespace krt
