#include "core/scenario_generator.h"

#include "core/project.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace listening {
namespace {

constexpr std::size_t maximumStemBytes = 2000;
constexpr std::size_t maximumOptionBytes = 1000;
constexpr std::size_t maximumTopicBytes = 1000;
constexpr std::size_t maximumDifficultyBytes = 100;
constexpr std::size_t maximumSceneMetadataBytes = 2048;
constexpr std::size_t minimumTargetWords = 50;
constexpr std::size_t maximumTargetWords = 240;
constexpr std::size_t maximumTurns = 64;
constexpr int maximumPauseAfterMs = 10000;

[[nodiscard]] bool isAsciiSpace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] bool isBlank(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), isAsciiSpace);
}

[[nodiscard]] bool hasForbiddenControl(std::string_view value) noexcept {
    return std::any_of(value.begin(), value.end(), [](char character) {
        return static_cast<unsigned char>(character) < 0x20U;
    });
}

[[nodiscard]] bool validAnswerLabel(AnswerLabel label) noexcept {
    switch (label) {
    case AnswerLabel::A:
    case AnswerLabel::B:
    case AnswerLabel::C:
        return true;
    }
    return false;
}

[[nodiscard]] bool validQuestionKind(QuestionKind kind) noexcept {
    switch (kind) {
    case QuestionKind::When:
    case QuestionKind::Where:
    case QuestionKind::Why:
    case QuestionKind::What:
    case QuestionKind::Unsupported:
        return true;
    }
    return false;
}

