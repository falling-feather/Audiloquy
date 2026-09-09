#include "app/main_window.h"

#include "../../tests/desktop_workflow_smoke.h"
#include "../../tests/recording_workflow_smoke.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QTextEdit>
#include <QTextBrowser>
#include <QTimer>

#include <cstring>
#include <iostream>

int main(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--workflow-smoke-test") == 0 ||
            std::strcmp(argv[index], "--recording-smoke-test") == 0) {
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
            break;
        }
    }
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("语澜"));
    QApplication::setApplicationDisplayName(QStringLiteral("语澜"));
    QApplication::setOrganizationName(QStringLiteral("AUDILOQUY"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/audiloquy-icon.png")));
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setFont(QFont(QStringLiteral("Segoe UI"), 10));

    listening::app::MainWindow window;
    window.show();

    if (application.arguments().contains(QStringLiteral("--workflow-smoke-test"))) {
        if (!listening::app::startDesktopWorkflowSmoke(application, window)) {
            return 20;
        }
    } else if (application.arguments().contains(QStringLiteral("--recording-smoke-test"))) {
        if (!listening::app::startRecordingWorkflowSmoke(application, window)) {
            return 21;
        }
    } else if (application.arguments().contains(QStringLiteral("--classroom-smoke-test"))) {
        QTimer::singleShot(120, &application, [&application, &window] {
            window.resize(1366, 768);
            application.processEvents();
            auto* classroomStep =
                window.findChild<QPushButton*>(QStringLiteral("classroomStepButton"));
            auto* center = window.findChild<QStackedWidget*>(QStringLiteral("centerStack"));
            auto* classroomTitle =
                window.findChild<QLabel*>(QStringLiteral("classroomQuestionLabel"));
            auto* classroomList =
                window.findChild<QListWidget*>(QStringLiteral("classroomSegmentList"));
            auto* returnButton =
                window.findChild<QPushButton*>(QStringLiteral("classroomReturnButton"));
            auto* script = window.findChild<QTextEdit*>(QStringLiteral("scriptEdit"));
            if (classroomStep == nullptr || center == nullptr || classroomTitle == nullptr ||
                classroomList == nullptr || returnButton == nullptr || script == nullptr) {
                std::cerr << "[classroom-smoke] missing controls: classroomStep="
                          << (classroomStep != nullptr) << " center=" << (center != nullptr)
                          << " title=" << (classroomTitle != nullptr)
                          << " list=" << (classroomList != nullptr)
                          << " return=" << (returnButton != nullptr)
                          << " script=" << (script != nullptr) << '\n';
                application.exit(31);
                return;
            }
            const bool scriptVisibleBefore = script->isVisible();
            classroomStep->click();
            application.processEvents();
            const bool classroomVisible = center->currentWidget() != nullptr &&
                                          center->currentWidget()->objectName() ==
                                              QStringLiteral("classroomPage");
            const bool contentReady = classroomList->count() > 0 &&
                                      classroomTitle->text().contains(QStringLiteral("第"));
            const QStringList paths = {QStringLiteral("tmp"),
                                       QStringLiteral("tmp/classroom-smoke-1366x768.png")};
            QDir().mkpath(paths.front());
            const bool captured = window.grab().save(paths.back(), "PNG");
            const bool scriptHidden = !script->isVisible();
            returnButton->click();
            application.processEvents();
            const bool preparationVisible = center->currentWidget() != nullptr &&
                                             center->currentWidget()->objectName() !=
                                                 QStringLiteral("classroomPage");
            if (!scriptVisibleBefore || !classroomVisible || !contentReady || !captured ||
                !scriptHidden || !preparationVisible) {
                std::cerr << "[classroom-smoke] checks: scriptBefore=" << scriptVisibleBefore
                          << " classroom=" << classroomVisible << " content=" << contentReady
                          << " captured=" << captured << " scriptHidden=" << scriptHidden
                          << " prep=" << preparationVisible << '\n';
            }
            application.exit(!scriptVisibleBefore || !classroomVisible || !contentReady ||
                                     !captured || !scriptHidden || !preparationVisible
                                 ? 32
                                 : 0);
        });
    } else if (application.arguments().contains(QStringLiteral("--scenario-smoke-test")) ||
               application.arguments().contains(QStringLiteral("--scenario-multi-smoke-test")) ||
        application.arguments().contains(QStringLiteral("--deepseek-live-smoke-test"))) {
        QTimer::singleShot(80, &application, [&application, &window] {
            const bool deepSeek = application.arguments().contains(
                QStringLiteral("--deepseek-live-smoke-test"));
            const bool multi = application.arguments().contains(
                QStringLiteral("--scenario-multi-smoke-test"));
            auto* smart =
                window.findChild<QPushButton*>(QStringLiteral("smartScenarioButton"));
            if (smart == nullptr) {
                application.exit(5);
                return;
            }
            smart->click();
            application.processEvents();
            auto* script = window.findChild<QTextEdit*>(QStringLiteral("scriptEdit"));
            const bool adopted = script != nullptr &&
                                 script->toPlainText().contains(QStringLiteral("MAN:")) &&
                                 script->toPlainText().contains(QStringLiteral("WOMAN:"));
            bool reviewPassed = true;
            if (multi && adopted) {
                auto* reviewButton =
                    window.findChild<QPushButton*>(QStringLiteral("viewGenerationButton"));
                if (reviewButton == nullptr || !reviewButton->isEnabled()) {
                    reviewPassed = false;
                } else {
                    reviewPassed = false;
                    QTimer reviewTimer;
                    reviewTimer.setSingleShot(true);
                    QObject::connect(&reviewTimer, &QTimer::timeout, &window, [&] {
                        auto* reviewDialog = window.findChild<QDialog*>(QStringLiteral("generationReviewDialog"));
                        auto* browser = reviewDialog ? reviewDialog->findChild<QTextBrowser*>(QStringLiteral("generationReviewBrowser")) : nullptr;
                        reviewPassed = browser && browser->toPlainText().contains(QStringLiteral("第 2 题"));
                        if (reviewDialog) reviewDialog->reject();
                        else if (auto* active = qobject_cast<QDialog*>(QApplication::activeModalWidget())) active->reject();
                    });
                    reviewTimer.start(80);
                    reviewButton->click();
                    reviewTimer.stop();
                }
            }
            const bool captured = QFileInfo::exists(
                QDir::current().filePath(
                    deepSeek ? QStringLiteral("tmp/deepseek-live-smoke.png")
                             : multi ? QStringLiteral("tmp/scenario-multi-smoke.png")
                                     : QStringLiteral("tmp/scenario-generator-smoke.png")));
            application.exit(adopted && captured && reviewPassed ? 0 : 8);
        });
    } else if (application.arguments().contains(QStringLiteral("--preview-smoke-test"))) {
        QTimer::singleShot(50, &application, [&application, &window] {
            auto* generate =
                window.findChild<QPushButton*>(QStringLiteral("generateSelectedButton"));
            auto* playbackTitle =
                window.findChild<QLabel*>(QStringLiteral("playbackTitle"));
            auto* stop =
                window.findChild<QPushButton*>(QStringLiteral("stopPlaybackButton"));
            auto* pause =
                window.findChild<QPushButton*>(QStringLiteral("pausePlaybackButton"));
            auto* progress =
                window.findChild<QSlider*>(QStringLiteral("playbackSlider"));
            auto* allowFallback =
                window.findChild<QCheckBox*>(QStringLiteral("allowGenderFallbackCheck"));
            auto* outputPath =
                window.findChild<QLabel*>(QStringLiteral("outputPathLabel"));
            if (generate == nullptr || playbackTitle == nullptr || stop == nullptr ||
                pause == nullptr || progress == nullptr || allowFallback == nullptr ||
                outputPath == nullptr) {
                application.exit(3);
                return;
            }

            // The test runner has only one stock English voice. This is an
            // explicit compatibility choice, never a silent product fallback.
            allowFallback->setChecked(true);
            generate->click();
            auto* poll = new QTimer(&window);
            poll->setInterval(50);
            auto* attempts = new int(0);
            QObject::connect(poll, &QTimer::timeout, &window,
                             [poll, attempts, &application, playbackTitle, stop, pause,
                              progress, outputPath] {
                                 ++*attempts;
                                 const bool previewStarted =
                                     playbackTitle->text().startsWith(QStringLiteral("正在播放"));
                                 const bool outputCreated =
                                     QFileInfo::exists(outputPath->text());
                                 const bool progressReady =
                                     progress->isEnabled() && progress->maximum() > 0;
                                 if (previewStarted && outputCreated && progressReady) {
                                     pause->click();
                                     application.processEvents();
                                     const bool paused = pause->text().contains(QStringLiteral("继续"));
                                     pause->click();
                                     stop->click();
                                     poll->stop();
                                     application.exit(paused ? 0 : 4);
                                 } else if (*attempts > 800) {
                                     poll->stop();
                                     application.exit(4);
                                 }
                             });
            poll->start();
        });
    } else if (application.arguments().contains(QStringLiteral("--editor-smoke-test"))) {
        QTimer::singleShot(100, &application, [&application, &window] {
            window.resize(1366, 768);
            application.processEvents();
            auto* newProject =
                window.findChild<QPushButton*>(QStringLiteral("newProjectButton"));
            auto* add = window.findChild<QPushButton*>(QStringLiteral("addSegmentButton"));
            auto* duplicate =
                window.findChild<QPushButton*>(QStringLiteral("duplicateSegmentButton"));
            auto* remove =
                window.findChild<QPushButton*>(QStringLiteral("deleteSegmentButton"));
            auto* up = window.findChild<QPushButton*>(QStringLiteral("moveSegmentUpButton"));
            auto* undo = window.findChild<QPushButton*>(QStringLiteral("undoEditButton"));
            auto* redo = window.findChild<QPushButton*>(QStringLiteral("redoEditButton"));
            auto* list = window.findChild<QListWidget*>(QStringLiteral("segmentList"));
            auto* script = window.findChild<QTextEdit*>(QStringLiteral("scriptEdit"));
            if (newProject == nullptr || add == nullptr || duplicate == nullptr ||
                remove == nullptr || up == nullptr || undo == nullptr || redo == nullptr ||
                list == nullptr || script == nullptr) {
                application.exit(9);
                return;
            }

            newProject->click();
            application.processEvents();
            const bool blankCreated = list->count() == 1;
            script->setPlainText(QStringLiteral("typed before structural edit"));
            application.processEvents();
            add->click();
            application.processEvents();
            const bool added = list->count() == 2;
            undo->click();
            application.processEvents();
            const bool undoKeepsText = list->count() == 1 &&
                                       script->toPlainText().contains(
                                           QStringLiteral("typed before structural edit"));
            redo->click();
            application.processEvents();
            const bool redoRestores = list->count() == 2;
            duplicate->click();
            application.processEvents();
            const bool duplicated = list->count() == 3;
            const QStringList beforeOrder = [&] {
                QStringList ids;
                for (int row = 0; row < list->count(); ++row) {
                    ids.push_back(list->item(row)->data(Qt::UserRole).toString());
                }
                return ids;
            }();
            const int beforeQuestion = list->item(2)->text().indexOf(QStringLiteral("第 2 题"));
            up->click();
            application.processEvents();
            const QStringList afterOrder = [&] {
                QStringList ids;
                for (int row = 0; row < list->count(); ++row) {
                    ids.push_back(list->item(row)->data(Qt::UserRole).toString());
                }
                return ids;
            }();
            remove->click();
            application.processEvents();
            const bool removed = list->count() == 2;
            undo->click();
            application.processEvents();
            const bool undoDelete = list->count() == 3;
            redo->click();
            application.processEvents();
            const bool redoDelete = list->count() == 2;
            QDir directory(QDir::current());
            directory.mkpath(QStringLiteral("tmp"));
            const bool captured =
                window.grab().save(directory.filePath(QStringLiteral("tmp/editor-smoke.png")),
                                   "PNG") &&
                window.grab().save(
                    directory.filePath(QStringLiteral("tmp/editor-smoke-1366x768.png")), "PNG");
            const bool reordered = beforeOrder != afterOrder && beforeQuestion >= 0;
            application.exit(blankCreated && added && undoKeepsText && redoRestores &&
                              duplicated && reordered && removed && undoDelete && redoDelete &&
                              captured
                                  ? 0
                                  : 10);
        });
    } else if (application.arguments().contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(500, &application, [&application, &window] {
            QDir directory(QDir::current());
            directory.mkpath(QStringLiteral("tmp"));
            const QString simpleOutput =
                directory.filePath(QStringLiteral("tmp/prototype-smoke.png"));
            const bool simpleSaved = window.grab().save(simpleOutput, "PNG");

            QPushButton* professionalButton = nullptr;
            for (auto* button : window.findChildren<QPushButton*>()) {
                if (button->text() == QStringLiteral("专业")) {
                    professionalButton = button;
                    break;
                }
            }
            if (professionalButton != nullptr) {
                professionalButton->click();
                application.processEvents();
            }
            const QString professionalOutput =
                directory.filePath(QStringLiteral("tmp/prototype-professional-smoke.png"));
            const bool professionalSaved = professionalButton != nullptr &&
                                           window.grab().save(professionalOutput, "PNG");
            application.exit(simpleSaved && professionalSaved ? 0 : 2);
        });
    }
    return application.exec();
}
