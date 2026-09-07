#include "app/render_job.h"

#include "core/project.h"

#include <QTimer>
#include <QThread>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace listening::app {
namespace {

constexpr std::string_view kDialogueAssemblyRulesVersion = "dialogue-assembly-v2";
constexpr std::string_view kSegmentAssemblyRulesVersion = "segment-assembly-v2";
constexpr std::string_view kProgramAssemblyRulesVersion = "program-assembly-v2";

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool cancelled(const CancellationProbe& probe) {
    return probe && probe();
}

std::string lowerAsciiCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return character >= 'A' && character <= 'Z'
                   ? static_cast<char>(character - 'A' + 'a')
                   : static_cast<char>(character);
    });
    return value;
}

platform::windows::VoiceGender speakerGender(std::string_view speaker) {
    const std::string normalized = lowerAsciiCopy(std::string(speaker));
    const auto hasRoleToken = [&](std::string_view token) {
        std::size_t position = normalized.find(token);
        while (position != std::string::npos) {
            const bool leftBoundary =
                position == 0 ||
                (normalized[position - 1] < 'a' || normalized[position - 1] > 'z');
            const std::size_t end = position + token.size();
            const bool rightBoundary =
                end >= normalized.size() || normalized[end] < 'a' || normalized[end] > 'z';
            if (leftBoundary && rightBoundary) {
                return true;
            }
            position = normalized.find(token, position + 1);
        }
        return false;
    };
    const bool hasWoman = hasRoleToken("woman") || hasRoleToken("female");
    const bool hasMan = hasRoleToken("man") || hasRoleToken("male");
    if (hasMan && hasWoman) {
        return platform::windows::VoiceGender::Any;
    }
    if (hasWoman) {
        return platform::windows::VoiceGender::Female;
    }
    if (hasMan) {
        return platform::windows::VoiceGender::Male;
    }
    return platform::windows::VoiceGender::Any;
}

std::string speechTextWithoutRoleLabels(std::string_view script) {
    std::istringstream input{std::string(script)};
    std::ostringstream output;
    std::string line;
    bool firstLine = true;
    constexpr std::array<std::string_view, 4> labels{
        "MAN:", "WOMAN:", "Man:", "Woman:"};
    while (std::getline(input, line)) {
        const std::size_t contentStart = line.find_first_not_of(" \t");
        if (contentStart != std::string::npos) {
            for (const std::string_view label : labels) {
                if (line.compare(contentStart, label.size(), label) == 0) {
                    std::size_t spokenStart = contentStart + label.size();
                    while (spokenStart < line.size() &&
                           (line[spokenStart] == ' ' || line[spokenStart] == '\t')) {
                        ++spokenStart;
                    }
                    line.erase(contentStart, spokenStart - contentStart);
                    break;
                }
            }
        }
        if (!firstLine) {
            output << '\n';
        }
        output << line;
        firstLine = false;
    }
    return output.str();
}

void hashField(std::uint64_t& hash, std::string_view value) {
    // Length-prefixing prevents concatenation ambiguities ("ab","c" vs
    // "a","bc") while keeping the cache key stable across processes.
    const auto length = static_cast<std::uint64_t>(value.size());
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        hash ^= (length >> shift) & 0xffU;
        hash *= 1099511628211ULL;
    }
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
}

std::string hashFields(const std::vector<std::string_view>& fields) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto field : fields) {
        hashField(hash, field);
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string fileFingerprint(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return "missing";
    }
    std::uint64_t hash = 1469598103934665603ULL;
    std::array<char, 16384> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::filesystem::path temporarySibling(const std::filesystem::path& destination,
                                        std::string_view purpose) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto value = sequence.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path filename = destination.filename();
    std::string owner;
#ifdef _WIN32
    owner = std::to_string(::GetCurrentProcessId()) + "-" +
            std::to_string(::GetCurrentThreadId());
#else
    owner = std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
#ifdef _WIN32
    std::wostringstream suffix;
    suffix << L'.';
    for (const char character : purpose) {
        suffix << static_cast<wchar_t>(static_cast<unsigned char>(character));
    }
    suffix << L'.';
    for (const char character : owner) {
        suffix << static_cast<wchar_t>(static_cast<unsigned char>(character));
    }
    suffix << L'.' << value << L".part";
    filename += suffix.str();
#else
    filename += "." + std::string(purpose) + "." + owner + "." +
                std::to_string(value) + ".part";
#endif
    return destination.parent_path() / filename;
}

bool publishFile(const std::filesystem::path& temporary,
                 const std::filesystem::path& destination,
                 std::string* error) {
    std::error_code directoryError;
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), directoryError);
        if (directoryError) {
            return fail(error, "Cannot create output directory: " + directoryError.message());
        }
    }
