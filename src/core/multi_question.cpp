#include "core/multi_question.h"

#include "core/project.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace listening {
namespace {

constexpr std::size_t maximumQuestions = 3;
constexpr std::size_t minimumQuestions = 2;
constexpr std::size_t maximumTopicBytes = 1000;
constexpr std::size_t maximumSceneBytes = 2048;
constexpr std::size_t maximumTurns = 64;

[[nodiscard]] bool isAsciiSpace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] bool isBlank(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), isAsciiSpace);
}

[[nodiscard]] std::string trimCopy(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && isAsciiSpace(value[first])) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && isAsciiSpace(value[last - 1])) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

[[nodiscard]] char asciiLower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

[[nodiscard]] std::string lowerFirstAscii(std::string value) {
    if (!value.empty()) {
        value.front() = asciiLower(value.front());
    }
    return value;
}

[[nodiscard]] std::string cleanOption(std::string_view value) {
    std::string result = trimCopy(value);
    while (!result.empty()) {
        const char last = result.back();
        if (last != '.' && last != '?' && last != '!' && last != ';' && last != ':') {
            break;
        }
        result.pop_back();
        while (!result.empty() && isAsciiSpace(result.back())) {
            result.pop_back();
        }
    }
    return result;
}

[[nodiscard]] std::string normalizedOption(std::string_view value) {
    const std::string cleaned = cleanOption(value);
    std::string result;
    result.reserve(cleaned.size());
    bool pendingSpace = false;
    for (const char character : cleaned) {
        if (isAsciiSpace(character)) {
            pendingSpace = !result.empty();
            continue;
        }
        if (pendingSpace) {
            result.push_back(' ');
            pendingSpace = false;
        }
        result.push_back(asciiLower(character));
    }
    return result;
}

[[nodiscard]] std::size_t labelIndex(AnswerLabel label) noexcept {
    switch (label) {
    case AnswerLabel::A:
        return 0;
    case AnswerLabel::B:
        return 1;
    case AnswerLabel::C:
        return 2;
    }
    return 3;
}

[[nodiscard]] AnswerLabel labelAt(std::size_t index) noexcept {
    return index == 0 ? AnswerLabel::A : index == 1 ? AnswerLabel::B : AnswerLabel::C;
}

[[nodiscard]] std::array<AnswerLabel, 2> wrongAnswers(AnswerLabel correct) {
    std::array<AnswerLabel, 2> result{};
    std::size_t next = 0;
    for (std::size_t index = 0; index < 3; ++index) {
        const AnswerLabel candidate = labelAt(index);
        if (candidate != correct) {
            result[next++] = candidate;
        }
    }
    return result;
}

[[nodiscard]] std::string normalizedStem(std::string_view value) {
    std::string result;
    bool pendingSpace = false;
    for (const char character : value) {
        if (isAsciiSpace(character)) {
            pendingSpace = !result.empty();
            continue;
        }
        if (pendingSpace) {
            result.push_back(' ');
            pendingSpace = false;
        }
        result.push_back(asciiLower(character));
    }
    return result;
}

[[nodiscard]] bool isAsciiWordCharacter(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '\'';
}

[[nodiscard]] std::string replacePhraseInsensitive(std::string value,
                                                   std::string_view phrase,
                                                   std::string_view replacement) {
    if (phrase.empty() || phrase.size() > value.size()) {
        return value;
    }
    std::size_t searchStart = 0;
    while (searchStart + phrase.size() <= value.size()) {
        std::size_t match = std::string::npos;
        for (std::size_t start = searchStart;
             start + phrase.size() <= value.size(); ++start) {
            const bool leftBoundary = start == 0 ||
                                      !isAsciiWordCharacter(value[start - 1]);
            const std::size_t end = start + phrase.size();
            const bool rightBoundary = end == value.size() ||
                                       !isAsciiWordCharacter(value[end]);
            if (!leftBoundary || !rightBoundary) {
                continue;
            }
            bool equal = true;
            for (std::size_t offset = 0; offset < phrase.size(); ++offset) {
                if (asciiLower(value[start + offset]) != asciiLower(phrase[offset])) {
                    equal = false;
                    break;
                }
            }
            if (equal) {
                match = start;
                break;
            }
        }
        if (match == std::string::npos) {
            break;
        }
        value.replace(match, phrase.size(), replacement);
        searchStart = match + replacement.size();
    }
    return value;
}

