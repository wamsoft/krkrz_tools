#include "krt/audio/Audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>

#include <ebur128.h>
#include <ogg/ogg.h>
#include <opus/opusenc.h>
#include <opus/opusfile.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>

#include "krt/app/Progress.h"
#include "krt/app/Text.h"

namespace fs = std::filesystem;

namespace krt::audio {

const char* formatName(Format f)
{
	switch (f) {
	case Format::Wav:    return "wav";
	case Format::Vorbis: return "ogg";
	case Format::Opus:   return "opus";
	default:             return "unknown";
	}
}

Format formatFromName(const std::string& s)
{
	if (s == "wav") return Format::Wav;
	if (s == "ogg" || s == "vorbis") return Format::Vorbis;
	if (s == "opus") return Format::Opus;
	return Format::Unknown;
}

const char* formatExtension(Format f)
{
	switch (f) {
	case Format::Wav:    return ".wav";
	case Format::Vorbis: return ".ogg";
	case Format::Opus:   return ".opus";
	default:             return "";
	}
}

namespace {

uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

//---------------------------------------------------------------------------
// WAV
//---------------------------------------------------------------------------
bool decodeWav(const std::string& d, Pcm& pcm, Info& info, std::string& err)
{
	const auto* u = reinterpret_cast<const unsigned char*>(d.data());
	if (d.size() < 12 || std::memcmp(u, "RIFF", 4) || std::memcmp(u + 8, "WAVE", 4)) { err = "WAV ではありません"; return false; }
	int fmtTag = 0, channels = 0, rate = 0, bits = 0;
	const unsigned char* data = nullptr;
	size_t dataLen = 0;
	for (size_t pos = 12; pos + 8 <= d.size();) {
		const uint32_t len = rd32(u + pos + 4);
		const unsigned char* body = u + pos + 8;
		const size_t avail = std::min<size_t>(len, d.size() - pos - 8);
		if (!std::memcmp(u + pos, "fmt ", 4) && avail >= 16) {
			fmtTag = rd16(body);
			channels = rd16(body + 2);
			rate = (int)rd32(body + 4);
			bits = rd16(body + 14);
			if (fmtTag == 0xFFFE && avail >= 26) fmtTag = rd16(body + 24);   // WAVE_FORMAT_EXTENSIBLE
		} else if (!std::memcmp(u + pos, "data", 4)) {
			data = body;
			dataLen = avail;
		}
		pos += 8 + len + (len & 1);
	}
	if (!data || !channels || !rate) { err = "WAV の fmt / data がありません"; return false; }
	const bool isFloat = fmtTag == 3;
	if (!(fmtTag == 1 || isFloat) || !(bits == 8 || bits == 16 || bits == 24 || bits == 32 || (isFloat && bits == 64))) {
		err = "対応していない WAV の形式です (PCM 8/16/24/32bit、float 32/64bit に対応)";
		return false;
	}
	const size_t bps = (size_t)bits / 8;
	const size_t n = dataLen / bps;
	pcm.sampleRate = rate;
	pcm.channels = channels;
	pcm.data.resize(n - n % (size_t)channels);
	for (size_t i = 0; i < pcm.data.size(); ++i) {
		const unsigned char* s = data + i * bps;
		float v;
		if (isFloat && bits == 32) { float f; std::memcpy(&f, s, 4); v = f; }
		else if (isFloat) { double f; std::memcpy(&f, s, 8); v = (float)f; }
		else if (bits == 8) v = ((int)s[0] - 128) / 128.0f;
		else if (bits == 16) v = (int16_t)rd16(s) / 32768.0f;
		else if (bits == 24) v = (float)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) / 8388608.0f;
		else v = (float)((double)(int32_t)rd32(s) / 2147483648.0);
		pcm.data[i] = v;
	}
	info.format = Format::Wav;
	info.sampleRate = rate;
	info.channels = channels;
	info.frames = pcm.frames();
	info.bitsPerSample = bits;
	info.floatSamples = isFloat;
	return true;
}

bool encodeWav(const Pcm& pcm, const fs::path& file, int bits, std::string& err)
{
	const bool isFloat = bits == 32;
	const int bps = bits / 8;
	const uint64_t dataLen = (uint64_t)pcm.data.size() * (uint64_t)bps;
	if (dataLen > 0xFFFFFFF0ull) { err = "4GB を超える WAV は書けません"; return false; }
	std::string h;
	auto p16 = [&](uint16_t v) { h += (char)(v & 0xff); h += (char)(v >> 8); };
	auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) h += (char)((v >> (8 * i)) & 0xff); };
	h += "RIFF"; p32((uint32_t)(36 + dataLen)); h += "WAVE";
	h += "fmt "; p32(16); p16(isFloat ? 3 : 1); p16((uint16_t)pcm.channels); p32((uint32_t)pcm.sampleRate);
	p32((uint32_t)(pcm.sampleRate * pcm.channels * bps)); p16((uint16_t)(pcm.channels * bps)); p16((uint16_t)bits);
	h += "data"; p32((uint32_t)dataLen);
	std::string body((size_t)dataLen, '\0');
	for (size_t i = 0; i < pcm.data.size(); ++i) {
		float v = std::clamp(pcm.data[i], -1.0f, 1.0f);
		char* d = body.data() + i * (size_t)bps;
		if (isFloat) { std::memcpy(d, &pcm.data[i], 4); continue; }
		if (bits == 16) {
			const int s = (int)std::lround(v * 32767.0f);
			d[0] = (char)(s & 0xff); d[1] = (char)((s >> 8) & 0xff);
		} else {
			const int s = (int)std::lround(v * 8388607.0f);
			d[0] = (char)(s & 0xff); d[1] = (char)((s >> 8) & 0xff); d[2] = (char)((s >> 16) & 0xff);
		}
	}
	if (!writeFile(file, h + body)) { err = "書き込めません"; return false; }
	return true;
}

