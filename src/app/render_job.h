#pragma once

#include "app/local_voice_pack.h"
#include "audio/wav_builder.h"
#include "core/project.h"
#include "platform/windows/windows_audio.h"

#include <QObject>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QThread;
class QTimer;

namespace listening::app {

// A speech turn is the smallest unit sent to a voice engine. The parser is
// shared by preview and full rendering so role-label behavior cannot drift
// between the two paths.
struct SpeechTurn {
    std::string text;
    platform::windows::VoiceGender gender{platform::windows::VoiceGender::Any};
};

[[nodiscard]] std::vector<SpeechTurn> parseSpeechTurns(const Segment& segment);

// The cache key deliberately excludes question numbers, repeats and pauses.
// Those values belong to assembly; changing them must reuse the rendered voice
// samples and only rebuild the surrounding WAV.
inline constexpr std::string_view kSpeechCacheRulesVersion = "speech-cache-v2";

[[nodiscard]] std::string speechCacheKey(
    std::string_view text,
    platform::windows::VoiceGender gender,
    platform::windows::Accent accent,
    int targetWpm,
    std::string_view voiceTokenId,
    std::string_view voiceConfigurationFingerprint = {},
    bool requireExactLocale = false,
    bool requireExactGender = false);

enum class RenderScope {
    Selected,
    All,
};

struct RenderJobRequest {
    // This is a value snapshot. The worker must never consult the live
    // MainWindow project while a job is running.
    Project project;
    std::filesystem::path outputDirectory;
    RenderScope scope{RenderScope::All};
    std::string selectedSegmentId;

    // Snapshot taken on the UI thread before start(). It keeps discovery and
    // mutable voice-pack manager state out of the worker.
    LocalVoicePackManager::Snapshot localVoicePacks;
    std::filesystem::path projectFile;
};

struct RenderedSegment {
    std::string id;
    std::filesystem::path path;
    std::optional<double> measuredWpm{};
};

struct RenderedVoiceUse {
    std::string segmentId;
    std::size_t turnIndex{};
    platform::windows::VoiceInfo voice;
    bool usedLocaleFallback{};
    bool usedGenderFallback{};
    bool cacheHit{};
};

enum class RenderPhase {
    Preparing,
    Synthesizing,
    Assembling,
    Publishing,
    Completed,
    Cancelled,
    Failed,
};

struct RenderJobProgress {
    RenderPhase phase{RenderPhase::Preparing};
    std::size_t currentSegmentIndex{};
    std::size_t totalSegments{};
    std::size_t completedSegments{};
    std::size_t currentTurnIndex{};
    std::size_t totalTurns{};
    bool cacheHit{};
    std::string currentSegmentId;
    std::string message;
};

struct RenderJobResult {
    bool success{};
    bool cancelled{};
    std::string error;
    std::vector<RenderedSegment> completedSegments;
    std::vector<RenderedVoiceUse> voiceUses;
    std::filesystem::path programPath;
};

class RenderCancellationToken final {
public:
    RenderCancellationToken();

    void cancel() noexcept;
    [[nodiscard]] bool isCancelled() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;

    friend class RenderJobRunner;
    friend class RenderJobController;
};

using RenderProgressCallback = std::function<void(const RenderJobProgress&)>;
using CancellationProbe = std::function<bool()>;
using SynthesizeFunction = std::function<bool(
    const platform::windows::SynthesisRequest&,
    platform::windows::SynthesisResult*,
    std::string*,
    const CancellationProbe&)>;

struct RenderJobDependencies {
    // Empty uses the production route: local voice-pack snapshot when the
    // selected token belongs to it, otherwise Windows SAPI.
    SynthesizeFunction synthesize;
};

class RenderJobRunner final {
public:
    [[nodiscard]] static RenderJobResult run(
        const RenderJobRequest& request,
        const RenderCancellationToken& cancellation,
        const RenderProgressCallback& progress = {},
        RenderJobDependencies dependencies = {});
};

// Qt-facing lifecycle wrapper. All progressChanged/finished emissions happen
// on the controller's thread (normally the UI thread). The worker only owns
// immutable request data and a cancellation token.
class RenderJobController final : public QObject {
    Q_OBJECT

public:
    explicit RenderJobController(QObject* parent = nullptr);
    ~RenderJobController() override;

    RenderJobController(const RenderJobController&) = delete;
    RenderJobController& operator=(const RenderJobController&) = delete;

    [[nodiscard]] bool start(RenderJobRequest request,
                             RenderJobDependencies dependencies = {});
    void cancel() noexcept;
    [[nodiscard]] bool isRunning() const noexcept;

signals:
    void progressChanged(const listening::app::RenderJobProgress& progress);
    void finished(const listening::app::RenderJobResult& result);

private:
    struct SharedState;

    void pollWorker();
    void stopWorker() noexcept;

    std::shared_ptr<SharedState> state_;
    std::unique_ptr<RenderCancellationToken> cancellation_;
    QThread* workerThread_{};
    QTimer* pollTimer_{};
    std::atomic_bool running_{false};
};

}  // namespace listening::app
