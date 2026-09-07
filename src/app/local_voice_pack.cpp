#include "app/local_voice_pack.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryFile>

#include <algorithm>
#include <chrono>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace listening::app {
namespace {

QString qPath(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromUtf8(path.string());
#endif
}

std::filesystem::path nativePath(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}

std::string utf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool stableId(const QString& value) {
    if (value.isEmpty() || value.size() > 80) {
        return false;
    }
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (!((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') ||
              (code >= '0' && code <= '9') || code == '-' || code == '_' || code == '.')) {
            return false;
        }
    }
    return true;
}

platform::windows::VoiceGender parseGender(const QString& value) {
    if (value.compare(QStringLiteral("male"), Qt::CaseInsensitive) == 0) {
        return platform::windows::VoiceGender::Male;
    }
    if (value.compare(QStringLiteral("female"), Qt::CaseInsensitive) == 0) {
        return platform::windows::VoiceGender::Female;
    }
    return platform::windows::VoiceGender::Any;
}

bool pathInside(const std::filesystem::path& child, const std::filesystem::path& parent) {
    std::error_code error;
    const auto canonicalChild = std::filesystem::weakly_canonical(child, error);
    if (error) {
        return false;
    }
    const auto canonicalParent = std::filesystem::weakly_canonical(parent, error);
    if (error) {
        return false;
    }
    auto childPart = canonicalChild.begin();
    for (auto parentPart = canonicalParent.begin(); parentPart != canonicalParent.end();
         ++parentPart, ++childPart) {
        if (childPart == canonicalChild.end() || *childPart != *parentPart) {
            return false;
        }
    }
    return true;
}

std::vector<std::filesystem::path> defaultRoots() {
    std::vector<std::filesystem::path> roots;
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (!localAppData.isEmpty()) {
        roots.push_back(nativePath(QDir(localAppData).filePath(
            QStringLiteral("Audiloquy/voice-packs"))));
    }
    roots.push_back(nativePath(QDir(QCoreApplication::applicationDirPath())
                                   .filePath(QStringLiteral("voice-packs"))));
    return roots;
}

}  // namespace

void LocalVoicePackManager::discover() {
    discover(defaultRoots());
}

void LocalVoicePackManager::discover(const std::vector<std::filesystem::path>& roots) {
    voices_.clear();
    diagnostics_.clear();
    for (const auto& root : roots) {
        const QDir directory(qPath(root));
        if (!directory.exists()) {
            continue;
        }
        const QFileInfoList manifests = directory.entryInfoList(
            {QStringLiteral("*.voice-pack.json")}, QDir::Files | QDir::Readable, QDir::Name);
        for (const QFileInfo& manifestInfo : manifests) {
            QFile manifestFile(manifestInfo.absoluteFilePath());
            if (!manifestFile.open(QIODevice::ReadOnly) || manifestFile.size() > 1024 * 1024) {
                diagnostics_.push_back("Cannot read voice-pack manifest: " +
                                       utf8(manifestInfo.absoluteFilePath()));
                continue;
            }
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                diagnostics_.push_back("Invalid voice-pack JSON: " +
                                       utf8(manifestInfo.absoluteFilePath()));
                continue;
            }
            const QJsonObject object = document.object();
            const QString packId = object.value(QStringLiteral("id")).toString();
            const QString helperRelative = object.value(QStringLiteral("helper")).toString();
            const QString distributionMode =
                object.value(QStringLiteral("distributionMode")).toString();
            const QString runtimeLicense =
                object.value(QStringLiteral("runtimeLicense")).toString().trimmed();
            const QString modelLicense =
                object.value(QStringLiteral("modelLicense")).toString().trimmed();
            if (object.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
                !stableId(packId) || helperRelative.isEmpty() ||
                runtimeLicense.isEmpty() || modelLicense.isEmpty() ||
                (distributionMode != QStringLiteral("user-supplied") &&
                 distributionMode != QStringLiteral("redistributable")) ||
                (distributionMode == QStringLiteral("redistributable") &&
                 !object.value(QStringLiteral("redistributionApproved")).toBool(false))) {
                diagnostics_.push_back("Voice-pack manifest has invalid identity or license mode: " +
                                       utf8(manifestInfo.absoluteFilePath()));
                continue;
            }

            const auto packRoot = nativePath(manifestInfo.absolutePath());
            const auto helper = packRoot / nativePath(helperRelative);
            if (!pathInside(helper, packRoot) || !std::filesystem::is_regular_file(helper)) {
                diagnostics_.push_back("Voice-pack helper is missing or outside its pack: " +
                                       utf8(manifestInfo.absoluteFilePath()));
                continue;
            }
            const QJsonArray voices = object.value(QStringLiteral("voices")).toArray();
            for (const QJsonValue& value : voices) {
                const QJsonObject voiceObject = value.toObject();
                const QString voiceId = voiceObject.value(QStringLiteral("id")).toString();
                const QString name = voiceObject.value(QStringLiteral("name")).toString().trimmed();
                const QString locale = voiceObject.value(QStringLiteral("locale")).toString().trimmed();
                const auto gender = parseGender(
                    voiceObject.value(QStringLiteral("gender")).toString());
                if (!stableId(voiceId) || name.isEmpty() ||
                    (locale != QStringLiteral("en-US") && locale != QStringLiteral("en-GB")) ||
                    gender == platform::windows::VoiceGender::Any) {
                    diagnostics_.push_back("Skipped an invalid voice in pack " + utf8(packId));
                    continue;
                }
                voices_.push_back(Voice{
                    std::string(kVoicePackTokenPrefix) + utf8(packId) + ":" + utf8(voiceId),
                    utf8(packId),
                    utf8(voiceId),
                    utf8(name),
                    utf8(locale),
                    gender,
                    nativePath(manifestInfo.absoluteFilePath()),
                    helper,
                });
            }
        }
    }
    std::sort(voices_.begin(), voices_.end(), [](const Voice& left, const Voice& right) {
        return left.tokenId < right.tokenId;
    });
}