//---------------------------------------------------------------------------
// Ogg Vorbis
//---------------------------------------------------------------------------
struct MemReader {
	const std::string* data;
	size_t pos = 0;
};

size_t memRead(void* ptr, size_t size, size_t nmemb, void* src)
{
	auto* m = static_cast<MemReader*>(src);
	const size_t want = size * nmemb;
	const size_t n = std::min(want, m->data->size() - m->pos);
	std::memcpy(ptr, m->data->data() + m->pos, n);
	m->pos += n;
	return size ? n / size : 0;
}
int memSeek(void* src, ogg_int64_t off, int whence)
{
	auto* m = static_cast<MemReader*>(src);
	int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)m->pos : (int64_t)m->data->size();
	const int64_t np = base + off;
	if (np < 0 || np > (int64_t)m->data->size()) return -1;
	m->pos = (size_t)np;
	return 0;
}
long memTell(void* src) { return (long)static_cast<MemReader*>(src)->pos; }

bool decodeVorbis(const std::string& d, Pcm& pcm, Info& info, std::string& err)
{
	MemReader mr{ &d, 0 };
	ov_callbacks cb{ memRead, memSeek, nullptr, memTell };
	OggVorbis_File vf;
	if (ov_open_callbacks(&mr, &vf, nullptr, 0, cb) < 0) { err = "Ogg Vorbis として読めません"; return false; }
	vorbis_info* vi = ov_info(&vf, -1);
	pcm.sampleRate = (int)vi->rate;
	pcm.channels = vi->channels;
	info.format = Format::Vorbis;
	info.sampleRate = pcm.sampleRate;
	info.channels = pcm.channels;
	info.nominalBitrate = (int)vi->bitrate_nominal;
	if (vorbis_comment* vc = ov_comment(&vf, -1)) {
		for (int i = 0; i < vc->comments; ++i) {
			const std::string c(vc->user_comments[i], (size_t)vc->comment_lengths[i]);
			const auto eq = c.find('=');
			info.tags.emplace_back(c.substr(0, eq), eq == std::string::npos ? std::string() : c.substr(eq + 1));
		}
	}
	const ogg_int64_t total = ov_pcm_total(&vf, -1);
	if (total > 0) pcm.data.reserve((size_t)total * (size_t)pcm.channels);
	int section = 0;
	for (;;) {
		float** buf;
		const long n = ov_read_float(&vf, &buf, 4096, &section);
		if (n == 0) break;
		if (n < 0) continue;   // 欠損は読み飛ばす (OV_HOLE)
		for (long i = 0; i < n; ++i)
			for (int c = 0; c < pcm.channels; ++c) pcm.data.push_back(buf[c][i]);
	}
	ov_clear(&vf);
	info.frames = pcm.frames();
	return true;
}