[[nodiscard]] bool validSpeaker(SpeakerGender speaker) noexcept {
    switch (speaker) {
    case SpeakerGender::Male:
    case SpeakerGender::Female:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEvidenceRole(EvidenceRole role) noexcept {
    switch (role) {
    case EvidenceRole::Supports:
    case EvidenceRole::Rejects:
        return true;
    }
    return false;
}

[[nodiscard]] bool validVerdict(OptionVerdict verdict) noexcept {
    switch (verdict) {
    case OptionVerdict::Supported:
    case OptionVerdict::Rejected:
        return true;
    }
    return false;
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
    constexpr std::array labels{AnswerLabel::A, AnswerLabel::B, AnswerLabel::C};
    return index < labels.size() ? labels[index] : static_cast<AnswerLabel>(-1);
}

[[nodiscard]] char asciiLower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
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

[[nodiscard]] bool startsWith(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] std::string lowerFirstAscii(std::string value) {
    if (!value.empty()) {
        value.front() = asciiLower(value.front());
    }
    return value;
}

[[nodiscard]] std::string upperFirstAscii(std::string value) {
    if (!value.empty() && value.front() >= 'a' && value.front() <= 'z') {
        value.front() = static_cast<char>(value.front() - 'a' + 'A');
    }
    return value;
}

[[nodiscard]] std::string timePhrase(std::string_view option) {
    const std::string cleaned = cleanOption(option);
    const std::string normalized = normalizedOption(cleaned);
    if (normalized == "morning" || normalized == "the morning") {
        return "in the morning";
    }
    if (normalized == "afternoon" || normalized == "the afternoon") {
        return "in the afternoon";
    }
    if (normalized == "evening" || normalized == "the evening") {
        return "in the evening";
    }
    if (normalized == "night" || normalized == "the night") {
        return "at night";
    }
    return lowerFirstAscii(cleaned);
}

[[nodiscard]] std::string locationPhrase(std::string_view option) {
    const std::string cleaned = lowerFirstAscii(cleanOption(option));
    const std::string normalized = normalizedOption(cleaned);
    constexpr std::array<std::string_view, 8> prefixes{
        "at ", "in ", "on ", "near ", "by ", "inside ", "outside ", "beside "};
    if (std::any_of(prefixes.begin(), prefixes.end(), [&](std::string_view prefix) {
            return startsWith(normalized, prefix);
        })) {
        return cleaned;
    }
    return "at " + cleaned;
}

[[nodiscard]] std::string reasonPhrase(std::string_view option) {
    return lowerFirstAscii(cleanOption(option));
}

[[nodiscard]] std::string reasonQuestion(std::string_view option) {
    const std::string phrase = reasonPhrase(option);
    const std::string normalized = normalizedOption(phrase);
    if (startsWith(normalized, "because ") || startsWith(normalized, "to ")) {
        return "Was it " + phrase + "?";
    }
    return "Was it because of " + phrase + "?";
}

[[nodiscard]] std::string negativeReason(std::string_view option) {
    const std::string phrase = reasonPhrase(option);
    const std::string normalized = normalizedOption(phrase);
    if (startsWith(normalized, "because ") || startsWith(normalized, "to ")) {
        return "No, it wasn't " + phrase + ".";
    }
    return "No, it wasn't because of " + phrase + ".";
}

[[nodiscard]] std::string positiveReason(std::string_view option) {
    const std::string phrase = reasonPhrase(option);
    return "The real reason is " + phrase + ".";
}

[[nodiscard]] std::string objectPhrase(std::string_view option) {
    return lowerFirstAscii(cleanOption(option));
}

[[nodiscard]] bool containsConvenienceStore(std::string_view topic) {
    std::string lowered;
    lowered.reserve(topic.size());
    for (const char character : topic) {
        lowered.push_back(asciiLower(character));
    }
    constexpr std::string_view chineseConvenienceStore =
        "\xe4\xbe\xbf\xe5\x88\xa9\xe5\xba\x97";
    return lowered.find("convenience store") != std::string::npos ||
           lowered.find("corner store") != std::string::npos ||
           topic.find(chineseConvenienceStore) != std::string_view::npos;
}

[[nodiscard]] std::string conciseTopic(std::string_view topic) {
    std::string result = lowerFirstAscii(cleanOption(topic));
    std::size_t wordCount = 0;
    bool inWord = false;
    for (std::size_t index = 0; index < result.size(); ++index) {
        if (isAsciiSpace(result[index])) {
            inWord = false;
            continue;
        }
        if (!inWord) {
            ++wordCount;
            inWord = true;
            if (wordCount > 6) {
                result.erase(index);
                while (!result.empty() && isAsciiSpace(result.back())) {
                    result.pop_back();
                }
                break;
            }
        }
    }
    constexpr std::size_t maximumSpokenTopicBytes = 80;
    if (result.size() > maximumSpokenTopicBytes) {
        std::size_t end = maximumSpokenTopicBytes;
        while (end > 0 &&
               (static_cast<unsigned char>(result[end]) & 0xC0U) == 0x80U) {
            --end;
        }
        result.erase(end);
    }
    return result.empty() ? std::string("the listening question") : result;
}

[[nodiscard]] bool wantsCompactDialogue(const ScenarioRequest& request) noexcept {
    return request.targetWordCount.has_value() && *request.targetWordCount <= 70;
}

[[nodiscard]] bool isFutureTimePhrase(std::string_view phrase) {
    const std::string normalized = normalizedOption(phrase);
    return normalized.find("tomorrow") != std::string::npos ||
           normalized.find("next week") != std::string::npos ||
           normalized.find("next month") != std::string::npos ||
           normalized.find("next year") != std::string::npos;
}

struct DialogueRoles {
    SpeakerGender subject{SpeakerGender::Male};
    SpeakerGender asker{SpeakerGender::Female};
};

[[nodiscard]] DialogueRoles rolesForQuestion(std::string_view questionStem) {
    const std::string normalized = normalizedOption(questionStem);
    const bool asksAboutWoman =
        normalized.find("woman") != std::string::npos ||
        normalized.find(" she ") != std::string::npos;
    const SpeakerGender subject =
        asksAboutWoman ? SpeakerGender::Female : SpeakerGender::Male;
    return DialogueRoles{
        subject,
        subject == SpeakerGender::Female ? SpeakerGender::Male : SpeakerGender::Female};
}

void addIssue(
    std::vector<ScenarioValidationIssue>& issues,
    ScenarioValidationCode code,
    std::string path,
    std::string message) {
    issues.push_back(ScenarioValidationIssue{code, std::move(path), std::move(message)});
}

void validateTextField(
    std::vector<ScenarioValidationIssue>& issues,
    std::string_view value,
    std::size_t maximumBytes,
    std::string path) {
    if (value.empty() || isBlank(value)) {
        addIssue(issues, ScenarioValidationCode::MissingValue, std::move(path), "must not be blank");
        return;
    }
    if (!isValidUtf8(value)) {
        addIssue(issues, ScenarioValidationCode::InvalidUtf8, std::move(path), "must be valid UTF-8");
        return;
    }
    if (hasForbiddenControl(value)) {
        addIssue(
            issues,
            ScenarioValidationCode::InvalidTurn,
            std::move(path),
            "must not contain control characters or line breaks");
        return;
    }
    if (value.size() > maximumBytes) {
        addIssue(issues, ScenarioValidationCode::LimitExceeded, std::move(path), "is too long");
    }
}

[[nodiscard]] std::string validationMessage(
    const std::vector<ScenarioValidationIssue>& issues) {
    if (issues.empty()) {
        return "Scenario validation failed";
    }
    std::ostringstream output;
    output << "Scenario validation failed at " << issues.front().path << ": "
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

[[nodiscard]] std::string turnId(std::size_t number) {
    std::ostringstream output;
    output << "turn-" << std::setw(2) << std::setfill('0') << number;
    return output.str();
}

[[nodiscard]] std::size_t dialogueWordCount(const ScenarioDraft& draft) noexcept {
    std::size_t total = 0;
    for (const DialogueTurn& turn : draft.turns) {
        total += countReadableWords(turn.text);
    }
    return total;
}

class DraftBuilder {
public:
    explicit DraftBuilder(const ScenarioRequest& request) {
        draft.sourceRequest = request;
        draft.questionKind = classifyQuestion(request.questionStem);
        draft.supportedAnswer = *request.correctAnswer;
        draft.requiresTeacherReview = true;
    }

    std::size_t addTurn(
        SpeakerGender speaker,
        std::string text,
        int pauseAfterMs = 250) {
        const std::size_t index = draft.turns.size();
        draft.turns.push_back(DialogueTurn{
            turnId(index + 1), speaker, std::move(text), pauseAfterMs});
        return index;
    }

    void addEvidence(
        AnswerLabel option,
        EvidenceRole role,
        std::size_t turnIndex,
        std::string quote) {
        const DialogueTurn& turn = draft.turns.at(turnIndex);
        const std::size_t offset = turn.text.find(quote);
        if (offset == std::string::npos) {
            throw std::logic_error("Local scenario template produced missing evidence text");
        }
        draft.evidence.push_back(ScenarioEvidence{
            option, role, turn.id, offset, quote.size(), std::move(quote)});
    }

    void appendTargetFillers() {
        if (!draft.sourceRequest.targetWordCount.has_value()) {
            return;
        }
        const std::size_t target = *draft.sourceRequest.targetWordCount;
        const std::size_t lowerTarget = target > 8 ? target - 8 : 0;
        constexpr std::array<std::pair<std::string_view, std::string_view>, 10> fillers{{
            {"I'll update the plan so everyone has the same information.",
             "Good. That should prevent any confusion later."},
            {"I will also check the details once more before we leave.",
             "Thanks. It is better to be certain about the arrangement."},
            {"We can send a short message to the others as well.",
             "Yes, then nobody will follow the old plan by mistake."},
            {"I have written the final detail in my calendar now.",
             "Perfect. I will put the same detail in mine."},
            {"That gives us enough time to prepare everything carefully.",
             "Exactly. The final arrangement should work well for everyone."},
            {"I will keep a copy of the updated plan on my phone.",
             "That will be useful if we need to check it later."},
            {"We should tell the group before anyone makes another arrangement.",
             "I agree. I can send the message as soon as we finish."},
            {"Nothing else in the plan needs to change at this point.",
             "Good. Keeping the other details unchanged will make things simpler."},
            {"I am glad we checked every possibility before making the decision.",
             "So am I. Now the final plan is clear to both of us."},
            {"Let me repeat that detail when I speak to the others.",
             "Please do. A clear reminder will help everyone remember it."},
        }};

        SpeakerGender nextSpeaker =
            draft.turns.empty() || draft.turns.back().speaker == SpeakerGender::Female
                ? SpeakerGender::Male
                : SpeakerGender::Female;
        for (const auto& [firstText, secondText] : fillers) {
            if (dialogueWordCount(draft) >= lowerTarget) {
                break;
            }
            addTurn(nextSpeaker, std::string(firstText));
            nextSpeaker = nextSpeaker == SpeakerGender::Male ? SpeakerGender::Female
                                                              : SpeakerGender::Male;
            addTurn(nextSpeaker, std::string(secondText), 600);
            nextSpeaker = nextSpeaker == SpeakerGender::Male ? SpeakerGender::Female
                                                              : SpeakerGender::Male;
        }
    }

    void finishJudgments() {
        for (std::size_t index = 0; index < draft.optionJudgments.size(); ++index) {
            const AnswerLabel label = labelAt(index);
            const bool correct = label == draft.supportedAnswer;
            OptionJudgment judgment;
            judgment.option = label;
            judgment.verdict = correct ? OptionVerdict::Supported : OptionVerdict::Rejected;
            judgment.explanation = correct
                                       ? "The dialogue explicitly states and confirms this answer."
                                       : "The dialogue explicitly considers and rejects this answer.";
            for (const ScenarioEvidence& item : draft.evidence) {
                if (item.option == label) {
                    judgment.evidenceTurnIds.push_back(item.turnId);
                }
            }
            draft.optionJudgments[index] = std::move(judgment);
        }
        draft.wordCount = dialogueWordCount(draft);
    }

    ScenarioDraft draft;
};

void addTopicOpening(
    DraftBuilder& builder,
    const DialogueRoles& roles,
    const ScenarioRequest& request) {
    const std::string topic = conciseTopic(request.topic);
    const std::string difficulty = request.difficulty.has_value()
                                       ? normalizedOption(*request.difficulty)
                                       : std::string();
    if (difficulty == "a2") {
        builder.addTurn(roles.asker, "Can I ask you about " + topic + "?");
        builder.addTurn(roles.subject, "Of course. What is it?");
    } else if (difficulty == "b2") {
        builder.addTurn(roles.asker, "I'd like to clear up one detail about " + topic + ".");
        builder.addTurn(roles.subject, "Certainly. Which detail do you mean?");
    } else {
        builder.addTurn(roles.asker, "I wanted to ask you about " + topic + ".");
        builder.addTurn(roles.subject, "Sure. What do you need to know?");
    }
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

[[nodiscard]] const std::string& optionFor(
    const ScenarioRequest& request,
    AnswerLabel label) {
    return request.options.at(labelIndex(label));
}

void buildWhen(DraftBuilder& builder) {
    const ScenarioRequest& request = builder.draft.sourceRequest;
    const AnswerLabel correct = builder.draft.supportedAnswer;
    const auto wrong = wrongAnswers(correct);
    const std::string firstWrong = timePhrase(optionFor(request, wrong[0]));
    const std::string secondWrong = timePhrase(optionFor(request, wrong[1]));
    const std::string answer = timePhrase(optionFor(request, correct));
    const bool store = containsConvenienceStore(request.topic) ||
                       containsConvenienceStore(request.questionStem);
    const std::string normalizedStem = normalizedOption(request.questionStem);
    const bool past = startsWith(normalizedStem, "when did ");
    const DialogueRoles roles = rolesForQuestion(request.questionStem);
    const SpeakerGender subject = roles.subject;
    const SpeakerGender asker = roles.asker;

    builder.draft.title = store ? "A visit to the convenience store" : "Choosing a time";
    builder.draft.setting =
        store ? "Two friends arranging a convenience-store visit"
              : "Two people confirming the time for a shared plan";

    addTopicOpening(builder, roles, request);
    const bool firstWrongIsFuture = past && isFutureTimePhrase(firstWrong);
    builder.addTurn(
        asker,
        past ? (firstWrongIsFuture ? "Did you mean " + firstWrong + "?"
                                   : "Did you go " + firstWrong + "?")
             : "Would going " + firstWrong + " work?");
    const std::string firstRejectionText =
        past ? (firstWrongIsFuture
                    ? "No, not " + firstWrong + "—the visit had already happened."
                    : "No, it wasn't " + firstWrong + ".")
             : "I'm afraid " + firstWrong + " won't work for me.";
    const std::size_t firstRejection = builder.addTurn(
        subject, firstRejectionText);
    builder.addEvidence(
        wrong[0],
        EvidenceRole::Rejects,
        firstRejection,
        past ? (firstWrongIsFuture ? "not " + firstWrong
                                   : "wasn't " + firstWrong)
             : firstWrong + " won't work");
    const bool secondWrongIsFuture = past && isFutureTimePhrase(secondWrong);
    builder.addTurn(
        asker,
        past ? (secondWrongIsFuture ? "Did you mean " + secondWrong + "?"
                                    : "Was it " + secondWrong + "?")
             : "What about going " + secondWrong + "?");
    const std::string normalizedSecondWrong = normalizedOption(secondWrong);
    const std::string normalizedAnswer = normalizedOption(answer);
    const bool secondWrongIsLate =
        normalizedSecondWrong.find("evening") != std::string::npos ||
        normalizedSecondWrong.find("night") != std::string::npos;
    const bool answerIsLate = normalizedAnswer.find("evening") != std::string::npos ||
                              normalizedAnswer.find("night") != std::string::npos;
    const std::string secondRejectionText =
        past ? (secondWrongIsFuture
                    ? "No, not " + secondWrong + "—the visit had already happened."
                    : "No, it wasn't " + secondWrong + " either.")
             : (store && secondWrongIsLate && !answerIsLate
                    ? "No, " + secondWrong +
                          " is not possible because the store closes early."
                      : "I'm afraid " + secondWrong + " won't work either.");
    const std::size_t secondRejection = builder.addTurn(
        subject, secondRejectionText);
    builder.addEvidence(
        wrong[1],
        EvidenceRole::Rejects,
        secondRejection,
        past ? (secondWrongIsFuture ? "not " + secondWrong
                                    : "wasn't " + secondWrong)
             : (store && secondWrongIsLate && !answerIsLate
                    ? secondWrong + " is not possible"
                    : secondWrong + " won't work"));
    builder.addTurn(
        asker,
        past ? "So when did you go?"
             : (store ? "So when will you go?" : "Which time will you choose, then?"));
    const std::string answerStatement =
        past ? "I went " + answer + "—that's when it happened."
             : (store ? "I'll go " + answer + ". That is the time I have chosen."
                      : "I'll do it " + answer + ". That is the final time.");
    const std::size_t support = builder.addTurn(subject, answerStatement);
    builder.addEvidence(
        correct,
        EvidenceRole::Supports,
        support,
        past ? "I went " + answer
             : (store ? "I'll go " + answer : "I'll do it " + answer));
    if (!wantsCompactDialogue(request)) {
        const std::string confirmationText =
            past ? "I see. Your visit was " + answer + "."
                 : "Good. The confirmed time is " + answer + ".";
        const std::size_t confirmation = builder.addTurn(
            asker,
            confirmationText,
            700);
        builder.addEvidence(
            correct,
            EvidenceRole::Supports,
            confirmation,
            past ? "Your visit was " + answer : "confirmed time is " + answer);
    }
}

void buildWhere(DraftBuilder& builder) {
    const ScenarioRequest& request = builder.draft.sourceRequest;
    const AnswerLabel correct = builder.draft.supportedAnswer;
    const auto wrong = wrongAnswers(correct);
    const std::string firstWrong = locationPhrase(optionFor(request, wrong[0]));
    const std::string secondWrong = locationPhrase(optionFor(request, wrong[1]));
    const std::string answer = locationPhrase(optionFor(request, correct));
    const DialogueRoles roles = rolesForQuestion(request.questionStem);

    builder.draft.title = "Choosing a place";
    builder.draft.setting = "Two people deciding where to meet";
    addTopicOpening(builder, roles, request);
    builder.addTurn(roles.asker, "Could you meet " + firstWrong + "?");
    const std::size_t firstRejection = builder.addTurn(
        roles.subject,
        "No, I can't meet " + firstWrong + ". That place won't work for me.");
    builder.addEvidence(
        wrong[0], EvidenceRole::Rejects, firstRejection, "can't meet " + firstWrong);
    builder.addTurn(roles.asker, "Then what about " + secondWrong + "?");
    const std::size_t secondRejection = builder.addTurn(
        roles.subject,
        "I'm afraid I can't meet " + secondWrong + " either.");
    builder.addEvidence(
        wrong[1], EvidenceRole::Rejects, secondRejection, "can't meet " + secondWrong);
    builder.addTurn(roles.asker, "Where will you meet, then?");
    const std::size_t support = builder.addTurn(
        roles.subject, "Let's meet " + answer + ". That works for me.");
    builder.addEvidence(
        correct, EvidenceRole::Supports, support, "Let's meet " + answer);
    if (!wantsCompactDialogue(request)) {
        const std::size_t confirmation = builder.addTurn(
            roles.asker,
            "Agreed. " + upperFirstAscii(answer) + " is our final location.",
            700);
        builder.addEvidence(
            correct,
            EvidenceRole::Supports,
            confirmation,
            upperFirstAscii(answer) + " is our final location");
    }
}

void buildWhy(DraftBuilder& builder) {
    const ScenarioRequest& request = builder.draft.sourceRequest;
    const AnswerLabel correct = builder.draft.supportedAnswer;
    const auto wrong = wrongAnswers(correct);
    const DialogueRoles roles = rolesForQuestion(request.questionStem);

    builder.draft.title = "Explaining a reason";
    builder.draft.setting = "Two people clarifying why something happened";
    addTopicOpening(builder, roles, request);
    builder.addTurn(roles.asker, reasonQuestion(optionFor(request, wrong[0])));
    const std::string firstNegative = negativeReason(optionFor(request, wrong[0]));
    const std::size_t firstRejection =
        builder.addTurn(roles.subject, firstNegative);
    builder.addEvidence(
        wrong[0], EvidenceRole::Rejects, firstRejection, cleanOption(firstNegative));
    builder.addTurn(roles.asker, reasonQuestion(optionFor(request, wrong[1])));
    const std::string secondNegative = negativeReason(optionFor(request, wrong[1]));
    const std::size_t secondRejection =
        builder.addTurn(roles.subject, secondNegative);
    builder.addEvidence(
        wrong[1], EvidenceRole::Rejects, secondRejection, cleanOption(secondNegative));
    builder.addTurn(roles.asker, "What was the real reason, then?");
    const std::string positive = positiveReason(optionFor(request, correct));
    const std::size_t support = builder.addTurn(roles.subject, positive);
    builder.addEvidence(correct, EvidenceRole::Supports, support, cleanOption(positive));
    if (!wantsCompactDialogue(request)) {
        builder.addTurn(
            roles.asker,
            "I understand. That is the reason we should remember.",
            700);
    }
}

void buildWhat(DraftBuilder& builder) {
    const ScenarioRequest& request = builder.draft.sourceRequest;
    const AnswerLabel correct = builder.draft.supportedAnswer;
    const auto wrong = wrongAnswers(correct);
    const std::string firstWrong = objectPhrase(optionFor(request, wrong[0]));
    const std::string secondWrong = objectPhrase(optionFor(request, wrong[1]));
    const std::string answer = objectPhrase(optionFor(request, correct));
    const DialogueRoles roles = rolesForQuestion(request.questionStem);

    builder.draft.title = "Clarifying a detail";
    builder.draft.setting = "Two people identifying the correct detail";
    addTopicOpening(builder, roles, request);
    builder.addTurn(roles.asker, "Was it " + firstWrong + "?");
    const std::size_t firstRejection = builder.addTurn(
        roles.subject, "No, it wasn't " + firstWrong + ".");
    builder.addEvidence(
        wrong[0], EvidenceRole::Rejects, firstRejection, "wasn't " + firstWrong);
    builder.addTurn(roles.asker, "Could it have been " + secondWrong + "?");
    const std::size_t secondRejection = builder.addTurn(
        roles.subject, "No, it wasn't " + secondWrong + " either.");
    builder.addEvidence(
        wrong[1], EvidenceRole::Rejects, secondRejection, "wasn't " + secondWrong);
    builder.addTurn(roles.asker, "What was it, then?");
    const std::size_t support = builder.addTurn(
        roles.subject, "It was " + answer + "—that's the one.");
    builder.addEvidence(correct, EvidenceRole::Supports, support, "It was " + answer);
    if (!wantsCompactDialogue(request)) {
        const std::size_t confirmation = builder.addTurn(
            roles.asker,
            "Right. " + upperFirstAscii(answer) + " is what we agreed on.",
            700);
        builder.addEvidence(
            correct,
            EvidenceRole::Supports,
            confirmation,
            upperFirstAscii(answer) + " is what we agreed on");
    }
}

}  // namespace

std::string_view answerLabelCode(AnswerLabel label) noexcept {
    switch (label) {
    case AnswerLabel::A:
        return "A";
    case AnswerLabel::B:
        return "B";
    case AnswerLabel::C:
        return "C";
    }
    return {};
}

std::string_view speakerGenderCode(SpeakerGender gender) noexcept {
    switch (gender) {
    case SpeakerGender::Male:
        return "male";
    case SpeakerGender::Female:
        return "female";
    }
    return {};
}

QuestionKind classifyQuestion(std::string_view questionStem) noexcept {
    std::size_t offset = 0;
    while (offset < questionStem.size() && isAsciiSpace(questionStem[offset])) {
        ++offset;
    }
    const std::size_t start = offset;
    while (offset < questionStem.size()) {
        const char character = questionStem[offset];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z'))) {
            break;
        }
        ++offset;
    }
    if (offset < questionStem.size() && !isAsciiSpace(questionStem[offset])) {
        return QuestionKind::Unsupported;
    }
    const std::string_view token = questionStem.substr(start, offset - start);
    const auto equals = [token](std::string_view expected) {
        if (token.size() != expected.size()) {
            return false;
        }
        for (std::size_t index = 0; index < token.size(); ++index) {
            if (asciiLower(token[index]) != expected[index]) {
                return false;
            }
        }
        return true;
    };
    if (equals("when")) {
        return QuestionKind::When;
    }
    if (equals("where")) {
        return QuestionKind::Where;
    }
    if (equals("why")) {
        return QuestionKind::Why;
    }
    if (equals("what")) {
        return QuestionKind::What;
    }
    return QuestionKind::Unsupported;
}

ScenarioValidationError::ScenarioValidationError(
    std::vector<ScenarioValidationIssue> issues)
    : std::invalid_argument(validationMessage(issues)), issues_(std::move(issues)) {}

const std::vector<ScenarioValidationIssue>& ScenarioValidationError::issues() const noexcept {
    return issues_;
}

std::vector<ScenarioValidationIssue> validateScenarioRequest(const ScenarioRequest& request) {
    std::vector<ScenarioValidationIssue> issues;
    validateTextField(
        issues, request.questionStem, maximumStemBytes, "questionStem");
    if (classifyQuestion(request.questionStem) == QuestionKind::Unsupported) {
        addIssue(
            issues,
            ScenarioValidationCode::UnsupportedQuestionKind,
            "questionStem",
            "must begin with When, Where, Why or What");
    }

    std::array<std::string, 3> normalized;
    for (std::size_t index = 0; index < request.options.size(); ++index) {
        const std::string path =
            "options[" + std::string(answerLabelCode(labelAt(index))) + "]";
        validateTextField(issues, request.options[index], maximumOptionBytes, path);
        normalized[index] = normalizedOption(request.options[index]);
        if (normalized[index].empty()) {
            addIssue(
                issues,
                ScenarioValidationCode::MissingValue,
                path,
                "must contain a readable option value, not only punctuation");
        }
    }
    for (std::size_t left = 0; left < normalized.size(); ++left) {
        if (normalized[left].empty()) {
            continue;
        }
        for (std::size_t right = left + 1; right < normalized.size(); ++right) {
            if (normalized[right].empty()) {
                continue;
            }
            if (normalized[left] == normalized[right]) {
                addIssue(
                    issues,
                    ScenarioValidationCode::DuplicateOption,
                    "options",
                    "options must remain distinct after normalization");
            } else if (normalized[left].find(normalized[right]) != std::string::npos ||
                       normalized[right].find(normalized[left]) != std::string::npos) {
                addIssue(
                    issues,
                    ScenarioValidationCode::DuplicateOption,
                    "options",
                    "one option contains another and may not have a unique meaning");
            }
        }
    }

    if (!request.correctAnswer.has_value()) {
        addIssue(
            issues,
            ScenarioValidationCode::MissingCorrectAnswer,
            "correctAnswer",
            "is required");
    } else if (!validAnswerLabel(*request.correctAnswer)) {
        addIssue(
            issues,
            ScenarioValidationCode::InvalidAnswerLabel,
            "correctAnswer",
            "must be A, B or C");
    }

    validateTextField(issues, request.topic, maximumTopicBytes, "topic");
    if (request.difficulty.has_value()) {
        validateTextField(
            issues, *request.difficulty, maximumDifficultyBytes, "difficulty");
    }
    if (request.targetWordCount.has_value() &&
        (*request.targetWordCount < minimumTargetWords ||
         *request.targetWordCount > maximumTargetWords)) {
        addIssue(
            issues,
            ScenarioValidationCode::InvalidTargetWordCount,
            "targetWordCount",
            "must be from 50 through 240 words");
    }
    return issues;
}

std::vector<ScenarioValidationIssue> validateScenarioDraft(
    const ScenarioRequest& request,
    const ScenarioDraft& draft) {
    std::vector<ScenarioValidationIssue> issues = validateScenarioRequest(request);
    const bool usableCorrectAnswer =
        request.correctAnswer.has_value() && validAnswerLabel(*request.correctAnswer);

    if (draft.sourceRequest != request) {
        addIssue(
            issues,
            ScenarioValidationCode::MetadataMismatch,
            "sourceRequest",
            "must exactly match the request used for validation");
    }

    const QuestionKind expectedKind = classifyQuestion(request.questionStem);
    if (!validQuestionKind(draft.questionKind) || draft.questionKind == QuestionKind::Unsupported ||
        draft.questionKind != expectedKind) {
        addIssue(
            issues,
            ScenarioValidationCode::MetadataMismatch,
            "questionKind",
            "does not match the anchored question word");
    }
    validateTextField(issues, draft.title, maximumSceneMetadataBytes, "title");
    validateTextField(issues, draft.setting, maximumSceneMetadataBytes, "setting");

    if (draft.turns.size() < 4 || draft.turns.size() > maximumTurns) {
        addIssue(
            issues,
            ScenarioValidationCode::LimitExceeded,
            "turns",
            "must contain from 4 through 64 turns");
    }

    std::unordered_map<std::string, const DialogueTurn*> turnsById;
    turnsById.reserve(draft.turns.size());
    bool hasMale = false;
    bool hasFemale = false;
    std::optional<SpeakerGender> previousSpeaker;
    for (std::size_t index = 0; index < draft.turns.size(); ++index) {
        const DialogueTurn& turn = draft.turns[index];
        const std::string path = "turns[" + std::to_string(index) + "]";
        if (turn.id.empty() || isBlank(turn.id) || hasForbiddenControl(turn.id) ||
            !isValidUtf8(turn.id)) {
            addIssue(
                issues,
                ScenarioValidationCode::InvalidTurn,
                path + ".id",
                "must be a non-blank UTF-8 identifier");
        } else if (!turnsById.emplace(turn.id, &turn).second) {
            addIssue(
                issues,
                ScenarioValidationCode::DuplicateTurnId,
                path + ".id",
                "duplicates another turn id");
        }
        if (!validSpeaker(turn.speaker)) {
            addIssue(
                issues,
                ScenarioValidationCode::InvalidSpeaker,
                path + ".speaker",
                "must be male or female");
        } else {
            hasMale = hasMale || turn.speaker == SpeakerGender::Male;
            hasFemale = hasFemale || turn.speaker == SpeakerGender::Female;
            if (previousSpeaker.has_value() && *previousSpeaker == turn.speaker) {
                addIssue(
                    issues,
                    ScenarioValidationCode::InvalidSpeaker,
                    path + ".speaker",
                    "dialogue speakers must alternate");
            }
            previousSpeaker = turn.speaker;
        }
        validateTextField(issues, turn.text, maximumStemBytes, path + ".text");
        if (turn.pauseAfterMs < 0 || turn.pauseAfterMs > maximumPauseAfterMs) {
            addIssue(
                issues,
                ScenarioValidationCode::InvalidTurn,
                path + ".pauseAfterMs",
                "must be from 0 through 10000");
        }
    }
    if (!hasMale || !hasFemale) {
        addIssue(
            issues,
            ScenarioValidationCode::MissingSpeaker,
            "turns",
            "must contain both a male and a female speaker");
    }

    std::array<int, 3> supportEvidence{};
    std::array<int, 3> rejectionEvidence{};
    for (std::size_t index = 0; index < draft.evidence.size(); ++index) {
        const ScenarioEvidence& item = draft.evidence[index];
        const std::string path = "evidence[" + std::to_string(index) + "]";
        const std::size_t optionIndex = labelIndex(item.option);
        if (optionIndex >= 3) {
            addIssue(
                issues,
                ScenarioValidationCode::InvalidEvidence,
                path + ".option",
                "must reference option A, B or C");
        }
        if (!validEvidenceRole(item.role)) {
            addIssue(
                issues,
                ScenarioValidationCode::InvalidEvidence,
                path + ".role",
                "must support or reject its option");
        }
        const auto turn = turnsById.find(item.turnId);
        bool exactRange = false;
        if (turn == turnsById.end()) {
            addIssue(
                issues,
                ScenarioValidationCode::InvalidEvidence,
                path + ".turnId",
                "does not reference an existing turn");
        } else if (item.quote.empty() || !isValidUtf8(item.quote) ||
                   item.byteOffset > turn->second->text.size() ||
                   item.byteLength > turn->second->text.size() - item.byteOffset ||
                   item.byteLength != item.quote.size() ||
                   turn->second->text.substr(item.byteOffset, item.byteLength) != item.quote) {
            addIssue(
                issues,
                ScenarioValidationCode::InvalidEvidence,
                path,
                "byte range must exactly identify the quoted UTF-8 text");
        } else {
            exactRange = true;
        }
        if (exactRange && optionIndex < 3) {
            const std::string normalizedQuote = normalizedOption(item.quote);
            const std::string normalizedAnswer = normalizedOption(request.options[optionIndex]);
            if (normalizedQuote.find(normalizedAnswer) == std::string::npos) {
                addIssue(
                    issues,
                    ScenarioValidationCode::InvalidEvidence,
                    path + ".quote",
                    "must explicitly contain the option value it supports or rejects");
            }
        }
        if (optionIndex < 3 && validEvidenceRole(item.role)) {
            if (item.role == EvidenceRole::Supports) {
                ++supportEvidence[optionIndex];
            } else {
                ++rejectionEvidence[optionIndex];
            }
        }
    }

    int supportedJudgments = 0;
    for (std::size_t index = 0; index < draft.optionJudgments.size(); ++index) {
        const OptionJudgment& judgment = draft.optionJudgments[index];
        const std::string path =
            "optionJudgments[" + std::string(answerLabelCode(labelAt(index))) + "]";
        if (!validAnswerLabel(judgment.option) || judgment.option != labelAt(index)) {
            addIssue(
                issues,
                ScenarioValidationCode::InvalidJudgment,
                path + ".option",
                "judgments must be ordered A, B and C exactly once");
        }
        if (!validVerdict(judgment.verdict)) {
            addIssue(
                issues,
                ScenarioValidationCode::InvalidJudgment,
                path + ".verdict",
                "must be supported or rejected");
        }
        if (judgment.explanation.empty() || isBlank(judgment.explanation) ||
            !isValidUtf8(judgment.explanation)) {
            addIssue(
                issues,
                ScenarioValidationCode::InvalidJudgment,
                path + ".explanation",
                "must not be blank and must be valid UTF-8");
        }
        if (judgment.evidenceTurnIds.empty()) {
            addIssue(
                issues,
                ScenarioValidationCode::MissingEvidence,
                path + ".evidenceTurnIds",
                "must reference explicit evidence");
        }
        for (const std::string& id : judgment.evidenceTurnIds) {
            if (turnsById.find(id) == turnsById.end()) {
                addIssue(
                    issues,
                    ScenarioValidationCode::InvalidJudgment,
                    path + ".evidenceTurnIds",
                    "contains an unknown turn id");
                continue;
            }
            const bool matchingEvidence =
                std::any_of(draft.evidence.begin(), draft.evidence.end(), [&](const auto& item) {
                    return item.option == judgment.option && item.turnId == id;
                });
            if (!matchingEvidence) {
                addIssue(
                    issues,
                    ScenarioValidationCode::InvalidJudgment,
                    path + ".evidenceTurnIds",
                    "must reference evidence for the same option");
            }
        }
        if (judgment.verdict == OptionVerdict::Supported) {
            ++supportedJudgments;
        }
        if (usableCorrectAnswer && validAnswerLabel(judgment.option)) {
            const OptionVerdict expected = judgment.option == *request.correctAnswer
                                               ? OptionVerdict::Supported
                                               : OptionVerdict::Rejected;
            if (judgment.verdict != expected) {
                addIssue(
                    issues,
                    ScenarioValidationCode::AnswerNotUnique,
                    path + ".verdict",
                    "must support only the declared correct answer");
            }
        }
    }

    if (supportedJudgments != 1) {
        addIssue(
            issues,
            ScenarioValidationCode::AnswerNotUnique,
            "optionJudgments",
            "must contain exactly one supported option");
    }
    if (usableCorrectAnswer) {
        const std::size_t correctIndex = labelIndex(*request.correctAnswer);
        for (std::size_t index = 0; index < 3; ++index) {
            if (index == correctIndex) {
                if (supportEvidence[index] == 0 || rejectionEvidence[index] != 0) {
                    addIssue(
                        issues,
                        ScenarioValidationCode::MissingEvidence,
                        "evidence",
                        "the correct option needs support and no rejection evidence");
                }
            } else if (rejectionEvidence[index] == 0 || supportEvidence[index] != 0) {
                addIssue(
                    issues,
                    ScenarioValidationCode::MissingEvidence,
                    "evidence",
                    "each incorrect option needs rejection and no support evidence");
            }
        }
        if (!validAnswerLabel(draft.supportedAnswer) ||
            draft.supportedAnswer != *request.correctAnswer) {
            addIssue(
                issues,
                ScenarioValidationCode::AnswerNotUnique,
                "supportedAnswer",
                "must equal the request's correct answer");
        }
    }

    if (draft.wordCount != dialogueWordCount(draft)) {
        addIssue(
            issues,
            ScenarioValidationCode::MetadataMismatch,
            "wordCount",
            "must equal the deterministic word count of all turns");
    }
    if (request.targetWordCount.has_value()) {
        const std::size_t target = *request.targetWordCount;
        const std::size_t tolerance = std::max<std::size_t>(20, target / 4);
        const std::size_t lower = target > tolerance ? target - tolerance : 0;
        const std::size_t upper = target + tolerance;
        if (draft.wordCount < lower || draft.wordCount > upper) {
            addIssue(
                issues,
                ScenarioValidationCode::MetadataMismatch,
                "wordCount",
                "must remain within the documented tolerance of targetWordCount (actual " +
                    std::to_string(draft.wordCount) + ", target " +
                    std::to_string(target) + ")");
        }
    }
    if (!draft.requiresTeacherReview) {
        addIssue(
            issues,
            ScenarioValidationCode::MetadataMismatch,
            "requiresTeacherReview",
            "must remain true because local templates cannot prove semantic uniqueness");
    }
    return issues;
}

ScenarioDraft generateLocalScenario(const ScenarioRequest& request) {
    auto requestIssues = validateScenarioRequest(request);
    if (!requestIssues.empty()) {
        throw ScenarioValidationError(std::move(requestIssues));
    }

    DraftBuilder builder(request);
    switch (builder.draft.questionKind) {
    case QuestionKind::When:
        buildWhen(builder);
        break;
    case QuestionKind::Where:
        buildWhere(builder);
        break;
    case QuestionKind::Why:
        buildWhy(builder);
        break;
    case QuestionKind::What:
        buildWhat(builder);
        break;
    case QuestionKind::Unsupported:
        throw std::logic_error("Validated scenario request has an unsupported question kind");
    }

    builder.draft.setting += " | Theme: " + trimCopy(request.topic);
    if (request.difficulty.has_value()) {
        builder.draft.setting += " | Difficulty: " + trimCopy(*request.difficulty);
    }
    builder.appendTargetFillers();
    builder.finishJudgments();
    auto draftIssues = validateScenarioDraft(request, builder.draft);
    if (!draftIssues.empty()) {
        throw ScenarioValidationError(std::move(draftIssues));
    }
    return std::move(builder.draft);
}

ScenarioDraft LocalScenarioProvider::generate(const ScenarioRequest& request) {
    return generateLocalScenario(request);
}

std::string renderDialogueScript(const ScenarioDraft& draft) {
    auto issues = validateScenarioDraft(draft.sourceRequest, draft);
    if (!issues.empty()) {
        throw ScenarioValidationError(std::move(issues));
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
