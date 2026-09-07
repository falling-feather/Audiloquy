#include "app/deepseek_scenario_polisher.h"
#include "core/scenario_generator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

namespace {

QJsonArray evidenceJson(const listening::ScenarioDraft& draft) {
    QJsonArray evidence;
    for (const auto& item : draft.evidence) {
        evidence.append(QJsonObject{
            {QStringLiteral("option"),
             QString::fromLatin1(listening::answerLabelCode(item.option).data(), 1)},
            {QStringLiteral("role"),
             item.role == listening::EvidenceRole::Supports ? QStringLiteral("supports")
                                                             : QStringLiteral("rejects")},
            {QStringLiteral("turn_id"), QString::fromStdString(item.turnId)},
            {QStringLiteral("quote"), QString::fromStdString(item.quote)},
        });
    }
    return evidence;
}

}  // namespace

int main() {
    using namespace listening;
    using listening::app::DeepSeekScenarioPolisher;

    ScenarioRequest request;
    request.questionStem = "When did the woman go to the convenience store?";
    request.options = {"In the morning.", "In the afternoon.", "Tomorrow."};
    request.correctAnswer = AnswerLabel::B;
    request.topic = "daily errands and convenience-store shopping";
    request.difficulty = "B1";
    request.targetWordCount = 55;
    const ScenarioDraft local = generateLocalScenario(request);

    QJsonArray turns;
    for (const auto& turn : local.turns) {
        turns.append(QJsonObject{{QStringLiteral("id"), QString::fromStdString(turn.id)},
                                 {QStringLiteral("text"), QString::fromStdString(turn.text)}});
    }
    const QByteArray valid =
        QJsonDocument(QJsonObject{{QStringLiteral("turns"), turns},
                                  {QStringLiteral("evidence"), evidenceJson(local)}})
            .toJson(QJsonDocument::Compact);
    try {
        const ScenarioDraft unchanged =
            DeepSeekScenarioPolisher::applyPolishJson(request, local, valid);
        if (unchanged != local) {
            std::cerr << "Identity polish changed a validated draft\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "Valid polish response was rejected: " << error.what() << '\n';
        return 2;
    }

    QJsonArray naturalTurns = turns;
    const std::string evidenceTurnId = local.evidence.front().turnId;
    for (qsizetype index = 0; index < turns.size(); ++index) {
        QJsonObject turn = naturalTurns[index].toObject();
        if (turn.value(QStringLiteral("id")).toString().toStdString() == evidenceTurnId) {
            turn.insert(QStringLiteral("text"),
                        QStringLiteral("Well, ") + turn.value(QStringLiteral("text")).toString());
            naturalTurns[index] = turn;
            break;
        }
    }
    const QByteArray naturalizedEvidence =
        QJsonDocument(QJsonObject{{QStringLiteral("turns"), naturalTurns},
                                  {QStringLiteral("evidence"), evidenceJson(local)}})
            .toJson(QJsonDocument::Compact);
    try {
        const ScenarioDraft naturalized =
            DeepSeekScenarioPolisher::applyPolishJson(request, local, naturalizedEvidence);
        if (naturalized == local || naturalized.evidence.front().byteOffset <=
                                        local.evidence.front().byteOffset) {
            std::cerr << "Naturalized evidence was not re-anchored\n";
            return 3;
        }
    } catch (const std::exception& error) {
        std::cerr << "Safe evidence-turn naturalization was rejected: " << error.what() << '\n';
        return 5;
    }

    QJsonArray badEvidence = evidenceJson(local);
    QJsonObject firstEvidence = badEvidence.first().toObject();
    firstEvidence.insert(QStringLiteral("quote"), QStringLiteral("some unrelated fact"));
    badEvidence[0] = firstEvidence;
    const QByteArray factChanged =
        QJsonDocument(QJsonObject{{QStringLiteral("turns"), turns},
                                  {QStringLiteral("evidence"), badEvidence}})
            .toJson(QJsonDocument::Compact);
    try {
        (void)DeepSeekScenarioPolisher::applyPolishJson(request, local, factChanged);
        std::cerr << "Evidence without the canonical option fact was accepted\n";
        return 6;
    } catch (const std::exception&) {
    }

    const QByteArray malformed = QByteArrayLiteral("{\"turns\":[]}");
    try {
        (void)DeepSeekScenarioPolisher::applyPolishJson(request, local, malformed);
        std::cerr << "Wrong turn count was accepted\n";
        return 4;
    } catch (const std::exception&) {
    }

    std::cout << "deepseek_scenario_tests passed\n";
    return 0;
}
