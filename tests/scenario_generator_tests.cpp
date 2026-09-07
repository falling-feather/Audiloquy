#include "core/project.h"
#include "core/scenario_generator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using listening::AnswerLabel;
using listening::EvidenceRole;
using listening::OptionVerdict;
using listening::QuestionKind;
using listening::ScenarioDraft;
using listening::ScenarioRequest;
using listening::SpeakerGender;

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

template <typename Function>
void expectGenerationFailure(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const listening::ScenarioValidationError&) {
        return;
    } catch (...) {
        fail(std::string(message) + " (wrong exception type)");
    }
    fail(message);
}

ScenarioRequest convenienceStoreRequest() {
    ScenarioRequest request;
    request.questionStem = "When did the woman go to the convenience store?";
    request.options = {
        "In the morning.",
        "In the afternoon.",
        "In the evening.",
    };
    request.correctAnswer = AnswerLabel::B;
    request.topic = "\xe4\xbe\xbf\xe5\x88\xa9\xe5\xba\x97";
    request.difficulty = "A2";
    request.targetWordCount = 90;
    return request;
}

void assertEvidenceRanges(const ScenarioDraft& draft) {
    for (const auto& item : draft.evidence) {
        const auto turn = std::find_if(
            draft.turns.begin(), draft.turns.end(), [&](const auto& candidate) {
                return candidate.id == item.turnId;
            });
        expect(turn != draft.turns.end(), "every evidence span must reference a turn");
        expect(item.byteOffset <= turn->text.size(), "evidence offset must be in range");
        expect(
            item.byteLength <= turn->text.size() - item.byteOffset,
            "evidence length must be in range");
        expect(
            turn->text.substr(item.byteOffset, item.byteLength) == item.quote,
            "evidence byte range must exactly match its quote");
    }
}

void assertStructurallyUnique(const ScenarioRequest& request, const ScenarioDraft& draft) {
    expect(request.correctAnswer.has_value(), "test request must contain an answer");
    int supported = 0;
    std::array<int, 3> supports{};
    std::array<int, 3> rejects{};
    for (const auto& item : draft.evidence) {
        const auto index = static_cast<std::size_t>(item.option);
        if (item.role == EvidenceRole::Supports) {
            ++supports[index];
        } else {
            ++rejects[index];
        }
    }
    for (std::size_t index = 0; index < draft.optionJudgments.size(); ++index) {
        const bool correct = static_cast<std::size_t>(*request.correctAnswer) == index;
        const auto expected = correct ? OptionVerdict::Supported : OptionVerdict::Rejected;
        expect(
            draft.optionJudgments[index].verdict == expected,
            "judgment verdict must match the answer contract");
        supported += draft.optionJudgments[index].verdict == OptionVerdict::Supported ? 1 : 0;
        expect(
            correct ? supports[index] > 0 && rejects[index] == 0
                    : rejects[index] > 0 && supports[index] == 0,
            "correct answer needs support and both distractors need rejection");
    }
    expect(supported == 1, "exactly one option must be structurally supported");
    expect(draft.requiresTeacherReview, "semantic uniqueness must still require teacher review");
}

void convenienceStoreBInAfternoonIsNaturalAndDeterministic() {
    const ScenarioRequest request = convenienceStoreRequest();
    expect(
        listening::validateScenarioRequest(request).empty(),
        "valid convenience-store request must pass input validation");

    const ScenarioDraft first = listening::generateLocalScenario(request);
    const ScenarioDraft second = listening::generateLocalScenario(request);
    expect(first == second, "local generation must be deterministic");
    expect(first.questionKind == QuestionKind::When, "When stem must select the time template");
    expect(first.supportedAnswer == AnswerLabel::B, "B must remain the supported answer");
    expect(first.sourceRequest == request, "draft must retain its exact source request");
    expect(
        listening::validateScenarioDraft(request, first).empty(),
        "generated convenience-store draft must self-validate");

    bool hasMale = false;
    bool hasFemale = false;
    for (std::size_t index = 0; index < first.turns.size(); ++index) {
        hasMale = hasMale || first.turns[index].speaker == SpeakerGender::Male;
        hasFemale = hasFemale || first.turns[index].speaker == SpeakerGender::Female;
        if (index != 0) {
            expect(
                first.turns[index - 1].speaker != first.turns[index].speaker,
                "male and female turns must alternate");
        }
    }
    expect(hasMale && hasFemale, "dialogue must contain one male and one female role");
    assertEvidenceRanges(first);
    assertStructurallyUnique(request, first);

    const bool afternoonEvidence = std::any_of(
        first.evidence.begin(), first.evidence.end(), [](const auto& item) {
            return item.option == AnswerLabel::B && item.role == EvidenceRole::Supports &&
                   item.quote.find("afternoon") != std::string::npos;
        });
    expect(afternoonEvidence, "B=afternoon must have explicit spoken evidence");
    const bool womanPastTenseEvidence = std::any_of(
        first.evidence.begin(), first.evidence.end(), [&](const auto& item) {
            if (item.option != AnswerLabel::B || item.role != EvidenceRole::Supports ||
                item.quote.find("I went in the afternoon") == std::string::npos) {
                return false;
            }
            const auto turn = std::find_if(
                first.turns.begin(), first.turns.end(), [&](const auto& candidate) {
                    return candidate.id == item.turnId;
                });
            return turn != first.turns.end() && turn->speaker == SpeakerGender::Female;
        });
    expect(
        womanPastTenseEvidence,
        "When did the woman... must be answered by the woman using explicit past-tense evidence");

    const std::string script = listening::renderDialogueScript(first);
    expect(script.find("MAN: ") != std::string::npos, "rendered script must label male turns");
    expect(script.find("WOMAN: ") != std::string::npos, "rendered script must label female turns");
    expect(
        script.find("WOMAN: I went in the afternoon") != std::string::npos,
        "rendered script must contain the confirmed afternoon answer");
    expect(
        script.find("the answer is B") == std::string::npos &&
            script.find("Option B") == std::string::npos,
        "dialogue must not leak exam answer labels");
    expect(
        static_cast<std::size_t>(std::count(script.begin(), script.end(), '\n')) ==
            first.turns.size(),
        "rendering must produce exactly one line per turn");
}