bool encodeVorbis(const Pcm& pcm, const fs::path& file, const EncodeOptions& opt, Progress* progress, std::string& err)
{
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	if (!out) { err = "書き込めません"; return false; }
	vorbis_info vi;
	vorbis_info_init(&vi);
	if (vorbis_encode_init_vbr(&vi, pcm.channels, pcm.sampleRate, opt.vorbisQuality) != 0) {
		vorbis_info_clear(&vi);
		err = "Vorbis エンコーダを初期化できません (チャンネル数 / サンプルレート / 品質を確認)";
		return false;
	}
	vorbis_comment vc;
	vorbis_comment_init(&vc);
	vorbis_comment_add_tag(&vc, "ENCODER", "krkrz_tools");
	for (const auto& [k, v] : opt.tags) vorbis_comment_add_tag(&vc, k.c_str(), v.c_str());
	vorbis_dsp_state vd;
	vorbis_block vb;
	vorbis_analysis_init(&vd, &vi);
	vorbis_block_init(&vd, &vb);
	ogg_stream_state os;
	ogg_stream_init(&os, (int)(std::time(nullptr) & 0x7fffffff));

	ogg_packet h, hc, hcode;
	vorbis_analysis_headerout(&vd, &vc, &h, &hc, &hcode);
	ogg_stream_packetin(&os, &h);
	ogg_stream_packetin(&os, &hc);
	ogg_stream_packetin(&os, &hcode);
	ogg_page og;
	while (ogg_stream_flush(&os, &og)) {
		out.write((const char*)og.header, og.header_len);
		out.write((const char*)og.body, og.body_len);
	}

	const uint64_t frames = pcm.frames();
	uint64_t pos = 0;
	bool eos = false;
	while (!eos) {
		const uint64_t n = std::min<uint64_t>(4096, frames - pos);
		if (n == 0) {
			vorbis_analysis_wrote(&vd, 0);
		} else {
			float** buf = vorbis_analysis_buffer(&vd, (int)n);
			for (uint64_t i = 0; i < n; ++i)
				for (int c = 0; c < pcm.channels; ++c) buf[c][i] = pcm.data[(size_t)((pos + i) * pcm.channels + c)];
			vorbis_analysis_wrote(&vd, (int)n);
			pos += n;
			if (progress) {
				if (progress->canceled()) break;
				progress->progress((double)pos / (double)std::max<uint64_t>(frames, 1), std::string());
			}
		}
		while (vorbis_analysis_blockout(&vd, &vb) == 1) {
			vorbis_analysis(&vb, nullptr);
			vorbis_bitrate_addblock(&vb);
			ogg_packet op;
			while (vorbis_bitrate_flushpacket(&vd, &op)) {
				ogg_stream_packetin(&os, &op);
				while (ogg_stream_pageout(&os, &og)) {
					out.write((const char*)og.header, og.header_len);
					out.write((const char*)og.body, og.body_len);
					if (ogg_page_eos(&og)) eos = true;
				}
			}
		}
	}
	ogg_stream_clear(&os);
	vorbis_block_clear(&vb);
	vorbis_dsp_clear(&vd);
	vorbis_comment_clear(&vc);
	vorbis_info_clear(&vi);
	if (!out) { err = "書き込みに失敗しました"; return false; }
	if (progress && progress->canceled()) { err = "中断しました"; return false; }
	return true;
}

