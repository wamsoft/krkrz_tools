// 読み込みは本体 WaveLoopManager.cpp の ReadInformation 以下をそのまま移植している。
// 挙動 (どこで失敗するか) を変えないこと。
#include "krt/loop/Sli.h"

#include <cctype>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

namespace krt::loop {

namespace {

bool isSpace(char c) { return std::isspace((unsigned char)c) != 0; }

template <class T>
bool getInt(char* s, T& v)
{
	T r = 0;
	bool sign = false;
	while (*s && (unsigned char)*s <= 0x20) s++;
	if (!*s) return false;
	if (*s == '-') {
		sign = true;
		s++;
		while (*s && (unsigned char)*s <= 0x20) s++;
		if (!*s) return false;
	}
	while (*s >= '0' && *s <= '9') {
		r *= 10;
		r += *s - '0';
		s++;
	}
	if (sign) r = -r;
	v = r;
	return true;
}

bool getBool(char* s, bool& v)
{
	if (!strcasecmp(s, "True"))  { v = true;  return true; }
	if (!strcasecmp(s, "False")) { v = false; return true; }
	if (!strcasecmp(s, "Yes"))   { v = true;  return true; }
	if (!strcasecmp(s, "No"))    { v = false; return true; }
	return false;
}

bool getEntityToken(char*& p, char** name, char** value)
{
	char* namelast;
	char* valuelast;
	char delimiter = '\0';

	while (isSpace(*p)) p++;
	if (!*p) return false;
	*name = p;
	while (!isSpace(*p) && *p != '=' && *p) p++;
	if (!*p) return false;
	namelast = p;
	while (isSpace(*p)) p++;
	if (!*p) return false;
	if (*p != '=') return false;
	p++;
	if (!*p) return false;
	while (isSpace(*p)) p++;
	if (!*p) return false;
	if (*p == '\'') delimiter = *p, p++;
	if (!*p) return false;
	*value = p;
	if (delimiter == '\0') {
		while ((!isSpace(*p) && *p != ';') && *p) p++;
	} else {
		while ((*p != delimiter) && *p) p++;
	}
	valuelast = p;
	if (*p == delimiter) p++;
	*namelast = '\0';
	*valuelast = '\0';
	return true;
}

// 本体の GetString は TVPUtf8ToWideCharString が失敗すると読み込みを失敗にする。
// 同じく UTF-8 として正しくない名前は受け付けない
bool isValidUtf8(const char* s)
{
	const auto* p = reinterpret_cast<const unsigned char*>(s);
	while (*p) {
		int n;
		if (*p < 0x80) n = 0;
		else if ((*p & 0xe0) == 0xc0) n = 1;
		else if ((*p & 0xf0) == 0xe0) n = 2;
		else if ((*p & 0xf8) == 0xf0) n = 3;
		else return false;
		++p;
		for (int k = 0; k < n; ++k, ++p)
			if ((*p & 0xc0) != 0x80) return false;
	}
	return true;
}

// Link { ... } / Label { ... } の中身を 1 つずつ読む共通部
template <class F>
bool readBlock(char*& p, F onEntry)
{
	if (*p != '{') return false;
	p++;
	if (!*p) return false;
	do {
		char* name;
		char* value;
		if (!getEntityToken(p, &name, &value)) return false;
		if (!onEntry(name, value)) return false;
		while (isSpace(*p)) p++;
		if (*p != ';' && *p != '\0') return false;
		p++;
		if (!*p) return false;
		while (isSpace(*p)) p++;
		if (!*p) return false;
		if (*p == '}') break;
	} while (true);
	p++;
	return true;
}

} // namespace

const char* conditionName(Condition c)
{
	switch (c) {
	case Condition::None:           return "no";
	case Condition::Equal:          return "eq";
	case Condition::NotEqual:       return "ne";
	case Condition::Greater:        return "gt";
	case Condition::GreaterOrEqual: return "ge";
	case Condition::Lesser:         return "lt";
	case Condition::LesserOrEqual:  return "le";
	}
	return "no";
}

bool conditionFromName(const std::string& s, Condition& c)
{
	const char* n = s.c_str();
	if (!strcasecmp(n, "no")) { c = Condition::None;           return true; }
	if (!strcasecmp(n, "eq")) { c = Condition::Equal;          return true; }
	if (!strcasecmp(n, "ne")) { c = Condition::NotEqual;       return true; }
	if (!strcasecmp(n, "gt")) { c = Condition::Greater;        return true; }
	if (!strcasecmp(n, "ge")) { c = Condition::GreaterOrEqual; return true; }
	if (!strcasecmp(n, "lt")) { c = Condition::Lesser;         return true; }
	if (!strcasecmp(n, "le")) { c = Condition::LesserOrEqual;  return true; }
	return false;
}

bool parse(const std::string& text, Sli& out, std::string& error)
{
	out = Sli();
	std::vector<char> buf(text.begin(), text.end());
	buf.push_back('\0');
	char* p = buf.data();
	char* p_org = p;

	if (*p != '#') {
		// 旧形式
		char* p_length = std::strstr(p, "LoopLength=");
		char* p_start  = std::strstr(p, "LoopStart=");
		if (!p_length || !p_start) { error = "LoopStart / LoopLength がありません"; return false; }
		int64_t start, length;
		if (!getInt(p_length + 11, length) || !getInt(p_start + 10, start)) { error = "数値を読めません"; return false; }
		Link link;
		link.from = start + length;
		link.to = start;
		out.links.push_back(link);
		return true;
	}

	if (std::strncmp(p, "#2.00", 5) > 0) { error = "対応していない版です"; return false; }
	while (true) {
		if ((p == p_org || p[-1] == '\n') && *p == '#') {
			while (*p != '\n' && *p) p++;
			if (!*p) break;
			p++;
			continue;
		}
		while (isSpace(*p)) p++;
		if (!*p) break;

		if (!strncasecmp(p, "Link", 4) && !std::isalpha((unsigned char)p[4])) {
			p += 4;
			while (isSpace(*p)) p++;
			if (!*p) { error = "Link の途中で終わっています"; return false; }
			Link link;
			const bool ok = readBlock(p, [&link](char* name, char* value) {
				if (!strcasecmp(name, "From"))      return getInt(value, link.from);
				if (!strcasecmp(name, "To"))        return getInt(value, link.to);
				if (!strcasecmp(name, "Smooth"))    return getBool(value, link.smooth);
				if (!strcasecmp(name, "Condition")) return conditionFromName(value, link.condition);
				if (!strcasecmp(name, "RefValue"))  return getInt(value, link.refValue);
				if (!strcasecmp(name, "CondVar"))   return getInt(value, link.condVar);
				return false;
			});
			if (!ok) { error = "Link の書式が正しくありません"; return false; }
			out.links.push_back(link);
		} else if (!strncasecmp(p, "Label", 5) && !std::isalpha((unsigned char)p[5])) {
			p += 5;
			while (isSpace(*p)) p++;
			if (!*p) { error = "Label の途中で終わっています"; return false; }
			Label label;
			const bool ok = readBlock(p, [&label](char* name, char* value) {
				if (!strcasecmp(name, "Position")) return getInt(value, label.position);
				if (!strcasecmp(name, "Name")) {
					if (!isValidUtf8(value)) return false;
					label.name = value;
					return true;
				}
				return false;
			});
			if (!ok) { error = "Label の書式が正しくありません"; return false; }
			out.labels.push_back(label);
		} else {
			error = "Link / Label 以外の記述があります";
			return false;
		}
		while (isSpace(*p)) p++;
		if (!*p) break;
	}
	return true;
}

namespace {
void pad(std::string& l, size_t col)
{
	if (l.size() < col) l.append(col - l.size(), ' ');
}
} // namespace

std::string write(const Sli& sli)
{
	std::string s = "#2.00\n# Sound Loop Information (utf-8)\n# Generated by krkrz_tools\n";
	for (const auto& k : sli.links) {
		std::string l = "Link { ";
		l += "From=" + std::to_string(k.from) + ";";
		pad(l, 30);
		l += "To=" + std::to_string(k.to) + ";";
		pad(l, 51);
		l += std::string("Smooth=") + (k.smooth ? "True" : "False") + ";";
		pad(l, 65);
		l += std::string("Condition=") + conditionName(k.condition) + ";";
		pad(l, 79);
		l += "RefValue=" + std::to_string(k.refValue) + ";";
		pad(l, 100);
		l += "CondVar=" + std::to_string(k.condVar) + ";";
		pad(l, 112);
		l += "}\n";
		s += l;
	}
	for (const auto& b : sli.labels) {
		std::string l = "Label { ";
		l += "Position=" + std::to_string(b.position) + ";";
		pad(l, 35);
		l += "Name='" + b.name + "'; ";
		pad(l, 85);
		l += "}\n";
		s += l;
	}
	return s;
}

void rescale(Sli& sli, int fromRate, int toRate)
{
	if (fromRate <= 0 || toRate <= 0 || fromRate == toRate) return;
	auto conv = [&](int64_t v) {
		const long double r = (long double)v * toRate / fromRate;
		return (int64_t)(r < 0 ? r - 0.5L : r + 0.5L);
	};
	for (auto& k : sli.links) { k.from = conv(k.from); k.to = conv(k.to); }
	for (auto& b : sli.labels) b.position = conv(b.position);
}

} // namespace krt::loop
