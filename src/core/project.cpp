#include "core/project.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace listening {
namespace {

constexpr double minimumTargetWpm = 40.0;
constexpr double maximumTargetWpm = 300.0;
constexpr double maximumPauseSeconds = 3600.0;
constexpr int maximumRepeatCount = 100;
constexpr int maximumQuestionNumber = 100000;
constexpr std::size_t maximumIdBytes = 128;
constexpr std::size_t maximumTitleBytes = 1024;
constexpr std::size_t maximumSpeakerBytes = 512;
constexpr std::size_t maximumTextBytes = 4 * 1024 * 1024;
constexpr std::size_t maximumSegments = 10000;
constexpr std::uint64_t maximumRecordingDurationMs = 2ULL * 60ULL * 60ULL * 1000ULL;

[[nodiscard]] bool isAsciiAlphaNumeric(unsigned char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

[[nodiscard]] bool isStableId(std::string_view value) noexcept {
    if (value.empty() || value.size() > maximumIdBytes ||
        !isAsciiAlphaNumeric(static_cast<unsigned char>(value.front()))) {
        return false;
    }

    return std::all_of(value.begin(), value.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return isAsciiAlphaNumeric(value) || value == '-' || value == '_' || value == '.' ||
               value == ':';
    });
}

[[nodiscard]] bool isBlank(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](char character) {
        switch (character) {
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            return true;
        default:
            return false;
        }
    });
}

void addIssue(
    std::vector<ValidationIssue>& issues,
    ValidationCode code,
    std::string path,
    std::string message) {
    issues.push_back(ValidationIssue{code, std::move(path), std::move(message)});
}

[[nodiscard]] std::string validationMessage(const std::vector<ValidationIssue>& issues) {
    if (issues.empty()) {
        return "Project validation failed";
    }

    std::ostringstream output;
    output << "Project validation failed at " << issues.front().path << ": "
           << issues.front().message;
    if (issues.size() > 1) {
        output << " (and " << (issues.size() - 1) << " more issue";
        if (issues.size() != 2) {
            output << 's';
        }
        output << ')';
    }
    return output.str();
}

[[nodiscard]] bool decodeUtf8(
    std::string_view value,
    std::size_t& offset,
    std::uint32_t& codePoint) noexcept {
    if (offset >= value.size()) {
        return false;
    }

    const auto first = static_cast<unsigned char>(value[offset]);
    std::size_t length = 0;
    std::uint32_t minimum = 0;
    if (first <= 0x7fU) {
        length = 1;
        minimum = 0;
        codePoint = first;
    } else if ((first & 0xe0U) == 0xc0U) {
        length = 2;
        minimum = 0x80U;
        codePoint = first & 0x1fU;
    } else if ((first & 0xf0U) == 0xe0U) {
        length = 3;
        minimum = 0x800U;
        codePoint = first & 0x0fU;
    } else if ((first & 0xf8U) == 0xf0U) {
        length = 4;
        minimum = 0x10000U;
        codePoint = first & 0x07U;
    } else {
        return false;
    }

    if (offset + length > value.size()) {
        return false;
    }
    for (std::size_t index = 1; index < length; ++index) {
        const auto next = static_cast<unsigned char>(value[offset + index]);
        if ((next & 0xc0U) != 0x80U) {
            return false;
        }
        codePoint = (codePoint << 6U) | (next & 0x3fU);
    }

    if (codePoint < minimum || codePoint > 0x10ffffU ||
        (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
        return false;
    }

    offset += length;
    return true;
}

[[nodiscard]] bool isAsciiWordCharacter(std::uint32_t codePoint) noexcept {
    return (codePoint >= 'a' && codePoint <= 'z') ||
           (codePoint >= 'A' && codePoint <= 'Z') ||
           (codePoint >= '0' && codePoint <= '9');
}

[[nodiscard]] bool isLikelyNonAsciiWordCharacter(std::uint32_t codePoint) noexcept {
    // This is intentionally small and locale-free. It handles the letter ranges
    // commonly found in English names while keeping emoji and punctuation from
    // becoming WPM words.
    return (codePoint >= 0x00c0U && codePoint <= 0x02afU) ||
           (codePoint >= 0x0370U && codePoint <= 0x052fU) ||
           (codePoint >= 0x4e00U && codePoint <= 0x9fffU);
}

[[nodiscard]] std::chrono::milliseconds checkedMilliseconds(double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0 ||
        milliseconds > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        throw std::overflow_error("Estimated duration is outside the supported range");
    }
    return std::chrono::milliseconds{static_cast<std::int64_t>(std::llround(milliseconds))};
}

}  // namespace

