#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <QDialog>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QSlider;
class QTimer;

namespace listening::app {

struct RecordingMark {
    std::string segmentId;
    std::uint64_t startMs{};
    std::uint64_t endMs{};
};

class RecordingMarkerDialog final : public QDialog {
    Q_OBJECT

public:
    RecordingMarkerDialog(std::filesystem::path audioPath,
                          std::uint64_t durationMs,
                          std::vector<std::pair<std::string, QString>> segments,
                          QWidget* parent = nullptr);

    [[nodiscard]] RecordingMark mark() const;
    void selectSegment(std::string_view segmentId);
    void setCurrentPositionProvider(std::function<std::uint64_t()> provider);
    void setPreviewHandler(std::function<void(std::uint64_t, std::uint64_t)> handler);
    void setPlaybackHandlers(std::function<void()> playSource,
                             std::function<void()> togglePause,
                             std::function<void(std::uint64_t)> seekSource,
                             std::function<bool()> isPlaying,
                             std::function<bool()> isPaused);

private:
    void setStartFromCurrent();
    void setEndFromCurrent();
    void previewSelection();
    void syncRangeBounds();
    void refreshPlaybackUi();
    void seekPlayback();

    std::filesystem::path audioPath_;
    std::uint64_t durationMs_{};
    std::function<std::uint64_t()> currentPositionProvider_;
    std::function<void(std::uint64_t, std::uint64_t)> previewHandler_;
    std::function<void()> playSourceHandler_;
    std::function<void()> togglePauseHandler_;
    std::function<void(std::uint64_t)> seekSourceHandler_;
    std::function<bool()> isPlayingProvider_;
    std::function<bool()> isPausedProvider_;
    QComboBox* segmentCombo_{};
    QSpinBox* startSpin_{};
    QSpinBox* endSpin_{};
    QLabel* rangeLabel_{};
    QLabel* playbackTimeLabel_{};
    QPushButton* playSourceButton_{};
    QPushButton* pauseSourceButton_{};
    QSlider* playbackSlider_{};
    QTimer* playbackTimer_{};
    bool playbackScrubbing_{};
};

}  // namespace listening::app
