#include "app/deepseek_scenario_polisher.h"
#include "core/scenario_generator.h"

#include <QCoreApplication>

#include <iostream>

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    listening::ScenarioRequest request;
    request.questionStem = "When did the woman go to the convenience store?";
    request.options = {"In the morning.", "In the afternoon.", "Tomorrow."};
    request.correctAnswer = listening::AnswerLabel::B;
    request.topic = "daily errands and convenience-store shopping";
    request.difficulty = "B1";
    request.targetWordCount = 55;
    try {
        const listening::ScenarioDraft local = listening::generateLocalScenario(request);
        const auto result =
            listening::app::DeepSeekScenarioPolisher().polish(request, local);
        if (!listening::validateScenarioDraft(request, result.draft).empty()) {
            std::cerr << "Validated live response became invalid\n";
            return 2;
        }
        std::cout << "DeepSeek live smoke passed; model="
                  << result.model.toStdString() << "; promptTokens=" << result.promptTokens
                  << "; completionTokens=" << result.completionTokens << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DeepSeek live smoke failed: " << error.what() << '\n';
        return 1;
    }
}
