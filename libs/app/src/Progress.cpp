#include "krt/app/Progress.h"

#include <cstdio>

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

/// UTF-8 の文字の境界で最大 maxChars 文字に切り、足りなければ空白で埋める
/// (printf の %-60.60s はバイト単位で切るので、日本語の途中で切れて表示が崩れる)
std::string fitText(const std::string& s, size_t maxChars)
{
	std::string out;
	size_t chars = 0;
	for (size_t i = 0; i < s.size() && chars < maxChars;) {
		const unsigned char c = (unsigned char)s[i];
		const size_t n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
		if (i + n > s.size()) break;
		out.append(s, i, n);
		i += n;
		++chars;
	}
	out.append(maxChars - chars, ' ');
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
