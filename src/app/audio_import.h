#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace listening::app {

struct ImportedAudio {
    std::filesystem::path path;
    std::uint64_t durationMs{};
};

// Blocking worker operation. Media Foundation decodes common Windows audio
// formats; published managed files are PCM16, mono, 44100 Hz. Source is untouched.
bool importRecording(const std::filesystem::path& source,
                     const std::filesystem::path& destinationDirectory,
                     ImportedAudio* result,
                     std::string* error,
                     const std::function<bool()>& shouldCancel = {});

// Half-open millisecond range [startMs,endMs). Used by the marking preview and
// render pipeline; rejects empty/out-of-bounds ranges and publishes atomically.
bool extractRecording(const std::filesystem::path& source,
                      std::uint64_t startMs,
                      std::uint64_t endMs,
                      const std::filesystem::path& destination,
                      std::string* error,
                      const std::function<bool()>& shouldCancel = {});

}  // namespace listening::app
