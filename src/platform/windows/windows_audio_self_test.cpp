#include "platform/windows/windows_audio.h"

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

// Minimal SDK-only smoke test. To build it outside the project build system:
//   cl /std:c++20 /EHsc /I src /DLISTENING_WINDOWS_AUDIO_SELF_TEST_MAIN src/platform/windows/windows_audio.cpp src/platform/windows/windows_audio_self_test.cpp /link ole32.lib sapi.lib winmm.lib
// Run without arguments to test conversion/COM/voice discovery. Pass a WAV path
// to additionally synthesize a short sample and verify the fixed PCM contract.

namespace listening::platform::windows {

bool runWindowsAudioSelfTest(const std::filesystem::path* optionalOutput,
                             std::string* report) {
    std::ostringstream log;
    std::string error;

    const std::string original =
        "English, \xE4\xB8\xAD\xE6\x96\x87, emoji \xF0\x9F\x8E\xA7";
    std::wstring wide;
    std::string roundTrip;
    if (!utf8ToUtf16(original, &wide, &error) ||
        !utf16ToUtf8(wide, &roundTrip, &error) || roundTrip != original) {
        if (report != nullptr) {
            *report = "UTF conversion round-trip failed: " + error;
        }
        return false;
    }
    const std::string invalidUtf8("\xC3\x28", 2);
    if (utf8ToUtf16(invalidUtf8, &wide, &error)) {
        if (report != nullptr) {
            *report = "Invalid UTF-8 was unexpectedly accepted.";
        }
        return false;
    }
    log << "UTF conversion: OK\n";

    const int slow = approximateSapiRateForWpm(90);
    const int normal = approximateSapiRateForWpm(150);
    const int fast = approximateSapiRateForWpm(240);
    if (!(slow < normal && normal == 0 && normal < fast && slow >= -10 && fast <= 10)) {
        if (report != nullptr) {
            *report = "Approximate WPM mapping invariant failed.";
        }
        return false;
    }
    if (kSynthesisPcmFormat.sampleRate != 44100 || kSynthesisPcmFormat.channels != 1 ||
        kSynthesisPcmFormat.bitsPerSample != 16) {
        if (report != nullptr) {
            *report = "Fixed synthesis PCM format invariant failed.";
        }
        return false;
    }
    log << "WPM mapping and fixed PCM format: OK\n";

    ComApartment apartment;
    if (!apartment.ready()) {
        if (report != nullptr) {
            *report = apartment.error();
        }
        return false;
    }
    log << "COM apartment: OK\n";

    std::vector<VoiceInfo> voices;
    if (!listVoices(&voices, &error)) {
        if (report != nullptr) {
            *report = error;
        }
        return false;
    }
    VoiceInfo american;
    VoiceInfo british;
    if (!selectVoice(Accent::AmericanEnglish, &american, &error) ||
        !selectVoice(Accent::BritishEnglish, &british, &error)) {
        if (report != nullptr) {
            *report = error;
        }
        return false;
    }
    log << "SAPI voice discovery: OK (" << voices.size() << " installed; en-US -> "
        << american.name << "; en-GB -> " << british.name << ")\n";

    if (optionalOutput != nullptr) {
        SynthesisRequest request;
        request.textUtf8 = "This is a Windows speech synthesis prototype test.";
        request.accent = Accent::AmericanEnglish;
        request.targetWpm = 150;
        request.outputWav = *optionalOutput;
        SynthesisResult result;
        if (!synthesizeToPcmWav(request, &result, &error)) {
            if (report != nullptr) {
                *report = error;
            }
            return false;
        }
        WavInfo info;
        if (!inspectPcmWav(*optionalOutput, &info, &error) ||
            info.format.sampleRate != kSynthesisPcmFormat.sampleRate ||
            info.format.channels != kSynthesisPcmFormat.channels ||
            info.format.bitsPerSample != kSynthesisPcmFormat.bitsPerSample ||
            info.frameCount == 0) {
            if (report != nullptr) {
                *report = "Synthesized WAV verification failed: " + error;
            }
            return false;
        }
        log << "SAPI synthesis: OK (" << info.durationSeconds << " seconds, rate "
            << result.sapiRate << ", " << result.voice.name << ")\n";
    }

    if (report != nullptr) {
        *report = log.str();
    }
    return true;
}

}  // namespace listening::platform::windows

#ifdef LISTENING_WINDOWS_AUDIO_SELF_TEST_MAIN

#include <iostream>

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path output;
    const std::filesystem::path* optionalOutput = nullptr;
    if (argc > 1) {
        output = argv[1];
        optionalOutput = &output;
    }
    std::string report;
    const bool ok =
        listening::platform::windows::runWindowsAudioSelfTest(optionalOutput, &report);
    std::cout << report;
    return ok ? 0 : 1;
}

#endif
