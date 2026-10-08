#include "krt/app/Text.h"

#include <fstream>
#include <iterator>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace krt {

fs::path toPath(const std::string& utf8)
{
#ifdef _WIN32
	if (utf8.empty()) return fs::path();
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
	return fs::path(w);
#else
	return fs::path(utf8);
#endif
}

std::string fromPath(const fs::path& p)
{
#ifdef _WIN32
	const std::wstring& w = p.native();
	if (w.empty()) return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	for (auto& c : s) if (c == '\\') c = '/';
	return s;
#else
	return p.string();
#endif
}

std::string trim(const std::string& s)
{
	const char* ws = " \t\r\n";
	const auto b = s.find_first_not_of(ws);
	if (b == std::string::npos) return std::string();
	const auto e = s.find_last_not_of(ws);
	return s.substr(b, e - b + 1);
}

bool readFile(const fs::path& p, std::string& out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return !f.bad();
}

bool writeFile(const fs::path& p, const std::string& data)
{
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	if (!f) return false;
	f.write(data.data(), (std::streamsize)data.size());
	return (bool)f;
}

fs::path selfPath()
{
#ifdef _WIN32
	std::wstring buf(32768, L'\0');
	const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
	buf.resize(n);
	return fs::path(buf);
#elif defined(__APPLE__)
	char buf[PATH_MAX];
	uint32_t size = sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) == 0) return fs::weakly_canonical(fs::path(buf));
	return fs::path();
#else
	std::error_code ec;
	return fs::read_symlink("/proc/self/exe", ec);
#endif
}

void hideConsoleIfOwned()
{
#ifdef _WIN32
	DWORD pids[2];
	// コンソールに繋がっているのが自分だけ = エクスプローラ等から起動された
	if (GetConsoleProcessList(pids, 2) == 1) {
		if (HWND w = GetConsoleWindow()) ShowWindow(w, SW_HIDE);
	}
#endif
}

} // namespace krt