void requestedConvenienceStoreExampleHandlesTomorrowNaturally() {
    ScenarioRequest request = convenienceStoreRequest();
    request.options[2] = "Tomorrow.";
    request.topic = "daily errands and convenience-store shopping";
    request.targetWordCount = 55;

    const ScenarioDraft draft = listening::generateLocalScenario(request);
    const std::string script = listening::renderDialogueScript(draft);
    expect(
        draft.title == "A visit to the convenience store",
        "the question stem must identify the convenience-store setting even with a broad topic");
    expect(
        script.find("Did you mean tomorrow?") != std::string::npos &&
            script.find("not tomorrow") != std::string::npos,
        "a future distractor in a past-tense question must be rejected naturally");
    expect(
        script.find("Can I ask you about daily errands and convenience-store shopping?") !=
            std::string::npos,
        "the requested topic must influence the spoken dialogue, not metadata alone");
    expect(
        script.find("didn't go tomorrow") == std::string::npos,
        "the generated past-tense dialogue must not use an ungrammatical future phrase");
    expect(
        listening::validateScenarioDraft(request, draft).empty(),
        "the exact requested convenience-store example must remain structurally valid");
}

void difficultySelectionChangesTheSpokenDraft() {
    ScenarioRequest a2Request = convenienceStoreRequest();
    a2Request.topic = "after-school shopping";
    a2Request.difficulty = "A2";
    const std::string a2Script =
        listening::renderDialogueScript(listening::generateLocalScenario(a2Request));

    ScenarioRequest b2Request = a2Request;
    b2Request.difficulty = "B2";
    const std::string b2Script =
        listening::renderDialogueScript(listening::generateLocalScenario(b2Request));

    expect(
        a2Script.find("Of course. What is it?") != std::string::npos,
        "A2 must select the simple opening expression");
    expect(
        b2Script.find("Certainly. Which detail do you mean?") !=
            std::string::npos,
        "B2 must select the more advanced opening expression");
    expect(a2Script != b2Script, "difficulty selection must change the spoken draft");
}