std::string_view accentCode(Accent accent) noexcept {
    switch (accent) {
    case Accent::American:
        return "en-US";
    case Accent::British:
        return "en-GB";
    }
    return {};
}

bool tryParseAccent(std::string_view code, Accent& accent) noexcept {
    if (code == "en-US") {
        accent = Accent::American;
        return true;
    }
    if (code == "en-GB") {
        accent = Accent::British;
        return true;
    }
    return false;
}

Segment* Project::findSegment(std::string_view segmentId) noexcept {
    const auto iterator = std::find_if(
        segments.begin(), segments.end(), [segmentId](const Segment& segment) {
            return segment.id == segmentId;
        });
    return iterator == segments.end() ? nullptr : &*iterator;
}

const Segment* Project::findSegment(std::string_view segmentId) const noexcept {
    const auto iterator = std::find_if(
        segments.cbegin(), segments.cend(), [segmentId](const Segment& segment) {
            return segment.id == segmentId;
        });
    return iterator == segments.cend() ? nullptr : &*iterator;
}

ValidationError::ValidationError(std::vector<ValidationIssue> issues)
    : std::invalid_argument(validationMessage(issues)), issues_(std::move(issues)) {}

const std::vector<ValidationIssue>& ValidationError::issues() const noexcept {
    return issues_;
}

bool isValidUtf8(std::string_view value) noexcept {
    std::size_t offset = 0;
    while (offset < value.size()) {
        std::uint32_t codePoint = 0;
        if (!decodeUtf8(value, offset, codePoint)) {
            return false;
        }
    }
    return true;
}

