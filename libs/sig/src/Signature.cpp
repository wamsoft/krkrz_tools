#include "krt/sig/Signature.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

#include <tomcrypt.h>

#include "krt/app/Progress.h"
#include "krt/app/Text.h"

namespace fs = std::filesystem;

namespace krt::sig {

namespace {

// 目印。ツール自身のバイナリに同じ並びを作らないよう先頭は 'X' ではなく
// 空白で持ち、照合では先頭 1 文字を 'X' として扱う (sigcheck と同じ)。
const char kOptEmbedArea[] = " OPT_EMBED_AREA_";
const char kCoreSig[]      = " CORE_SIG_______";
const char kReleaseSig[]   = " RELEASE_SIG____";
const char kXp3Sig[]       = " P3\x0d\x0a\x20\x0a\x1a\x8b\x67\x01";

const char kSignMark[]    = "-- SIGNATURE - SHA256/PSS/RSA --";
const char kPubBegin[]    = "-----BEGIN PUBLIC KEY-----";
const char kPubEnd[]      = "-----END PUBLIC KEY-----";
const char kPrivBegin[]   = "-----BEGIN RSA PRIVATE KEY-----";
const char kPrivEnd[]     = "-----END RSA PRIVATE KEY-----";

constexpr int     kHashSize = 32;
constexpr int64_t kEmbeddedSigOffset = 16 + 4;   // RELEASE_SIG の目印からの距離
constexpr size_t  kEmbeddedSigMax = 15 * 1024;   // 埋め込み署名を読む上限

// libtomcrypt の初期化 (数値演算ライブラリの選択、SHA256 と乱数の登録)
void initCrypt()
{
	static std::once_flag once;
	std::call_once(once, [] {
		ltc_mp = ltm_desc;
		if (find_hash("sha256") == -1) register_hash(&sha256_desc);
		if (find_prng("sprng") == -1) register_prng(&sprng_desc);
	});
}

bool matchMark(const unsigned char* p, const char* mark)
{
	const size_t n = std::strlen(mark);
	return p[0] == 'X' && std::memcmp(p + 1, mark + 1, n - 1) == 0;
}

bool decodeBase64(const char* begin, size_t len, std::vector<unsigned char>& out, std::string& err)
{
	out.resize(len + 16);
	unsigned long outLen = (unsigned long)out.size();
	const int e = base64_decode(reinterpret_cast<const unsigned char*>(begin), (unsigned long)len,
	                            out.data(), &outLen);
	if (e != CRYPT_OK) {
		err = error_to_string(e);
		return false;
	}
	out.resize(outLen);
	return true;
}

std::string encodeBase64(const unsigned char* data, size_t len)
{
	std::string out(len * 4 / 3 + 8, '\0');
	unsigned long outLen = (unsigned long)out.size();
	base64_encode(data, (unsigned long)len, reinterpret_cast<unsigned char*>(out.data()), &outLen);
	out.resize(outLen);
	return out;
}

// 64 桁で折り返す (旧 krkrsign の Split64 と同じ。行末は CRLF)
std::string split64(const std::string& s)
{
	std::string r;
	for (size_t i = 0; i < s.size(); i += 64) r += s.substr(i, 64) + "\r\n";
	return r;
}

// テキストの中から PEM ブロックを探して DER にする
bool readPem(const std::string& text, const char* begin, const char* end,
             std::vector<unsigned char>& der, std::string& err)
{
	// BEGIN 行は末尾のダッシュの数が揺れても読めるよう、最後の 1 文字を除いて探す
	const std::string head(begin, std::strlen(begin) - 1);
	const auto b = text.find(head);
	if (b == std::string::npos) {
		err = std::string("鍵に \"") + begin + "\" が見つかりません";
		return false;
	}
	const auto e = text.find(end, b);
	if (e == std::string::npos) {
		err = std::string("鍵に \"") + end + "\" が見つかりません";
		return false;
	}
	size_t s = b + head.size();
	while (s < e && text[s] == '-') ++s;
	return decodeBase64(text.data() + s, e - s, der, err);
}

// 署名テキスト (目印 + base64) から署名バイト列を取り出す
bool parseSignature(const std::string& text, std::vector<unsigned char>& sig, std::string& err)
{
	const size_t markLen = std::strlen(kSignMark);
	if (text.compare(0, markLen, kSignMark) != 0) {
		err = "署名の書式が正しくありません";
		return false;
	}
	// 埋め込み署名は NUL で終わる
	size_t end = text.find('\0', markLen);
	if (end == std::string::npos) end = text.size();
	return decodeBase64(text.data() + markLen, end - markLen, sig, err);
}

// ハッシュの対象範囲。吉里吉里の exe はオプション領域から xp3 の手前まで
// (署名領域を含む) を外す
struct HashRange {
	int64_t size = 0;
	int64_t ignoreStart = -1;
	int64_t ignoreEnd = -1;
};

HashRange hashRangeOf(const fs::path& file, const ExeMarks& marks)
{
	HashRange r;
	std::error_code ec;
	r.size = (int64_t)fs::file_size(file, ec);
	if (marks.isKrkrExecutable()) {
		r.ignoreStart = marks.optEmbedArea;
		r.ignoreEnd   = marks.xp3 ? marks.xp3 : r.size;
	}
	return r;
}

/// 0 = 成功 / -1 = 中断 / -2 = 読めない
int computeHash(const fs::path& file, const HashRange& range, Progress* progress, unsigned char* hash)
{
	std::ifstream f(file, std::ios::binary);
	if (!f) return -2;
	hash_state st;
	sha256_init(&st);
	std::vector<unsigned char> buf(1 << 20);
	auto feed = [&](int64_t from, int64_t to) -> bool {
		f.clear();
		f.seekg(from);
		int64_t pos = from;
		while (pos < to) {
			const int64_t want = std::min<int64_t>((int64_t)buf.size(), to - pos);
			f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)want);
			const auto got = f.gcount();
			if (got <= 0) break;
			sha256_process(&st, buf.data(), (unsigned long)got);
			pos += got;
			if (progress) {
				if (progress->canceled()) return false;
				progress->progress(range.size ? (double)pos / (double)range.size : 1.0, std::string());
			}
		}
		return true;
	};
	const bool ok = range.ignoreStart >= 0
		? feed(0, range.ignoreStart) && feed(range.ignoreEnd, range.size)
		: feed(0, range.size);
	if (!ok) return -1;
	sha256_done(&st, hash);
	return 0;
}

} // namespace

