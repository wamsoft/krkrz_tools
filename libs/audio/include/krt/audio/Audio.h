//---------------------------------------------------------------------------
// 音声の読み書き (WAV / Ogg Vorbis / Ogg Opus) と解析
//
// 本体のデコーダ (krkrz_dev/src/core/common/sound) との関係:
//   - Opus のヘッダゲイン (output_gain、Q7.8 dB) は本体が常に適用する
//   - Vorbis の "replaygain_track_gain" / "replaygain_album_gain" コメントは、
//     本体を -ogg_rg=track|album で起動したときだけ適用される
//   - Opus は常に 48kHz で再生される (.sli の位置も 48kHz のサンプル数)
//---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace krt {
class Progress;
}

namespace krt::audio {

enum class Format { Unknown, Wav, Vorbis, Opus };

const char* formatName(Format f);              ///< "wav" / "ogg" / "opus"
Format formatFromName(const std::string& s);   ///< "wav" / "ogg" / "vorbis" / "opus"
const char* formatExtension(Format f);         ///< ".wav" / ".ogg" / ".opus"

using Tags = std::vector<std::pair<std::string, std::string>>;

struct Info {
	Format   format = Format::Unknown;
	int      sampleRate = 0;
	int      channels = 0;
	uint64_t frames = 0;
	int      bitsPerSample = 0;     ///< WAV のみ
	bool     floatSamples = false;  ///< WAV のみ
	int      opusHeaderGainQ8 = 0;  ///< Opus のみ (Q7.8 dB)
	int      nominalBitrate = 0;    ///< Vorbis の公称ビットレート (bps)
	Tags     tags;                  ///< Vorbis / Opus のコメント
	double   seconds() const { return sampleRate ? (double)frames / sampleRate : 0.0; }
};

/// インターリーブの float PCM (-1..1)
struct Pcm {
	int                sampleRate = 0;
	int                channels = 0;
	std::vector<float> data;
	uint64_t frames() const { return channels ? data.size() / (size_t)channels : 0; }
};

/// 先頭のバイト列で形式を判定する
Format detect(const std::filesystem::path& file);

/// 読み込む。Opus のヘッダゲインは PCM に**適用しない** (info.opusHeaderGainQ8 に残す)
bool decode(const std::filesystem::path& file, Pcm& pcm, Info& info, std::string& error);

struct EncodeOptions {
	Format format = Format::Vorbis;
	int    wavBits = 16;            ///< 16 / 24 / 32 (32 は float)
	float  vorbisQuality = 0.4f;    ///< -0.1 .. 1.0
	int    opusBitrateKbps = 0;     ///< 0 = 既定 (libopus の自動)
	int    opusHeaderGainQ8 = 0;    ///< Opus のヘッダゲイン (Q7.8 dB)
	Tags   tags;                    ///< Vorbis / Opus のコメント
};

bool encode(const Pcm& pcm, const std::filesystem::path& file, const EncodeOptions& opt,
            Progress* progress, std::string& error);

/// 音量を dB で変える (波形に焼き込む)
void applyGain(Pcm& pcm, double db);

struct Loudness {
	double integratedLufs = 0.0;   ///< EBU R128 の統合ラウドネス
	double samplePeak = 0.0;       ///< 0..1
	/// ReplayGain 2.0 の基準 (-18 LUFS) に合わせるためのゲイン
	double replayGainDb() const { return -18.0 - integratedLufs; }
};
bool measureLoudness(const Pcm& pcm, Loudness& out, std::string& error);

/// 口パク用: 一定間隔 (fps) ごとの音量。全チャンネルを混ぜた RMS とピーク (0..1)
struct Level {
	float rms = 0.0f;
	float peak = 0.0f;
};
std::vector<Level> levels(const Pcm& pcm, double fps);

} // namespace krt::audio