#ifdef _WIN32
    if (!::MoveFileExW(temporary.c_str(),
                       destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return fail(error, "Cannot publish generated WAV atomically: " +
                               std::to_string(::GetLastError()));
    }
#else
    std::filesystem::remove(destination, directoryError);
    directoryError.clear();
    std::filesystem::rename(temporary, destination, directoryError);
    if (directoryError) {
        return fail(error, "Cannot publish generated WAV atomically: " +
                               directoryError.message());
    }
#endif
    return true;
}

bool inspectValidWav(const std::filesystem::path& path) {
    platform::windows::WavInfo info;
    std::string ignored;
    return platform::windows::inspectPcmWav(path, &info, &ignored) &&
           info.format.sampleRate == platform::windows::kSynthesisPcmFormat.sampleRate &&
           info.format.channels == platform::windows::kSynthesisPcmFormat.channels &&
           info.format.bitsPerSample == platform::windows::kSynthesisPcmFormat.bitsPerSample &&
           info.frameCount > 0;
}

std::string validationFailure(const Project& project) {
    const auto issues = validate(project);
    if (issues.empty()) {
        return {};
    }
    return issues.front().path + ": " + issues.front().message;
}

struct ResolvedVoice {
    platform::windows::VoiceInfo voice;
    std::string token;
    std::string configurationFingerprint;
    bool usedLocaleFallback{};
    bool usedGenderFallback{};
};

std::string localeFor(platform::windows::Accent accent) {
    return accent == platform::windows::Accent::AmericanEnglish ? "en-US" : "en-GB";
}

bool localVoiceMatches(const LocalVoicePackManager::Voice& voice,
                       platform::windows::Accent accent,
                       platform::windows::VoiceGender gender,
                       bool requireExactLocale,
                       bool requireExactGender,
                       ResolvedVoice* result,
                       std::string* error) {
    const bool localeMatch = voice.locale == localeFor(accent);
    const bool genderMatch = gender == platform::windows::VoiceGender::Any ||
                             voice.gender == gender;
    if (requireExactLocale && !localeMatch) {
        return fail(error, "Selected local voice does not match the requested accent locale");
    }
    if (requireExactGender && !genderMatch) {
        return fail(error, "Selected local voice does not match the requested speaker gender");
    }
    result->voice = platform::windows::VoiceInfo{
        voice.tokenId,
        voice.name,
        voice.locale,
        {voice.locale},
        voice.gender,
        false,
        localeMatch,
    };
    result->token = voice.tokenId;
    result->configurationFingerprint =
        "manifest:" + fileFingerprint(voice.manifestPath) +
        ":helper:" + fileFingerprint(voice.helperPath);
    result->usedLocaleFallback = !localeMatch;
    result->usedGenderFallback = !genderMatch;
    return true;
}

bool resolveVoice(const RenderJobRequest& request,
                  platform::windows::VoiceGender gender,
                  bool injectedSynthesis,
                  ResolvedVoice* result,
                  std::string* error) {
    if (result == nullptr) {
        return fail(error, "Voice resolution output is null");
    }
    *result = {};
    const auto accent = request.project.accent == Accent::American
                            ? platform::windows::Accent::AmericanEnglish
                            : platform::windows::Accent::BritishEnglish;
    const bool requireExactLocale = request.project.voiceSettings.strictAccent;
    const bool requireExactGender =
        gender != platform::windows::VoiceGender::Any &&
        !request.project.voiceSettings.allowGenderFallback;
    const std::string requestedToken =
        gender == platform::windows::VoiceGender::Male
            ? request.project.voiceSettings.maleVoiceTokenId
            : request.project.voiceSettings.femaleVoiceTokenId;

    if (!requestedToken.empty() &&
        requestedToken.rfind(kVoicePackTokenPrefix, 0) == 0) {
        const auto found = std::find_if(
            request.localVoicePacks.voices.begin(),
            request.localVoicePacks.voices.end(),
            [&](const LocalVoicePackManager::Voice& voice) {
                return voice.tokenId == requestedToken;
            });
        if (found == request.localVoicePacks.voices.end()) {
            return fail(error, "The selected local voice pack is unavailable");
        }
        return localVoiceMatches(*found,
                                 accent,
                                 gender,
                                 requireExactLocale,
                                 requireExactGender,
                                 result,
                                 error);
    }

    // Injected synthesis is used by deterministic tests and does not require
    // a machine SAPI voice. Production always resolves the installed voice so
    // an empty token cannot make the cache depend on an unspecified default.
    if (injectedSynthesis) {
        result->token = requestedToken.empty() ? "injected-default" : requestedToken;
        result->voice = platform::windows::VoiceInfo{
            result->token,
            "Injected voice",
            localeFor(accent),
            {localeFor(accent)},
            gender,
            false,
            true,
        };
        return true;
    }

    platform::windows::VoiceSelectionCriteria criteria;
    criteria.accent = accent;
    criteria.preferredGender = gender;
    criteria.voiceTokenId = requestedToken;
    criteria.requireExactLocale = requireExactLocale;
    criteria.requireExactGender = requireExactGender;
    platform::windows::VoiceSelectionOutcome outcome;
    if (!platform::windows::selectVoice(criteria, &result->voice, &outcome, error)) {
        return false;
    }
    result->token = result->voice.tokenId;
    result->usedLocaleFallback = outcome.usedLocaleFallback;
    result->usedGenderFallback = outcome.usedGenderFallback;
    return true;
}

