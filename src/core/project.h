#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

namespace listening {

enum class Accent {
    American,
    British,
};

// Draft documents are intentionally allowed to contain an empty title or
// script so a teacher can save work in progress. Generation and export callers
// should keep using the default Strict purpose.
enum class ValidationPurpose {
    Strict,
    Draft,
};

[[nodiscard]] std::string_view accentCode(Accent accent) noexcept;
[[nodiscard]] bool tryParseAccent(std::string_view code, Accent& accent) noexcept;

struct QuestionRange {
    int first{1};
    int last{1};

    bool operator==(const QuestionRange&) const = default;
};

struct GenerationEvidence {
    std::string option;
    std::string role;
    std::string turnId;
    std::string quote;

    bool operator==(const GenerationEvidence&) const = default;
};

// Audit data survives save/reopen so a structurally valid AI/local draft is
// never mistaken for a teacher-approved listening question.
struct GenerationRecord {
    std::string provider;
    std::string model;
    std::string questionStem;
    std::array<std::string, 3> options;
    std::string correctAnswer;
    std::vector<GenerationEvidence> evidence;
    bool requiresTeacherReview{true};
    bool teacherReviewed{false};

    bool operator==(const GenerationRecord&) const = default;
};

struct VoiceSettings {
    std::string maleVoiceTokenId;
    std::string femaleVoiceTokenId;
    bool strictAccent{true};
    bool allowGenderFallback{false};

    bool operator==(const VoiceSettings&) const = default;
};

// repeatCount is the total number of times the text is played. A value of 1
// means "play once". pauseAfterSeconds is applied after every play, including
// the final one, which keeps the estimated program duration deterministic.
struct Segment {
    std::string id;
    QuestionRange questions;
    std::string speaker;
    std::string text;
    double pauseAfterSeconds{0.0};
    int repeatCount{1};
    std::string renderedAudioFile;
    std::optional<GenerationRecord> generation;

    Segment() = default;
    Segment(std::string segmentId,
            QuestionRange questionRange,
            std::string speakerName,
            std::string script,
            double pauseSeconds,
            int totalRepeats,
            std::string audioFile = {},
            std::optional<GenerationRecord> generationRecord = std::nullopt)
        : id(std::move(segmentId)),
          questions(questionRange),
          speaker(std::move(speakerName)),
          text(std::move(script)),
          pauseAfterSeconds(pauseSeconds),
          repeatCount(totalRepeats),
          renderedAudioFile(std::move(audioFile)),
          generation(std::move(generationRecord)) {}

    bool operator==(const Segment&) const = default;
};

struct Project {
    static constexpr int currentSchemaVersion = 2;

    int schemaVersion{currentSchemaVersion};
    std::string id;
    std::string title;
    Accent accent{Accent::American};
    double targetWpm{120.0};
    VoiceSettings voiceSettings;
    std::vector<Segment> segments;
    std::string renderedProgramFile;

    [[nodiscard]] Segment* findSegment(std::string_view segmentId) noexcept;
    [[nodiscard]] const Segment* findSegment(std::string_view segmentId) const noexcept;

    bool operator==(const Project&) const = default;
};

enum class ValidationCode {
    UnsupportedSchemaVersion,
    InvalidId,
    DuplicateSegmentId,
    MissingValue,
    InvalidUtf8,
    InvalidAccent,
    InvalidTargetWpm,
    InvalidQuestionRange,
    InvalidPause,
    InvalidRepeatCount,
    InvalidMetadata,
    LimitExceeded,
};

struct ValidationIssue {
    ValidationCode code{};
    std::string path;
    std::string message;

    bool operator==(const ValidationIssue&) const = default;
};

class ValidationError : public std::invalid_argument {
public:
    explicit ValidationError(std::vector<ValidationIssue> issues);

    [[nodiscard]] const std::vector<ValidationIssue>& issues() const noexcept;

private:
    std::vector<ValidationIssue> issues_;
};

[[nodiscard]] bool isValidUtf8(std::string_view value) noexcept;
[[nodiscard]] std::vector<ValidationIssue> validate(
    const Project& project,
    ValidationPurpose purpose = ValidationPurpose::Strict);
void requireValid(
    const Project& project,
    ValidationPurpose purpose = ValidationPurpose::Strict);

[[nodiscard]] std::size_t countReadableWords(std::string_view text) noexcept;

// Estimates one reading of text, without any segment pause or repetition.
[[nodiscard]] std::chrono::milliseconds estimateReadingDuration(
    std::string_view text,
    double targetWpm);

// Includes all repetitions and the pause following each play.
[[nodiscard]] std::chrono::milliseconds estimateSegmentDuration(
    const Segment& segment,
    double targetWpm);

// Includes every segment repetition and pause using project.targetWpm.
[[nodiscard]] std::chrono::milliseconds estimateProjectDuration(const Project& project);

}  // namespace listening
