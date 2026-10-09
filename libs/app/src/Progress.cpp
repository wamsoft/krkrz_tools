#include "krt/app/Progress.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define KRT_ISATTY _isatty
#define KRT_FILENO _fileno
#else
#include <unistd.h>
#define KRT_ISATTY isatty
#define KRT_FILENO fileno
#endif

namespace krt {

namespace {
/// 標準エラーが端末か (パイプやファイルへのリダイレクトでは «\r» の上書き表示をしない)
bool stderrIsTerminal()
{
	static const bool tty = KRT_ISATTY(KRT_FILENO(stderr)) != 0;
	return tty;
}

/// 端末での表示幅 (全角 = 2 桁)。East Asian Width の W / F に当たる主な範囲
int displayWidth(uint32_t cp)
{
	if (cp < 0x1100) return 1;
	if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0x303e) || (cp >= 0x3041 && cp <= 0x33ff) ||
	    (cp >= 0x3400 && cp <= 0x4dbf) || (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xa000 && cp <= 0xa4cf) ||
	    (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) ||
	    (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1faff) ||
	    (cp >= 0x20000 && cp <= 0x3fffd))
		return 2;
	return 1;
}

/// 表示幅 maxCols 桁に収める。長いときは末尾 (ファイル名の側) を残して前を «...» で省く
/// (同じフォルダの中のファイルが同じ表示にならないように)。足りなければ空白で埋める。
/// printf の %-60.60s はバイト単位で切るので日本語の途中で切れ、文字数で切ると
/// 全角の多い行が端末の幅を超えて折り返し、«\r» で書き戻しても前の行が残る
std::string fitText(const std::string& s, int maxCols)
{
	struct Ch { size_t pos, len; int w; };
	std::vector<Ch> chars;
	int total = 0;
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = (unsigned char)s[i];
		const size_t n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
		if (i + n > s.size()) break;
		uint32_t cp = n == 1 ? c : (c & (0xff >> (n + 1)));
		for (size_t k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3f);
		const int w = displayWidth(cp);
		chars.push_back({ i, n, w });
		total += w;
		i += n;
	}
	std::string out;
	int cols = 0;
	if (total <= maxCols) {
		for (const auto& ch : chars) out.append(s, ch.pos, ch.len);
		cols = total;
	} else {
		// 末尾から maxCols - 3 桁ぶん (先頭の «...» の 3 桁を除く) を拾う。«…» (U+2026) は
		// 日本語環境の端末で 2 桁になることがあり幅がずれるので、半角の «...» を使う
		size_t first = chars.size();
		int w = 0;
		while (first > 0 && w + chars[first - 1].w <= maxCols - 3) w += chars[--first].w;
		out = "...";
		for (size_t k = first; k < chars.size(); ++k) out.append(s, chars[k].pos, chars[k].len);
		cols = w + 3;
	}
	out.append((size_t)(maxCols - cols), ' ');
	return out;
}
} // namespace

void ConsoleProgress::progress(double ratio, const std::string& message)
{
	if (quiet_ || !stderrIsTerminal()) return;
	const int percent = ratio < 0.0 ? -1 : static_cast<int>(ratio * 100.0 + 0.5);
	if (percent == lastPercent_ && percent >= 0) return;   // 同じ % の連続は省く
	lastPercent_ = percent;
	if (percent >= 0)
		std::fprintf(stderr, "\r[%3d%%] %s", percent, fitText(message, 60).c_str());
	else
		std::fprintf(stderr, "\r[ .. ] %s", fitText(message, 60).c_str());
	std::fflush(stderr);
	lineOpen_ = true;
}

void ConsoleProgress::log(const std::string& line)
{
	if (quiet_) return;
	finish();
	std::fprintf(stderr, "%s\n", line.c_str());
}

void ConsoleProgress::finish()
{
	if (lineOpen_) {
		std::fprintf(stderr, "\n");
		lineOpen_ = false;
	}
}

} // namespace krt
