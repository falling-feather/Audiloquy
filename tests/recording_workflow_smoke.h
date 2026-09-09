#pragma once

class QApplication;

namespace listening::app {
class MainWindow;

bool startRecordingWorkflowSmoke(QApplication& application, MainWindow& window);

}  // namespace listening::app
