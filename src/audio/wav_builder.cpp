#include "audio/wav_builder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>

namespace listening::audio {
namespace {

template <typename T>
bool readLittle(std::istream& input, T& value) {
    std::array<unsigned char, sizeof(T)> bytes{};
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<T>(bytes[i]) << (i * 8U);
    }
    return true;
}

template <typename T>
void writeLittle(std::ostream& output, T value) {
    std::array<unsigned char, sizeof(T)> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>((value >> (i * 8U)) & 0xFFU);
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct PcmData {
    WavInfo info;
    std::vector<std::byte> bytes;
};

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool loadPcm(const std::filesystem::path& path, PcmData& result, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return fail(error, "Cannot open WAV file: " + path.string());
    }

    char riff[4]{};
    std::uint32_t riffSize{};
    char wave[4]{};
    if (!input.read(riff, 4) || !readLittle(input, riffSize) || !input.read(wave, 4) ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        return fail(error, "Not a RIFF/WAVE file: " + path.string());
    }

    bool haveFormat = false;
    bool haveData = false;
    std::uint16_t formatTag{};
    std::uint16_t blockAlign{};

    while (input && !(haveFormat && haveData)) {
        char id[4]{};
        std::uint32_t size{};
        if (!input.read(id, 4) || !readLittle(input, size)) {
            break;
        }
        const auto payloadStart = input.tellg();
        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::uint32_t byteRate{};
            if (size < 16 || !readLittle(input, formatTag) ||
                !readLittle(input, result.info.channels) ||
                !readLittle(input, result.info.sampleRate) || !readLittle(input, byteRate) ||
                !readLittle(input, blockAlign) || !readLittle(input, result.info.bitsPerSample)) {
                return fail(error, "Invalid fmt chunk: " + path.string());
            }
            haveFormat = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (size > static_cast<std::uint32_t>(std::numeric_limits<std::streamsize>::max())) {
                return fail(error, "WAV data chunk is too large: " + path.string());
            }
            result.bytes.resize(size);
            if (size != 0 && !input.read(reinterpret_cast<char*>(result.bytes.data()), size)) {
                return fail(error, "Truncated WAV data: " + path.string());
            }
            haveData = true;
        }
        if (!(haveFormat && std::memcmp(id, "data", 4) == 0)) {
            const auto padded = static_cast<std::streamoff>(size + (size & 1U));
            input.clear();
            input.seekg(payloadStart + padded);
        }
    }

    if (!haveFormat || !haveData || formatTag != 1 || result.info.channels == 0 ||
        result.info.sampleRate == 0 || result.info.bitsPerSample == 0 || blockAlign == 0) {
        return fail(error, "Only uncompressed PCM WAV is supported: " + path.string());
    }
    if (result.bytes.size() % blockAlign != 0) {
        return fail(error, "WAV data is not frame-aligned: " + path.string());
    }
    result.info.frameCount = result.bytes.size() / blockAlign;
    return true;
}

bool sameFormat(const WavInfo& left, const WavInfo& right) {
    return left.sampleRate == right.sampleRate && left.channels == right.channels &&
           left.bitsPerSample == right.bitsPerSample;
}

}  // namespace

bool inspectPcmWav(const std::filesystem::path& path, WavInfo* info, std::string* error) {
    PcmData data;
    if (!loadPcm(path, data, error)) {
        return false;
    }
    if (info != nullptr) {
        *info = data.info;
    }
    return true;
}