bool findExeMarks(const fs::path& file, ExeMarks& out, std::string* error)
{
	out = ExeMarks();
	std::ifstream f(file, std::ios::binary);
	if (!f) {
		if (error) *error = "ファイルを開けません";
		return false;
	}
	constexpr size_t kChunk = 1 << 20;   // 16 の倍数 (目印は 16 バイト境界にある)
	std::vector<unsigned char> buf(kChunk);
	int64_t ofs = 0;
	for (;;) {
		f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)buf.size());
		const size_t got = (size_t)f.gcount();
		if (got == 0) break;
		for (size_t i = 0; i + 16 <= got; i += 16) {
			const unsigned char* p = buf.data() + i;
			if (p[0] != 'X') continue;
			if (!out.optEmbedArea && matchMark(p, kOptEmbedArea)) out.optEmbedArea = ofs + (int64_t)i;
			else if (!out.coreSig && matchMark(p, kCoreSig))      out.coreSig = ofs + (int64_t)i;
			else if (!out.releaseSig && matchMark(p, kReleaseSig)) out.releaseSig = ofs + (int64_t)i;
			else if (!out.xp3 && matchMark(p, kXp3Sig))           out.xp3 = ofs + (int64_t)i;
		}
		ofs += (int64_t)got;
		if (got < buf.size()) break;
	}
	return true;
}

bool hasEmbeddedSignature(const fs::path& file)
{
	ExeMarks m;
	if (!findExeMarks(file, m) || !m.isKrkrExecutable()) return false;
	std::ifstream f(file, std::ios::binary);
	f.seekg(m.releaseSig + kEmbeddedSigOffset);
	const size_t n = std::strlen(kSignMark);
	std::string head(n, '\0');
	f.read(head.data(), (std::streamsize)n);
	return f.gcount() == (std::streamsize)n && head == kSignMark;
}

VerifyResult verifyFile(const fs::path& file, const std::string& publicKeyText, Progress* progress)
{
	initCrypt();
	VerifyResult r;

	std::vector<unsigned char> der;
	if (!readPem(publicKeyText, kPubBegin, kPubEnd, der, r.message)) return r;

	// 吉里吉里の exe か (埋め込み署名) / 普通のファイルか (.sig)
	ExeMarks marks;
	if (!findExeMarks(file, marks, &r.message)) return r;
	const HashRange range = hashRangeOf(file, marks);

	std::string sigText;
	if (marks.isKrkrExecutable()) {
		std::ifstream f(file, std::ios::binary);
		f.seekg(marks.releaseSig + kEmbeddedSigOffset);
		sigText.resize(kEmbeddedSigMax);
		f.read(sigText.data(), (std::streamsize)sigText.size());
		sigText.resize((size_t)f.gcount());
	} else {
		fs::path sigPath = file;
		sigPath += ".sig";
		std::error_code ec;
		if (!fs::exists(sigPath, ec)) {
			r.message = "署名ファイル (.sig) がありません";
			return r;
		}
		if (!readFile(sigPath, sigText)) {
			r.message = "署名ファイルを開けません";
			return r;
		}
	}
	std::vector<unsigned char> sig;
	if (!parseSignature(sigText, sig, r.message)) return r;

	unsigned char hash[kHashSize];
	const int h = computeHash(file, range, progress, hash);
	if (h == -1) { r.status = Status::Canceled; return r; }
	if (h != 0)  { r.message = "ファイルを開けません"; return r; }

	rsa_key key;
	int e = rsa_import(der.data(), (unsigned long)der.size(), &key);
	if (e != CRYPT_OK) {
		r.message = std::string("公開鍵を読めません: ") + error_to_string(e);
		return r;
	}
	int stat = 0;
	e = rsa_verify_hash(sig.data(), (unsigned long)sig.size(), hash, kHashSize,
	                    find_hash("sha256"), kHashSize, &stat, &key);
	rsa_free(&key);
	if (e != CRYPT_OK) {
		// 署名の形が鍵と合わない (別の鍵で作った署名 / 壊れた署名) は «破損» 扱い
		r.status = Status::Broken;
		r.message = error_to_string(e);
		return r;
	}
	r.status = stat ? Status::Ok : Status::Broken;
	return r;
}

