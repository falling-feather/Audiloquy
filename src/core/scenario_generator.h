#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace listening {

enum class AnswerLabel {
    A,
    B,
    C,
};

enum class QuestionKind {
    When,
    Where,
    Why,
    What,
    Unsupported,
};

enum class SpeakerGender {
    Male,
    Female,
};

enum class EvidenceRole {
    Supports,
    Rejects,
};

enum class OptionVerdict {
    Supported,
    Rejected,
};

[[nodiscard]] std::string_view answerLabelCode(AnswerLabel label) noexcept;
[[nodiscard]] std::string_view speakerGenderCode(SpeakerGender gender) noexcept;
[[nodiscard]] QuestionKind classifyQuestion(std::string_view questionStem) noexcept;

struct ScenarioRequest {
    std::string questionStem;
    std::array<std::string, 3> options;
    std::optional<AnswerLabel> correctAnswer;
    std::string topic;
    std::optional<std::string> difficulty;
    std::optional<std::size_t> targetWordCount;

    bool operator==(const ScenarioRequest&) const = default;
};

struct DialogueTurn {
    std::string id;
    SpeakerGender speaker{SpeakerGender::Male};
    std::string text;
    int pauseAfterMs{250};

    bool operator==(const DialogueTurn&) const = default;
};

// Offsets and lengths are UTF-8 byte positions. The validator requires
// turn.text.substr(byteOffset, byteLength) == quote, so evidence remains exact
// even when the same word appears in multiple turns.
struct ScenarioEvidence {
    AnswerLabel option{AnswerLabel::A};
    EvidenceRole role{EvidenceRole::Supports};
    std::string turnId;
    std::size_t byteOffset{};
    std::size_t byteLength{};
    std::string quote;

    bool operator==(const ScenarioEvidence&) const = default;
};

struct OptionJudgment {
    AnswerLabel option{AnswerLabel::A};
    OptionVerdict verdict{OptionVerdict::Rejected};
    std::string explanation;
    std::vector<std::string> evidenceTurnIds;

    bool operator==(const OptionJudgment&) const = default;
};

struct ScenarioDraft {
    ScenarioRequest sourceRequest;
    QuestionKind questionKind{QuestionKind::Unsupported};
    std::string title;
    std::string setting;
    std::vector<DialogueTurn> turns;
    std::vector<ScenarioEvidence> evidence;
    std::array<OptionJudgment, 3> optionJudgments;
    AnswerLabel supportedAnswer{AnswerLabel::A};
    std::size_t wordCount{};
    bool requiresTeacherReview{true};

    bool operator==(const ScenarioDraft&) const = default;
};

enum class ScenarioValidationCode {
    MissingValue,
    InvalidUtf8,
    LimitExceeded,
    UnsupportedQuestionKind,
    DuplicateOption,
    MissingCorrectAnswer,
    InvalidAnswerLabel,
    InvalidTargetWordCount,
    InvalidSpeaker,
    InvalidTurn,
    DuplicateTurnId,
    MissingSpeaker,
    InvalidEvidence,
    MissingEvidence,
    InvalidJudgment,
    AnswerNotUnique,
    MetadataMismatch,
};

struct ScenarioValidationIssue {
    ScenarioValidationCode code{};
    std::string path;
    std::string message;

    bool operator==(const ScenarioValidationIssue&) const = default;
};

class ScenarioValidationError : public std::invalid_argument {
public:
    explicit ScenarioValidationError(std::vector<ScenarioValidationIssue> issues);

    [[nodiscard]] const std::vector<ScenarioValidationIssue>& issues() const noexcept;

private:
    std::vector<ScenarioValidationIssue> issues_;
};

[[nodiscard]] std::vector<ScenarioValidationIssue> validateScenarioRequest(
    const ScenarioRequest& request);

[[nodiscard]] std::vector<ScenarioValidationIssue> validateScenarioDraft(
    const ScenarioRequest& request,
    const ScenarioDraft& draft);

// Provider-neutral seam: a future cloud or local LLM adapter can implement the
// same contract. Callers must still run validateScenarioDraft before adoption.
class ITextGenerationProvider {
public:
    virtual ~ITextGenerationProvider() = default;
    [[nodiscard]] virtual ScenarioDraft generate(const ScenarioRequest& request) = 0;
};

class LocalScenarioProvider final : public ITextGenerationProvider {
public:
    [[nodiscard]] ScenarioDraft generate(const ScenarioRequest& request) override;
};

// Deterministic, offline baseline for anchored When/Where/Why/What questions.
// Invalid requests fail before a draft is returned.
[[nodiscard]] ScenarioDraft generateLocalScenario(const ScenarioRequest& request);

// Plain UTF-8 preview/TTS script, one "MAN:" or "WOMAN:" line per turn.
[[nodiscard]] std::string renderDialogueScript(const ScenarioDraft& draft);

}  // namespace listening
