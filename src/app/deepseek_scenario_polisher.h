#pragma once

#include "core/scenario_generator.h"

#include <QByteArray>
#include <QString>

namespace listening::app {

struct DeepSeekPolishResult {
    ScenarioDraft draft;
    QString model;
    int promptTokens{};
    int completionTokens{};
};

// Optional network adapter. Callers must always create and preserve a valid
// local draft first; this class only polishes unlocked wording and re-runs the
// same deterministic evidence/uniqueness validator before returning.
class DeepSeekScenarioPolisher final {
public:
    [[nodiscard]] static bool credentialAvailable();
    [[nodiscard]] static QString credentialDescription();

    [[nodiscard]] DeepSeekPolishResult polish(const ScenarioRequest& request,
                                               const ScenarioDraft& localDraft) const;

    // Pure response parser exposed for deterministic tests; responseJson is
    // the JSON text contained in choices[0].message.content.
    [[nodiscard]] static ScenarioDraft applyPolishJson(const ScenarioRequest& request,
                                                       const ScenarioDraft& localDraft,
                                                       const QByteArray& responseJson);
};

}  // namespace listening::app
