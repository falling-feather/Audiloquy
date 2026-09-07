#include "app/render_job.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

template <typename T>
void writeLittle(std::ostream& output, T value) {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.put(static_cast<char>((value >> (index * 8U)) & 0xffU));
    }
}

bool writeFakeWav(const std::filesystem::path& path, int frames = 441) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    constexpr std::uint32_t sampleRate = 44100;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    const auto dataBytes = static_cast<std::uint32_t>(frames * 2);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write("RIFF", 4);
    writeLittle<std::uint32_t>(output, 36U + dataBytes);
    output.write("WAVEfmt ", 8);
    writeLittle<std::uint32_t>(output, 16U);
    writeLittle<std::uint16_t>(output, 1U);
    writeLittle<std::uint16_t>(output, channels);
    writeLittle<std::uint32_t>(output, sampleRate);
    writeLittle<std::uint32_t>(output, sampleRate * 2U);
    writeLittle<std::uint16_t>(output, 2U);
    writeLittle<std::uint16_t>(output, bits);
    output.write("data", 4);
    writeLittle<std::uint32_t>(output, dataBytes);
    for (int frame = 0; frame < frames; ++frame) {
        writeLittle<std::uint16_t>(output, static_cast<std::uint16_t>((frame % 100) + 1));
    }
    return static_cast<bool>(output);
}

listening::Project makeProject() {
    listening::Project project;
    project.id = "render-job-tests";
    project.title = "Render job tests";
    project.accent = listening::Accent::American;
    project.targetWpm = 120.0;
    project.voiceSettings.maleVoiceTokenId = "fake-male";
    project.voiceSettings.femaleVoiceTokenId = "fake-female";
    project.voiceSettings.strictAccent = true;
    project.voiceSettings.allowGenderFallback = false;
    project.segments.push_back(listening::Segment{
        "s1",
        {1, 1},
        "Man + Woman",
        "MAN: Hello from the first turn.\nWOMAN: Hello from the second turn.",
        0.1,
        2,
    });
    return project;
}