std::vector<ValidationIssue> validate(const Project& project, ValidationPurpose purpose) {
    std::vector<ValidationIssue> issues;

    if (project.schemaVersion != Project::currentSchemaVersion) {
        addIssue(
            issues,
            ValidationCode::UnsupportedSchemaVersion,
            "schemaVersion",
            "only schema version 3 is supported after migration");
    }

    if (!isStableId(project.id)) {
        addIssue(
            issues,
            ValidationCode::InvalidId,
            "id",
            "must be 1-128 ASCII letters, digits, '.', ':', '_' or '-', starting with a letter or digit");
    }

    if ((project.title.empty() || isBlank(project.title)) &&
        purpose == ValidationPurpose::Strict) {
        addIssue(issues, ValidationCode::MissingValue, "title", "must not be blank");
    } else if (!project.title.empty() && !isValidUtf8(project.title)) {
        addIssue(issues, ValidationCode::InvalidUtf8, "title", "must be valid UTF-8");
    } else if (project.title.size() > maximumTitleBytes) {
        addIssue(issues, ValidationCode::LimitExceeded, "title", "is too long");
    }

    if (accentCode(project.accent).empty()) {
        addIssue(issues, ValidationCode::InvalidAccent, "accent", "is not supported");
    }

    if (!std::isfinite(project.targetWpm) || project.targetWpm < minimumTargetWpm ||
        project.targetWpm > maximumTargetWpm) {
        addIssue(
            issues,
            ValidationCode::InvalidTargetWpm,
            "targetWpm",
            "must be a finite value from 40 through 300");
    }

    const auto validateOptionalUtf8 = [&](std::string_view value,
                                          std::string path,
                                          std::size_t maximumBytes) {
        if (!value.empty() && !isValidUtf8(value)) {
            addIssue(issues, ValidationCode::InvalidUtf8, std::move(path),
                     "must be valid UTF-8");
        } else if (value.size() > maximumBytes) {
            addIssue(issues, ValidationCode::LimitExceeded, std::move(path), "is too long");
        }
    };
    validateOptionalUtf8(project.voiceSettings.maleVoiceTokenId,
                         "voiceSettings.maleVoiceTokenId", 16 * 1024);
    validateOptionalUtf8(project.voiceSettings.femaleVoiceTokenId,
                         "voiceSettings.femaleVoiceTokenId", 16 * 1024);
    validateOptionalUtf8(project.renderedProgramFile, "renderedProgramFile", 32 * 1024);

    const auto validateGenerationQuestion = [&](const GenerationQuestion& question,
                                                const std::string& questionPath) {
        if (question.questionStem.empty() || isBlank(question.questionStem)) {
            addIssue(issues, ValidationCode::MissingValue,
                     questionPath + ".questionStem", "must not be blank");
        } else {
            validateOptionalUtf8(question.questionStem,
                                 questionPath + ".questionStem", 32 * 1024);
        }
        for (std::size_t option = 0; option < question.options.size(); ++option) {
            if (question.options[option].empty() || isBlank(question.options[option])) {
                addIssue(issues, ValidationCode::MissingValue,
                         questionPath + ".options[" + std::to_string(option) + "]",
                         "must not be blank");
            } else {
                validateOptionalUtf8(question.options[option],
                                     questionPath + ".options[" + std::to_string(option) + "]",
                                     16 * 1024);
            }
        }
        if (question.correctAnswer != "A" && question.correctAnswer != "B" &&
            question.correctAnswer != "C") {
            addIssue(issues, ValidationCode::InvalidMetadata,
                     questionPath + ".correctAnswer", "must be A, B or C");
        }
        if (question.evidence.size() > 32) {
            addIssue(issues, ValidationCode::LimitExceeded,
                     questionPath + ".evidence", "contains more than 32 entries");
        }
        for (std::size_t evidence = 0; evidence < question.evidence.size(); ++evidence) {
            const auto evidencePath = questionPath + ".evidence[" +
                                       std::to_string(evidence) + "]";
            const GenerationEvidence& item = question.evidence[evidence];
            validateOptionalUtf8(item.option, evidencePath + ".option", 16);
            validateOptionalUtf8(item.role, evidencePath + ".role", 32);
            validateOptionalUtf8(item.turnId, evidencePath + ".turnId", 256);
            validateOptionalUtf8(item.quote, evidencePath + ".quote", 32 * 1024);
        }
    };

    if (project.segments.size() > maximumSegments) {
        addIssue(
            issues,
            ValidationCode::LimitExceeded,
            "segments",
            "contains more than 10000 segments");
    }

    std::unordered_set<std::string> ids;
    ids.reserve(project.segments.size());
    for (std::size_t index = 0; index < project.segments.size(); ++index) {
        const Segment& segment = project.segments[index];
        const std::string path = "segments[" + std::to_string(index) + "]";

        if (!isStableId(segment.id)) {
            addIssue(
                issues,
                ValidationCode::InvalidId,
                path + ".id",
                "must be a stable ASCII identifier");
        } else if (!ids.insert(segment.id).second) {
            addIssue(
                issues,
                ValidationCode::DuplicateSegmentId,
                path + ".id",
                "duplicates another segment id");
        }

        if (segment.questions.first < 1 || segment.questions.last < segment.questions.first ||
            segment.questions.last > maximumQuestionNumber) {
            addIssue(
                issues,
                ValidationCode::InvalidQuestionRange,
                path + ".questions",
                "must be an ascending range from 1 through 100000");
        }

        if (segment.speaker.empty() || isBlank(segment.speaker)) {
            addIssue(
                issues,
                ValidationCode::MissingValue,
                path + ".speaker",
                "must not be blank");
        } else if (!isValidUtf8(segment.speaker)) {
            addIssue(
                issues,
                ValidationCode::InvalidUtf8,
                path + ".speaker",
                "must be valid UTF-8");
        } else if (segment.speaker.size() > maximumSpeakerBytes) {
            addIssue(
                issues,
                ValidationCode::LimitExceeded,
                path + ".speaker",
                "is too long");
        }

        if ((segment.text.empty() || isBlank(segment.text)) &&
            purpose == ValidationPurpose::Strict && !segment.recording.has_value()) {
            addIssue(
                issues,
                ValidationCode::MissingValue,
                path + ".text",
                "must not be blank");
        } else if (!segment.text.empty() && !isValidUtf8(segment.text)) {
            addIssue(
                issues,
                ValidationCode::InvalidUtf8,
                path + ".text",
                "must be valid UTF-8");
        } else if (segment.text.size() > maximumTextBytes) {
            addIssue(
                issues,
                ValidationCode::LimitExceeded,
                path + ".text",
                "is larger than 4 MiB");
        }

        if (!std::isfinite(segment.pauseAfterSeconds) || segment.pauseAfterSeconds < 0.0 ||
            segment.pauseAfterSeconds > maximumPauseSeconds) {
            addIssue(
                issues,
                ValidationCode::InvalidPause,
                path + ".pauseAfterSeconds",
                "must be a finite value from 0 through 3600");
        }

        if (segment.repeatCount < 1 || segment.repeatCount > maximumRepeatCount) {
            addIssue(
                issues,
                ValidationCode::InvalidRepeatCount,
                path + ".repeatCount",
                "must be from 1 through 100");
        }

        validateOptionalUtf8(segment.renderedAudioFile, path + ".renderedAudioFile", 32 * 1024);
        if (segment.recording.has_value()) {
            const RecordingSource& recording = *segment.recording;
            if (recording.audioFile.empty() || isBlank(recording.audioFile)) {
                addIssue(issues, ValidationCode::MissingValue,
                         path + ".recording.audioFile", "must not be blank");
            } else {
                validateOptionalUtf8(recording.audioFile, path + ".recording.audioFile", 32 * 1024);
            }
            if (recording.startMs >= recording.endMs) {
                addIssue(issues, ValidationCode::InvalidMetadata,
                         path + ".recording", "startMs must be smaller than endMs");
            } else if (recording.endMs - recording.startMs > maximumRecordingDurationMs) {
                addIssue(issues, ValidationCode::LimitExceeded,
                         path + ".recording", "recording range is longer than 2 hours");
            }
        }
        if (segment.generation.has_value()) {
            const GenerationRecord& record = *segment.generation;
            if (record.provider.empty() || isBlank(record.provider) ||
                !isValidUtf8(record.provider)) {
                addIssue(issues, ValidationCode::InvalidMetadata, path + ".generation.provider",
                         "must be non-blank UTF-8");
            }
            validateOptionalUtf8(record.model, path + ".generation.model", 1024);
            validateGenerationQuestion(record, path + ".generation");
            if (record.additionalQuestions.size() > 2) {
                addIssue(issues, ValidationCode::LimitExceeded,
                         path + ".generation.additionalQuestions",
                         "contains more than two additional questions");
            }
            for (std::size_t question = 0; question < record.additionalQuestions.size();
                 ++question) {
                validateGenerationQuestion(
                    record.additionalQuestions[question],
                    path + ".generation.additionalQuestions[" + std::to_string(question) + "]");
            }
            if (!record.requiresTeacherReview && !record.teacherReviewed) {
                addIssue(issues, ValidationCode::InvalidMetadata,
                         path + ".generation.teacherReviewed",
                         "must be true when teacher review is no longer required");
            }
        }
    }

    return issues;
}

