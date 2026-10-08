#include "krt/app/Progress.h"

#include <cstdio>

namespace krt {

void ConsoleProgress::progress(double ratio, const std::string& message)
{
	if (quiet_) return;
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
