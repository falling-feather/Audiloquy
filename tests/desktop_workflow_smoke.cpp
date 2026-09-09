#include "desktop_workflow_smoke.h"

#include "app/main_window.h"
#include "core/project_storage.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

namespace listening::app {
namespace {

std::filesystem::path nativePath(const QString& value) {
#ifdef _WIN32
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toUtf8().constData());
#endif
}

QFileDialog* visibleFileDialog() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<QFileDialog*>(widget);
        if (dialog != nullptr && dialog->isVisible()) {
            return dialog;
        }
    }
    return nullptr;
}

struct WorkflowState final {
    QApplication& application;
    MainWindow& window;
    QTemporaryDir temporaryDirectory;
    QTimer timer;
    QElapsedTimer elapsed;
    int phase{};
    int attempts{};
    QString savePath;
    QString movedProjectPath;
    QString generatedAudioPath;
    std::filesystem::path packageDirectory;
    std::filesystem::path movedDirectory;
    std::string failure;

    WorkflowState(QApplication& app, MainWindow& mainWindow)
        : application(app),
          window(mainWindow),
          temporaryDirectory(QDir::tempPath() + QStringLiteral("/语澜-desktop-workflow-XXXXXX")) {
        timer.setInterval(20);
    }

    void fail(int exitCode, std::string message) {
        if (!failure.empty()) {
            return;
        }
        failure = std::move(message);
        std::cerr << "[desktop-workflow-smoke] phase " << phase << ", exit " << exitCode
                  << ": " << failure << '\n';
        timer.stop();
        application.exit(exitCode);
    }

    void pass() {
        timer.stop();
        application.exit(0);
    }

    template <typename T>
    T* find(const char* objectName) {
        return window.findChild<T*>(QString::fromLatin1(objectName));
    }

    QPushButton* button(const char* objectName, const QString& textPart) {
        if (auto* named = find<QPushButton>(objectName); named != nullptr) {
            return named;
        }
        for (QPushButton* candidate : window.findChildren<QPushButton*>()) {
            if (candidate->text().trimmed() == textPart) {
                return candidate;
            }
        }
        for (QPushButton* candidate : window.findChildren<QPushButton*>()) {
            if (candidate->text().contains(textPart)) {
                return candidate;
            }
        }
        return nullptr;
    }

    bool timedOut() {
        if (elapsed.elapsed() <= 38000) {
            return false;
        }
        fail(79, "workflow exceeded the 38-second smoke-test deadline");
        return true;
    }

    void selectDialogFile(const QString& path, int nextPhase) {
        QFileDialog* dialog = visibleFileDialog();
        if (dialog == nullptr) {
            if (++attempts > 150) {
                fail(61, "save/open file dialog did not become visible");
            }
            return;
        }
        attempts = 0;
        const QFileInfo selectedFile(path);
        dialog->setDirectory(selectedFile.absolutePath());
        dialog->selectFile(selectedFile.fileName());
        phase = nextPhase;
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
    }

