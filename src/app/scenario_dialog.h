#pragma once

#include "core/multi_question.h"
#include "core/project.h"
#include "core/scenario_generator.h"

#include <QDialog>

#include <optional>

class QComboBox;
class QEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTextBrowser;
class QObject;

namespace listening::app {

class ScenarioDialog final : public QDialog {
public:
    explicit ScenarioDialog(bool replacesExistingText, QWidget* parent = nullptr);

    [[nodiscard]] const std::optional<ScenarioDraft>& adoptedDraft() const noexcept;
    [[nodiscard]] std::optional<GenerationRecord> adoptedGeneration() const;
    [[nodiscard]] std::string adoptedScript() const;
    [[nodiscard]] std::string adoptedProvider() const;
    [[nodiscard]] std::string adoptedModel() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void buildUi();
    void connectUi();
    void populateExample();
    void saveCurrentQuestion();
    void loadQuestion(std::size_t index);
    void refreshQuestionList();
    void addQuestion();
    void removeQuestion();
    void generateDraft();
    [[nodiscard]] ScenarioRequest currentRequest() const;
    void adoptDraft();
    void clearResult();
    void refreshPreview();
    void showStatus(const QString& message, bool error = false);

    QLineEdit* questionStemEdit_{};
    QFrame* headerFrame_{};
    QLineEdit* optionAEdit_{};
    QLineEdit* optionBEdit_{};
    QLineEdit* optionCEdit_{};
    QComboBox* answerCombo_{};
    QLineEdit* topicEdit_{};
    QComboBox* difficultyCombo_{};
    QSpinBox* targetWordsSpin_{};
    QPushButton* exampleButton_{};
    QListWidget* questionList_{};
    QPushButton* addQuestionButton_{};
    QPushButton* removeQuestionButton_{};
    QPushButton* generateButton_{};
    QPushButton* localProviderButton_{};
    QPushButton* deepSeekProviderButton_{};
    QPushButton* closeButton_{};
    QListWidget* previewList_{};
    QTextBrowser* evidenceBrowser_{};
    QLabel* statusLabel_{};
    QLabel* generationSourceLabel_{};
    QPushButton* adoptButton_{};
    QPushButton* cancelButton_{};

    std::optional<ScenarioDraft> generatedDraft_;
    std::optional<ScenarioDraft> localDraft_;
    std::optional<ScenarioDraft> adoptedDraft_;
    std::optional<MultiQuestionDraft> generatedMultiDraft_;
    std::optional<GenerationRecord> adoptedGeneration_;
    std::string adoptedScript_;
    std::vector<ScenarioRequest> questionRequests_;
    std::size_t activeQuestionIndex_{};
    bool replacesExistingText_{false};
    bool usedDeepSeek_{false};
    QString deepSeekModel_;
    int deepSeekPromptTokens_{};
    int deepSeekCompletionTokens_{};
};

}  // namespace listening::app