struct PreparedTurn {
    SpeechTurn turn;
    ResolvedVoice voice;
    std::string key;
};

struct RenderedSpeech {
    std::filesystem::path path;
    std::vector<RenderedVoiceUse> voices;
};

void report(const RenderProgressCallback& callback, RenderJobProgress progress) {
    if (callback) {
        callback(progress);
    }
}

std::string assemblyKey(const Segment& segment,
                        std::string_view rawKey,
                        int pauseMs,
                        int repeatCount) {
    const std::string first = std::to_string(segment.questions.first);
    const std::string last = std::to_string(segment.questions.last);
    const std::string pause = std::to_string(pauseMs);
    const std::string repeat = std::to_string(repeatCount);
    return hashFields({kSegmentAssemblyRulesVersion,
                       segment.id,
                       first,
                       last,
                       rawKey,
                       pause,
                       repeat});
}

std::string programKey(const std::vector<RenderedSegment>& segments) {
    std::vector<std::string> fields;
    fields.reserve(segments.size() + 1);
    fields.emplace_back(kProgramAssemblyRulesVersion);
    for (const auto& segment : segments) {
        fields.push_back(segment.id);
        fields.push_back(segment.path.filename().string());
    }
    std::vector<std::string_view> views;
    views.reserve(fields.size());
    for (const auto& field : fields) {
        views.push_back(field);
    }
    return hashFields(views);
}

bool ensureTurnCache(const RenderJobRequest& request,
                     const PreparedTurn& prepared,
                     const std::filesystem::path& cachePath,
                     const SynthesizeFunction& synthesize,
                     const CancellationProbe& shouldCancel,
                     bool* cacheHit,
                     platform::windows::VoiceInfo* actualVoice,
                     bool* usedLocaleFallback,
                     bool* usedGenderFallback,
                     std::string* error) {
    if (cacheHit != nullptr) {
        *cacheHit = false;
    }
    if (actualVoice != nullptr) {
        *actualVoice = prepared.voice.voice;
    }
    if (usedLocaleFallback != nullptr) {
        *usedLocaleFallback = prepared.voice.usedLocaleFallback;
    }
    if (usedGenderFallback != nullptr) {
        *usedGenderFallback = prepared.voice.usedGenderFallback;
    }
    if (inspectValidWav(cachePath)) {
        if (cacheHit != nullptr) {
            *cacheHit = true;
        }
        return true;
    }
    if (cancelled(shouldCancel)) {
        return fail(error, "Speech cache generation cancelled");
    }

    std::error_code directoryError;
    std::filesystem::create_directories(cachePath.parent_path(), directoryError);
    if (directoryError) {
        return fail(error, "Cannot create speech cache directory: " + directoryError.message());
    }
    const auto temporary = temporarySibling(cachePath, "voice");
    std::filesystem::remove(temporary, directoryError);
    platform::windows::SynthesisRequest synthesisRequest;
    synthesisRequest.textUtf8 = prepared.turn.text;
    synthesisRequest.accent = request.project.accent == Accent::American
                                  ? platform::windows::Accent::AmericanEnglish
                                  : platform::windows::Accent::BritishEnglish;
    synthesisRequest.targetWpm = static_cast<int>(std::lround(request.project.targetWpm));
    synthesisRequest.outputWav = temporary;
    synthesisRequest.voiceTokenId = prepared.voice.token;
    synthesisRequest.preferredGender = prepared.turn.gender;
    synthesisRequest.requireExactLocale = request.project.voiceSettings.strictAccent;
    synthesisRequest.requireExactGender =
        prepared.turn.gender != platform::windows::VoiceGender::Any &&
        !request.project.voiceSettings.allowGenderFallback;

    platform::windows::SynthesisResult synthesisResult;
    const bool synthesized = synthesize(synthesisRequest,
                                        &synthesisResult,
                                        error,
                                        shouldCancel);
    if (!synthesized) {
        std::filesystem::remove(temporary, directoryError);
        return false;
    }
    if (cancelled(shouldCancel)) {
        std::filesystem::remove(temporary, directoryError);
        return fail(error, "Speech cache generation cancelled");
    }
    if (!inspectValidWav(temporary)) {
        std::filesystem::remove(temporary, directoryError);
        return fail(error, "Speech synthesizer returned an invalid PCM16 mono 44.1 kHz WAV");
    }
    if (actualVoice != nullptr && !synthesisResult.voice.tokenId.empty()) {
        *actualVoice = synthesisResult.voice;
    }
    if (usedLocaleFallback != nullptr) {
        *usedLocaleFallback = synthesisResult.usedLocaleFallback;
    }
    if (usedGenderFallback != nullptr) {
        *usedGenderFallback = synthesisResult.usedGenderFallback;
    }
    if (!publishFile(temporary, cachePath, error)) {
        std::filesystem::remove(temporary, directoryError);
        return false;
    }
    return true;
}

