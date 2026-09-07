#include "audio/wav_builder.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

template <typename T>
void writeLittle(std::ostream& output, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        output.put(static_cast<char>((value >> (i * 8U)) & 0xFFU));
    }
}

bool makeTonePlaceholder(const std::filesystem::path& path, int frames) {
    constexpr std::uint32_t sampleRate = 44100;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    const auto dataBytes = static_cast<std::uint32_t>(frames * 2);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write("RIFF", 4);
    writeLittle<std::uint32_t>(output, 36 + dataBytes);
    output.write("WAVEfmt ", 8);
    writeLittle<std::uint32_t>(output, 16);
    writeLittle<std::uint16_t>(output, 1);
    writeLittle<std::uint16_t>(output, channels);
    writeLittle<std::uint32_t>(output, sampleRate);
    writeLittle<std::uint32_t>(output, sampleRate * 2);
    writeLittle<std::uint16_t>(output, 2);
    writeLittle<std::uint16_t>(output, bits);
    output.write("data", 4);
    writeLittle<std::uint32_t>(output, dataBytes);
    for (int i = 0; i < frames; ++i) {
        writeLittle<std::uint16_t>(output, static_cast<std::uint16_t>(i % 64));
    }
    return static_cast<bool>(output);
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "listening-wav-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    const auto source = root / "source.wav";
    const auto output = root / "program.wav";
    if (!makeTonePlaceholder(source, 4410)) {
        std::cerr << "Failed to create source WAV\n";
        return 1;
    }

    std::string error;
    listening::audio::WavInfo info;
    if (!listening::audio::buildProgramWav({{source, 2, 100}}, output, &info, &error)) {
        std::cerr << error << '\n';
        return 2;
    }
    // The pause belongs to every play, including the final one:
    // (0.1 seconds of source + 0.1 seconds of silence) * 2 = 0.4 seconds.
    if (info.sampleRate != 44100 || info.channels != 1 || info.bitsPerSample != 16 ||
        info.frameCount != 17640) {
        std::cerr << "Unexpected output format or duration\n";
        return 3;
    }

    // Verify the pause is inserted after each repetition, rather than once
    // after the whole repeated block.
    std::ifstream rendered(output, std::ios::binary);
    auto sampleAt = [&](std::uint64_t frame, std::uint16_t* sample) {
        rendered.seekg(static_cast<std::streamoff>(44 + frame * 2), std::ios::beg);
        rendered.read(reinterpret_cast<char*>(sample), sizeof(*sample));
        return static_cast<bool>(rendered);
    };
    std::uint16_t sample{};
    if (!sampleAt(4409, &sample) || sample != 57 || !sampleAt(4410, &sample) || sample != 0 ||
        !sampleAt(8820, &sample) || sample != 0 || !sampleAt(13229, &sample) || sample != 57 ||
        !sampleAt(13230, &sample) || sample != 0) {
        rendered.close();
        std::cerr << "Pause was not inserted after every repeated play\n";
        std::filesystem::remove_all(root, ignored);
        return 5;
    }
    rendered.close();

    const auto fiveMsOutput = root / "five-ms.wav";
    if (!listening::audio::buildProgramWav({{source, 1, 5}}, fiveMsOutput, &info, &error) ||
        info.frameCount != 4630) {
        std::cerr << "Non-frame-aligned pause was not rounded to whole PCM frames\n";
        std::filesystem::remove_all(root, ignored);
        return 6;
    }

    listening::audio::WavInfo inspected;
    if (!listening::audio::inspectPcmWav(output, &inspected, &error) ||
        inspected.frameCount != 17640) {
        std::cerr << "Inspection failed: " << error << '\n';
        return 7;
    }
    if (!listening::audio::inspectPcmWav(fiveMsOutput, &inspected, &error) ||
        inspected.frameCount != 4630) {
        std::cerr << "Five-millisecond WAV inspection failed: " << error << '\n';
        return 8;
    }

    const auto fiveSecondOutput = root / "five-second.wav";
    if (!listening::audio::buildProgramWav({{source, 1, 5000}},
                                           fiveSecondOutput,
                                           &info,
                                           &error) ||
        info.frameCount != 224910) {
        std::cerr << "Long pause WAV assembly failed: " << error << '\n';
        std::filesystem::remove_all(root, ignored);
        return 9;
    }
    std::ifstream longPause(fiveSecondOutput, std::ios::binary);
    longPause.seekg(44 + static_cast<std::streamoff>(4410 * 2), std::ios::beg);
    std::vector<char> silenceBuffer(16384);
    std::uint64_t remainingSilenceBytes = 44100ULL * 5ULL * 2ULL;
    bool silenceIsZero = true;
    while (remainingSilenceBytes > 0 && longPause) {
        const auto chunk = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remainingSilenceBytes, silenceBuffer.size()));
        longPause.read(silenceBuffer.data(), chunk);
        if (longPause.gcount() != chunk) {
            silenceIsZero = false;
            break;
        }
        for (std::streamsize index = 0; index < chunk; ++index) {
            if (silenceBuffer[static_cast<std::size_t>(index)] != 0) {
                silenceIsZero = false;
                break;
            }
        }
        if (!silenceIsZero) {
            break;
        }
        remainingSilenceBytes -= static_cast<std::uint64_t>(chunk);
    }
    longPause.close();
    if (!silenceIsZero || remainingSilenceBytes != 0) {
        std::cerr << "Long pause contains non-silent bytes\n";
        std::filesystem::remove_all(root, ignored);
        return 10;
    }
    std::filesystem::remove_all(root, ignored);
    std::cout << "wav_builder_tests passed\n";
    return 0;
}