const char* statusName(Status s)
{
	switch (s) {
	case Status::Ok:       return "ok";
	case Status::Broken:   return "broken";
	case Status::Error:    return "error";
	case Status::Canceled: return "canceled";
	}
	return "error";
}

bool generateKeyPair(int bits, KeyPair& out, std::string& error)
{
	initCrypt();
	if (bits != 1024 && bits != 2048 && bits != 3072 && bits != 4096) {
		error = "鍵の長さは 1024 / 2048 / 3072 / 4096 のいずれかです";
		return false;
	}
	rsa_key key;
	int e = rsa_make_key(nullptr, find_prng("sprng"), bits / 8, 65537, &key);
	if (e != CRYPT_OK) {
		error = error_to_string(e);
		return false;
	}
	std::vector<unsigned char> buf(16 * 1024);
	unsigned long len = (unsigned long)buf.size();
	// PK_STD を付けない = PKCS#1 (旧 krkrsign の公開鍵と同じ形)
	e = rsa_export(buf.data(), &len, PK_PUBLIC, &key);
	if (e == CRYPT_OK) {
		out.publicKey = std::string(kPubBegin) + "\r\n" + split64(encodeBase64(buf.data(), len)) + kPubEnd + "\r\n";
		len = (unsigned long)buf.size();
		e = rsa_export(buf.data(), &len, PK_PRIVATE, &key);
	}
	if (e == CRYPT_OK) {
		out.privateKey = std::string(kPrivBegin) + "\r\n" + split64(encodeBase64(buf.data(), len)) + kPrivEnd + "\r\n";
	}
	rsa_free(&key);
	if (e != CRYPT_OK) {
		error = error_to_string(e);
		return false;
	}
	return true;
}

SignResult signFile(const fs::path& file, const std::string& privateKeyText, Progress* progress)
{
	initCrypt();
	SignResult r;

	std::vector<unsigned char> der;
	if (!readPem(privateKeyText, kPrivBegin, kPrivEnd, der, r.message)) return r;

	ExeMarks marks;
	if (!findExeMarks(file, marks, &r.message)) return r;
	const HashRange range = hashRangeOf(file, marks);

	unsigned char hash[kHashSize];
	const int h = computeHash(file, range, progress, hash);
	if (h == -1) { r.message = "中断しました"; return r; }
	if (h != 0)  { r.message = "ファイルを開けません"; return r; }

	rsa_key key;
	int e = rsa_import(der.data(), (unsigned long)der.size(), &key);
	if (e != CRYPT_OK) {
		r.message = std::string("秘密鍵を読めません: ") + error_to_string(e);
		return r;
	}
	if (key.type != PK_PRIVATE) {
		rsa_free(&key);
		r.message = "秘密鍵ではありません";
		return r;
	}
	std::vector<unsigned char> sig(4096);
	unsigned long sigLen = (unsigned long)sig.size();
	e = rsa_sign_hash(hash, kHashSize, sig.data(), &sigLen, nullptr, find_prng("sprng"),
	                  find_hash("sha256"), kHashSize, &key);
	rsa_free(&key);
	if (e != CRYPT_OK) {
		r.message = error_to_string(e);
		return r;
	}
	const std::string text = std::string(kSignMark) + "\r\n" + split64(encodeBase64(sig.data(), sigLen));

	if (marks.isKrkrExecutable()) {
		// exe の署名領域へ書き込む (NUL 終端)。領域の後ろにはオプション領域等が
		// 続くので、旧ツールと同じく書き込む長さだけを置き換える
		std::fstream f(file, std::ios::binary | std::ios::in | std::ios::out);
		if (!f) {
			r.message = "ファイルに書き込めません";
			return r;
		}
		f.seekp(marks.releaseSig + kEmbeddedSigOffset);
		f.write(text.c_str(), (std::streamsize)text.size() + 1);
		if (!f) {
			r.message = "ファイルに書き込めません";
			return r;
		}
		r.embedded = true;
		r.written = fromPath(file);
	} else {
		fs::path sigPath = file;
		sigPath += ".sig";
		if (!writeFile(sigPath, text)) {
			r.message = "署名ファイルを書き込めません";
			return r;
		}
		r.written = fromPath(sigPath);
	}
	r.ok = true;
	return r;
}

} // namespace krt::sig