bool renderSpeech(const RenderJobRequest& request,
                  const Segment& segment,
                  const std::filesystem::path& cacheDirectory,
                  const SynthesizeFunction& synthesize,
                  bool injectedSynthesis,
                  const CancellationProbe& shouldCancel,
                  std::size_t segmentIndex,
                  std::size_t totalSegments,
                  const RenderProgressCallback& progress,
                  RenderedSpeech* result,
                  std::string* error) {
    if (result == nullptr) {
        return fail(error, "Rendered speech output is null");
    }
    *result = {};
    const auto turns = parseSpeechTurns(segment);
    std::vector<PreparedTurn> prepared;
    prepared.reserve(turns.size());
    std::array<std::optional<ResolvedVoice>, 3> resolvedVoices;
    const auto voiceSlot = [](platform::windows::VoiceGender gender) {
        switch (gender) {
        case platform::windows::VoiceGender::Any:
            return std::size_t{0};
        case platform::windows::VoiceGender::Male:
            return std::size_t{1};
        case platform::windows::VoiceGender::Female:
            return std::size_t{2};
        }
        return std::size_t{0};
    };
    for (const auto& turn : turns) {
        if (cancelled(shouldCancel)) {
            return fail(error, "Speech rendering cancelled");
        }
        const auto slot = voiceSlot(turn.gender);
        if (!resolvedVoices[slot].has_value()) {
            ResolvedVoice resolved;
            if (!resolveVoice(request,
                              turn.gender,
                              injectedSynthesis,
                              &resolved,
                              error)) {
                return false;
            }
            resolvedVoices[slot] = std::move(resolved);
        }
        ResolvedVoice voice = *resolvedVoices[slot];
        const int targetWpm = static_cast<int>(std::lround(request.project.targetWpm));
        const std::string key = speechCacheKey(
            turn.text,
            turn.gender,
            request.project.accent == Accent::American
                ? platform::windows::Accent::AmericanEnglish
                : platform::windows::Accent::BritishEnglish,
            targetWpm,
            voice.token,
            voice.configurationFingerprint,
            request.project.voiceSettings.strictAccent,
            turn.gender != platform::windows::VoiceGender::Any &&
                !request.project.voiceSettings.allowGenderFallback);
        prepared.push_back(PreparedTurn{turn, std::move(voice), key});
    }

    std::vector<std::string> rawFields;
    rawFields.reserve(prepared.size() + 2);
    rawFields.emplace_back(kDialogueAssemblyRulesVersion);
    for (const auto& item : prepared) {
        rawFields.push_back(item.key);
    }
    std::vector<std::string_view> rawViews;
    rawViews.reserve(rawFields.size());
    for (const auto& field : rawFields) {
        rawViews.push_back(field);
    }
    const std::string rawKey = hashFields(rawViews);
    const auto rawPath = cacheDirectory / (rawKey + ".wav");

    if (inspectValidWav(rawPath)) {
        result->path = rawPath;
        result->voices.reserve(prepared.size());
        for (std::size_t index = 0; index < prepared.size(); ++index) {
            result->voices.push_back(RenderedVoiceUse{
                segment.id,
                index,
                prepared[index].voice.voice,
                prepared[index].voice.usedLocaleFallback,
                prepared[index].voice.usedGenderFallback,
                true,
            });
        }
        return true;
    }

    std::vector<audio::ProgramClip> clips;
    clips.reserve(prepared.size());
    result->voices.reserve(prepared.size());
    for (std::size_t index = 0; index < prepared.size(); ++index) {
        RenderJobProgress current;
        current.phase = RenderPhase::Synthesizing;
        current.currentSegmentIndex = segmentIndex;
        current.totalSegments = totalSegments;
        current.currentTurnIndex = index;
        current.totalTurns = prepared.size();
        current.currentSegmentId = segment.id;
        current.message = "Synthesizing speech";
        report(progress, std::move(current));

        const auto turnPath = cacheDirectory / (prepared[index].key + ".wav");
        bool hit = false;
        platform::windows::VoiceInfo actualVoice;
        bool localeFallback = false;
        bool genderFallback = false;
        if (!ensureTurnCache(request,
                             prepared[index],
                             turnPath,
                             synthesize,
                             shouldCancel,
                             &hit,
                             &actualVoice,
                             &localeFallback,
                             &genderFallback,
                             error)) {
            return false;
        }
        result->voices.push_back(RenderedVoiceUse{
            segment.id,
            index,
            actualVoice,
            localeFallback,
            genderFallback,
            hit,
        });
        clips.push_back(audio::ProgramClip{
            turnPath,
            1,
            index + 1 < prepared.size() ? 280 : 0,
        });
    }

    if (cancelled(shouldCancel)) {
        return fail(error, "Speech assembly cancelled");
    }
    const auto temporary = temporarySibling(rawPath, "speech");
    audio::WavInfo rawInfo;
    std::string assemblyError;
    if (!audio::buildProgramWav(clips,
                                temporary,
                                &rawInfo,
                                &assemblyError,
                                shouldCancel)) {
        std::filesystem::remove(temporary);
        return fail(error, assemblyError.empty() ? "Cannot assemble speech turns" : assemblyError);
    }
    if (!publishFile(temporary, rawPath, error)) {
        std::filesystem::remove(temporary);
        return false;
    }
    result->path = rawPath;
    return true;
}

