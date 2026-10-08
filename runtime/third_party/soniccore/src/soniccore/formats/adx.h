#pragma once
#include "../core/common.h"

namespace sonic {

// CRI ADX (4-bit ADPCM). SADX uses ADX for music (SoundData/bgm/wma/*.adx),
// voices, and every sound effect inside the SE/*.dat banks.
struct PcmSound {
    int sampleRate = 44100;
    int channels = 1;
    std::vector<s16> samples;  // interleaved
    bool loop = false;
    size_t loopStart = 0, loopEnd = 0;  // in frames (per-channel samples)
    size_t frames() const { return channels ? samples.size() / channels : 0; }
};

bool decodeAdx(const u8* data, size_t len, PcmSound& out);

// SE bank (*.dat, "archive  V2.DMZ"): a table of named ADX blobs.
struct SoundBankEntry {
    std::string name;
    size_t offset = 0, size = 0;
};
bool parseSoundBank(const std::vector<u8>& data, std::vector<SoundBankEntry>& out);

}  // namespace sonic
