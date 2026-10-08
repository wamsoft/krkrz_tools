//---------------------------------------------------------------------------
// 公開 API: encodeTlg5 / encodeTlg6
//
// 移植元: 本体 SaveTLG6.cpp TVPSaveAsTLG
//   - タグがあれば "TLG0.0\0sds\x1a\0" + 生 TLG のバイト長 (LE32) + 生 TLG +
//     "tags" + チャンク長 (LE32) + "<namelen>:<name>=<valuelen>:<value>," の列
//   - タグが無ければ生 TLG のみ
//   - 名前が空のタグは書かない (チャンク自体は書く)。本体と同じ
//   - "tlg524" / "tlg624" は is24 = true (colors=3) に相当 → withAlpha=false
//
// 本体との差分:
//   - タグ名 / 値は本体では TJS 文字列 → ナロー文字列 (AsNarrowStdString) 変換を
//     経るが、ここでは呼び出し側の UTF-8 バイト列をそのまま書く。
//     ASCII の範囲なら本体と同一バイトになる。
//   - タグの順序は本体では辞書の列挙順 (ハッシュ順) で不定。ここでは引数の順。
//---------------------------------------------------------------------------
#include "krt/tlg/Tlg.h"

#include "TlgInternal.h"

#include <new>

namespace krt {
namespace tlg {

namespace {

enum class Kind { Tlg5, Tlg6 };

bool encodeCommon(Kind kind, const uint8_t* bgra, int width, int height, bool withAlpha, const Tags& tags,
                  std::vector<uint8_t>& out, std::string& error) {
	using namespace detail;
	try {
		out.clear();
		if (!bgra) throw TlgError("TLG: no image data");
		// 本体: if( height == 0 || width == 0 ) TVPThrowInternalError;
		if (width <= 0 || height <= 0) throw TlgError("TLG: invalid image size");
		if ((uint64_t)width * (uint64_t)height > (uint64_t)(1u << 30)) throw TlgError("TLG: image too large");

		const bool is24 = !withAlpha;
		auto writeRaw = [&](std::vector<uint8_t>& dst) {
			if (kind == Kind::Tlg6)
				saveTlg6Raw(bgra, width, height, is24, dst);
			else
				saveTlg5Raw(bgra, width, height, is24, dst);
		};

		if (!tags.empty()) {
			// write TLG0.0 Structured Data Stream header
			writeBytes(out, "TLG0.0\x00sds\x1a\x00", 11);
			const size_t rawlenpos = out.size();
			writeBytes(out, "0000", 4);

			// write raw TLG stream
			writeRaw(out);

			// write raw data size
			patchInt32(out, rawlenpos, (uint32_t)(out.size() - rawlenpos - 4));

			// write "tags" chunk name
			writeBytes(out, "tags", 4);

			// build tag data
			std::string tagstr;
			for (const auto& kv : tags) {
				const std::string& name = kv.first;
				if (name.empty()) continue;
				const std::string& value = kv.second;
				tagstr += std::to_string(name.length());
				tagstr += ':';
				tagstr += name;
				tagstr += '=';
				tagstr += std::to_string(value.length());
				tagstr += ':';
				tagstr += value;
				tagstr += ',';
			}

			// write chunk size / data
			writeInt32(out, (long)(uint32_t)tagstr.length());
			writeBytes(out, tagstr.data(), tagstr.length());
		} else {
			writeRaw(out);
		}
		return true;
	} catch (const std::bad_alloc&) {
		error = "TLG: insufficient memory";
	} catch (const std::exception& e) {
		error = e.what();
	}
	out.clear();
	return false;
}

} // namespace

bool encodeTlg5(const uint8_t* bgra, int width, int height, bool withAlpha, const Tags& tags,
                std::vector<uint8_t>& out, std::string& error) {
	return encodeCommon(Kind::Tlg5, bgra, width, height, withAlpha, tags, out, error);
}

bool encodeTlg6(const uint8_t* bgra, int width, int height, bool withAlpha, const Tags& tags,
                std::vector<uint8_t>& out, std::string& error) {
	return encodeCommon(Kind::Tlg6, bgra, width, height, withAlpha, tags, out, error);
}

} // namespace tlg
} // namespace krt