bool renderSegment(const RenderJobRequest& request,
                   const Segment& segment,
                   const std::filesystem::path& cacheDirectory,
                   const std::filesystem::path& outputDirectory,
                   const SynthesizeFunction& synthesize,
                   bool injectedSynthesis,
                   const CancellationProbe& shouldCancel,
                   std::size_t segmentIndex,
                   std::size_t totalSegments,
                   const RenderProgressCallback& progress,
                   RenderedSegment* output,
                   std::vector<RenderedVoiceUse>* voices,
                   std::string* error) {
    if (output == nullptr) {
        return fail(error, "Rendered segment output is null");
    }
    const auto turns = parseSpeechTurns(segment);
    if (turns.empty()) {
        return fail(error, "Segment has no speech turns");
    }
    const int pauseMs = static_cast<int>(std::llround(segment.pauseAfterSeconds * 1000.0));

    RenderedSpeech speech;
    if (!renderSpeech(request,
                      segment,
                      cacheDirectory,
                      synthesize,
                      injectedSynthesis,
                      shouldCancel,
                      segmentIndex,
                      totalSegments,
                      progress,
                      &speech,
                      error)) {
        return false;
    }
    // The raw cache filename contains the fully resolved voice identity and
    // text keys. It must be part of the assembled output name so an existing
    // valid WAV from a previous voice selection cannot be mistaken for the
    // current result.
    const std::string segmentIdentity =
        assemblyKey(segment, speech.path.filename().string(), pauseMs, segment.repeatCount);
    const auto outputPath = outputDirectory /
                            ("questions-" + std::to_string(segment.questions.first) + "-" +
                             std::to_string(segment.questions.last) + "-" + segmentIdentity +
                             ".wav");
    if (!inspectValidWav(outputPath)) {
        RenderJobProgress current;
        current.phase = RenderPhase::Assembling;
        current.currentSegmentIndex = segmentIndex;
        current.totalSegments = totalSegments;
        current.currentSegmentId = segment.id;
        current.message = "Assembling question-group audio";
        report(progress, std::move(current));
        const auto temporary = temporarySibling(outputPath, "segment");
        audio::WavInfo info;
        std::string assemblyError;
        if (!audio::buildProgramWav({audio::ProgramClip{
                                         speech.path,
                                         segment.repeatCount,
                                         pauseMs,
                                     }},
                                     temporary,
                                     &info,
                                     &assemblyError,
                                     shouldCancel)) {
            std::filesystem::remove(temporary);
            return fail(error,
                        assemblyError.empty() ? "Cannot assemble question-group WAV"
                                               : assemblyError);
        }
        if (!publishFile(temporary, outputPath, error)) {
            std::filesystem::remove(temporary);
            return false;
        }
    }
    RenderJobProgress published;
    published.phase = RenderPhase::Publishing;
    published.currentSegmentIndex = segmentIndex;
    published.totalSegments = totalSegments;
    published.currentSegmentId = segment.id;
    published.message = "Question-group audio is ready";
    report(progress, std::move(published));
    *output = RenderedSegment{segment.id, outputPath};
    if (voices != nullptr) {
        voices->insert(voices->end(), speech.voices.begin(), speech.voices.end());
    }
    return true;
}

}  // namespace

