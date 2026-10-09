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
} // namespace

void ConsoleProgress::progress(double ratio, const std::string& message)
{
	if (quiet_ || !stderrIsTerminal()) return;
	const int percent = ratio < 0.0 ? -1 : static_cast<int>(ratio * 100.0 + 0.5);
	if (percent == lastPercent_ && percent >= 0) return;   // 同じ % の連続は省く
	lastPercent_ = percent;
	if (percent >= 0)
		std::fprintf(stderr, "\r[%3d%%] %-60.60s", percent, message.c_str());
	else
		std::fprintf(stderr, "\r[ .. ] %-60.60s", message.c_str());
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
