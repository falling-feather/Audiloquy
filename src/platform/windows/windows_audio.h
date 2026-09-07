#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace listening::platform::windows {

// All public text is UTF-8. Windows/SAPI strings are converted at the platform
// boundary with strict validation (invalid UTF-8/UTF-16 is rejected).
bool utf8ToUtf16(std::string_view utf8, std::wstring* utf16, std::string* error);
bool utf16ToUtf8(std::wstring_view utf16, std::string* utf8, std::string* error);

enum class ComApartmentModel {
    SingleThreaded,
    MultiThreaded,
};

// Balances every successful CoInitializeEx call with CoUninitialize. If COM was
// already initialized using another apartment model, ready() remains true and
// this object does not uninitialize COM owned by the caller.
class ComApartment final {
public:
    explicit ComApartment(
        ComApartmentModel model = ComApartmentModel::SingleThreaded) noexcept;
    ~ComApartment();

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] long nativeResult() const noexcept;
    [[nodiscard]] std::string error() const;

private:
    long result_{};
    bool ready_{};
    bool ownsInitialization_{};
};

enum class Accent {
    AmericanEnglish,  // Prefer en-US.
    BritishEnglish,   // Prefer en-GB.
};

enum class VoiceGender {
    Any,     // No preference / metadata unavailable.
    Male,
    Female,
};

[[nodiscard]] std::string_view voiceGenderName(VoiceGender gender) noexcept;

struct VoiceInfo {
    std::string tokenId;
    std::string name;
    std::string locale;  // Primary locale, normally "en-US" or "en-GB".
    std::vector<std::string> locales;
    VoiceGender gender{VoiceGender::Any};
    bool isSystemDefault{};
    bool exactLocaleMatch{};  // Filled by selectVoice/synthesis selection.
};

bool listVoices(std::vector<VoiceInfo>* voices, std::string* error);

// Selection order is: requested locale, another installed English voice,
// system-default voice, then any installed SAPI voice.
bool selectVoice(Accent accent, VoiceInfo* voice, std::string* error);

struct VoiceSelectionCriteria {
    Accent accent{Accent::AmericanEnglish};
    VoiceGender preferredGender{VoiceGender::Any};
    std::string voiceTokenId;
    bool requireExactLocale{};
    bool requireExactGender{};
};

struct VoiceSelectionOutcome {
    std::size_t index{};
    bool exactLocaleMatch{};
    bool exactGenderMatch{};
    bool usedLocaleFallback{};
    bool usedGenderFallback{};
};

// Pure selection helper used by the native adapter and unit tests. Strict
// requirements reject an inaccurate locale/gender instead of silently
// presenting a fallback voice as the requested accent or role.
bool chooseBestVoice(const std::vector<VoiceInfo>& voices,
                     const VoiceSelectionCriteria& criteria,
                     VoiceSelectionOutcome* outcome,
                     std::string* error);

bool selectVoice(const VoiceSelectionCriteria& criteria,
                 VoiceInfo* voice,
                 VoiceSelectionOutcome* outcome,
                 std::string* error);

struct PcmFormat {
    std::uint32_t sampleRate{};
    std::uint16_t channels{};
    std::uint16_t bitsPerSample{};
};

inline constexpr PcmFormat kSynthesisPcmFormat{44100U, 1U, 16U};

struct SynthesisRequest {
    std::string textUtf8;
    Accent accent{Accent::AmericanEnglish};
    int targetWpm{150};
    std::filesystem::path outputWav;

    // Optional stable SAPI token id returned by listVoices(). When empty, the
    // locale preference above is used.
    std::string voiceTokenId;
    VoiceGender preferredGender{VoiceGender::Any};
    bool requireExactLocale{};
    bool requireExactGender{};
};

struct SynthesisResult {
    std::filesystem::path outputWav;
    VoiceInfo voice;
    PcmFormat format{kSynthesisPcmFormat};
    int targetWpm{};
    int sapiRate{};
    std::uint64_t pcmFrameCount{};
    double durationSeconds{};
    bool usedLocaleFallback{};
    bool usedGenderFallback{};
    std::string rateNotice;
};

// SAPI only exposes the engine-dependent integer range [-10, 10]. This maps a
// requested WPM around a 150 WPM reference using 10% logarithmic steps, then
// clamps to the SAPI range. It is a preview approximation, not measured WPM.
[[nodiscard]] int approximateSapiRateForWpm(int targetWpm) noexcept;

// Synchronously synthesizes UTF-8 text and atomically publishes a RIFF/WAVE
// file. The resulting stream is guaranteed to be PCM16, mono, 44.1 kHz or the
// call fails. Playback is separate and asynchronous.
bool synthesizeToPcmWav(const SynthesisRequest& request,
                        SynthesisResult* result,
                        std::string* error,
                        const std::function<bool()>& shouldCancel = {});

struct WavInfo {
    PcmFormat format;
    std::uint16_t blockAlign{};
    std::uint32_t byteRate{};
    std::uint64_t dataBytes{};
    std::uint64_t frameCount{};
    double durationSeconds{};
};

// Small RIFF parser used to verify synthesis output and useful to callers that
// need to assert the fixed PCM contract before concatenating clips.
bool inspectPcmWav(const std::filesystem::path& path, WavInfo* info, std::string* error);

class WavPlayer final {
public:
    enum class State {
        Stopped,
        Playing,
        Paused,
    };

    struct Snapshot {
        State state{State::Stopped};
        std::uint64_t positionMilliseconds{};
        std::uint64_t durationMilliseconds{};
        bool looping{};
    };

    WavPlayer();
    ~WavPlayer();

    WavPlayer(const WavPlayer&) = delete;
    WavPlayer& operator=(const WavPlayer&) = delete;

    // Starts playback and returns immediately. loop=true repeats until stop().
    // The player owns an in-memory PCM buffer and a waveOut device, so state,
    // duration and seeking are queryable without handing playback to another
    // application.
    bool playAsync(const std::filesystem::path& wavPath, bool loop, std::string* error);
    bool pause(std::string* error = nullptr);
    bool resume(std::string* error = nullptr);
    bool seek(std::uint64_t positionMilliseconds, std::string* error = nullptr);
    bool stop(std::string* error = nullptr) noexcept;

    // snapshot() also performs the lightweight end-of-stream loop handoff.
    // Call it periodically while the UI is visible.
    [[nodiscard]] Snapshot snapshot(std::string* error = nullptr) noexcept;
    [[nodiscard]] bool active() noexcept;
    [[nodiscard]] bool looping() const noexcept;
    [[nodiscard]] std::filesystem::path currentPath() const;

private:
    struct PlaybackContext;
    bool startUnlocked(std::uint64_t offsetBytes, std::string* error);
    bool closeUnlocked(std::string* error) noexcept;

    mutable std::mutex mutex_;
    std::filesystem::path currentPath_;
    std::unique_ptr<PlaybackContext> context_;
    std::uint64_t durationMilliseconds_{};
    bool active_{};
    bool looping_{};
    bool paused_{};
};

}  // namespace listening::platform::windows