std::vector<SpeechTurn> parseSpeechTurns(const Segment& segment) {
    std::vector<SpeechTurn> turns;
    std::istringstream input(segment.text);
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) {
            continue;
        }
        const std::string normalized = lowerAsciiCopy(line.substr(first));
        platform::windows::VoiceGender gender = platform::windows::VoiceGender::Any;
        std::size_t labelLength = 0;
        if (normalized.rfind("woman:", 0) == 0) {
            gender = platform::windows::VoiceGender::Female;
            labelLength = 6;
        } else if (normalized.rfind("man:", 0) == 0) {
            gender = platform::windows::VoiceGender::Male;
            labelLength = 4;
        }
        if (labelLength == 0) {
            if (!turns.empty() && turns.back().gender != platform::windows::VoiceGender::Any) {
                turns.back().text += " " + line.substr(first);
            }
            continue;
        }
        std::size_t textStart = first + labelLength;
        while (textStart < line.size() && (line[textStart] == ' ' || line[textStart] == '\t')) {
            ++textStart;
        }
        if (textStart < line.size()) {
            turns.push_back({line.substr(textStart), gender});
        }
    }
    if (turns.empty()) {
        turns.push_back({speechTextWithoutRoleLabels(segment.text), speakerGender(segment.speaker)});
    }
    return turns;
}

std::string speechCacheKey(std::string_view text,
                           platform::windows::VoiceGender gender,
                           platform::windows::Accent accent,
                           int targetWpm,
                           std::string_view voiceTokenId,
                           std::string_view voiceConfigurationFingerprint,
                           bool requireExactLocale,
                           bool requireExactGender) {
    const std::string genderValue{platform::windows::voiceGenderName(gender)};
    const std::string accentValue =
        accent == platform::windows::Accent::AmericanEnglish ? "en-US" : "en-GB";
    const std::string wpmValue = std::to_string(targetWpm);
    const std::string exactLocaleValue = requireExactLocale ? "strict-locale" : "fallback-locale";
    const std::string exactGenderValue = requireExactGender ? "strict-gender" : "fallback-gender";
    return hashFields({kSpeechCacheRulesVersion,
                       text,
                       genderValue,
                       accentValue,
                       wpmValue,
                       voiceTokenId.empty() ? std::string_view{"<empty>"} : voiceTokenId,
                       voiceConfigurationFingerprint,
                       exactLocaleValue,
                       exactGenderValue});
}

struct RenderCancellationToken::State {
    std::atomic_bool cancelled{false};
};

RenderCancellationToken::RenderCancellationToken() : state_(std::make_shared<State>()) {}

void RenderCancellationToken::cancel() noexcept {
    if (state_ != nullptr) {
        state_->cancelled.store(true, std::memory_order_release);
    }
}

bool RenderCancellationToken::isCancelled() const noexcept {
    return state_ != nullptr && state_->cancelled.load(std::memory_order_acquire);
}

