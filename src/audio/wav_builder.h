#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace listening::audio {

struct ProgramClip {
    std::filesystem::path wavPath;
    int repeatCount{1};
    int pauseAfterMs{0};
};

struct WavInfo {
    std::uint32_t sampleRate{};
    std::uint16_t channels{};
    std::uint16_t bitsPerSample{};
    std::uint64_t frameCount{};
};

// Concatenates uncompressed PCM WAV files and inserts digital silence.
// All source files must have the same format.
bool buildProgramWav(const std::vector<ProgramClip>& clips,
                     const std::filesystem::path& outputPath,
                     WavInfo* outputInfo,
                     std::string* error,
                     const std::function<bool()>& shouldCancel = {});

bool inspectPcmWav(const std::filesystem::path& path,
                   WavInfo* info,
                   std::string* error);

}  // namespace listening::audio