void everySupportedQuestionKindProducesAValidDraft() {
    const std::array<ScenarioRequest, 3> requests{{
        ScenarioRequest{
            "Where will the speakers meet?",
            {"At the station.", "In the office.", "At the caf\xc3\xa9."},
            AnswerLabel::C,
            "Arranging a project meeting",
            std::nullopt,
            std::nullopt,
        },
        ScenarioRequest{
            "Why does the woman make the call?",
            {"To check the price.", "To make an apology.", "To cancel her order."},
            AnswerLabel::B,
            "A customer making a phone call",
            "B1",
            110,
        },
        ScenarioRequest{
            "What are the speakers mainly talking about?",
            {"A school trip.", "A cooking lesson.", "A football match."},
            AnswerLabel::A,
            "Two classmates discussing their plans",
            std::nullopt,
            std::nullopt,
        },
    }};
    const std::array kinds{QuestionKind::Where, QuestionKind::Why, QuestionKind::What};

    for (std::size_t index = 0; index < requests.size(); ++index) {
        const ScenarioDraft draft = listening::generateLocalScenario(requests[index]);
        expect(draft.questionKind == kinds[index], "stem must choose its anchored template");
        expect(
            listening::validateScenarioDraft(requests[index], draft).empty(),
            "every supported local template must produce a valid draft");
        assertEvidenceRanges(draft);
        assertStructurallyUnique(requests[index], draft);
        if (draft.questionKind == QuestionKind::Why) {
            const auto support = std::find_if(
                draft.evidence.begin(), draft.evidence.end(), [](const auto& item) {
                    return item.option == AnswerLabel::B &&
                           item.role == EvidenceRole::Supports;
                });
            expect(support != draft.evidence.end(), "Why/B draft must have support evidence");
            const auto turn = std::find_if(
                draft.turns.begin(), draft.turns.end(), [&](const auto& candidate) {
                    return candidate.id == support->turnId;
                });
            expect(
                turn != draft.turns.end() && turn->speaker == SpeakerGender::Female,
                "Why does the woman... must be answered by the woman");
        }
    }

    const ScenarioDraft where = listening::generateLocalScenario(requests.front());
    const bool utf8Evidence = std::any_of(
        where.evidence.begin(), where.evidence.end(), [](const auto& item) {
            return item.quote.find("caf\xc3\xa9") != std::string::npos;
        });
    expect(utf8Evidence, "UTF-8 location must survive dialogue and byte evidence mapping");
}

void convenienceStoreEveningAnswerDoesNotContradictItsRejections() {
    ScenarioRequest request = convenienceStoreRequest();
    request.correctAnswer = AnswerLabel::C;
    const ScenarioDraft draft = listening::generateLocalScenario(request);
    const std::string script = listening::renderDialogueScript(draft);
    expect(
        script.find("WOMAN: I went in the evening") != std::string::npos,
        "C=evening must be explicitly spoken as the final visit time");
    expect(
        script.find("in the afternoon; the store was already closed") == std::string::npos,
        "template must not claim the store closed in the afternoon before accepting evening");
    expect(
        listening::validateScenarioDraft(request, draft).empty(),
        "evening-answer convenience-store draft must remain structurally unique");
}

void providerSeamUsesTheSameValidatedContract() {
    const ScenarioRequest request = convenienceStoreRequest();
    std::unique_ptr<listening::ITextGenerationProvider> provider =
        std::make_unique<listening::LocalScenarioProvider>();
    const ScenarioDraft throughProvider = provider->generate(request);
    expect(
        throughProvider == listening::generateLocalScenario(request),
        "provider seam must preserve deterministic local semantics");
}

void invalidRequestsNeverProduceDrafts() {
    ScenarioRequest request = convenienceStoreRequest();

    request.options[2] = "  in  THE   afternoon! ";
    expect(
        !listening::validateScenarioRequest(request).empty(),
        "normalized duplicate options must be rejected");
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "duplicate options must not produce a draft");

    request = convenienceStoreRequest();
    request.options[0].clear();
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "missing option must not produce a draft");

    request = convenienceStoreRequest();
    request.options[0] = "...";
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "punctuation-only option must not produce an empty spoken value");

    request = convenienceStoreRequest();
    request.correctAnswer.reset();
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "missing correct answer must not produce a draft");

    request = convenienceStoreRequest();
    request.correctAnswer = static_cast<AnswerLabel>(99);
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "invalid answer enum must not produce a draft");

    request = convenienceStoreRequest();
    request.questionStem = "Tell me when they will go.";
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "question classification must be anchored to the first token");

    request = convenienceStoreRequest();
    request.questionStem = "How will they travel?";
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "unsupported question kind must fail closed");

    request = convenienceStoreRequest();
    request.questionStem = "When123 will they go?";
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "question word must end at a token boundary");

    request = convenienceStoreRequest();
    request.topic = "bad\ntopic";
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "line breaks in user data must not forge dialogue lines");

    request = convenienceStoreRequest();
    request.topic = std::string("\xc3\x28", 2);
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "invalid UTF-8 must not produce a draft");

    request = convenienceStoreRequest();
    request.targetWordCount = 49;
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "too-small word target must be rejected");

    request = convenienceStoreRequest();
    request.targetWordCount = 241;
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "too-large word target must be rejected");

    request = convenienceStoreRequest();
    request.options = {"The office.", "At the office.", "At home."};
    expectGenerationFailure(
        [&] { (void)listening::generateLocalScenario(request); },
        "an option that contains another must fail closed as ambiguous");
}

