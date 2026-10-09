// PE のリソースの読み書き (Windows API)。Release.cpp から使う
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace krt::release::pe {

/// PE のイメージの終わり (各セクションの PointerToRawData + SizeOfRawData の最大)。読めなければ 0
uint64_t imageEnd(const std::filesystem::path& file, uint64_t& fileSize);

/// 読み取り用にリソースを開いた exe
class ResourceReader {
public:
	explicit ResourceReader(const std::filesystem::path& file);
	~ResourceReader();
	bool ok() const { return module_ != nullptr; }

	/// type / name (整数 ID は «#139» のように渡す) のリソースの言語の一覧
	std::vector<uint16_t> languages(const wchar_t* type, const wchar_t* name) const;
	/// リソースの中身 (無ければ false)
	bool read(const wchar_t* type, const wchar_t* name, uint16_t lang, std::vector<uint8_t>& out) const;
	/// その型のリソースが 1 つでもあるか
	bool hasType(const wchar_t* type) const;
	/// RT_ICON の ID の一覧
	std::vector<uint16_t> iconIds() const;
	/// その型のリソースの名前の一覧 (整数 ID は «#107» の形)
	std::vector<std::wstring> names(const wchar_t* type) const;

private:
	void* module_ = nullptr;
};

/// names() の名前を Windows API に渡す形にする («#107» → MAKEINTRESOURCE(107))
const wchar_t* resName(const std::wstring& n);

/// .ico ファイルの 1 枚
struct IconImage {
	uint8_t  width = 0, height = 0, colors = 0;
	uint16_t planes = 0, bitCount = 0;
	std::vector<uint8_t> data;   ///< BMP (DIB) または PNG
};
bool loadIco(const std::filesystem::path& ico, std::vector<IconImage>& out, std::string& error);

/// RT_GROUP_ICON の中身から、参照している RT_ICON の ID を取り出す
std::vector<uint16_t> groupIconIds(const std::vector<uint8_t>& group);
/// RT_GROUP_ICON の中身を作る
std::vector<uint8_t> buildGroupIcon(const std::vector<IconImage>& images, const std::vector<uint16_t>& ids);

/// RT_VERSION の中身
struct VersionInfo {
	std::vector<uint8_t> fixed;   ///< VS_FIXEDFILEINFO (52 バイト)
	std::wstring table = L"041104b0";   ///< StringTable の鍵 (言語 + コードページ、16 進 8 桁)
	std::vector<std::pair<std::wstring, std::wstring>> strings;
};
bool parseVersion(const std::vector<uint8_t>& data, VersionInfo& out);
std::vector<uint8_t> buildVersion(const VersionInfo& v);
/// "1.2.3.4" を VS_FIXEDFILEINFO の版 (上位 / 下位 32bit) にする。読めなければ false
bool parseVersionNumber(const std::wstring& s, uint32_t& ms, uint32_t& ls);

} // namespace krt::release::pe