[[nodiscard]] std::string conversationalQuestion(std::string_view stem) {
    std::string result = trimCopy(stem);
    result = replacePhraseInsensitive(result, "the woman", "you");
    result = replacePhraseInsensitive(result, "the man", "you");
    result = replacePhraseInsensitive(result, "she", "you");
    result = replacePhraseInsensitive(result, "he", "you");
    result = replacePhraseInsensitive(result, "her", "your");
    result = replacePhraseInsensitive(result, "his", "your");
    return result;
}

[[nodiscard]] SpeakerGender subjectGenderForQuestion(std::string_view stem) noexcept {
    const std::string normalized = normalizedStem(stem);
    for (std::size_t i = 0; i < normalized.size();) {
        if (normalized[i] < 'a' || normalized[i] > 'z') { ++i; continue; }
        const auto start = i;
        while (i < normalized.size() && normalized[i] >= 'a' && normalized[i] <= 'z') ++i;
        const auto word = std::string_view(normalized).substr(start, i-start);
        if (word == "woman" || word == "she") return SpeakerGender::Female;
        if (word == "man" || word == "he") return SpeakerGender::Male;
    }
    return SpeakerGender::Male;
}

[[nodiscard]] ScenarioRequest effectiveRequest(const MultiQuestionRequest& request,
                                               const ScenarioRequest& question) {
    ScenarioRequest result = question;
    if (!request.topic.empty()) {
        result.topic = request.topic;
    }
    if (request.difficulty.has_value()) {
        result.difficulty = request.difficulty;
    }
    if (request.targetWordCount.has_value()) {
        result.targetWordCount = request.targetWordCount;
    }
    return result;
}

void addIssue(std::vector<ScenarioValidationIssue>& issues,
              ScenarioValidationCode code,
              std::string path,
              std::string message) {
    issues.push_back(ScenarioValidationIssue{code, std::move(path), std::move(message)});
}

[[nodiscard]] std::string titleForTopic(std::string_view topic) {
    const std::string trimmed = trimCopy(topic);
    return trimmed.empty() ? "A shared listening situation" : "Talking about " + trimmed;
}

[[nodiscard]] std::string settingForTopic(std::string_view topic) {
    const std::string trimmed = trimCopy(topic);
    return trimmed.empty() ? "Two people confirming details of one shared plan"
                           : "Two people confirming several details about " + trimmed;
}

[[nodiscard]] std::size_t dialogueWordCount(const std::vector<DialogueTurn>& turns) noexcept {
    std::size_t total = 0;
    for (const DialogueTurn& turn : turns) {
        total += countReadableWords(turn.text);
    }
    return total;
}

[[nodiscard]] std::string turnId(std::size_t index) {
    std::ostringstream output;
    output << "turn-" << (index < 10 ? "0" : "") << index;
    return output.str();
}

[[nodiscard]] std::string optionText(const ScenarioRequest& request, AnswerLabel label) {
    // Preserve proper nouns and pronouns; only terminal punctuation is removed.
    return cleanOption(request.options.at(labelIndex(label)));
}

[[nodiscard]] std::string lowerClauseOption(std::string value) {
    if (value.empty()) {
        return value;
    }
    std::size_t end = 0;
    while (end < value.size() &&
           ((value[end] >= 'a' && value[end] <= 'z') ||
            (value[end] >= 'A' && value[end] <= 'Z'))) {
        ++end;
    }
    const std::string firstWord = normalizedOption(value.substr(0, end));
    constexpr std::array<std::string_view, 18> functionalWords{
        "a", "an", "the", "at", "in", "on", "by", "near", "beside",
        "inside", "outside", "to", "because", "for", "with", "from",
        "today", "tomorrow"};
    if (std::find(functionalWords.begin(), functionalWords.end(), firstWord) !=
        functionalWords.end()) {
        value.front() = asciiLower(value.front());
    }
    return value;
}