void draftValidationDetectsTampering() {
    const ScenarioRequest request = convenienceStoreRequest();
    const ScenarioDraft original = listening::generateLocalScenario(request);

    ScenarioDraft changed = original;
    changed.sourceRequest.topic = "changed after generation";
    expect(
        !listening::validateScenarioDraft(request, changed).empty(),
        "source request mismatch must be detected");

    changed = original;
    changed.evidence.front().byteOffset = std::numeric_limits<std::size_t>::max();
    expect(
        !listening::validateScenarioDraft(request, changed).empty(),
        "overflowing evidence offset must be rejected safely");

    changed = original;
    changed.evidence.front().quote = "not the referenced text";
    expect(
        !listening::validateScenarioDraft(request, changed).empty(),
        "mismatched evidence quote must be rejected");

    changed = original;
    auto support = std::find_if(
        changed.evidence.begin(), changed.evidence.end(), [](const auto& item) {
            return item.option == AnswerLabel::B && item.role == EvidenceRole::Supports;
        });
    expect(support != changed.evidence.end(), "golden draft must contain B support evidence");
    const auto supportTurn = std::find_if(
        changed.turns.begin(), changed.turns.end(), [&](const auto& turn) {
            return turn.id == support->turnId;
        });
    expect(supportTurn != changed.turns.end(), "support evidence turn must exist");
    support->quote = "I went";
    support->byteOffset = supportTurn->text.find(support->quote);
    expect(
        support->byteOffset != std::string::npos,
        "tamper test must select exact but option-irrelevant text from the support turn");
    support->byteLength = support->quote.size();
    expect(
        !listening::validateScenarioDraft(request, changed).empty(),
        "exact but option-irrelevant evidence must be rejected");

    changed = original;
    changed.turns[1].id = changed.turns[0].id;
    expect(
        !listening::validateScenarioDraft(request, changed).empty(),
        "duplicate turn ids must be rejected");

    changed = original;
    changed.turns[1].speaker = changed.turns[0].speaker;
    expect(
        !listening::validateScenarioDraft(request, changed).empty(),
        "adjacent identical speakers must be rejected");

    changed = original;
    changed.turns.front().pauseAfterMs = -1;
    expect(
        !listening::validateScenarioDraft(request, changed).empty(),
        "negative pause must be rejected");

    changed = original;
    changed.optionJudgments[0].verdict = OptionVerdict::Supported;
    expect(
        !listening::validateScenarioDraft(request, changed).empty(),
        "a second supported option must be rejected");

    changed = original;
    ++changed.wordCount;
    expect(
        !listening::validateScenarioDraft(request, changed).empty(),
        "tampered word-count metadata must be rejected");

    changed = original;
    changed.requiresTeacherReview = false;
    expect(
        !listening::validateScenarioDraft(request, changed).empty(),
        "local semantic uniqueness must continue to require teacher review");
}

void wordTargetAndRenderingBoundariesAreClear() {
    ScenarioRequest request = convenienceStoreRequest();
    request.targetWordCount = 240;
    const ScenarioDraft expanded = listening::generateLocalScenario(request);
    expect(expanded.wordCount >= 232, "large word target should add deterministic neutral context");
    expect(
        expanded.wordCount == [&] {
            std::size_t total = 0;
            for (const auto& turn : expanded.turns) {
                total += listening::countReadableWords(turn.text);
            }
            return total;
        }(),
        "word-count metadata must be recomputable from final dialogue");

    request.targetWordCount = 50;
    const ScenarioDraft compact = listening::generateLocalScenario(request);
    expect(
        compact.wordCount >= 30 && compact.wordCount <= 70,
        "small valid target must select a compact template within tolerance");

    ScenarioRequest differentTheme = request;
    differentTheme.topic = "A railway-station errand";
    const ScenarioDraft themed = listening::generateLocalScenario(differentTheme);
    expect(
        themed.setting != compact.setting &&
            themed.setting.find("A railway-station errand") != std::string::npos,
        "topic must remain visible in generated scene metadata");

    ScenarioDraft invalid = expanded;
    invalid.turns.front().text = "bad\nline";
    try {
        (void)listening::renderDialogueScript(invalid);
        fail("renderer must reject embedded line breaks");
    } catch (const std::invalid_argument&) {
    }
}

}  // namespace

int main() {
    try {
        convenienceStoreBInAfternoonIsNaturalAndDeterministic();
        requestedConvenienceStoreExampleHandlesTomorrowNaturally();
        difficultySelectionChangesTheSpokenDraft();
        everySupportedQuestionKindProducesAValidDraft();
        convenienceStoreEveningAnswerDoesNotContradictItsRejections();
        providerSeamUsesTheSameValidatedContract();
        invalidRequestsNeverProduceDrafts();
        draftValidationDetectsTampering();
        wordTargetAndRenderingBoundariesAreClear();
        std::cout << "All scenario generator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Scenario generator test failure: " << error.what() << '\n';
        return 1;
    }
}