    void tick() {
        if (timedOut()) {
            return;
        }

        try {
            switch (phase) {
            case 0: {
                auto* title = find<QLineEdit>("projectTitleEdit");
                auto* script = find<QTextEdit>("scriptEdit");
                auto* save = button("saveButton", QStringLiteral("保存"));
                if (title == nullptr || script == nullptr || save == nullptr) {
                    fail(51, "required editor controls are missing");
                    return;
                }
                if (!temporaryDirectory.isValid()) {
                    fail(52, "cannot create the temporary Chinese workflow directory");
                    return;
                }
                const QDir root(temporaryDirectory.path());
                if (!root.mkpath(QStringLiteral("源工程")) ||
                    !root.mkpath(QStringLiteral("打包目标"))) {
                    fail(53, "cannot create workflow subdirectories");
                    return;
                }
                savePath = root.filePath(QStringLiteral("源工程/课堂流程.json"));
                title->setText(QStringLiteral("桌面流程验收"));
                script->setPlainText(QStringLiteral(
                    "Please listen carefully. The science museum opens at ten o'clock.\n"
                    "You have five seconds to choose the best answer."));
                application.processEvents();
                phase = 1;
                attempts = 0;
                QTimer::singleShot(0, save, [save] { save->click(); });
                return;
            }
            case 1:
                selectDialogFile(savePath, 2);
                return;
            case 2: {
                if (!QFileInfo::exists(savePath)) {
                    if (++attempts > 150) {
                        fail(62, "project JSON was not saved");
                    }
                    return;
                }
                attempts = 0;
                auto* open = button("openButton", QStringLiteral("打开"));
                if (open == nullptr) {
                    fail(54, "open button is missing after save");
                    return;
                }
                phase = 3;
                QTimer::singleShot(0, open, [open] { open->click(); });
                return;
            }
            case 3:
                selectDialogFile(savePath, 4);
                return;
            case 4: {
                auto* script = find<QTextEdit>("scriptEdit");
                if (visibleFileDialog() != nullptr || script == nullptr ||
                    !script->toPlainText().contains(QStringLiteral("science museum"))) {
                    if (++attempts > 150) {
                        fail(63, "reopened project did not restore the manuscript");
                    }
                    return;
                }
                attempts = 0;
                auto* generate = find<QPushButton>("generateSelectedButton");
                if (generate == nullptr) {
                    fail(55, "selected-generation button is missing");
                    return;
                }
                phase = 5;
                generate->click();
                auto* maleVoice = find<QComboBox>("maleVoiceCombo");
                auto* save = button("saveButton", QStringLiteral("保存"));
                auto* open = button("openButton", QStringLiteral("打开"));
                if (maleVoice == nullptr || save == nullptr || open == nullptr ||
                    maleVoice->isEnabled() || save->isEnabled() || open->isEnabled()) {
                    fail(58, "generation did not immediately disable editor and file controls");
                    return;
                }
                return;
            }
            case 5: {
                auto* output = find<QLabel>("outputPathLabel");
                auto* generate = find<QPushButton>("generateSelectedButton");
                if (output == nullptr || generate == nullptr ||
                    !QFileInfo::exists(output->text()) || !generate->isEnabled()) {
                    if (++attempts > 1500) {
                        fail(64, "selected audio was not generated within the deadline");
                    }
                    return;
                }
                attempts = 0;
                if (auto* stop = find<QPushButton>("stopPlaybackButton"); stop != nullptr) {
                    stop->click();
                }
                generatedAudioPath = output->text();
                auto* save = button("saveButton", QStringLiteral("保存"));
                if (save == nullptr) {
                    fail(56, "save button is missing after generation");
                    return;
                }
                phase = 6;
                QTimer::singleShot(0, save, [save] { save->click(); });
                return;
            }
            case 6: {
                const auto sourceProject = nativePath(savePath);
                const storage::LoadResult loaded =
                    storage::load(sourceProject, ValidationPurpose::Draft);
                std::error_code savedResourceError;
                const bool audioSaved = !loaded.project.segments.empty() &&
                    !loaded.project.segments.front().renderedAudioFile.empty() &&
                    std::filesystem::equivalent(
                        storage::resolveResource(sourceProject, loaded.project.segments.front().renderedAudioFile),
                        nativePath(generatedAudioPath), savedResourceError);
                if (!audioSaved) {
                    if (++attempts > 150) fail(65, "the second save did not persist the generated audio reference");
                    return;
                }
                attempts = 0;
                if (!loaded.missingResources.empty()) {
                    fail(65, "saved generated project has missing audio resources");
                    return;
                }
                const auto destinationRoot =
                    nativePath(QDir(temporaryDirectory.path()).filePath(QStringLiteral("打包目标")));
                const storage::PackageResult packaged =
                    storage::package(loaded.project, sourceProject, destinationRoot);
                packageDirectory = packaged.directory;
                if (!std::filesystem::is_regular_file(packaged.projectPath) ||
                    !std::filesystem::is_regular_file(packaged.directory / "audio" / "segment-1.wav")) {
                    fail(66, "package service did not create the project and segment audio");
                    return;
                }
                movedDirectory = nativePath(QDir(temporaryDirectory.path())
                                                .filePath(QStringLiteral("搬移后的包")));
                std::error_code moveError;
                std::filesystem::rename(packageDirectory, movedDirectory, moveError);
                if (moveError) {
                    fail(67, "cannot move the packaged project: " + moveError.message());
                    return;
                }
#ifdef _WIN32
                movedProjectPath = QString::fromStdWString(
                    (movedDirectory / "project.json").wstring());
#else
                movedProjectPath = QString::fromUtf8(
                    (movedDirectory / "project.json").string().c_str());
#endif
                attempts = 0;
                auto* open = button("openButton", QStringLiteral("打开"));
                if (open == nullptr) {
                    fail(57, "open button is missing before moved-package check");
                    return;
                }
                phase = 7;
                QTimer::singleShot(0, open, [open] { open->click(); });
                return;
            }
            case 7:
                selectDialogFile(movedProjectPath, 8);
                return;
            case 8: {
                auto* script = find<QTextEdit>("scriptEdit");
                auto* play = button("playSelectedButton", QStringLiteral("播放选中"));
                auto* output = find<QLabel>("outputPathLabel");
                std::error_code resourceError;
                const auto expectedAudio = movedDirectory / "audio" / "segment-1.wav";
                const auto actualAudio = output == nullptr
                                             ? std::filesystem::path{}
                                             : nativePath(output->text());
                const bool restoredExpectedAudio =
                    output != nullptr && std::filesystem::is_regular_file(expectedAudio) &&
                    std::filesystem::is_regular_file(actualAudio) &&
                    std::filesystem::equivalent(expectedAudio, actualAudio, resourceError) &&
                    !resourceError;
                if (visibleFileDialog() != nullptr || script == nullptr || play == nullptr ||
                    output == nullptr ||
                    !script->toPlainText().contains(QStringLiteral("science museum")) ||
                    !restoredExpectedAudio || !play->isEnabled()) {
                    if (++attempts > 200) {
                        fail(68, "moved package did not restore its relative audio resource");
                    }
                    return;
                }
                attempts = 0;
                phase = 9;
                play->click();
                return;
            }
            case 9: {
                auto* playbackTitle = find<QLabel>("playbackTitle");
                auto* stop = find<QPushButton>("stopPlaybackButton");
                if (playbackTitle == nullptr || stop == nullptr ||
                    !playbackTitle->text().contains(QStringLiteral("正在播放"))) {
                    if (++attempts > 200) {
                        fail(69, "moved packaged audio did not enter playback state; title=" +
                                 (playbackTitle == nullptr
                                      ? std::string("<missing>")
                                      : playbackTitle->text().toStdString()));
                    }
                    return;
                }
                stop->click();
                phase = 10;
                attempts = 0;
                return;
            }
            case 10: {
                auto* playbackTitle = find<QLabel>("playbackTitle");
                if (playbackTitle == nullptr ||
                    !playbackTitle->text().contains(QStringLiteral("已停止"))) {
                    if (++attempts > 100) {
                        fail(70, "playback did not stop cleanly");
                    }
                    return;
                }
                pass();
                return;
            }
            default:
                fail(59, "invalid desktop workflow smoke state");
                return;
            }
        } catch (const std::exception& error) {
            fail(71, std::string("workflow exception in phase ") + std::to_string(phase) +
                         ": " + error.what());
        } catch (...) {
            fail(72, std::string("unknown workflow exception in phase ") +
                         std::to_string(phase));
        }
    }

    void start() {
        elapsed.start();
        QObject::connect(&timer, &QTimer::timeout, &application, [this] { tick(); });
        timer.start();
    }
};

}  // namespace

bool startDesktopWorkflowSmoke(QApplication& application, MainWindow& window) {
    auto state = std::make_shared<WorkflowState>(application, window);
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &application,
                     [state] { state->timer.stop(); });
    state->start();
    return true;
}

}  // namespace listening::app
