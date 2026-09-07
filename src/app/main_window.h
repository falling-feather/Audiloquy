#pragma once

#include "app/local_voice_pack.h"
#include "app/render_job.h"
#include "core/editor_history.h"
#include "core/project.h"
#include "core/project_storage.h"
#include "core/recovery_store.h"
#include "platform/windows/windows_audio.h"

#include <QMainWindow>

#include <filesystem>
#include <exception>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QCloseEvent;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTextEdit;
class QTimer;
class QWidget;
class QEvent;
class QObject;

namespace listening::app {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    enum class DisplayMode {
        Simple,
        Professional,
    };

    enum class RenderIntent {
        None,
        Selected,
        All,
        PreviewMale,
        PreviewFemale,
    };

    void buildUi();
    void connectUi();
    void applyTheme();

    void loadExampleProject(bool askBeforeDiscard = true);
    void newBlankProject(bool askBeforeDiscard = true);
    void addSegment();
    void duplicateSegment();
    void deleteSegment();
    void moveSegment(int delta);
    void undoEdit();
    void redoEdit();
    void recordStructuralEdit(const Project& before,
                              const std::string& beforeSelection = {});
    void scheduleHistoryBaseline();
    void commitListOrderFromUi();
    void applyHistoryProject(Project project,
                             std::string selectedSegmentId,
                             const QString& status);
    void checkRecoveryOnStartup();
    [[nodiscard]] bool autosaveRecovery();
    void clearRecovery();
    void packageProjectToDisk();
    void setEditorWidgetsEnabled(bool enabled);
    void refreshProjectUi();
    void refreshSegmentList();
    void loadCurrentSegment();
    void commitProjectSettings();
    void commitCurrentSegment();
    void updateSelectedListItem();
    void updateMetrics();
    void updateAudioAvailability();
    void invalidateAllRenderedAudio();
    void invalidateCurrentRenderedAudio();
    void restoreRenderedAudio();
    void refreshVoiceOptions();
    void startRender(RenderScope scope);
    void cancelRender();
    void startVoicePreview(platform::windows::VoiceGender gender);
    void handleRenderProgress(const RenderJobProgress& progress);
    void handleRenderFinished(const RenderJobResult& result);
    [[nodiscard]] std::string selectedVoiceToken(
        platform::windows::VoiceGender gender) const;
    void setDisplayMode(DisplayMode mode);
    void activateWorkflowStep(int step);
    void updatePageContext();
    void openScenarioGenerator();
    void setDirty(bool dirty = true);

    [[nodiscard]] Segment* currentSegment() noexcept;
    [[nodiscard]] const Segment* currentSegment() const noexcept;
    [[nodiscard]] bool validateForAction(const QString& action);
    [[nodiscard]] bool confirmDiscardChanges();
    [[nodiscard]] bool saveProjectToDisk(bool forceChoosePath);
    void openProjectFromDisk();

    [[nodiscard]] std::filesystem::path outputDirectory() const;

    void playSelected(bool loop);
    void playAll();
    void togglePlaybackPause();
    void seekPlaybackFromSlider();
    void pollPlayback();
    void stopPlayback();
    void moveSelection(int delta);

    void beginBusy(const QString& message);
    void endBusy();
    void showStatus(const QString& message, bool error = false);
    void showFailure(const QString& title, const std::exception& error);

    Project project_;
    std::filesystem::path projectPath_;
    std::string currentSegmentId_;
    std::unordered_map<std::string, std::filesystem::path> segmentWavs_;
    std::filesystem::path programWav_;
    LocalVoicePackManager localVoicePacks_;
    platform::windows::WavPlayer player_;
    RenderJobController renderJob_;
    DisplayMode displayMode_{DisplayMode::Simple};
    int workflowStep_{0};
    bool loadingUi_{false};
    bool dirty_{false};
    bool busy_{false};
    bool playbackScrubbing_{false};
    bool recoveryEnabled_{false};
    bool applyingHistory_{false};
    bool closePending_{false};
    RenderIntent renderIntent_{RenderIntent::None};
    std::optional<Project> dragBeforeProject_;
    std::string dragBeforeSelection_;
    std::vector<platform::windows::VoiceInfo> installedVoices_;
    editor::ProjectHistory history_;
    recovery::Store recoveryStore_;

