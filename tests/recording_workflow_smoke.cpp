#include "recording_workflow_smoke.h"

#include "app/main_window.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace listening::app {
namespace {

template <typename T>
void writeLittle(std::ostream& output, T value) {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.put(static_cast<char>((value >> (index * 8U)) & 0xffU));
    }
}

bool writeManagedSilence(const std::filesystem::path& path) {
    constexpr std::uint32_t sampleRate = 44100;
    constexpr std::uint32_t frames = sampleRate;
    constexpr std::uint32_t dataBytes = frames * 2U;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write("RIFF", 4);
    writeLittle<std::uint32_t>(output, dataBytes + 36U);
    output.write("WAVEfmt ", 8);
    writeLittle<std::uint32_t>(output, 16U);
    writeLittle<std::uint16_t>(output, 1U);
    writeLittle<std::uint16_t>(output, 1U);
    writeLittle<std::uint32_t>(output, sampleRate);
    writeLittle<std::uint32_t>(output, sampleRate * 2U);
    writeLittle<std::uint16_t>(output, 2U);
    writeLittle<std::uint16_t>(output, 16U);
    output.write("data", 4);
    writeLittle<std::uint32_t>(output, dataBytes);
    std::array<char, 4096> silence{};
    std::uint32_t remaining = dataBytes;
    while (remaining > 0U) {
        const auto count = static_cast<std::streamsize>(
            std::min<std::uint32_t>(remaining, static_cast<std::uint32_t>(silence.size())));
        output.write(silence.data(), count);
        remaining -= static_cast<std::uint32_t>(count);
    }
    return static_cast<bool>(output);
}

QFileDialog* visibleFileDialog() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* dialog = qobject_cast<QFileDialog*>(widget);
            dialog != nullptr && dialog->isVisible()) {
            return dialog;
        }
    }
    return nullptr;
}

QDialog* visibleRecordingMarkerDialog() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<QDialog*>(widget);
        if (dialog != nullptr && dialog->isVisible() &&
            dialog->objectName() == QStringLiteral("recordingMarkerDialog")) {
            return dialog;
        }
    }
    return nullptr;
}

std::string visibleWindowSummary() {
    std::string summary;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (!widget->isVisible()) {
            continue;
        }
        if (!summary.empty()) {
            summary += "; ";
        }
        summary += widget->metaObject()->className();
        summary += "/";
        summary += widget->objectName().toStdString();
        if (auto* dialog = qobject_cast<QDialog*>(widget); dialog != nullptr) {
            summary += " title=" + dialog->windowTitle().toStdString();
        }
    }
    return summary.empty() ? std::string("<none>") : summary;
}

struct State final {
    QApplication& application;
    MainWindow& window;
    QTemporaryDir temporaryDirectory;
    QTimer timer;
    int phase{};
    int attempts{};
    QString sourcePath;

    State(QApplication& app, MainWindow& mainWindow)
        : application(app),
          window(mainWindow),
          temporaryDirectory(QDir::tempPath() + QStringLiteral("/语澜-recording-smoke-XXXXXX")) {
        timer.setInterval(20);
    }

    template <typename T>
    T* find(const char* objectName) {
        return window.findChild<T*>(QString::fromLatin1(objectName));
    }

    void fail(int code, const std::string& message) {
        std::cerr << "[recording-workflow-smoke] phase " << phase << ": " << message << '\n';
        timer.stop();
        application.exit(code);
    }

