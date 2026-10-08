//---------------------------------------------------------------------------
// 電子署名 (吉里吉里の .sig / exe 埋め込み署名)
//
// 書式は吉里吉里2 のキー生成・署名ツール (krkrsign) と、本体の sigcheck
// プラグイン (krkrz_dev/src/plugins/sigcheck) に合わせる。検証の手順
// (ハッシュから外す範囲、埋め込み署名の位置) は sigcheck と同じでなければ
// ならない — 片方だけ変えると «ツールでは正常、ゲームでは破損» になる。
//
//   方式      : SHA256 + RSA-PSS (libtomcrypt)
//   公開鍵    : "-----BEGIN PUBLIC KEY-----" 〜 "-----END PUBLIC KEY-----" の base64
//   .sig      : "-- SIGNATURE - SHA256/PSS/RSA --" + base64 (改行可)
//   exe       : 16 バイト境界の目印 "XOPT_EMBED_AREA_" / "XRELEASE_SIG____" /
//               "XP3\r\n \n\x1a\x8b\x67\x01" を探し、
//               - ハッシュ = 先頭〜OPT_EMBED_AREA と、xp3 の先頭 (無ければ末尾) 以降
//               - 署名     = RELEASE_SIG の目印 + 16 + 4 の位置
//---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace krt {
class Progress;
}

namespace krt::sig {

/// 吉里吉里の exe の目印の位置 (見つからなければ 0)
struct ExeMarks {
	int64_t optEmbedArea = 0;   ///< "XOPT_EMBED_AREA_"
	int64_t coreSig      = 0;   ///< "XCORE_SIG_______"
	int64_t releaseSig   = 0;   ///< "XRELEASE_SIG____"
	int64_t xp3          = 0;   ///< 結合された xp3 の先頭

	/// 吉里吉里の exe (オプション領域と署名領域を持つ)
	bool isKrkrExecutable() const { return optEmbedArea != 0 && releaseSig != 0; }
};

/// ファイルの中の目印を探す (16 バイト境界のみ)
bool findExeMarks(const std::filesystem::path& file, ExeMarks& out, std::string* error = nullptr);

/// 吉里吉里の exe で、署名が埋め込まれているか
bool hasEmbeddedSignature(const std::filesystem::path& file);

enum class Status {
	Ok,          ///< 署名が一致した
	Broken,      ///< 署名が一致しない (破損 / 改変)
	Error,       ///< 検証できなかった (署名が無い / 鍵が読めない / ファイルが開けない)
	Canceled,
};

struct VerifyResult {
	Status      status = Status::Error;
	std::string message;   ///< Error のときの理由
};

/// 1 ファイルを検証する。
///   publicKeyText … 公開鍵の PEM を含むテキスト (前後に他の行があってもよい)
///   吉里吉里の exe なら埋め込み署名、それ以外は «ファイル名.sig» で検証する。
///   progress を渡すと 0..1 の進み具合と中断要求をやりとりする。
VerifyResult verifyFile(const std::filesystem::path& file,
                        const std::string& publicKeyText,
                        Progress* progress = nullptr);

/// Status の英語名 ("ok" / "broken" / "error" / "canceled")
const char* statusName(Status s);

//---------------------------------------------------------------------------
// 鍵の生成と署名 (旧 krkrsign と同じ書式)
//
//   公開鍵 : "-----BEGIN PUBLIC KEY-----" + PKCS#1 RSAPublicKey の base64 (64 桁折返し、CRLF)
//   秘密鍵 : "-----BEGIN RSA PRIVATE KEY-----" + PKCS#1 RSAPrivateKey の base64 (同)
//   署名   : "-- SIGNATURE - SHA256/PSS/RSA --" CRLF + base64 (同)
//            吉里吉里の exe は RELEASE_SIG の目印 + 16 + 4 の位置へ NUL 終端で書き込む
//---------------------------------------------------------------------------
struct KeyPair {
	std::string publicKey;    ///< PEM テキスト
	std::string privateKey;   ///< PEM テキスト
};

/// 鍵を作る。bits は 1024 (旧ツールと同じ。既定) / 2048 / 3072 / 4096
bool generateKeyPair(int bits, KeyPair& out, std::string& error);

struct SignResult {
	bool        ok = false;
	bool        embedded = false;   ///< exe に埋め込んだ (false = .sig を書いた)
	std::string written;            ///< 書いたファイル
	std::string message;
};

/// 1 ファイルに署名する。吉里吉里の exe なら埋め込み、それ以外は «ファイル名.sig» を書く
SignResult signFile(const std::filesystem::path& file,
                    const std::string& privateKeyText,
                    Progress* progress = nullptr);

} // namespace krt::sig