listening::app::RenderJobDependencies fakeDependencies(int* calls,
                                                        bool slow = false) {
    listening::app::RenderJobDependencies dependencies;
    dependencies.synthesize = [calls, slow](
                                  const listening::platform::windows::SynthesisRequest& request,
                                  listening::platform::windows::SynthesisResult* result,
                                  std::string* error,
                                  const listening::app::CancellationProbe& probe) {
        ++*calls;
        if (slow) {
            for (int tick = 0; tick < 500; ++tick) {
                if (probe && probe()) {
                    if (error != nullptr) {
                        *error = "fake synthesis cancelled";
                    }
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if (probe && probe()) {
            if (error != nullptr) {
                *error = "fake synthesis cancelled";
            }
            return false;
        }
        if (!writeFakeWav(request.outputWav)) {
            if (error != nullptr) {
                *error = "fake synthesis could not write WAV";
            }
            return false;
        }
        if (result != nullptr) {
            result->outputWav = request.outputWav;
            result->voice = listening::platform::windows::VoiceInfo{
                request.voiceTokenId,
                request.voiceTokenId == "fake-male" ? "Fake male" : "Fake female",
                "en-US",
                {"en-US"},
                request.preferredGender,
                false,
                true,
            };
            result->format = listening::platform::windows::kSynthesisPcmFormat;
            result->targetWpm = request.targetWpm;
            result->pcmFrameCount = 441;
            result->durationSeconds = 0.01;
        }
        return true;
    };
    return dependencies;
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace listening;
    using namespace listening::app;

    QCoreApplication application(argc, argv);

    Segment parserSegment{
        "parser",
        {1, 1},
        "Man + Woman",
        "MAN: First line.\n  continued line.\nWOMAN: Second line.",
        0.0,
        1,
    };
    const auto parsed = parseSpeechTurns(parserSegment);
    if (parsed.size() != 2 || parsed[0].gender != platform::windows::VoiceGender::Male ||
        parsed[0].text != "First line. continued line." ||
        parsed[1].gender != platform::windows::VoiceGender::Female) {
        std::cerr << "Speech role parser regression\n";
        return 1;
    }
    const auto plainWoman = parseSpeechTurns(Segment{
        "plain-woman", {1, 1}, "Woman", "A single female speaker.", 0.0, 1});
    const auto plainMan = parseSpeechTurns(Segment{
        "plain-man", {1, 1}, "Man", "A single male speaker.", 0.0, 1});
    const auto mixedSpeaker = parseSpeechTurns(Segment{
        "mixed-speaker", {1, 1}, "Man + Woman", "Two speakers.", 0.0, 1});
    if (plainWoman.size() != 1 ||
        plainWoman.front().gender != platform::windows::VoiceGender::Female ||
        plainMan.size() != 1 ||
        plainMan.front().gender != platform::windows::VoiceGender::Male ||
        mixedSpeaker.size() != 1 ||
        mixedSpeaker.front().gender != platform::windows::VoiceGender::Any) {
        std::cerr << "Speaker gender parser regression\n";
        return 2;
    }

    const auto root = std::filesystem::temp_directory_path() / "audiloquy-render-job-tests";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    Project project = makeProject();
    RenderJobRequest request;
    request.project = project;
    request.outputDirectory = root / "first";
    request.scope = RenderScope::Selected;
    request.selectedSegmentId = "s1";

    int calls = 0;
    RenderCancellationToken token;
    const RenderJobResult first = RenderJobRunner::run(
        request, token, {}, fakeDependencies(&calls));
    if (!first.success || first.cancelled || first.completedSegments.size() != 1 ||
        !first.programPath.empty() || calls != 2 ||
        !std::filesystem::is_regular_file(first.completedSegments.front().path)) {
        std::cerr << "Selected render or role synthesis failed; calls=" << calls
                  << ", success=" << first.success << ", cancelled=" << first.cancelled
                  << ", error=" << first.error << ", completed="
                  << first.completedSegments.size() << ", voiceUses=" << first.voiceUses.size()
                  << '\n';
        std::filesystem::remove_all(root, ignored);
        return 3;
    }

    // Assembly settings change the immutable result filename but must reuse
    // the two speech cache entries.
    request.project.segments.front().pauseAfterSeconds = 0.4;
    request.project.segments.front().repeatCount = 3;
    const RenderJobResult second = RenderJobRunner::run(
        request, token, {}, fakeDependencies(&calls));
    if (!second.success || second.completedSegments.size() != 1 || calls != 2 ||
        second.completedSegments.front().path == first.completedSegments.front().path ||
        !std::filesystem::is_regular_file(second.completedSegments.front().path)) {
        std::cerr << "Speech cache was not reused across assembly changes; calls=" << calls
                  << ", success=" << second.success << ", cancelled=" << second.cancelled
                  << ", error=" << second.error << "\nfirst="
                  << first.completedSegments.front().path.string() << "\nsecond="
                  << (second.completedSegments.empty()
                          ? std::string("<none>")
                          : second.completedSegments.front().path.string())
                  << "\nvoiceUses=" << second.voiceUses.size();
        for (const auto& use : second.voiceUses) {
            std::cerr << " [" << use.turnIndex << ":" << use.voice.tokenId
                      << ",cacheHit=" << use.cacheHit << "]";
        }
        std::cerr << '\n';
        std::filesystem::remove_all(root, ignored);
        return 4;
    }

    // Invalidating the raw and per-turn cache files forces both speech turns
    // to be synthesized again rather than accepting corrupted bytes.
    for (const auto& entry : std::filesystem::directory_iterator(request.outputDirectory /
                                                                   ".cache" / "voice")) {
        if (entry.is_regular_file()) {
            std::ofstream corrupt(entry.path(), std::ios::binary | std::ios::trunc);
            corrupt << "broken";
        }
    }
    const RenderJobResult rebuilt = RenderJobRunner::run(
        request, token, {}, fakeDependencies(&calls));
    if (!rebuilt.success || calls != 4 || rebuilt.completedSegments.size() != 1) {
        std::cerr << "Corrupted speech cache was not rebuilt\n";
        std::filesystem::remove_all(root, ignored);
        return 5;
    }

    // Text and selected voice are part of the speech identity. Each change
    // should invalidate only the affected voice cache entry.
    request.project.segments.front().text =
        "MAN: Changed text for the first turn.\nWOMAN: The second turn remains clear.";
    const RenderJobResult textChanged = RenderJobRunner::run(
        request, token, {}, fakeDependencies(&calls));
    if (!textChanged.success || calls != 6 || textChanged.completedSegments.size() != 1 ||
        textChanged.completedSegments.front().path == rebuilt.completedSegments.front().path) {
        std::cerr << "Text change did not invalidate speech cache\n";
        std::filesystem::remove_all(root, ignored);
        return 6;
    }
    request.project.voiceSettings.maleVoiceTokenId = "fake-male-v2";
    const RenderJobResult voiceChanged = RenderJobRunner::run(
        request, token, {}, fakeDependencies(&calls));
    if (!voiceChanged.success || calls != 7 || voiceChanged.completedSegments.size() != 1 ||
        voiceChanged.completedSegments.front().path == textChanged.completedSegments.front().path) {
        std::cerr << "Voice change did not invalidate speech cache\n";
        std::filesystem::remove_all(root, ignored);
        return 7;
    }

    // Selected generation validates only the selected group; all generation
    // still rejects an invalid group before publishing a complete program.
    Project partiallyInvalid = makeProject();
    partiallyInvalid.segments.push_back(Segment{
        "s2", {2, 2}, "Woman", "", 0.0, 1});
    RenderJobRequest selectedOnly{
        partiallyInvalid,
        root / "selected-only",
        RenderScope::Selected,
        "s1",
        {},
    };
    int selectedCalls = 0;
    const RenderJobResult selectedResult = RenderJobRunner::run(
        selectedOnly, token, {}, fakeDependencies(&selectedCalls));
    if (!selectedResult.success || selectedResult.completedSegments.size() != 1) {
        std::cerr << "Selected render incorrectly validated unrelated groups\n";
        std::filesystem::remove_all(root, ignored);
        return 8;
    }
    selectedOnly.scope = RenderScope::All;
    const RenderJobResult allResult = RenderJobRunner::run(
        selectedOnly, token, {}, fakeDependencies(&selectedCalls));
    if (allResult.success || !allResult.completedSegments.empty()) {
        std::cerr << "All render accepted an invalid group\n";
        std::filesystem::remove_all(root, ignored);
        return 9;
    }

    // The injected engine blocks in small increments and observes the same
    // cancellation token used by the production SAPI/helper adapters.
    RenderJobRequest slowRequest{
        makeProject(),
        root / "cancelled",
        RenderScope::Selected,
        "s1",
        {},
    };
    int slowCalls = 0;
    RenderCancellationToken slowToken;
    RenderJobResult cancelledResult;
    std::thread worker([&] {
        cancelledResult = RenderJobRunner::run(
            slowRequest, slowToken, {}, fakeDependencies(&slowCalls, true));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    slowToken.cancel();
    worker.join();
    bool published = false;
    if (std::filesystem::exists(slowRequest.outputDirectory)) {
        for (const auto& entry :
             std::filesystem::directory_iterator(slowRequest.outputDirectory)) {
            published = published ||
                        (entry.is_regular_file() && entry.path().extension() == ".wav");
        }
    }
    if (cancelledResult.success || !cancelledResult.cancelled ||
        !cancelledResult.completedSegments.empty() || published) {
        std::cerr << "Render cancellation did not stop before publishing\n";
        std::filesystem::remove_all(root, ignored);
        return 10;
    }

    // The Qt wrapper must keep the worker off the UI thread, reject a second
    // start while busy, and remain reusable after cancellation.
    RenderJobRequest controllerRequest{
        makeProject(),
        root / "controller",
        RenderScope::Selected,
        "s1",
        {},
    };
    int controllerCalls = 0;
    RenderJobController controller;
    bool controllerFinished = false;
    bool controllerProgressSeen = false;
    bool controllerCallbacksOnUiThread = true;
    RenderJobResult controllerResult;
    QObject::connect(&controller,
                     &RenderJobController::progressChanged,
                     &application,
                     [&](const RenderJobProgress&) {
                         controllerProgressSeen = true;
                         controllerCallbacksOnUiThread =
                             controllerCallbacksOnUiThread &&
                             QThread::currentThread() == application.thread();
                     });
    QObject::connect(&controller,
                     &RenderJobController::finished,
                     &application,
                     [&](const RenderJobResult& result) {
                         controllerFinished = true;
                         controllerResult = result;
                         controllerCallbacksOnUiThread =
                             controllerCallbacksOnUiThread &&
                             QThread::currentThread() == application.thread();
                         application.quit();
                     });
    const auto startTime = std::chrono::steady_clock::now();
    if (!controller.start(controllerRequest, fakeDependencies(&controllerCalls)) ||
        std::chrono::steady_clock::now() - startTime > std::chrono::milliseconds(200) ||
        controller.start(controllerRequest, fakeDependencies(&controllerCalls))) {
        std::cerr << "Render controller did not start immediately or reject a duplicate\n";
        std::filesystem::remove_all(root, ignored);
        return 11;
    }
    QTimer::singleShot(3000, &application, [&] { application.quit(); });
    application.exec();
    if (!controllerFinished || !controllerProgressSeen || !controllerResult.success ||
        !controllerCallbacksOnUiThread || controllerCalls != 2 || controller.isRunning()) {
        std::cerr << "Render controller completion/progress thread contract failed\n";
        std::filesystem::remove_all(root, ignored);
        return 12;
    }

    RenderJobRequest controllerCancelRequest{
        makeProject(),
        root / "controller-cancelled",
        RenderScope::Selected,
        "s1",
        {},
    };
    int controllerCancelCalls = 0;
    RenderJobController cancellingController;
    bool cancellationFinished = false;
    bool cancellationCallbacksOnUiThread = true;
    RenderJobResult cancellationResult;
    QObject::connect(&cancellingController,
                     &RenderJobController::progressChanged,
                     &application,
                     [&](const RenderJobProgress&) {
                         cancellationCallbacksOnUiThread =
                             cancellationCallbacksOnUiThread &&
                             QThread::currentThread() == application.thread();
                     });
    QObject::connect(&cancellingController,
                     &RenderJobController::finished,
                     &application,
                     [&](const RenderJobResult& result) {
                         cancellationFinished = true;
                         cancellationResult = result;
                         cancellationCallbacksOnUiThread =
                             cancellationCallbacksOnUiThread &&
                             QThread::currentThread() == application.thread();
                         application.quit();
                     });
    if (!cancellingController.start(controllerCancelRequest,
                                    fakeDependencies(&controllerCancelCalls, true)) ||
        cancellingController.start(controllerCancelRequest,
                                   fakeDependencies(&controllerCancelCalls, true))) {
        std::cerr << "Render controller did not reject a running cancellation job\n";
        std::filesystem::remove_all(root, ignored);
        return 13;
    }
    QTimer::singleShot(20, &application, [&] { cancellingController.cancel(); });
    QTimer::singleShot(3000, &application, [&] { application.quit(); });
    application.exec();
    if (!cancellationFinished || !cancellationResult.cancelled ||
        cancellationResult.success || !cancellationCallbacksOnUiThread ||
        !cancellationResult.completedSegments.empty() || cancellingController.isRunning()) {
        std::cerr << "Render controller cancellation contract failed\n";
        std::filesystem::remove_all(root, ignored);
        return 14;
    }

    // A controller can be started again after its worker has been cancelled.
    bool restartedFinished = false;
    RenderJobResult restartedResult;
    QObject::connect(&cancellingController,
                     &RenderJobController::finished,
                     &application,
                     [&](const RenderJobResult& result) {
                         restartedFinished = true;
                         restartedResult = result;
                         application.quit();
                     });
    if (!cancellingController.start(controllerRequest,
                                    fakeDependencies(&controllerCancelCalls))) {
        std::cerr << "Render controller could not restart after cancellation\n";
        std::filesystem::remove_all(root, ignored);
        return 15;
    }
    QTimer::singleShot(3000, &application, [&] { application.quit(); });
    application.exec();
    if (!restartedFinished || !restartedResult.success) {
        std::cerr << "Render controller restart did not complete\n";
        std::filesystem::remove_all(root, ignored);
        return 16;
    }

    // Destruction while a worker is in a cancellable synthesis must wait for
    // the worker and return safely instead of leaving a thread behind.
    const auto destructionStart = std::chrono::steady_clock::now();
    {
        RenderJobController doomed;
        if (!doomed.start(controllerCancelRequest,
                          fakeDependencies(&controllerCancelCalls, true))) {
            std::cerr << "Render controller refused destruction safety job\n";
            std::filesystem::remove_all(root, ignored);
            return 17;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (std::chrono::steady_clock::now() - destructionStart > std::chrono::seconds(3)) {
        std::cerr << "Render controller destruction did not stop its worker promptly\n";
        std::filesystem::remove_all(root, ignored);
        return 18;
    }

    // Production resolution with empty token settings must select a concrete
    // installed SAPI voice. A second run should reuse that resolved identity
    // and report the same actual voice on a cache hit.
    Project sapiProject = makeProject();
    sapiProject.voiceSettings.maleVoiceTokenId.clear();
    sapiProject.voiceSettings.femaleVoiceTokenId.clear();
    sapiProject.voiceSettings.strictAccent = false;
    sapiProject.voiceSettings.allowGenderFallback = true;
    sapiProject.segments.front().text = "MAN: Hello.\nWOMAN: Good morning.";
    sapiProject.segments.front().pauseAfterSeconds = 0.0;
    sapiProject.segments.front().repeatCount = 1;
    RenderJobRequest sapiRequest{
        sapiProject,
        root / "production-sapi",
        RenderScope::Selected,
        "s1",
        {},
    };
    RenderCancellationToken sapiToken;
    const RenderJobResult sapiFirst = RenderJobRunner::run(sapiRequest, sapiToken);
    if (!sapiFirst.success || sapiFirst.voiceUses.size() != 2 ||
        std::any_of(sapiFirst.voiceUses.begin(),
                    sapiFirst.voiceUses.end(),
                    [](const RenderedVoiceUse& use) {
                        return use.voice.tokenId.empty() || use.voice.name.empty() ||
                               use.voice.name == "Injected voice";
                    })) {
        std::cerr << "Production SAPI voice resolution failed: " << sapiFirst.error << '\n';
        std::filesystem::remove_all(root, ignored);
        return 19;
    }
    const RenderJobResult sapiSecond = RenderJobRunner::run(sapiRequest, sapiToken);
    if (!sapiSecond.success || sapiSecond.voiceUses.size() != sapiFirst.voiceUses.size()) {
        std::cerr << "Production SAPI cache rerun failed: " << sapiSecond.error << '\n';
        std::filesystem::remove_all(root, ignored);
        return 20;
    }
    for (std::size_t index = 0; index < sapiFirst.voiceUses.size(); ++index) {
        if (sapiFirst.voiceUses[index].voice.tokenId !=
            sapiSecond.voiceUses[index].voice.tokenId) {
            std::cerr << "Production SAPI cache changed the resolved voice\n";
            std::filesystem::remove_all(root, ignored);
            return 21;
        }
    }

    std::filesystem::remove_all(root, ignored);
    std::cout << "render_job_tests passed\n";
    return 0;
}