RenderJobResult RenderJobRunner::run(const RenderJobRequest& request,
                                     const RenderCancellationToken& cancellation,
                                     const RenderProgressCallback& progress,
                                     RenderJobDependencies dependencies) {
    RenderJobResult result;
    std::size_t totalTargets = 0;
    const CancellationProbe shouldCancel = [&cancellation] {
        return cancellation.isCancelled();
    };
    const auto reportTerminal = [&](RenderPhase phase, std::string message) {
        RenderJobProgress terminal;
        terminal.phase = phase;
        terminal.totalSegments = totalTargets;
        terminal.completedSegments = result.completedSegments.size();
        terminal.message = std::move(message);
        report(progress, std::move(terminal));
    };
    try {
        if (request.outputDirectory.empty()) {
            result.error = "Output directory is empty";
            reportTerminal(RenderPhase::Failed, result.error);
            return result;
        }
        if (cancelled(shouldCancel)) {
            result.cancelled = true;
            result.error = "Render job cancelled";
            reportTerminal(RenderPhase::Cancelled, result.error);
            return result;
        }

        std::vector<const Segment*> targets;
        if (request.scope == RenderScope::Selected) {
            const Segment* selected = request.project.findSegment(request.selectedSegmentId);
            if (selected == nullptr) {
                result.error = "Selected segment is unavailable";
                reportTerminal(RenderPhase::Failed, result.error);
                return result;
            }
            targets.push_back(selected);
            Project selectedValidation = request.project;
            selectedValidation.segments = {*selected};
            if (const std::string issue = validationFailure(selectedValidation); !issue.empty()) {
                result.error = issue;
                reportTerminal(RenderPhase::Failed, result.error);
                return result;
            }
        } else {
            targets.reserve(request.project.segments.size());
            for (const Segment& segment : request.project.segments) {
                targets.push_back(&segment);
            }
            if (const std::string issue = validationFailure(request.project); !issue.empty()) {
                result.error = issue;
                reportTerminal(RenderPhase::Failed, result.error);
                return result;
            }
        }
        if (targets.empty()) {
            result.error = "No segments were supplied";
            reportTerminal(RenderPhase::Failed, result.error);
            return result;
        }
        totalTargets = targets.size();

        std::error_code directoryError;
        std::filesystem::create_directories(request.outputDirectory, directoryError);
        if (directoryError) {
            result.error = "Cannot create output directory: " + directoryError.message();
            reportTerminal(RenderPhase::Failed, result.error);
            return result;
        }
        const auto cacheDirectory = request.outputDirectory / ".cache" / "voice";
        std::filesystem::create_directories(cacheDirectory, directoryError);
        if (directoryError) {
            result.error = "Cannot create speech cache directory: " + directoryError.message();
            reportTerminal(RenderPhase::Failed, result.error);
            return result;
        }

        const bool injectedSynthesis = static_cast<bool>(dependencies.synthesize);
        const SynthesizeFunction synthesize = dependencies.synthesize
            ? dependencies.synthesize
            : [&](const platform::windows::SynthesisRequest& synthesisRequest,
                  platform::windows::SynthesisResult* synthesisResult,
                  std::string* synthesisError,
                  const CancellationProbe& probe) {
                  if (synthesisRequest.voiceTokenId.rfind(kVoicePackTokenPrefix, 0) == 0) {
                      return LocalVoicePackManager::synthesize(request.localVoicePacks,
                                                               synthesisRequest,
                                                               synthesisResult,
                                                               synthesisError,
                                                               probe);
                  }
                  return platform::windows::synthesizeToPcmWav(
                      synthesisRequest, synthesisResult, synthesisError, probe);
              };

        RenderJobProgress preparing;
        preparing.phase = RenderPhase::Preparing;
        preparing.totalSegments = targets.size();
        preparing.message = request.scope == RenderScope::Selected
                                ? "Preparing selected question group"
                                : "Preparing all question groups";
        report(progress, std::move(preparing));

        for (std::size_t index = 0; index < targets.size(); ++index) {
            if (cancelled(shouldCancel)) {
                result.cancelled = true;
                result.error = "Render job cancelled";
                reportTerminal(RenderPhase::Cancelled, result.error);
                return result;
            }
            RenderedSegment rendered;
            std::string renderError;
            if (!renderSegment(request,
                               *targets[index],
                               cacheDirectory,
                               request.outputDirectory,
                               synthesize,
                               injectedSynthesis,
                               shouldCancel,
                               index,
                               targets.size(),
                               progress,
                               &rendered,
                               &result.voiceUses,
                               &renderError)) {
                result.cancelled = cancellation.isCancelled();
                result.error = renderError.empty() ? "Question-group rendering failed" : renderError;
                reportTerminal(result.cancelled ? RenderPhase::Cancelled : RenderPhase::Failed,
                               result.error);
                return result;
            }
            result.completedSegments.push_back(std::move(rendered));
            RenderJobProgress completed;
            completed.phase = RenderPhase::Publishing;
            completed.currentSegmentIndex = index;
            completed.totalSegments = targets.size();
            completed.completedSegments = result.completedSegments.size();
            completed.currentSegmentId = targets[index]->id;
            completed.message = "Question group completed";
            report(progress, std::move(completed));
        }

        if (request.scope == RenderScope::All) {
            if (cancelled(shouldCancel)) {
                result.cancelled = true;
                result.error = "Render job cancelled";
                reportTerminal(RenderPhase::Cancelled, result.error);
                return result;
            }
            const std::string key = programKey(result.completedSegments);
            const auto programPath =
                request.outputDirectory / ("complete-listening-program-" + key + ".wav");
            if (!inspectValidWav(programPath)) {
                RenderJobProgress assembling;
                assembling.phase = RenderPhase::Assembling;
                assembling.totalSegments = targets.size();
                assembling.completedSegments = result.completedSegments.size();
                assembling.message = "Assembling complete listening program";
                report(progress, std::move(assembling));
                std::vector<audio::ProgramClip> clips;
                clips.reserve(result.completedSegments.size());
                for (const auto& segment : result.completedSegments) {
                    clips.push_back(audio::ProgramClip{segment.path, 1, 0});
                }
                const auto temporary = temporarySibling(programPath, "program");
                audio::WavInfo info;
                std::string assemblyError;
                if (!audio::buildProgramWav(clips,
                                            temporary,
                                            &info,
                                            &assemblyError,
                                            shouldCancel)) {
                    std::filesystem::remove(temporary);
                    result.cancelled = cancellation.isCancelled();
                    result.error = assemblyError.empty() ? "Cannot assemble complete WAV"
                                                          : assemblyError;
                    reportTerminal(result.cancelled ? RenderPhase::Cancelled : RenderPhase::Failed,
                                   result.error);
                    return result;
                }
                if (!publishFile(temporary, programPath, &result.error)) {
                    std::filesystem::remove(temporary);
                    reportTerminal(RenderPhase::Failed, result.error);
                    return result;
                }
            }
            result.programPath = programPath;
        }

        result.success = true;
        reportTerminal(RenderPhase::Completed, "Render job completed");
        return result;
    } catch (const std::exception& error) {
        result.cancelled = cancellation.isCancelled();
        result.error = error.what();
        reportTerminal(result.cancelled ? RenderPhase::Cancelled : RenderPhase::Failed,
                       result.error);
        return result;
    } catch (...) {
        result.cancelled = cancellation.isCancelled();
        result.error = "Unknown render job failure";
        reportTerminal(result.cancelled ? RenderPhase::Cancelled : RenderPhase::Failed,
                       result.error);
        return result;
    }
}