    void tick() {
        try {
            switch (phase) {
            case 0: {
                if (!temporaryDirectory.isValid()) {
                    fail(81, "cannot create the temporary recording directory");
                    return;
                }
                const auto source =
                    std::filesystem::path(temporaryDirectory.path().toStdWString()) /
                    std::filesystem::path(L"原始录音.wav");
                if (!writeManagedSilence(source)) {
                    fail(82, "cannot write a managed PCM WAV fixture");
                    return;
                }
                sourcePath = QString::fromStdWString(source.wstring());
                auto* classroom = find<QPushButton>("classroomStepButton");
                if (classroom == nullptr) {
                    fail(83, "classroom step button is missing");
                    return;
                }
                classroom->click();
                application.processEvents();
                auto* importButton = find<QPushButton>("importRecordingButton");
                if (importButton == nullptr) {
                    fail(84, "recording import button is missing");
                    return;
                }
                phase = 1;
                QTimer::singleShot(0, importButton, [importButton] { importButton->click(); });
                return;
            }
            case 1: {
                auto* dialog = visibleFileDialog();
                if (dialog == nullptr) {
                    if (++attempts > 250) {
                        fail(85, "recording file dialog did not open");
                    }
                    return;
                }
                attempts = 0;
                const QFileInfo selected(sourcePath);
                dialog->setDirectory(selected.absolutePath());
                dialog->selectFile(selected.fileName());
                phase = 2;
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                return;
            }
            case 2: {
                auto* marker = visibleRecordingMarkerDialog();
                if (marker == nullptr || !marker->isVisible()) {
                    if (++attempts > 1000) {
                        const auto* status = find<QLabel>("statusLabel");
                        fail(86,
                             std::string("recording marker dialog did not open after import; status=") +
                                 (status == nullptr ? "<missing>" : status->text().toStdString()) +
                                 "; visible=" + visibleWindowSummary());
                    }
                    return;
                }
                attempts = 0;
                auto* start = marker->findChild<QSpinBox*>("recordingStartSpin");
                auto* end = marker->findChild<QSpinBox*>("recordingEndSpin");
                auto* target = marker->findChild<QComboBox*>("recordingSegmentCombo");
                auto* buttons = marker->findChild<QDialogButtonBox*>();
                if (start == nullptr || end == nullptr || target == nullptr || buttons == nullptr ||
                    target->count() == 0) {
                    fail(87, "recording marker controls are incomplete");
                    return;
                }
                start->setValue(200);
                end->setValue(800);
                if (target->count() > 1) target->setCurrentIndex(1);
                QDir().mkpath(QStringLiteral("tmp"));
                if (!marker->grab().save(
                        QDir::current().filePath(QStringLiteral("tmp/recording-marker-smoke.png")),
                        "PNG")) {
                    fail(94, "could not capture recording marker dialog");
                    return;
                }
                phase = 3;
                QTimer::singleShot(0, buttons, [buttons] {
                    if (auto* accept = buttons->button(QDialogButtonBox::Ok); accept != nullptr) {
                        accept->click();
                    }
                });
                return;
            }
            case 3: {
                auto* sourceLabel = find<QLabel>("recordingSourceLabel");
                if (sourceLabel == nullptr ||
                    !sourceLabel->text().contains(QStringLiteral("200"))) {
                    if (++attempts > 250) {
                        fail(88, "accepted recording range was not shown in the editor");
                    }
                    return;
                }
                attempts = 0;
                auto* classroomReturn = find<QPushButton>("classroomReturnButton");
                if (classroomReturn == nullptr) {
                    fail(93, "classroom return button is missing after marking");
                    return;
                }
                classroomReturn->click();
                application.processEvents();
                auto* generate = find<QPushButton>("generateSelectedButton");
                if (generate == nullptr) {
                    fail(89, "selected generation button is missing");
                    return;
                }
                phase = 4;
                generate->click();
                return;
            }
            case 4: {
                auto* output = find<QLabel>("outputPathLabel");
                auto* generate = find<QPushButton>("generateSelectedButton");
                if (output == nullptr || generate == nullptr || !generate->isEnabled() ||
                    !QFileInfo::exists(output->text())) {
                    if (++attempts > 1500) {
                        fail(90, "recording-backed selected audio was not generated");
                    }
                    return;
                }
                timer.stop();
                application.exit(0);
                return;
            }
            default:
                fail(91, "invalid recording workflow state");
                return;
            }
        } catch (const std::exception& error) {
            std::cerr << "[recording-workflow-smoke] exception: " << error.what() << '\n';
            timer.stop();
            application.exit(92);
        }
    }

    void start() {
        timer.start();
        QObject::connect(&timer, &QTimer::timeout, &application, [this] { tick(); });
    }
};

}  // namespace

bool startRecordingWorkflowSmoke(QApplication& application, MainWindow& window) {
    auto state = std::make_shared<State>(application, window);
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &application,
                     [state] { state->timer.stop(); });
    state->start();
    return true;
}

}  // namespace listening::app