void requireValid(const Project& project, ValidationPurpose purpose) {
    auto issues = validate(project, purpose);
    if (!issues.empty()) {
        throw ValidationError(std::move(issues));
    }
}

std::size_t countReadableWords(std::string_view text) noexcept {
    std::size_t count = 0;
    std::size_t offset = 0;
    bool insideWord = false;

    while (offset < text.size()) {
        std::uint32_t codePoint = 0;
        if (!decodeUtf8(text, offset, codePoint)) {
            // Invalid UTF-8 is rejected by validation. Treat an invalid byte as a
            // separator here so this noexcept utility remains safe on raw input.
            ++offset;
            insideWord = false;
            continue;
        }

        const bool apostrophe = codePoint == '\'' || codePoint == 0x2019U;
        const bool wordCharacter = isAsciiWordCharacter(codePoint) ||
                                   isLikelyNonAsciiWordCharacter(codePoint) ||
                                   (apostrophe && insideWord);
        if (wordCharacter && !insideWord) {
            ++count;
        }
        insideWord = wordCharacter;
    }

    return count;
}

std::chrono::milliseconds estimateReadingDuration(
    std::string_view text,
    double targetWpm) {
    if (!std::isfinite(targetWpm) || targetWpm <= 0.0) {
        throw std::invalid_argument("targetWpm must be a positive finite value");
    }

    const auto words = static_cast<double>(countReadableWords(text));
    return checkedMilliseconds(words * 60'000.0 / targetWpm);
}

std::chrono::milliseconds estimateSegmentDuration(const Segment& segment, double targetWpm) {
    if (segment.repeatCount < 1) {
        throw std::invalid_argument("repeatCount must be at least 1");
    }
    if (!std::isfinite(segment.pauseAfterSeconds) || segment.pauseAfterSeconds < 0.0) {
        throw std::invalid_argument("pauseAfterSeconds must be a non-negative finite value");
    }

    if (segment.recording && (segment.recording->endMs <= segment.recording->startMs || segment.recording->endMs > 7'200'000)) {
        throw std::invalid_argument("recording range is invalid");
    }
    const auto speech = segment.recording
        ? std::chrono::milliseconds(segment.recording->endMs - segment.recording->startMs)
        : estimateReadingDuration(segment.text, targetWpm);
    const double totalMilliseconds =
        static_cast<double>(speech.count()) * static_cast<double>(segment.repeatCount) +
        segment.pauseAfterSeconds * 1000.0 * static_cast<double>(segment.repeatCount);
    return checkedMilliseconds(totalMilliseconds);
}

std::chrono::milliseconds estimateProjectDuration(const Project& project) {
    requireValid(project);

    std::chrono::milliseconds total{0};
    for (const Segment& segment : project.segments) {
        const auto duration = estimateSegmentDuration(segment, project.targetWpm);
        if (duration.count() > std::chrono::milliseconds::max().count() - total.count()) {
            throw std::overflow_error("Estimated project duration is outside the supported range");
        }
        total += duration;
    }
    return total;
}

}  // namespace listening