//---------------------------------------------------------------------------
// Ogg Opus
//---------------------------------------------------------------------------
bool decodeOpus(const std::string& d, Pcm& pcm, Info& info, std::string& err)
{
	int e = 0;
	OggOpusFile* of = op_open_memory(reinterpret_cast<const unsigned char*>(d.data()), d.size(), &e);
	if (!of) { err = "Ogg Opus として読めません"; return false; }
	const OpusHead* head = op_head(of, -1);
	info.format = Format::Opus;
	info.sampleRate = pcm.sampleRate = 48000;   // Opus の出力は常に 48kHz
	info.channels = pcm.channels = op_channel_count(of, -1);
	info.opusHeaderGainQ8 = head ? head->output_gain : 0;
	if (const OpusTags* t = op_tags(of, -1)) {
		for (int i = 0; i < t->comments; ++i) {
			const std::string c(t->user_comments[i], (size_t)t->comment_lengths[i]);
			const auto eq = c.find('=');
			info.tags.emplace_back(c.substr(0, eq), eq == std::string::npos ? std::string() : c.substr(eq + 1));
		}
	}
	// ヘッダゲインは PCM に入れない (変換先で扱いを決める)
	op_set_gain_offset(of, OP_ABSOLUTE_GAIN, 0);
	const ogg_int64_t total = op_pcm_total(of, -1);
	if (total > 0) pcm.data.reserve((size_t)total * (size_t)pcm.channels);
	std::vector<float> buf(5760 * 8);
	for (;;) {
		const int n = op_read_float(of, buf.data(), (int)buf.size(), nullptr);
		if (n == 0) break;
		if (n < 0) {
			if (n == OP_HOLE) continue;
			op_free(of);
			err = "Ogg Opus の読み込みに失敗しました";
			return false;
		}
		pcm.data.insert(pcm.data.end(), buf.begin(), buf.begin() + (size_t)n * (size_t)pcm.channels);
	}
	op_free(of);
	info.frames = pcm.frames();
	return true;
}

int opusWrite(void* user, const unsigned char* ptr, opus_int32 len)
{
	auto* out = static_cast<std::ofstream*>(user);
	out->write(reinterpret_cast<const char*>(ptr), len);
	return out->good() ? 0 : 1;
}
int opusClose(void*) { return 0; }

bool encodeOpus(const Pcm& pcm, const fs::path& file, const EncodeOptions& opt, Progress* progress, std::string& err)
{
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	if (!out) { err = "書き込めません"; return false; }
	OggOpusComments* comments = ope_comments_create();
	ope_comments_add(comments, "ENCODER", "krkrz_tools");
	for (const auto& [k, v] : opt.tags) ope_comments_add(comments, k.c_str(), v.c_str());
	OpusEncCallbacks cb{ opusWrite, opusClose };
	int e = 0;
	OggOpusEnc* enc = ope_encoder_create_callbacks(&cb, &out, comments, pcm.sampleRate, pcm.channels,
	                                               pcm.channels > 2 ? 1 : 0, &e);
	ope_comments_destroy(comments);
	if (!enc) { err = std::string("Opus エンコーダを初期化できません: ") + ope_strerror(e); return false; }
	if (opt.opusBitrateKbps > 0) ope_encoder_ctl(enc, OPUS_SET_BITRATE(opt.opusBitrateKbps * 1000));
	if (opt.opusHeaderGainQ8) ope_encoder_ctl(enc, OPE_SET_HEADER_GAIN(opt.opusHeaderGainQ8));

	const uint64_t frames = pcm.frames();
	bool canceled = false;
	for (uint64_t pos = 0; pos < frames;) {
		const uint64_t n = std::min<uint64_t>(48000, frames - pos);
		e = ope_encoder_write_float(enc, pcm.data.data() + pos * (uint64_t)pcm.channels, (int)n);
		if (e != OPE_OK) { ope_encoder_destroy(enc); err = ope_strerror(e); return false; }
		pos += n;
		if (progress) {
			if (progress->canceled()) { canceled = true; break; }
			progress->progress((double)pos / (double)frames, std::string());
		}
	}
	e = ope_encoder_drain(enc);
	ope_encoder_destroy(enc);
	if (canceled) { err = "中断しました"; return false; }
	if (e != OPE_OK) { err = ope_strerror(e); return false; }
	return (bool)out;
}

} // namespace

