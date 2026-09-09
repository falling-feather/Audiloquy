#include "app/recording_marker_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace listening::app {
namespace {

QString formatMilliseconds(std::uint64_t milliseconds) {
    const std::uint64_t seconds = milliseconds / 1000U;
    const std::uint64_t minutes = seconds / 60U;
    const std::uint64_t remainder = seconds % 60U;
    return QStringLiteral("%1:%2.%3")
        .arg(static_cast<qulonglong>(minutes), 2, 10, QLatin1Char('0'))
        .arg(static_cast<qulonglong>(remainder), 2, 10, QLatin1Char('0'))
        .arg(static_cast<qulonglong>((milliseconds % 1000U) / 10U), 2, 10,
             QLatin1Char('0'));
}

int boundedSpinValue(std::uint64_t value) {
    return static_cast<int>(std::min<std::uint64_t>(
        value, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
}

}  // namespace

RecordingMarkerDialog::RecordingMarkerDialog(
    std::filesystem::path audioPath,
    std::uint64_t durationMs,
    std::vector<std::pair<std::string, QString>> segments,
    QWidget* parent)
    : QDialog(parent), audioPath_(std::move(audioPath)), durationMs_(durationMs) {
    setObjectName(QStringLiteral("recordingMarkerDialog"));
    setWindowTitle(QStringLiteral("标记录音题段"));
    setMinimumWidth(520);
    setStyleSheet(QStringLiteral(R"CSS(
        QDialog#recordingMarkerDialog {
            background: #F7F5EF;
            color: #273047;
        }
        QDialog#recordingMarkerDialog QLabel {
            color: #273047;
        }
        QDialog#recordingMarkerDialog QLabel#dialogTitle {
            color: #20283E;
            font-size: 15pt;
            font-weight: 700;
        }
        QDialog#recordingMarkerDialog QLabel#mutedLabel {
            color: #7E7D80;
            font-size: 9pt;
        }
        QDialog#recordingMarkerDialog QComboBox,
        QDialog#recordingMarkerDialog QSpinBox {
            background: #FBFAF7;
            border: 1px solid #DFDBD4;
            border-radius: 8px;
            padding: 7px 9px;
            color: #273047;
        }
        QDialog#recordingMarkerDialog QPushButton {
            border: 1px solid #DED9D0;
            border-radius: 8px;
            background: #FFFEFA;
            padding: 8px 12px;
            color: #273047;
            font-weight: 600;
        }
        QDialog#recordingMarkerDialog QPushButton:hover {
            border-color: #B9BFE8;
            background: #F7F6FB;
        }
        QDialog#recordingMarkerDialog QPushButton[quietButton="true"] {
            color: #4D5569;
        }
        QDialog#recordingMarkerDialog QPushButton[accentButton="true"] {
            background: #E6F5F1;
            border-color: #B9DED6;
            color: #117C6E;
        }
        QDialog#recordingMarkerDialog QSlider::groove:horizontal {
            height: 5px;
            background: #DDD9D1;
            border-radius: 2px;
        }
        QDialog#recordingMarkerDialog QSlider::sub-page:horizontal {
            background: #4652B8;
            border-radius: 2px;
        }
        QDialog#recordingMarkerDialog QSlider::handle:horizontal {
            width: 16px;
            margin: -6px 0;
            background: #FFFEFA;
            border: 2px solid #4652B8;
            border-radius: 8px;
        }
        QDialog#recordingMarkerDialog QLabel#recordingPlaybackTime,
        QDialog#recordingMarkerDialog QLabel#recordingRangeLabel {
            color: #536078;
            font-size: 9pt;
        }
    )CSS"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 20);
    root->setSpacing(14);

    auto* title = new QLabel(QStringLiteral("把录音绑定到一个题组"), this);
    title->setObjectName(QStringLiteral("dialogTitle"));
    root->addWidget(title);
    auto* description = new QLabel(
        QStringLiteral("拖动上方进度条定位，或直接填写起止时间；可以试听选段，原文件不会被修改。"),
        this);
    description->setWordWrap(true);
    description->setObjectName(QStringLiteral("mutedLabel"));
    root->addWidget(description);

    auto* fileLabel = new QLabel(
        QStringLiteral("文件：%1\n总时长：%2")
            .arg(QString::fromStdWString(audioPath_.wstring()), formatMilliseconds(durationMs_)),
        this);
    fileLabel->setWordWrap(true);
    root->addWidget(fileLabel);

    auto* playbackRow = new QHBoxLayout;
    playbackTimeLabel_ = new QLabel(QStringLiteral("0:00.00 / 0:00.00"), this);
    playbackTimeLabel_->setObjectName(QStringLiteral("recordingPlaybackTime"));
    playbackSlider_ = new QSlider(Qt::Horizontal, this);
    playbackSlider_->setObjectName(QStringLiteral("recordingPlaybackSlider"));
    playbackSlider_->setRange(0, boundedSpinValue(durationMs_));
    playbackSlider_->setValue(0);
    playSourceButton_ = new QPushButton(QStringLiteral("▶  播放整段"), this);
    playSourceButton_->setObjectName(QStringLiteral("recordingPlaySourceButton"));
    pauseSourceButton_ = new QPushButton(QStringLiteral("Ⅱ  暂停"), this);
    pauseSourceButton_->setObjectName(QStringLiteral("recordingPauseSourceButton"));
    pauseSourceButton_->setProperty("quietButton", true);
    playbackRow->addWidget(playbackTimeLabel_);
    playbackRow->addWidget(playbackSlider_, 1);
    playbackRow->addWidget(playSourceButton_);
    playbackRow->addWidget(pauseSourceButton_);
    root->addLayout(playbackRow);

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(10);
    segmentCombo_ = new QComboBox(this);
    segmentCombo_->setObjectName(QStringLiteral("recordingSegmentCombo"));
    for (const auto& segment : segments) {
        segmentCombo_->addItem(
            segment.second,
            QString::fromUtf8(segment.first.data(),
                              static_cast<qsizetype>(segment.first.size())));
    }
    form->addRow(QStringLiteral("采用到题组"), segmentCombo_);

    auto* startRow = new QHBoxLayout;
    startSpin_ = new QSpinBox(this);
    startSpin_->setObjectName(QStringLiteral("recordingStartSpin"));
    startSpin_->setRange(0, boundedSpinValue(durationMs_));
    startSpin_->setSuffix(QStringLiteral(" ms"));
    auto* setStartButton = new QPushButton(QStringLiteral("以当前播放位置设起点"), this);
    setStartButton->setProperty("quietButton", true);
    startRow->addWidget(startSpin_, 1);
    startRow->addWidget(setStartButton);
    form->addRow(QStringLiteral("起点"), startRow);

    auto* endRow = new QHBoxLayout;
    endSpin_ = new QSpinBox(this);
    endSpin_->setObjectName(QStringLiteral("recordingEndSpin"));
    endSpin_->setRange(0, boundedSpinValue(durationMs_));
    endSpin_->setValue(boundedSpinValue(durationMs_));
    endSpin_->setSuffix(QStringLiteral(" ms"));
    auto* setEndButton = new QPushButton(QStringLiteral("以当前播放位置设终点"), this);
    setEndButton->setProperty("quietButton", true);
    endRow->addWidget(endSpin_, 1);
    endRow->addWidget(setEndButton);
    form->addRow(QStringLiteral("终点"), endRow);
    root->addLayout(form);

    rangeLabel_ = new QLabel(this);
    rangeLabel_->setObjectName(QStringLiteral("recordingRangeLabel"));
    root->addWidget(rangeLabel_);

    auto* previewButton = new QPushButton(QStringLiteral("试听当前选段"), this);
    previewButton->setObjectName(QStringLiteral("previewRecordingButton"));
    previewButton->setProperty("accentButton", true);
    root->addWidget(previewButton, 0, Qt::AlignLeft);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("采用标记"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    root->addWidget(buttons);

    connect(setStartButton, &QPushButton::clicked, this,
            [this] { setStartFromCurrent(); });
    connect(setEndButton, &QPushButton::clicked, this,
            [this] { setEndFromCurrent(); });
    connect(previewButton, &QPushButton::clicked, this,
            [this] { previewSelection(); });
    connect(playSourceButton_, &QPushButton::clicked, this, [this] {
        if (playSourceHandler_) {
            playSourceHandler_();
        }
    });
    connect(pauseSourceButton_, &QPushButton::clicked, this, [this] {
        if (togglePauseHandler_) {
            togglePauseHandler_();
        }
    });
    connect(playbackSlider_, &QSlider::sliderPressed, this,
            [this] { playbackScrubbing_ = true; });
    connect(playbackSlider_, &QSlider::sliderReleased, this,
            [this] {
                seekPlayback();
                playbackScrubbing_ = false;
            });
    connect(startSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int) { syncRangeBounds(); });
    connect(endSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int) { syncRangeBounds(); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        syncRangeBounds();
        if (startSpin_->value() >= endSpin_->value()) {
            rangeLabel_->setText(QStringLiteral("起点必须早于终点"));
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    playbackTimer_ = new QTimer(this);
    playbackTimer_->setInterval(100);
    connect(playbackTimer_, &QTimer::timeout, this,
            [this] { refreshPlaybackUi(); });
    playbackTimer_->start();
    syncRangeBounds();
    refreshPlaybackUi();
}

RecordingMark RecordingMarkerDialog::mark() const {
    RecordingMark result;
    if (segmentCombo_ != nullptr) {
        result.segmentId = segmentCombo_->currentData().toString().toUtf8().constData();
    }
    result.startMs = static_cast<std::uint64_t>(std::max(0, startSpin_->value()));
    result.endMs = static_cast<std::uint64_t>(std::max(0, endSpin_->value()));
    return result;
}

void RecordingMarkerDialog::selectSegment(std::string_view segmentId) {
    const QString target = QString::fromUtf8(segmentId.data(),
                                             static_cast<qsizetype>(segmentId.size()));
    const int index = segmentCombo_ == nullptr ? -1 : segmentCombo_->findData(target);
    if (index >= 0) {
        segmentCombo_->setCurrentIndex(index);
    }
}

void RecordingMarkerDialog::setCurrentPositionProvider(std::function<std::uint64_t()> provider) {
    currentPositionProvider_ = std::move(provider);
}

void RecordingMarkerDialog::setPreviewHandler(
    std::function<void(std::uint64_t, std::uint64_t)> handler) {
    previewHandler_ = std::move(handler);
}

void RecordingMarkerDialog::setPlaybackHandlers(
    std::function<void()> playSource,
    std::function<void()> togglePause,
    std::function<void(std::uint64_t)> seekSource,
    std::function<bool()> isPlaying,
    std::function<bool()> isPaused) {
    playSourceHandler_ = std::move(playSource);
    togglePauseHandler_ = std::move(togglePause);
    seekSourceHandler_ = std::move(seekSource);
    isPlayingProvider_ = std::move(isPlaying);
    isPausedProvider_ = std::move(isPaused);
    refreshPlaybackUi();
}

void RecordingMarkerDialog::setStartFromCurrent() {
    if (!currentPositionProvider_) {
        return;
    }
    startSpin_->setValue(boundedSpinValue(currentPositionProvider_()));
}

void RecordingMarkerDialog::setEndFromCurrent() {
    if (!currentPositionProvider_) {
        return;
    }
    endSpin_->setValue(boundedSpinValue(currentPositionProvider_()));
}

void RecordingMarkerDialog::previewSelection() {
    syncRangeBounds();
    if (startSpin_->value() >= endSpin_->value()) {
        rangeLabel_->setText(QStringLiteral("起点必须早于终点，才能试听"));
        return;
    }
    if (previewHandler_) {
        previewHandler_(static_cast<std::uint64_t>(startSpin_->value()),
                        static_cast<std::uint64_t>(endSpin_->value()));
    }
}

void RecordingMarkerDialog::syncRangeBounds() {
    if (startSpin_ == nullptr || endSpin_ == nullptr || rangeLabel_ == nullptr) {
        return;
    }
    const int end = std::max(startSpin_->value(), endSpin_->value());
    rangeLabel_->setText(QStringLiteral("当前选段：%1 — %2 · 时长 %3")
                             .arg(formatMilliseconds(static_cast<std::uint64_t>(startSpin_->value())))
                             .arg(formatMilliseconds(static_cast<std::uint64_t>(endSpin_->value())))
                             .arg(formatMilliseconds(static_cast<std::uint64_t>(
                                 std::max(0, end - startSpin_->value())))));
}

void RecordingMarkerDialog::refreshPlaybackUi() {
    if (playbackSlider_ == nullptr || playbackTimeLabel_ == nullptr) {
        return;
    }
    const std::uint64_t position = currentPositionProvider_ ? currentPositionProvider_() : 0U;
    const int boundedPosition = boundedSpinValue(std::min(position, durationMs_));
    if (!playbackScrubbing_) {
        const QSignalBlocker blocker(playbackSlider_);
        playbackSlider_->setValue(boundedPosition);
    }
    playbackTimeLabel_->setText(QStringLiteral("%1 / %2")
                                    .arg(formatMilliseconds(position))
                                    .arg(formatMilliseconds(durationMs_)));
    const bool playing = isPlayingProvider_ && isPlayingProvider_();
    const bool paused = isPausedProvider_ && isPausedProvider_();
    playSourceButton_->setEnabled(static_cast<bool>(playSourceHandler_));
    pauseSourceButton_->setEnabled(static_cast<bool>(togglePauseHandler_) && (playing || paused));
    pauseSourceButton_->setText(paused ? QStringLiteral("▶  继续")
                                       : QStringLiteral("Ⅱ  暂停"));
}

void RecordingMarkerDialog::seekPlayback() {
    if (seekSourceHandler_ != nullptr) {
        seekSourceHandler_(static_cast<std::uint64_t>(std::max(0, playbackSlider_->value())));
    }
}

}  // namespace listening::app
