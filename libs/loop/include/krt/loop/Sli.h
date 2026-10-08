//---------------------------------------------------------------------------
// ループ情報 (.sli) の読み書き
//
// 本体の WaveLoopManager (krkrz_dev/src/core/common/sound/WaveLoopManager.cpp)
// の ReadInformation / WriteInformation と同じ書式。読み込みは本体の処理を
// そのまま移植している (本体が読めないものは読めない、の関係を保つ)。
//
//   旧形式 : 先頭が '#' 以外。"LoopStart=n" と "LoopLength=n" から
//            Link { From = Start + Length; To = Start } を作る
//   v2 形式: 先頭行 "#2.00"。'#' で始まる行はコメント。
//     Link  { From=<int64>; To=<int64>; Smooth=True|False; Condition=no|eq|ne|gt|ge|lt|le;
//             RefValue=<int>; CondVar=<int>; }
//     Label { Position=<int64>; Name='<utf-8>'; }
//   位置はサンプル単位。キー名の大文字小文字は区別しない。
//---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace krt::loop {

enum class Condition { None, Equal, NotEqual, Greater, GreaterOrEqual, Lesser, LesserOrEqual };

struct Link {
	int64_t   from = 0;
	int64_t   to = 0;
	bool      smooth = false;
	Condition condition = Condition::None;
	int       refValue = 0;
	int       condVar = 0;
};

struct Label {
	int64_t     position = 0;
	std::string name;   ///< UTF-8
};

struct Sli {
	std::vector<Link>  links;
	std::vector<Label> labels;
};

/// 読む。本体が読めない書式なら false (error に理由)
bool parse(const std::string& text, Sli& out, std::string& error);
/// v2 形式で書く (本体のループチューナと同じ桁揃え)
std::string write(const Sli& sli);

/// サンプルレートが変わるときの位置の換算 (四捨五入)。from → to Hz
void rescale(Sli& sli, int fromRate, int toRate);

const char* conditionName(Condition c);   ///< "no" / "eq" ...
bool conditionFromName(const std::string& s, Condition& c);

} // namespace krt::loop