Format detect(const fs::path& file)
{
	std::ifstream f(file, std::ios::binary);
	char h[64] = {};
	f.read(h, sizeof(h));
	const auto n = f.gcount();
	if (n >= 12 && !std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVE", 4)) return Format::Wav;
	if (n >= 36 && !std::memcmp(h, "OggS", 4)) {
		if (!std::memcmp(h + 28, "OpusHead", 8)) return Format::Opus;
		if (!std::memcmp(h + 29, "vorbis", 6)) return Format::Vorbis;
	}
	return Format::Unknown;
}

bool decode(const fs::path& file, Pcm& pcm, Info& info, std::string& error)
{
	pcm = Pcm();
	info = Info();
	std::string d;
	if (!readFile(file, d)) { error = "ファイルを開けません"; return false; }
	switch (detect(file)) {
	case Format::Wav:    return decodeWav(d, pcm, info, error);
	case Format::Vorbis: return decodeVorbis(d, pcm, info, error);
	case Format::Opus:   return decodeOpus(d, pcm, info, error);
	default:             error = "対応していない形式です (WAV / Ogg Vorbis / Ogg Opus)"; return false;
	}
}

bool encode(const Pcm& pcm, const fs::path& file, const EncodeOptions& opt, Progress* progress, std::string& error)
{
	if (!pcm.channels || !pcm.sampleRate) { error = "音声がありません"; return false; }
	switch (opt.format) {
	case Format::Wav:
		if (opt.wavBits != 16 && opt.wavBits != 24 && opt.wavBits != 32) { error = "WAV のビット数は 16 / 24 / 32 (float)"; return false; }
		return encodeWav(pcm, file, opt.wavBits, error);
	case Format::Vorbis: return encodeVorbis(pcm, file, opt, progress, error);
	case Format::Opus:   return encodeOpus(pcm, file, opt, progress, error);
	default:             error = "出力形式を指定してください"; return false;
	}
}

void applyGain(Pcm& pcm, double db)
{
	if (db == 0.0) return;
	const float g = (float)std::pow(10.0, db / 20.0);
	for (auto& v : pcm.data) v *= g;
}

bool measureLoudness(const Pcm& pcm, Loudness& out, std::string& error)
{
	ebur128_state* st = ebur128_init((unsigned)pcm.channels, (unsigned long)pcm.sampleRate,
	                                 EBUR128_MODE_I | EBUR128_MODE_SAMPLE_PEAK);
	if (!st) { error = "ラウドネスの測定を始められません"; return false; }
	const uint64_t frames = pcm.frames();
	for (uint64_t pos = 0; pos < frames;) {
		const uint64_t n = std::min<uint64_t>(65536, frames - pos);
		ebur128_add_frames_float(st, pcm.data.data() + pos * (uint64_t)pcm.channels, (size_t)n);
		pos += n;
	}
	double lufs = 0.0;
	if (ebur128_loudness_global(st, &lufs) != EBUR128_SUCCESS) { ebur128_destroy(&st); error = "ラウドネスを測れません"; return false; }
	double peak = 0.0;
	for (int c = 0; c < pcm.channels; ++c) {
		double p = 0.0;
		ebur128_sample_peak(st, (unsigned)c, &p);
		peak = std::max(peak, p);
	}
	ebur128_destroy(&st);
	out.integratedLufs = lufs;
	out.samplePeak = peak;
	return true;
}

std::vector<Level> levels(const Pcm& pcm, double fps)
{
	std::vector<Level> r;
	if (fps <= 0 || !pcm.channels) return r;
	const uint64_t frames = pcm.frames();
	const double step = pcm.sampleRate / fps;
	for (uint64_t k = 0;; ++k) {
		const uint64_t a = (uint64_t)std::llround(k * step);
		if (a >= frames) break;
		const uint64_t b = std::min<uint64_t>(frames, (uint64_t)std::llround((k + 1) * step));
		double sum = 0.0;
		float peak = 0.0f;
		for (uint64_t i = a; i < b; ++i) {
			float m = 0.0f;
			for (int c = 0; c < pcm.channels; ++c) m += pcm.data[(size_t)(i * pcm.channels + c)];
			m /= (float)pcm.channels;
			sum += (double)m * m;
			peak = std::max(peak, std::fabs(m));
		}
		Level l;
		l.rms = b > a ? (float)std::sqrt(sum / (double)(b - a)) : 0.0f;
		l.peak = peak;
		r.push_back(l);
	}
	return r;
}

} // namespace krt::audio
