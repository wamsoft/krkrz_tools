//---------------------------------------------------------------------------
// Progress — 長い処理から進捗・ログ・中断要求をやりとりする口
//
// ツールの処理本体はこのインタフェースだけを相手にする。CLI では
// ConsoleProgress (標準エラーへ表示)、GUI では JobRunner の実装
// (SSE でブラウザへ配信) が渡されるので、処理側は同じコードで済む。
//---------------------------------------------------------------------------
#pragma once
#include <string>

namespace krt {

class Progress {
public:
	virtual ~Progress() = default;

	/// 進み具合 (0..1。不明なら負) と、いま何をしているかの短い説明
	virtual void progress(double ratio, const std::string& message) = 0;
	/// 1 行のログ (結果の途中経過、警告など)
	virtual void log(const std::string& line) = 0;
	/// 中断を求められているか。処理側はループの区切りで見る
	virtual bool canceled() const = 0;
};

/// CLI 用: 進捗は標準エラーへ 1 行で上書き表示 (端末のときだけ)、ログは標準エラーへ行で出す
class ConsoleProgress : public Progress {
public:
	explicit ConsoleProgress(bool quiet = false) : quiet_(quiet) {}
	void progress(double ratio, const std::string& message) override;
	void log(const std::string& line) override;
	bool canceled() const override { return false; }
	/// 上書き表示中の行を確定させる (最後の出力の前に呼ぶ)
	void finish();

private:
	bool quiet_;
	bool lineOpen_ = false;
	int  lastPercent_ = -1;
};

} // namespace krt
