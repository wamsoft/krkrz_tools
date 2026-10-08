//---------------------------------------------------------------------------
// xp3 アーカイブの読み書き
//
// 書式は本体の読み込み実装 (krkrz_dev/src/core/common/base/XP3Archive.cpp) と
// 吉里吉里2 のリリーサ (krkrrel) の書き出しに合わせる。
//
//   ヘッダ   : "XP3\r\n \n\x1a\x8b\x67\x01" + I64 (インデックス位置)
//              作成時は krkrrel と同じ «クッション» 形式で書く:
//              I64 = 0x17、I32 = 1 (版)、0x17 に «続きあり (0x80) の空インデックス» +
//              I64 本当のインデックス位置
//   インデックス: 1 byte (0 = 生 / 1 = zlib、0x80 = 続きあり) + サイズ + 本体
//              本体は "File" チャンクの並び。各 File は
//                "info" : I32 flags (bit31 = 展開プロテクト), I64 元サイズ, I64 格納サイズ,
//                         I16 名前の長さ, UTF-16LE の名前 ('/' 区切り)
//                "segm" : 28 バイト × n (I32 0 = 生 / 1 = zlib, I64 位置, I64 元サイズ, I64 格納サイズ)
//                "adlr" : I32 元データの adler32
//   数値はすべてリトルエンディアン。
//
// 吉里吉里2 の exe に結合された xp3 (先頭 "MZ") も読める。
//---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace krt {
class Progress;
}

namespace krt::xp3 {

constexpr uint32_t kFileProtected = 0x80000000u;

struct Segment {
	bool     compressed = false;
	uint64_t start = 0;      ///< アーカイブ先頭 (結合 exe なら xp3 の先頭) からの位置
	uint64_t orgSize = 0;
	uint64_t arcSize = 0;
};

struct Entry {
	std::string          name;      ///< UTF-8、'/' 区切り
	uint32_t             flags = 0;
	uint64_t             orgSize = 0;
	uint64_t             arcSize = 0;
	uint32_t             adler32 = 0;
	std::vector<Segment> segments;

	bool isProtected() const { return (flags & kFileProtected) != 0; }
};

//---------------------------------------------------------------------------
// 読み込み
//---------------------------------------------------------------------------
class Archive {
public:
	bool open(const std::filesystem::path& file, std::string& error);

	const std::vector<Entry>& entries() const { return entries_; }
	uint64_t baseOffset() const { return offset_; }   ///< exe 結合なら xp3 の位置
	bool     indexCompressed() const { return indexCompressed_; }
	/// 展開プロテクトの付いたファイルがあるか
	bool     anyProtected() const;

	/// 1 ファイルを読み出す (adler32 も確かめる)
	bool read(const Entry& e, std::string& out, std::string& error) const;

private:
	std::filesystem::path path_;
	uint64_t              offset_ = 0;
	bool                  indexCompressed_ = false;
	std::vector<Entry>    entries_;
};

//---------------------------------------------------------------------------
// 作成
//---------------------------------------------------------------------------
enum class Action { Compress, Store, Discard };

/// 拡張子ごとの扱いと出力のオプション (旧リリーサの .rpf と同じ項目)
struct PackOptions {
	std::set<std::string> compress;   ///< 小文字の ".ext"。拡張子なしは ""
	std::set<std::string> store;
	std::set<std::string> discard;
	bool     doCompressSizeLimit = true;
	uint64_t compressSizeLimitKB = 1024;   ///< これ以上のファイルは圧縮しない
	bool     protect = false;              ///< 展開プロテクト
	bool     compressIndex = true;

	/// 旧リリーサの既定の分類で初期化する
	static PackOptions defaults();
	/// 拡張子の扱い (リストに無い拡張子は既定の分類)
	Action classify(const std::string& ext) const;
};

/// 旧リリーサのプロファイル (.rpf、INI) を読む。出力ファイル名があれば outputName へ
bool loadRpf(const std::filesystem::path& rpf, PackOptions& opt, std::string* outputName, std::string& error);
/// プロファイルを書く (旧リリーサで読める形)
bool saveRpf(const std::filesystem::path& rpf, const PackOptions& opt, const std::string& outputName, std::string& error);

struct PackResult {
	int      files = 0;         ///< 格納したファイル
	int      compressed = 0;
	int      deduplicated = 0;  ///< 同じ中身のファイルを共有した数
	int      discarded = 0;
	uint64_t orgBytes = 0;
	uint64_t arcBytes = 0;      ///< 出力ファイルのサイズ
	bool     canceled = false;
};

/// フォルダの中身から xp3 を作る ('.' で始まる名前と CVS フォルダは除く)
PackResult pack(const std::filesystem::path& sourceDir, const std::filesystem::path& output,
                const PackOptions& opt, Progress& progress);

} // namespace krt::xp3
