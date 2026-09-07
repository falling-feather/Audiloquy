#include "app/local_voice_pack.h"

#include <QCoreApplication>
#include <QFile>
#include <QThread>
#include <QTemporaryDir>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <atomic>
#include <thread>

namespace {

void write16(std::ostream& output, std::uint16_t value) {
    output.put(static_cast<char>(value & 0xffU));
    output.put(static_cast<char>((value >> 8U) & 0xffU));
}

void write32(std::ostream& output, std::uint32_t value) {
    write16(output, static_cast<std::uint16_t>(value & 0xffffU));
    write16(output, static_cast<std::uint16_t>((value >> 16U) & 0xffffU));
}

int runFakeHelper(const QString& outputPath) {
    const int delayMs = qEnvironmentVariable("AUDILOQUY_TEST_HELPER_DELAY_MS").toInt();
    if (delayMs > 0) {
        QThread::msleep(static_cast<unsigned long>(delayMs));
    }
    std::ofstream output(std::filesystem::path(outputPath.toStdWString()),
                         std::ios::binary | std::ios::trunc);
    constexpr std::uint32_t frames = 4410;
    constexpr std::uint32_t dataBytes = frames * 2;
    output.write("RIFF", 4);
    write32(output, 36 + dataBytes);
    output.write("WAVEfmt ", 8);
    write32(output, 16);
    write16(output, 1);
    write16(output, 1);
    write32(output, 44100);
    write32(output, 88200);
    write16(output, 2);
    write16(output, 16);
    output.write("data", 4);
    write32(output, dataBytes);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        write16(output, 0);
    }
    return output ? 0 : 2;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    const int outputIndex = arguments.indexOf(QStringLiteral("--output-wav"));
    if (outputIndex >= 0 && outputIndex + 1 < arguments.size()) {
        return runFakeHelper(arguments[outputIndex + 1]);
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        std::cerr << "Cannot create temporary voice-pack directory\n";
        return 10;
    }
    const QString helper = temporary.filePath(QStringLiteral("fake-neural-helper.exe"));
    if (!QFile::copy(QCoreApplication::applicationFilePath(), helper)) {
        std::cerr << "Cannot stage fake voice-pack helper\n";
        return 11;
    }
    const QString manifestPath = temporary.filePath(QStringLiteral("test.voice-pack.json"));
    QFile manifest(manifestPath);
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return 12;
    }
    manifest.write(R"JSON({
  "schemaVersion": 1,
  "id": "test-neural-pack",
  "name": "Test Neural Pack",
  "helper": "fake-neural-helper.exe",
  "distributionMode": "user-supplied",
  "runtimeLicense": "test-only",
  "modelLicense": "test-only",
  "voices": [
    {"id":"us-male","name":"Test US Male","locale":"en-US","gender":"male"},
    {"id":"gb-female","name":"Test GB Female","locale":"en-GB","gender":"female"}
  ]
})JSON");
    manifest.close();

    listening::app::LocalVoicePackManager manager;
    manager.discover({std::filesystem::path(temporary.path().toStdWString())});
    if (manager.voices().size() != 2 || !manager.diagnostics().empty()) {
        std::cerr << "Voice-pack discovery failed\n";
        return 13;
    }
    const auto& voice = manager.voices().front();
    if (!manager.ownsToken(voice.tokenId)) {
        return 14;
    }

    listening::platform::windows::SynthesisRequest request;
    request.textUtf8 = "A local neural helper contract test.";
    request.voiceTokenId = voice.tokenId;
    request.accent = voice.locale == "en-US"
                         ? listening::platform::windows::Accent::AmericanEnglish
                         : listening::platform::windows::Accent::BritishEnglish;
    request.preferredGender = voice.gender;
    request.requireExactLocale = true;
    request.requireExactGender = true;
    request.targetWpm = 130;
    request.outputWav = std::filesystem::path(
        temporary.filePath(QStringLiteral("rendered.wav")).toStdWString());
    listening::platform::windows::SynthesisResult result;
    std::string error;
    if (!manager.synthesize(request, &result, &error)) {
        std::cerr << "Voice-pack helper contract failed: " << error << '\n';
        return 15;
    }
    if (result.voice.tokenId != voice.tokenId || result.pcmFrameCount == 0 ||
        !std::filesystem::is_regular_file(result.outputWav)) {
        std::cerr << "Voice-pack result metadata is incomplete\n";
        return 16;
    }

    // A blocked helper must be killed promptly, and cancellation must leave an
    // already-published output untouched.
    qputenv("AUDILOQUY_TEST_HELPER_DELAY_MS", QByteArrayLiteral("5000"));
    std::atomic_bool cancel{false};
    request.outputWav = std::filesystem::path(
        temporary.filePath(QStringLiteral("rendered.wav")).toStdWString());
    const auto previousOutput = request.outputWav;
    std::atomic_bool helperReturned{false};
    bool cancelledResult = true;
    std::string cancelledError;
    std::thread helperThread([&] {
        cancelledResult = manager.synthesize(
            request,
            &result,
            &cancelledError,
            [&] { return cancel.load(std::memory_order_acquire); });
        helperReturned.store(true, std::memory_order_release);
    });
    QThread::msleep(100);
    cancel.store(true, std::memory_order_release);
    helperThread.join();
    qunsetenv("AUDILOQUY_TEST_HELPER_DELAY_MS");
    if (cancelledResult || !helperReturned.load(std::memory_order_acquire) ||
        cancelledError.find("cancelled") == std::string::npos ||
        !std::filesystem::is_regular_file(previousOutput)) {
        std::cerr << "Voice-pack helper cancellation did not preserve the old WAV: "
                  << cancelledError << '\n';
        return 17;
    }
    std::cout << "local_voice_pack_tests passed\n";
    return 0;
}
