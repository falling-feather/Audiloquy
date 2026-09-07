#pragma once

class QApplication;

namespace listening::app {

class MainWindow;

// Starts the bounded end-to-end desktop smoke state machine. The function
// returns true when the smoke run was scheduled; the state machine later calls
// QApplication::exit(0) on success or a non-zero code with a concrete stderr
// diagnostic on failure.
bool startDesktopWorkflowSmoke(QApplication& application, MainWindow& window);

}  // namespace listening::app

