// SkillEffect Studio - editor/EdVorbis.h
// Small Ogg Vorbis I decoder (Xiph Vorbis I specification: codebooks, floor 1, residue 0/1/2, channel coupling,
// IMDCT via DCT-IV + N/4 complex FFT, Vorbis window, overlap-add) for the BGM of Music.pk2 — this PC's Media
// Foundation has no Vorbis decoder for desktop apps. Floor 0 (very old encoders) is reported as unsupported.
#pragma once

#include "../core/Text.h"

#include <cstdint>
#include <string>
#include <vector>

namespace se::ed {

// interleaved 16-bit PCM; maxFrames caps the length (0 = all)
bool DecodeOggVorbis(const Bytes& ogg, std::vector<std::int16_t>& pcm, int& channels, int& rate, std::wstring& err,
                     size_t maxFrames = 0);
// fast IMDCT path == direct formula on random input (max abs error), for the tests
double VorbisImdctSelfTest(int n);

}  // namespace se::ed
