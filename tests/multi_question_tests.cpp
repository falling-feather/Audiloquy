#include "core/multi_question.h"

#include "core/project.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using listening::AnswerLabel;
using listening::MultiQuestionDraft;
using listening::MultiQuestionRequest;
using listening::ScenarioRequest;

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

template <typename Function>
void expectFailure(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const listening::ScenarioValidationError&) {
        return;
    } catch (...) {
        fail(std::string(message) + " (wrong exception type)");
    }
    fail(message);
}

ScenarioRequest whenQuestion() {
    ScenarioRequest request;
    request.questionStem = "When did the woman leave the school?";
    request.options = {"In the morning.", "In the afternoon.", "Tomorrow."};
    request.correctAnswer = AnswerLabel::B;
    return request;
}

ScenarioRequest whereQuestion() {
    ScenarioRequest request;
    request.questionStem = "Where did the woman leave her bag?";
    request.options = {"At the station.", "In the classroom.", "At the cafe."};
    request.correctAnswer = AnswerLabel::B;
    return request;
}

MultiQuestionRequest request() {
    MultiQuestionRequest result;
    result.questions = {whenQuestion(), whereQuestion()};
    result.topic = "an after-school trip";
    result.difficulty = "B1";
    result.targetWordCount = 90;
    return result;
}

void sharedPassageIsDeterministicAndAnchored() {
    const MultiQuestionRequest source = request();
    expect(listening::validateMultiQuestionRequest(source).empty(),
           "valid multi-question request must pass validation");
    const MultiQuestionDraft first = listening::generateLocalMultiQuestion(source);
    const MultiQuestionDraft second = listening::generateLocalMultiQuestion(source);
    expect(first == second, "local multi-question generation must be deterministic");
    expect(first.questions.size() == 2, "one result must be returned for each question");
    expect(first.turns.size() < 16, "questions must share a compact passage");
    const std::string script = listening::renderMultiDialogueScript(first);
    expect(script.find("Let's sort out the details of an after-school trip.") != std::string::npos,
           "shared topic must appear in one common opening");
    expect(std::count(script.begin(), script.end(), '\n') ==
               static_cast<std::ptrdiff_t>(first.turns.size()),
           "shared script must contain one line per turn");
    for (const auto& item : first.questions) {
        expect(!item.evidence.empty(), "every question must have explicit evidence");
        expect(item.optionJudgments[static_cast<std::size_t>(*item.sourceRequest.correctAnswer)].verdict ==
                   listening::OptionVerdict::Supported,
               "every question must retain its declared answer");
    }
    expect(listening::validateMultiQuestionDraft(source, first).empty(),
           "generated shared passage must validate");
}

void duplicateAndContradictoryFactsAreRejected() {
    MultiQuestionRequest duplicate = request();
    duplicate.questions[1].questionStem = duplicate.questions[0].questionStem;
    duplicate.questions[1].correctAnswer = AnswerLabel::A;
    expect(!listening::validateMultiQuestionRequest(duplicate).empty(),
           "repeated question stems with different answers must be rejected");
    expectFailure([&] { (void)listening::generateLocalMultiQuestion(duplicate); },
                  "repeated question stems must not generate a passage");

    MultiQuestionRequest sharedTime = request();
    sharedTime.questions[1].questionStem = "When did the man leave the school?";
    sharedTime.questions[1].options = sharedTime.questions[0].options;
    sharedTime.questions[1].correctAnswer = AnswerLabel::B;
    expect(listening::validateMultiQuestionRequest(sharedTime).empty(),
           "different people may legitimately share the same time choices");
    expect(listening::validateMultiQuestionDraft(
               sharedTime, listening::generateLocalMultiQuestion(sharedTime)).empty(),
           "shared time choices must still produce a valid shared passage");
}

void evidenceCanBeRevalidatedAfterEditing() {
    const MultiQuestionDraft draft = listening::generateLocalMultiQuestion(request());
    listening::GenerationRecord record;
    record.provider = "local-template";
    record.questionStem = draft.questions.front().sourceRequest.questionStem;
    record.options = draft.questions.front().sourceRequest.options;
    record.correctAnswer = "B";
    const auto& evidence = draft.questions.front().evidence.front();
    const auto support = std::find_if(
        draft.questions.front().evidence.begin(), draft.questions.front().evidence.end(),
        [](const auto& item) {
            return item.option == AnswerLabel::B &&
                   item.role == listening::EvidenceRole::Supports;
        });
    expect(support != draft.questions.front().evidence.end(),
           "generated first question must have support evidence");
    record.evidence.push_back(listening::GenerationEvidence{
        "A", "rejects", evidence.turnId, evidence.quote});
    record.evidence.push_back(listening::GenerationEvidence{
        "B", "supports", support->turnId, support->quote});
    const auto additionalSupport = std::find_if(
        draft.questions[1].evidence.begin(), draft.questions[1].evidence.end(),
        [](const auto& item) {
            return item.option == AnswerLabel::B &&
                   item.role == listening::EvidenceRole::Supports;
        });
    expect(additionalSupport != draft.questions[1].evidence.end(),
           "generated second question must have support evidence");
    record.additionalQuestions.push_back(listening::GenerationQuestion{
        draft.questions[1].sourceRequest.questionStem,
        draft.questions[1].sourceRequest.options,
        "B",
        {listening::GenerationEvidence{"B", "supports", additionalSupport->turnId,
                                       additionalSupport->quote}},
    });

    const std::string edited = listening::renderMultiDialogueScript(draft);
    listening::revalidateGenerationEvidence(record, edited);
    expect(record.teacherReviewed == false && record.requiresTeacherReview,
           "editing must require teacher review again");
    expect(!record.evidence.front().quote.empty(), "evidence still present in edited text remains");
    record.evidence.front().quote = "a sentence that is no longer present";
    listening::revalidateGenerationEvidence(record, edited);
    expect(record.evidence.front().quote.empty() && record.evidence.front().turnId.empty(),
           "missing evidence must be cleared rather than silently trusted");
}

}  // namespace

int main() {
    try {
        auto actors = request();
        actors.questions[0].questionStem = "What did the man write on the sheet?";
        actors.questions[0].options = {"An address.", "His name.", "A number."};
        const auto actorDraft = listening::generateLocalMultiQuestion(actors);
        const auto& support = *std::find_if(actorDraft.questions.front().evidence.begin(), actorDraft.questions.front().evidence.end(), [](const auto& e) { return e.role == listening::EvidenceRole::Supports; });
        const auto turn = std::find_if(actorDraft.turns.begin(), actorDraft.turns.end(), [&](const auto& t) { return t.id == support.turnId; });
        expect(turn != actorDraft.turns.end() && turn->speaker == listening::SpeakerGender::Male,
               "sheet must not be mistaken for the subject pronoun she");
        sharedPassageIsDeterministicAndAnchored();
        duplicateAndContradictoryFactsAreRejected();
        evidenceCanBeRevalidatedAfterEditing();
        std::cout << "All multi-question tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Multi-question test failure: " << error.what() << '\n';
        return 1;
    }
}