    QFrame* topBar_{};
    QLineEdit* projectTitleEdit_{};
    QPushButton* simpleModeButton_{};
    QPushButton* professionalModeButton_{};
    QPushButton* openButton_{};
    QPushButton* saveButton_{};
    QPushButton* minimizeButton_{};
    QPushButton* maximizeButton_{};
    QPushButton* closeButton_{};
    QPushButton* exampleButton_{};
    QPushButton* newProjectButton_{};
    QPushButton* addSegmentButton_{};
    QPushButton* duplicateSegmentButton_{};
    QPushButton* deleteSegmentButton_{};
    QPushButton* moveUpButton_{};
    QPushButton* moveDownButton_{};
    QPushButton* undoButton_{};
    QPushButton* redoButton_{};
    QPushButton* manuscriptStepButton_{};
    QPushButton* voiceStepButton_{};
    QPushButton* rhythmStepButton_{};
    QPushButton* generateStepButton_{};
    QPushButton* classroomStepButton_{};
    QListWidget* segmentList_{};
    QLabel* groupCountLabel_{};
    QPushButton* generateSelectedButton_{};
    QPushButton* generateAllButton_{};

    QLabel* pageEyebrowLabel_{};
    QLabel* pageTitleLabel_{};
    QLabel* pageDescriptionLabel_{};
    QLabel* currentGroupBadge_{};
    QScrollArea* workspaceScroll_{};
    QScrollArea* inspectorScroll_{};
    QFrame* scenarioGeneratorCard_{};
    QPushButton* smartScenarioButton_{};
    QFrame* editorCard_{};
    QFrame* globalSettingsCard_{};
    QFrame* outputCard_{};
    QFrame* playerBar_{};
    QComboBox* accentCombo_{};
    QComboBox* maleVoiceCombo_{};
    QComboBox* femaleVoiceCombo_{};
    QPushButton* previewMaleVoiceButton_{};
    QPushButton* previewFemaleVoiceButton_{};
    QCheckBox* strictAccentCheck_{};
    QCheckBox* allowGenderFallbackCheck_{};
    QLabel* voiceAuditLabel_{};
    QSpinBox* wpmSpin_{};
    QSlider* wpmSlider_{};
    QLabel* totalDurationLabel_{};

    QSpinBox* questionStartSpin_{};
    QSpinBox* questionEndSpin_{};
    QComboBox* speakerCombo_{};
    QTextEdit* scriptEdit_{};
    QLabel* wordCountLabel_{};
    QDoubleSpinBox* pauseSpin_{};
    QSpinBox* repeatSpin_{};
    QLabel* segmentDurationLabel_{};
    QFrame* professionalCard_{};
    QLabel* segmentIdLabel_{};
    QLabel* pcmFormatLabel_{};
    QLabel* voiceHintLabel_{};
    QLabel* generationAuditLabel_{};
    QCheckBox* teacherReviewedCheck_{};
    QLabel* outputPathLabel_{};
    QPushButton* packageButton_{};
    QPushButton* cancelRenderButton_{};

    QLabel* playbackTitleLabel_{};
    QLabel* playbackDetailLabel_{};
    QPushButton* previousButton_{};
    QPushButton* playSelectedButton_{};
    QPushButton* loopSelectedButton_{};
    QPushButton* playAllButton_{};
    QPushButton* pauseButton_{};
    QPushButton* stopButton_{};
    QPushButton* nextButton_{};
    QSlider* playbackSlider_{};
    QLabel* playbackTimeLabel_{};
    QTimer* playbackTimer_{};
    QLabel* statusLabel_{};
    QTimer* recoveryTimer_{};
    QTimer* historyTimer_{};
};

}  // namespace listening::app