struct RenderJobController::SharedState {
    mutable std::mutex mutex;
    std::vector<RenderJobProgress> pendingProgress;
    std::optional<RenderJobResult> result;
    bool finished{};
};

RenderJobController::RenderJobController(QObject* parent) : QObject(parent) {
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(50);
    connect(pollTimer_, &QTimer::timeout, this, &RenderJobController::pollWorker);
}

RenderJobController::~RenderJobController() {
    stopWorker();
}

bool RenderJobController::start(RenderJobRequest request,
                                RenderJobDependencies dependencies) {
    if (running_.load(std::memory_order_acquire) || workerThread_ != nullptr) {
        return false;
    }
    state_ = std::make_shared<SharedState>();
    cancellation_ = std::make_unique<RenderCancellationToken>();
    const auto state = state_;
    const auto cancellation = cancellation_.get();
    workerThread_ = QThread::create([state,
                                     cancellation,
                                     request = std::move(request),
                                     dependencies = std::move(dependencies)]() mutable {
        const RenderProgressCallback callback = [state](const RenderJobProgress& progress) {
            std::scoped_lock lock(state->mutex);
            state->pendingProgress.push_back(progress);
        };
        RenderJobResult result = RenderJobRunner::run(
            request, *cancellation, callback, std::move(dependencies));
        std::scoped_lock lock(state->mutex);
        state->result = std::move(result);
        state->finished = true;
    });
    running_.store(true, std::memory_order_release);
    workerThread_->start();
    pollTimer_->start();
    return true;
}

void RenderJobController::cancel() noexcept {
    if (cancellation_ != nullptr) {
        cancellation_->cancel();
    }
}

bool RenderJobController::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void RenderJobController::pollWorker() {
    const auto state = state_;
    if (state == nullptr) {
        return;
    }
    std::vector<RenderJobProgress> pending;
    std::optional<RenderJobResult> completed;
    {
        std::scoped_lock lock(state->mutex);
        pending.swap(state->pendingProgress);
        if (state->finished && state->result.has_value()) {
            completed = std::move(state->result);
        }
    }
    for (auto& progress : pending) {
        emit progressChanged(progress);
    }
    if (!completed.has_value()) {
        return;
    }
    if (pollTimer_ != nullptr) {
        pollTimer_->stop();
    }
    if (workerThread_ != nullptr) {
        workerThread_->wait();
        delete workerThread_;
        workerThread_ = nullptr;
    }
    running_.store(false, std::memory_order_release);
    cancellation_.reset();
    state_.reset();
    emit finished(*completed);
}

void RenderJobController::stopWorker() noexcept {
    if (pollTimer_ != nullptr) {
        pollTimer_->stop();
    }
    if (cancellation_ != nullptr) {
        cancellation_->cancel();
    }
    if (workerThread_ != nullptr) {
        workerThread_->wait();
        delete workerThread_;
        workerThread_ = nullptr;
    }
    cancellation_.reset();
    state_.reset();
    running_.store(false, std::memory_order_release);
}

}  // namespace listening::app