bool buildProgramWav(const std::vector<ProgramClip>& clips,
                     const std::filesystem::path& outputPath,
                     WavInfo* outputInfo,
                     std::string* error,
                     const std::function<bool()>& shouldCancel) {
    if (clips.empty()) {
        return fail(error, "No clips were supplied.");
    }
    if (outputPath.empty()) {
        return fail(error, "Output WAV path is empty.");
    }
    if (shouldCancel && shouldCancel()) {
        return fail(error, "WAV assembly cancelled.");
    }

    std::vector<PcmData> sources;
    sources.reserve(clips.size());
    for (const auto& clip : clips) {
        if (clip.repeatCount < 1 || clip.repeatCount > 100) {
            return fail(error, "WAV repeat count must be from 1 through 100.");
        }
        if (clip.pauseAfterMs < 0 || clip.pauseAfterMs > 3600000) {
            return fail(error, "WAV pause must be from 0 through 3600000 milliseconds.");
        }
        if (shouldCancel && shouldCancel()) {
            return fail(error, "WAV assembly cancelled.");
        }
        PcmData source;
        if (!loadPcm(clip.wavPath, source, error)) {
            return false;
        }
        if (!sources.empty() && !sameFormat(sources.front().info, source.info)) {
            return fail(error, "All WAV clips must use the same PCM format.");
        }
        sources.push_back(std::move(source));
    }

    const auto& format = sources.front().info;
    const std::uint64_t bytesPerFrame =
        static_cast<std::uint64_t>(format.channels) * format.bitsPerSample / 8U;
    if (bytesPerFrame == 0) {
        return fail(error, "Invalid PCM frame size.");
    }

    const std::uint64_t maxDataBytes =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) - 36U;
    std::uint64_t totalDataBytes = 0;
    std::vector<std::uint64_t> pauseBytes;
    pauseBytes.reserve(clips.size());
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const auto repeats = static_cast<std::uint64_t>(clips[i].repeatCount);
        const auto pauseMs = static_cast<std::uint64_t>(clips[i].pauseAfterMs);
        const auto pauseFrames = static_cast<std::uint64_t>(format.sampleRate) * pauseMs / 1000U;
        if (pauseFrames > std::numeric_limits<std::uint64_t>::max() / bytesPerFrame) {
            return fail(error, "WAV pause size exceeds the supported range.");
        }
        const auto pauseSize = pauseFrames * bytesPerFrame;
        pauseBytes.push_back(pauseSize);
        const auto sourceBytes = static_cast<std::uint64_t>(sources[i].bytes.size());
        if (sourceBytes > std::numeric_limits<std::uint64_t>::max() - pauseSize) {
            return fail(error, "WAV source size exceeds the supported range.");
        }
        const auto bytesPerRepeat = sourceBytes + pauseSize;
        if (bytesPerRepeat != 0 && repeats >
                                      std::numeric_limits<std::uint64_t>::max() / bytesPerRepeat) {
            return fail(error, "Output WAV size exceeds the supported range.");
        }
        const auto clipBytes = bytesPerRepeat * repeats;
        if (clipBytes > maxDataBytes - std::min(totalDataBytes, maxDataBytes)) {
            return fail(error, "Output WAV would exceed the RIFF 4 GiB limit.");
        }
        totalDataBytes += clipBytes;
    }

    std::error_code directoryError;
    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path(), directoryError);
        if (directoryError) {
            return fail(error, "Cannot create output directory: " + directoryError.message());
        }
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        return fail(error, "Cannot create output WAV: " + outputPath.string());
    }

    output.write("RIFF", 4);
    writeLittle<std::uint32_t>(output, static_cast<std::uint32_t>(36U + totalDataBytes));
    output.write("WAVEfmt ", 8);
    writeLittle<std::uint32_t>(output, 16);
    writeLittle<std::uint16_t>(output, 1);
    writeLittle<std::uint16_t>(output, format.channels);
    writeLittle<std::uint32_t>(output, format.sampleRate);
    const auto byteRate = static_cast<std::uint32_t>(format.sampleRate * bytesPerFrame);
    writeLittle<std::uint32_t>(output, byteRate);
    writeLittle<std::uint16_t>(output, static_cast<std::uint16_t>(bytesPerFrame));
    writeLittle<std::uint16_t>(output, format.bitsPerSample);
    output.write("data", 4);
    writeLittle<std::uint32_t>(output, static_cast<std::uint32_t>(totalDataBytes));

    std::array<char, 16384> silence{};
    const auto writeBytes = [&](const char* data, std::uint64_t size) {
        while (size > 0) {
            if (shouldCancel && shouldCancel()) {
                return false;
            }
            const auto chunk = static_cast<std::streamsize>(
                std::min<std::uint64_t>(size, silence.size()));
            output.write(data, chunk);
            if (!output) {
                return false;
            }
            data += chunk;
            size -= static_cast<std::uint64_t>(chunk);
        }
        return true;
    };
    const auto writeSilence = [&](std::uint64_t size) {
        while (size > 0) {
            if (shouldCancel && shouldCancel()) {
                return false;
            }
            const auto chunk = static_cast<std::streamsize>(
                std::min<std::uint64_t>(size, silence.size()));
            output.write(silence.data(), chunk);
            if (!output) {
                return false;
            }
            size -= static_cast<std::uint64_t>(chunk);
        }
        return true;
    };
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const auto repeats = clips[i].repeatCount;
        for (int repeat = 0; repeat < repeats; ++repeat) {
            if (!writeBytes(reinterpret_cast<const char*>(sources[i].bytes.data()),
                            static_cast<std::uint64_t>(sources[i].bytes.size()))) {
                if (shouldCancel && shouldCancel()) {
                    return fail(error, "WAV assembly cancelled.");
                }
                return fail(error, "Failed while writing output WAV.");
            }
            if (!writeSilence(pauseBytes[i])) {
                if (shouldCancel && shouldCancel()) {
                    return fail(error, "WAV assembly cancelled.");
                }
                return fail(error, "Failed while writing output WAV.");
            }
        }
    }
    if (!output) {
        return fail(error, "Failed while writing output WAV.");
    }

    if (outputInfo != nullptr) {
        *outputInfo = format;
        outputInfo->frameCount = totalDataBytes / bytesPerFrame;
    }
    return true;
}

}  // namespace listening::audio