[[nodiscard]] std::vector<ScenarioValidationIssue> validateSharedFacts(
    const MultiQuestionRequest& request) {
    std::vector<ScenarioValidationIssue> issues;
    std::unordered_set<std::string> stems;
    for (std::size_t questionIndex = 0; questionIndex < request.questions.size();
         ++questionIndex) {
        const ScenarioRequest effective = effectiveRequest(request, request.questions[questionIndex]);
        const std::string stem = normalizedStem(effective.questionStem);
        if (!stem.empty() && !stems.insert(stem).second) {
            addIssue(issues, ScenarioValidationCode::DuplicateOption,
                     "questions[" + std::to_string(questionIndex) + "].questionStem",
                     "questions in one passage must ask different things");
        }
    }
    return issues;
}

}  // namespace

std::vector<ScenarioValidationIssue> validateMultiQuestionRequest(
    const MultiQuestionRequest& request) {
    std::vector<ScenarioValidationIssue> issues;
    if (request.questions.size() < minimumQuestions || request.questions.size() > maximumQuestions) {
        addIssue(issues, ScenarioValidationCode::LimitExceeded, "questions",
                 "one shared passage must contain from two through three questions");
    }
    if (request.topic.empty() || isBlank(request.topic)) {
        addIssue(issues, ScenarioValidationCode::MissingValue, "topic", "must not be blank");
    } else if (!isValidUtf8(request.topic)) {
        addIssue(issues, ScenarioValidationCode::InvalidUtf8, "topic", "must be valid UTF-8");
    } else if (request.topic.size() > maximumTopicBytes) {
        addIssue(issues, ScenarioValidationCode::LimitExceeded, "topic", "is too long");
    }
    if (request.difficulty.has_value() &&
        (request.difficulty->empty() || isBlank(*request.difficulty))) {
        addIssue(issues, ScenarioValidationCode::MissingValue, "difficulty", "must not be blank");
    }
    if (request.questions.size() >= minimumQuestions && request.questions.size() <= maximumQuestions) {
        for (std::size_t index = 0; index < request.questions.size(); ++index) {
            const ScenarioRequest effective = effectiveRequest(request, request.questions[index]);
            const auto childIssues = validateScenarioRequest(effective);
            for (const auto& issue : childIssues) {
                addIssue(issues, issue.code,
                         "questions[" + std::to_string(index) + "]." + issue.path,
                         issue.message);
            }
        }
        const auto factIssues = validateSharedFacts(request);
        issues.insert(issues.end(), factIssues.begin(), factIssues.end());
    }
    return issues;
}

std::vector<ScenarioValidationIssue> validateMultiQuestionDraft(
    const MultiQuestionRequest& request,
    const MultiQuestionDraft& draft) {
    std::vector<ScenarioValidationIssue> issues = validateMultiQuestionRequest(request);
    if (draft.sourceRequest != request) {
        addIssue(issues, ScenarioValidationCode::MetadataMismatch, "sourceRequest",
                 "must exactly match the request used for validation");
    }
    if (draft.questions.size() != request.questions.size()) {
        addIssue(issues, ScenarioValidationCode::MetadataMismatch, "questions",
                 "must contain one result for every source question");
    }
    if (draft.title.empty() || isBlank(draft.title) || draft.title.size() > maximumSceneBytes ||
        !isValidUtf8(draft.title)) {
        addIssue(issues, ScenarioValidationCode::MissingValue, "title",
                 "must be a non-blank UTF-8 title");
    }
    if (draft.setting.empty() || isBlank(draft.setting) || draft.setting.size() > maximumSceneBytes ||
        !isValidUtf8(draft.setting)) {
        addIssue(issues, ScenarioValidationCode::MissingValue, "setting",
                 "must be a non-blank UTF-8 setting");
    }
    if (draft.turns.size() > maximumTurns) {
        addIssue(issues, ScenarioValidationCode::LimitExceeded, "turns",
                 "must contain no more than 64 turns");
    }
    const std::size_t questionCount = std::min(draft.questions.size(), request.questions.size());
    for (std::size_t index = 0; index < questionCount; ++index) {
        const ScenarioRequest effective = effectiveRequest(request, request.questions[index]);
        ScenarioRequest validationRequest = effective;
        // A shared passage has one opening and one closing, so its useful
        // length is not the sum of three independent single-question drafts.
        // Validate the common structure here and apply the wider shared-length
        // band below.
        validationRequest.targetWordCount.reset();
        ScenarioDraft view;
        view.sourceRequest = validationRequest;
        view.questionKind = draft.questions[index].questionKind;
        view.title = draft.title;
        view.setting = draft.setting;
        view.turns = draft.turns;
        view.evidence = draft.questions[index].evidence;
        view.optionJudgments = draft.questions[index].optionJudgments;
        view.supportedAnswer = draft.questions[index].supportedAnswer;
        view.wordCount = draft.wordCount;
        view.requiresTeacherReview = draft.requiresTeacherReview;
        const auto childIssues = validateScenarioDraft(validationRequest, view);
        for (const auto& issue : childIssues) {
            addIssue(issues, issue.code,
                     "questions[" + std::to_string(index) + "]." + issue.path,
                     issue.message);
        }
        if (draft.questions[index].sourceRequest != effective) {
            addIssue(issues, ScenarioValidationCode::MetadataMismatch,
                     "questions[" + std::to_string(index) + "].sourceRequest",
                     "must use the shared topic, level and length");
        }
    }
    if (request.targetWordCount.has_value()) {
        const std::size_t target = *request.targetWordCount;
        const std::size_t lower = target / 2;
        // A shared passage has a fixed cost for every question's three-choice
        // fact check. A small target must not force us to remove evidence or
        // make the passage less natural; only an implausibly short result is
        // rejected. The actual word count remains visible in the preview.
        if (draft.wordCount < lower) {
            addIssue(issues, ScenarioValidationCode::MetadataMismatch, "wordCount",
                     "shared passage is shorter than the minimum honest range around targetWordCount");
        }
    }
    if (draft.wordCount != dialogueWordCount(draft.turns)) {
        addIssue(issues, ScenarioValidationCode::MetadataMismatch, "wordCount",
                 "must equal the deterministic word count of all turns");
    }
    return issues;
}

