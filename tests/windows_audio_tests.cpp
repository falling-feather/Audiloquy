#include "platform/windows/windows_audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

int main() {
    namespace win = listening::platform::windows;

    const std::vector<win::VoiceInfo> sampleVoices{
        {"us-female", "US Female", "en-US", {"en-US"}, win::VoiceGender::Female},
        {"us-male", "US Male", "en-US", {"en-US"}, win::VoiceGender::Male},
        {"gb-female", "GB Female", "en-GB", {"en-GB"}, win::VoiceGender::Female},
    };
    win::VoiceSelectionCriteria criteria;
    criteria.accent = win::Accent::AmericanEnglish;
    criteria.preferredGender = win::VoiceGender::Male;
    criteria.requireExactLocale = true;
    criteria.requireExactGender = true;
    win::VoiceSelectionOutcome selected;
    std::string selectionError;
    if (!win::chooseBestVoice(sampleVoices, criteria, &selected, &selectionError) ||
        selected.index != 1 || selected.usedLocaleFallback || selected.usedGenderFallback) {
        std::cerr << "Strict US male selection failed: " << selectionError << '\n';
        return 20;
    }
    criteria.accent = win::Accent::BritishEnglish;
    criteria.preferredGender = win::VoiceGender::Female;
    if (!win::chooseBestVoice(sampleVoices, criteria, &selected, &selectionError) ||
        selected.index != 2) {
        std::cerr << "Strict GB female selection failed: " << selectionError << '\n';
        return 21;
    }
    criteria.preferredGender = win::VoiceGender::Male;
    if (win::chooseBestVoice(sampleVoices, criteria, &selected, &selectionError) ||
        selectionError.empty()) {
        std::cerr << "Unavailable strict GB male voice was not rejected\n";
        return 22;
    }

    const auto root = std::filesystem::temp_directory_path() / "listening-sapi-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    win::SynthesisRequest request;
    request.textUtf8 = "Preview check.";
    request.accent = win::Accent::AmericanEnglish;
    request.targetWpm = 120;
    request.outputWav = root / "speech.wav";

    win::SynthesisResult result;
    std::string error;
    if (!win::synthesizeToPcmWav(request, &result, &error)) {
        std::cerr << "SAPI synthesis failed: " << error << '\n';
        return 1;
    }
    if (!std::filesystem::is_regular_file(result.outputWav) || result.pcmFrameCount == 0 ||
        result.durationSeconds <= 0.0) {
        std::cerr << "SAPI produced an empty WAV\n";
        return 2;
    }

    win::WavInfo info;
    if (!win::inspectPcmWav(result.outputWav, &info, &error)) {
        std::cerr << "Synthesized WAV inspection failed: " << error << '\n';
        return 3;
    }
    if (info.format.sampleRate != 44100 || info.format.channels != 1 ||
        info.format.bitsPerSample != 16) {
        std::cerr << "Unexpected SAPI output format\n";
        return 4;
    }

    // SAPI cancellation must purge the asynchronous utterance and leave an
    // already-published output untouched.
    const auto preservedPath = root / "preserved.wav";
    std::filesystem::copy_file(result.outputWav,
                                preservedPath,
                                std::filesystem::copy_options::overwrite_existing);
    win::SynthesisRequest cancellationRequest = request;
    cancellationRequest.outputWav = preservedPath;
    cancellationRequest.textUtf8.clear();
    cancellationRequest.textUtf8.reserve(250000);
    for (int sentence = 0; sentence < 6000; ++sentence) {
        cancellationRequest.textUtf8 +=
            "This long utterance exists to verify that a teacher can cancel a pending render. ";
    }
    std::atomic_bool cancelRequested{false};
    bool cancellationSucceeded = true;
    std::string cancellationError;
    win::SynthesisResult cancellationOutput;
    std::thread cancellationThread([&] {
        cancellationSucceeded = win::synthesizeToPcmWav(
            cancellationRequest,
            &cancellationOutput,
            &cancellationError,
            [&] { return cancelRequested.load(std::memory_order_acquire); });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cancelRequested.store(true, std::memory_order_release);
    cancellationThread.join();
    win::WavInfo preservedInfo;
    if (cancellationSucceeded || cancellationError.find("cancelled") == std::string::npos ||
        !std::filesystem::is_regular_file(preservedPath) ||
        !win::inspectPcmWav(preservedPath, &preservedInfo, &error) ||
        preservedInfo.frameCount != info.frameCount) {
        std::cerr << "SAPI cancellation did not preserve the previous WAV: "
                  << cancellationError << '\n';
        std::filesystem::remove_all(root, ignored);
        return 27;
    }

    // Start the generated clip in loop mode so it cannot naturally finish
    // between the asynchronous call and the state assertions. Stop it
    // immediately, keeping this smoke test effectively silent and fast.
    win::WavPlayer player;
    if (!player.playAsync(result.outputWav, true, &error)) {
        std::cerr << "Asynchronous WAV playback failed: " << error << '\n';
        std::filesystem::remove_all(root, ignored);
        return 5;
    }
    std::error_code pathError;
    const bool samePath =
        std::filesystem::equivalent(player.currentPath(), result.outputWav, pathError);
    if (!player.active() || !player.looping() || pathError || !samePath) {
        std::string stopError;
        player.stop(&stopError);
        std::cerr << "WAV player did not retain its asynchronous loop state\n";
        std::filesystem::remove_all(root, ignored);
        return 6;
    }
    const auto playing = player.snapshot(&error);
    if (playing.state != win::WavPlayer::State::Playing ||
        playing.durationMilliseconds == 0) {
        std::cerr << "waveOut player did not expose playing state and duration: " << error << '\n';
        player.stop();
        std::filesystem::remove_all(root, ignored);
        return 23;
    }
    if (!player.pause(&error) ||
        player.snapshot(&error).state != win::WavPlayer::State::Paused) {
        std::cerr << "waveOut player pause failed: " << error << '\n';
        player.stop();
        std::filesystem::remove_all(root, ignored);
        return 24;
    }
    const auto seekTarget = std::min<std::uint64_t>(20, playing.durationMilliseconds);
    if (!player.seek(seekTarget, &error)) {
        std::cerr << "waveOut player seek failed: " << error << '\n';
        player.stop();
        std::filesystem::remove_all(root, ignored);
        return 25;
    }
    if (!player.resume(&error) ||
        player.snapshot(&error).state != win::WavPlayer::State::Playing) {
        std::cerr << "waveOut player resume failed: " << error << '\n';
        player.stop();
        std::filesystem::remove_all(root, ignored);
        return 26;
    }
    if (!player.stop(&error)) {
        std::cerr << "Stopping WAV playback failed: " << error << '\n';
        std::filesystem::remove_all(root, ignored);
        return 7;
    }
    if (player.active() || player.looping() || !player.currentPath().empty()) {
        std::cerr << "WAV player did not clear its state after stop\n";
        std::filesystem::remove_all(root, ignored);
        return 8;
    }

    std::filesystem::remove_all(root, ignored);
    std::cout << "windows_audio_tests passed using " << result.voice.name << '\n';
    return 0;
}