const std::vector<LocalVoicePackManager::Voice>& LocalVoicePackManager::voices() const noexcept {
    return voices_;
}

const std::vector<std::string>& LocalVoicePackManager::diagnostics() const noexcept {
    return diagnostics_;
}

bool LocalVoicePackManager::ownsToken(std::string_view tokenId) const noexcept {
    return std::any_of(voices_.begin(), voices_.end(), [&](const Voice& voice) {
        return voice.tokenId == tokenId;
    });
}

LocalVoicePackManager::Snapshot LocalVoicePackManager::snapshot() const {
    return Snapshot{voices_};
}

bool LocalVoicePackManager::synthesize(
    const platform::windows::SynthesisRequest& request,
    platform::windows::SynthesisResult* result,
    std::string* error,
    const std::function<bool()>& shouldCancel) const {
    return synthesize(snapshot(), request, result, error, shouldCancel);
}

bool LocalVoicePackManager::synthesize(
    const Snapshot& snapshot,
    const platform::windows::SynthesisRequest& request,
    platform::windows::SynthesisResult* result,
    std::string* error,
    const std::function<bool()>& shouldCancel) {
    if (error != nullptr) {
        error->clear();
    }
    if (result == nullptr) {
        return fail(error, "Local voice-pack synthesis needs a result object");
    }
    if (shouldCancel && shouldCancel()) {
        return fail(error, "Local voice-pack synthesis cancelled");
    }
    const auto voice = std::find_if(snapshot.voices.begin(), snapshot.voices.end(), [&](const Voice& item) {
        return item.tokenId == request.voiceTokenId;
    });
    if (voice == snapshot.voices.end()) {
        return fail(error, "The selected local voice-pack token is unavailable");
    }
    if ((request.accent == platform::windows::Accent::AmericanEnglish &&
         voice->locale != "en-US") ||
        (request.accent == platform::windows::Accent::BritishEnglish &&
         voice->locale != "en-GB")) {
        if (request.requireExactLocale) {
            return fail(error, "The selected voice pack does not match the requested accent locale");
        }
    }
    if (request.preferredGender != platform::windows::VoiceGender::Any &&
        voice->gender != request.preferredGender && request.requireExactGender) {
        return fail(error, "The selected voice pack does not match the requested speaker gender");
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(request.outputWav.parent_path(), filesystemError);
    if (filesystemError) {
        return fail(error, "Cannot create the local voice-pack output directory: " +
                               filesystemError.message());
    }
    const std::filesystem::path inputPath(request.outputWav.wstring() + L".input.txt");
    const std::filesystem::path temporaryOutput(
        request.outputWav.wstring() + L".voice-pack.part.wav");
    QFile input(qPath(inputPath));
    if (!input.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(error, "Cannot create the local voice-pack text input");
    }
    input.write(QByteArray::fromStdString(request.textUtf8));
    if (!input.flush()) {
        input.close();
        QFile::remove(qPath(inputPath));
        return fail(error, "Cannot write the local voice-pack text input");
    }
    input.close();
    QFile::remove(qPath(temporaryOutput));

    QProcess process;
    process.setProgram(qPath(voice->helperPath));
    process.setArguments({
        QStringLiteral("--manifest"), qPath(voice->manifestPath),
        QStringLiteral("--voice"), QString::fromUtf8(voice->voiceId.c_str()),
        QStringLiteral("--locale"), QString::fromLatin1(voice->locale.c_str()),
        QStringLiteral("--wpm"), QString::number(request.targetWpm),
        QStringLiteral("--text-file"), qPath(inputPath),
        QStringLiteral("--output-wav"), qPath(temporaryOutput),
    });
    process.start();
    if (!process.waitForStarted(5000)) {
        QFile::remove(qPath(inputPath));
        return fail(error, "Local voice-pack helper could not start: " +
                               utf8(process.errorString()));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (process.state() != QProcess::NotRunning) {
        if (shouldCancel && shouldCancel()) {
            process.kill();
            process.waitForFinished(3000);
            QFile::remove(qPath(inputPath));
            QFile::remove(qPath(temporaryOutput));
            return fail(error, "Local voice-pack synthesis cancelled");
        }
        if (process.waitForFinished(50)) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            process.kill();
            process.waitForFinished(3000);
            QFile::remove(qPath(inputPath));
            QFile::remove(qPath(temporaryOutput));
            return fail(error, "Local voice-pack synthesis timed out after 120 seconds");
        }
    }
    if (shouldCancel && shouldCancel()) {
        QFile::remove(qPath(inputPath));
        QFile::remove(qPath(temporaryOutput));
        return fail(error, "Local voice-pack synthesis cancelled");
    }
    QFile::remove(qPath(inputPath));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = QString::fromUtf8(process.readAllStandardError()).trimmed().left(1200);
        QFile::remove(qPath(temporaryOutput));
        return fail(error, "Local voice-pack helper failed" +
                               (detail.isEmpty() ? std::string{} : ": " + utf8(detail)));
    }

    platform::windows::WavInfo info;
    if (!platform::windows::inspectPcmWav(temporaryOutput, &info, error)) {
        QFile::remove(qPath(temporaryOutput));
        return false;
    }
    if (info.format.sampleRate != platform::windows::kSynthesisPcmFormat.sampleRate ||
        info.format.channels != platform::windows::kSynthesisPcmFormat.channels ||
        info.format.bitsPerSample != platform::windows::kSynthesisPcmFormat.bitsPerSample) {
        QFile::remove(qPath(temporaryOutput));
        return fail(error, "Local voice-pack helper must output PCM16 mono 44.1 kHz WAV");
    }
#ifdef _WIN32
    if (!::MoveFileExW(temporaryOutput.c_str(),
                       request.outputWav.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        QFile::remove(qPath(temporaryOutput));
        return fail(error, "Cannot publish local voice-pack WAV: " +
                               std::to_string(::GetLastError()));
    }
#else
    filesystemError.clear();
    std::filesystem::rename(temporaryOutput, request.outputWav, filesystemError);
    if (filesystemError) {
        QFile::remove(qPath(temporaryOutput));
        return fail(error, "Cannot publish local voice-pack WAV: " + filesystemError.message());
    }
#endif

    result->outputWav = request.outputWav;
    result->voice = platform::windows::VoiceInfo{
        voice->tokenId,
        voice->name,
        voice->locale,
        {voice->locale},
        voice->gender,
        false,
        true,
    };
    result->format = info.format;
    result->targetWpm = request.targetWpm;
    result->sapiRate = 0;
    result->pcmFrameCount = info.frameCount;
    result->durationSeconds = info.durationSeconds;
    result->usedLocaleFallback =
        (request.accent == platform::windows::Accent::AmericanEnglish && voice->locale != "en-US") ||
        (request.accent == platform::windows::Accent::BritishEnglish && voice->locale != "en-GB");
    result->usedGenderFallback = request.preferredGender != platform::windows::VoiceGender::Any &&
                                 voice->gender != request.preferredGender;
    result->rateNotice = "Local neural voice-pack helper; WPM calibration is pack-defined.";
    return true;
}

}  // namespace listening::app