MultiQuestionDraft generateLocalMultiQuestion(const MultiQuestionRequest& request) {
    const auto requestIssues = validateMultiQuestionRequest(request);
    if (!requestIssues.empty()) {
        throw ScenarioValidationError(requestIssues);
    }

    MultiQuestionDraft draft;
    draft.sourceRequest = request;
    draft.title = titleForTopic(request.topic);
    draft.setting = settingForTopic(request.topic);
    draft.questions.reserve(request.questions.size());

    const SpeakerGender firstSubject =
        subjectGenderForQuestion(request.questions.front().questionStem);
    const SpeakerGender firstAsker = firstSubject == SpeakerGender::Female
                                         ? SpeakerGender::Male
                                         : SpeakerGender::Female;

    const auto addTurn = [&](SpeakerGender speaker, std::string text, int pauseAfterMs = 250) {
        const std::size_t index = draft.turns.size();
        draft.turns.push_back(DialogueTurn{turnId(index + 1), speaker, std::move(text), pauseAfterMs});
        return index;
    };
    const auto addEvidence = [&](std::vector<ScenarioEvidence>& evidence,
                                 AnswerLabel option,
                                 EvidenceRole role,
                                 std::size_t turnIndex,
                                 const std::string& quote,
                                 std::size_t questionIndex) {
        const DialogueTurn& turn = draft.turns.at(turnIndex);
        const std::size_t offset = turn.text.find(quote);
        if (offset == std::string::npos) {
            std::ostringstream detail;
            detail << "Multi-question template produced missing evidence text at question "
                   << (questionIndex + 1) << ", option " << answerLabelCode(option)
                   << ", quote [" << quote << "] in turn [" << turn.text << "]";
            throw std::logic_error(detail.str());
        }
        evidence.push_back(ScenarioEvidence{option, role, turn.id, offset, quote.size(), quote});
    };

    const std::string topic = trimCopy(request.topic);
    addTurn(firstAsker, "Let's sort out the details of " + topic + ".");
    addTurn(firstSubject, "Sure. What would you like to know?");
    SpeakerGender lastSpeaker = firstSubject;

    for (std::size_t questionIndex = 0; questionIndex < request.questions.size(); ++questionIndex) {
        const ScenarioRequest effective = effectiveRequest(request, request.questions[questionIndex]);
        MultiQuestionItem item;
        item.sourceRequest = effective;
        item.questionKind = classifyQuestion(effective.questionStem);
        item.supportedAnswer = *effective.correctAnswer;

        const SpeakerGender questionSubject = subjectGenderForQuestion(effective.questionStem);
        const SpeakerGender questionAsker = questionSubject == SpeakerGender::Female
                                                 ? SpeakerGender::Male
                                                 : SpeakerGender::Female;
        if (lastSpeaker == questionAsker) {
            // If the next question belongs to the other person, give that
            // person a short natural handoff before the asker speaks.
            addTurn(questionSubject, "And what about your side of the plan?");
            lastSpeaker = questionSubject;
        }
        std::string questionText = conversationalQuestion(effective.questionStem);
        if (questionIndex != 0) {
            questionText = "And " + lowerFirstAscii(std::move(questionText));
        }
        addTurn(questionAsker, std::move(questionText));
        const auto wrong = wrongAnswers(*effective.correctAnswer);
        const std::string wrongOne = optionText(effective, wrong[0]);
        const std::string wrongTwo = optionText(effective, wrong[1]);
        const std::string correct = optionText(effective, *effective.correctAnswer);
        const std::string firstRejection =
            "I first thought it might be " + lowerClauseOption(wrongOne) +
            ", but that was only the old plan.";
        addTurn(questionSubject, firstRejection, 300);
        const std::string secondQuestionOption = lowerClauseOption(wrongTwo);
        addTurn(questionAsker, "Or was it " + secondQuestionOption + "?");
        const std::string rejectionQuote = "not " + lowerClauseOption(wrongOne) +
                                            " or " + secondQuestionOption;
        const std::string answer = correct + ", " + rejectionQuote + ".";
        const std::size_t supportTurn = addTurn(questionSubject, answer, 350);
        // Both distractors are anchored to the same final clarification. The
        // earlier old-plan remark is context, not a second semantic proof.
        addEvidence(item.evidence, wrong[0], EvidenceRole::Rejects, supportTurn,
                    rejectionQuote, questionIndex);
        addEvidence(item.evidence, wrong[1], EvidenceRole::Rejects, supportTurn,
                    rejectionQuote, questionIndex);
        addEvidence(item.evidence, *effective.correctAnswer, EvidenceRole::Supports,
                    supportTurn, correct, questionIndex);
        lastSpeaker = questionSubject;

        for (std::size_t option = 0; option < item.optionJudgments.size(); ++option) {
            const AnswerLabel label = labelAt(option);
            OptionJudgment judgment;
            judgment.option = label;
            judgment.verdict = label == item.supportedAnswer ? OptionVerdict::Supported
                                                              : OptionVerdict::Rejected;
            judgment.explanation = label == item.supportedAnswer
                                       ? "The shared dialogue confirms this detail."
                                       : "The shared dialogue rules out this detail.";
            for (const ScenarioEvidence& evidence : item.evidence) {
                if (evidence.option == label) {
                    judgment.evidenceTurnIds.push_back(evidence.turnId);
                }
            }
            item.optionJudgments[option] = std::move(judgment);
        }
        draft.questions.push_back(std::move(item));
    }

    const SpeakerGender closingAsker = lastSpeaker == SpeakerGender::Female
                                           ? SpeakerGender::Male
                                           : SpeakerGender::Female;
    addTurn(closingAsker, "That clears it up. I'll tell the others what we decided.");
    addTurn(lastSpeaker, "Good. Then everyone will have the same information.", 700);
    if (request.targetWordCount.has_value() &&
        dialogueWordCount(draft.turns) < *request.targetWordCount / 2) {
        // A high requested length earns one meaningful continuation about the
        // corrected plan; it is tied to the distractor correction above and
        // does not invent another answer fact.
        addTurn(closingAsker,
                "I remember the first suggestion, but it was only part of our discussion.");
        addTurn(lastSpeaker,
                "Right. We changed the arrangement after we talked, and the final detail is the one we should share.",
                700);
    }

    draft.wordCount = dialogueWordCount(draft.turns);
    const auto draftIssues = validateMultiQuestionDraft(request, draft);
    if (!draftIssues.empty()) {
        throw ScenarioValidationError(draftIssues);
    }
    return draft;
}

std::string renderMultiDialogueScript(const MultiQuestionDraft& draft) {
    const auto issues = validateMultiQuestionDraft(draft.sourceRequest, draft);
    if (!issues.empty()) {
        throw ScenarioValidationError(issues);
    }
    std::string output;
    for (const DialogueTurn& turn : draft.turns) {
        output += turn.speaker == SpeakerGender::Male ? "MAN: " : "WOMAN: ";
        output += turn.text;
        output.push_back('\n');
    }
    return output;
}

}  // namespace listening
