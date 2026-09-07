#pragma once

#include "platform/windows/windows_audio.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace listening::app {

inline constexpr std::string_view kVoicePackTokenPrefix = "audiloquy-pack:";

// A voice pack is deliberately outside the base distribution. Its helper
// process isolates model/runtime toolchains from the MinGW Qt application and
// makes third-party license approval an explicit packaging decision.
class LocalVoicePackManager final {
public:
    struct Voice {
        std::string tokenId;
        std::string packId;
        std::string voiceId;
        std::string name;
        std::string locale;
        platform::windows::VoiceGender gender{platform::windows::VoiceGender::Any};
        std::filesystem::path manifestPath;
        std::filesystem::path helperPath;
    };

    struct Snapshot {
        std::vector<Voice> voices;
    };

    void discover();
    void discover(const std::vector<std::filesystem::path>& roots);

    [[nodiscard]] const std::vector<Voice>& voices() const noexcept;
    [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept;
    [[nodiscard]] bool ownsToken(std::string_view tokenId) const noexcept;
    [[nodiscard]] Snapshot snapshot() const;

    // Helper contract:
    //   helper --manifest <json> --voice <id> --locale <locale> --wpm <n>
    //          --text-file <utf8.txt> --output-wav <temporary.wav>
    // The helper must return PCM16 mono 44.1 kHz and exit 0.
    bool synthesize(const platform::windows::SynthesisRequest& request,
                    platform::windows::SynthesisResult* result,
                    std::string* error,
                    const std::function<bool()>& shouldCancel = {}) const;

    static bool synthesize(const Snapshot& snapshot,
                           const platform::windows::SynthesisRequest& request,
                           platform::windows::SynthesisResult* result,
                           std::string* error,
                           const std::function<bool()>& shouldCancel = {});

private:
    std::vector<Voice> voices_;
    std::vector<std::string> diagnostics_;
};

}  // namespace listening::app
