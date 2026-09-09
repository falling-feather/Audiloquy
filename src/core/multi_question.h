#pragma once

#include "core/scenario_generator.h"

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace listening {

// Several questions share one passage. Each request still carries its own
// stem, choices and answer; topic, level and desired length belong to the
// shared listening situation.
struct MultiQuestionRequest {
    std::vector<ScenarioRequest> questions;
    std::string topic;
    std::optional<std::string> difficulty;
    std::optional<std::size_t> targetWordCount;

    bool operator==(const MultiQuestionRequest&) const = default;
};

struct MultiQuestionItem {
    ScenarioRequest sourceRequest;
    QuestionKind questionKind{QuestionKind::Unsupported};
    std::vector<ScenarioEvidence> evidence;
    std::array<OptionJudgment, 3> optionJudgments;
    AnswerLabel supportedAnswer{AnswerLabel::A};

    bool operator==(const MultiQuestionItem&) const = default;
};

struct MultiQuestionDraft {
    MultiQuestionRequest sourceRequest;
    std::string title;
    std::string setting;
    std::vector<DialogueTurn> turns;
    std::vector<MultiQuestionItem> questions;
    std::size_t wordCount{};
    bool requiresTeacherReview{true};

    bool operator==(const MultiQuestionDraft&) const = default;
};

[[nodiscard]] std::vector<ScenarioValidationIssue> validateMultiQuestionRequest(
    const MultiQuestionRequest& request);

[[nodiscard]] std::vector<ScenarioValidationIssue> validateMultiQuestionDraft(
    const MultiQuestionRequest& request,
    const MultiQuestionDraft& draft);

[[nodiscard]] MultiQuestionDraft generateLocalMultiQuestion(
    const MultiQuestionRequest& request);

[[nodiscard]] std::string renderMultiDialogueScript(const MultiQuestionDraft& draft);

}  // namespace listening
