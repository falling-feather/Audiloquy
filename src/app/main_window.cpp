#include "app/main_window.h"
#include "app/scenario_dialog.h"
#include "app/styled_message_dialog.h"

#include "audio/wav_builder.h"
#include "core/output_paths.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QStringList>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace listening::app {
namespace {

QString qString(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string utf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

std::filesystem::path nativePath(const QString& value) {
#ifdef _WIN32
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(utf8(value));
#endif
}

QString qPath(const std::filesystem::path& value) {
#ifdef _WIN32
    return QString::fromStdWString(value.wstring());
#else
    return qString(value.string());
#endif
}

std::filesystem::path recoveryRootPath() {
    const QString configured = qEnvironmentVariable("AUDILOQUY_RECOVERY_ROOT");
    const QString base = configured.isEmpty()
                             ? QDir(qEnvironmentVariable("LOCALAPPDATA"))
                                   .filePath(QStringLiteral("Audiloquy/recovery"))
                             : configured;
    return nativePath(base);
}

std::string freshStableId(std::string_view prefix) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream output;
    output << prefix << '-' << std::hex << static_cast<std::uint64_t>(now) << '-'
           << serial;
    return output.str();
}

bool isSmokeRun() {
    const QStringList arguments = QCoreApplication::arguments();
    return arguments.contains(QStringLiteral("--smoke-test")) ||
           arguments.contains(QStringLiteral("--preview-smoke-test")) ||
           arguments.contains(QStringLiteral("--scenario-smoke-test")) ||
           arguments.contains(QStringLiteral("--deepseek-live-smoke-test")) ||
           arguments.contains(QStringLiteral("--workflow-smoke-test")) ||
           arguments.contains(QStringLiteral("--editor-smoke-test"));
}

QString durationText(std::chrono::milliseconds duration) {
    const qint64 totalSeconds = std::max<qint64>(0, (duration.count() + 500) / 1000);
    if (totalSeconds < 60) {
        return QStringLiteral("%1 秒").arg(totalSeconds);
    }
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

QString questionLabel(const Segment& segment) {
    if (segment.questions.first == segment.questions.last) {
        return QStringLiteral("第 %1 题").arg(segment.questions.first);
    }
    return QStringLiteral("第 %1–%2 题")
        .arg(segment.questions.first)
        .arg(segment.questions.last);
}

QString segmentListText(const Segment& segment, bool generated) {
    const QString marker = generated ? QStringLiteral("●  ") : QStringLiteral("○  ");
    return QStringLiteral("%1%2\n     %3  ·  %4 词  ·  %5 遍")
        .arg(marker)
        .arg(questionLabel(segment))
        .arg(qString(segment.speaker))
        .arg(static_cast<qulonglong>(countReadableWords(segment.text)))
        .arg(segment.repeatCount);
}

QLabel* sectionTitle(const QString& title, QWidget* parent = nullptr) {
    auto* label = new QLabel(title, parent);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QLabel* mutedLabel(const QString& text, QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QFrame* makeCard(QWidget* parent = nullptr) {
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("card"));
    frame->setFrameShape(QFrame::NoFrame);
    return frame;
}

std::string lowerAsciiCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return character >= 'A' && character <= 'Z'
                   ? static_cast<char>(character - 'A' + 'a')
                   : static_cast<char>(character);
    });
    return value;
}

QString renderPhaseText(RenderPhase phase) {
    switch (phase) {
    case RenderPhase::Preparing:
        return QStringLiteral("准备生成");
    case RenderPhase::Synthesizing:
        return QStringLiteral("合成语音");
    case RenderPhase::Assembling:
        return QStringLiteral("组装题组音频");
    case RenderPhase::Publishing:
        return QStringLiteral("写入音频");
    case RenderPhase::Completed:
        return QStringLiteral("生成完成");
    case RenderPhase::Cancelled:
        return QStringLiteral("正在取消");
    case RenderPhase::Failed:
        return QStringLiteral("生成失败");
    }
    return QStringLiteral("生成中");
}

QString renderVoiceUseText(const RenderedVoiceUse& use) {
    const QString role = use.voice.gender == platform::windows::VoiceGender::Male
                             ? QStringLiteral("男")
                             : use.voice.gender == platform::windows::VoiceGender::Female
                                   ? QStringLiteral("女")
                                   : QStringLiteral("旁白");
    return QStringLiteral("%1：%2 (%3)%4%5")
        .arg(role, qString(use.voice.name), qString(use.voice.locale),
             use.usedLocaleFallback ? QStringLiteral(" · 口音回退") : QString(),
             use.usedGenderFallback ? QStringLiteral(" · 性别回退") : QString());
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), renderJob_(this), recoveryStore_(recoveryRootPath()) {
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    buildUi();
    refreshVoiceOptions();
    connectUi();
    applyTheme();
    if (isSmokeRun()) {
        loadExampleProject(false);
    } else {
        newBlankProject(false);
        try {
            recoveryStore_.start();
            recoveryEnabled_ = true;
            QTimer::singleShot(0, this, [this] { checkRecoveryOnStartup(); });
        } catch (const std::exception& error) {
            showStatus(QStringLiteral("自动恢复不可用：%1").arg(qString(error.what())), true);
        }
    }
}

MainWindow::~MainWindow() {
    player_.stop();
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("语澜"));
    setMinimumSize(1060, 700);
    resize(1320, 860);

    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("appRoot"));
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    topBar_ = new QFrame(root);
    topBar_->setObjectName(QStringLiteral("topBar"));
    topBar_->setFixedHeight(64);
    topBar_->installEventFilter(this);
    auto* topLayout = new QHBoxLayout(topBar_);
    topLayout->setContentsMargins(18, 8, 8, 8);
    topLayout->setSpacing(10);

    auto* brandMark = new QLabel(topBar_);
    brandMark->setObjectName(QStringLiteral("brandMark"));
    brandMark->setAlignment(Qt::AlignCenter);
    brandMark->setFixedSize(38, 38);
    const QPixmap brandPixmap(QStringLiteral(":/brand/audiloquy-icon.png"));
    if (!brandPixmap.isNull()) {
        brandMark->setPixmap(brandPixmap.scaled(34, 34, Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation));
    } else {
        brandMark->setText(QStringLiteral("澜"));
    }
    auto* brandText = new QHBoxLayout;
    brandText->setSpacing(7);
    auto* brandName = new QLabel(QStringLiteral("语澜"), topBar_);
    brandName->setObjectName(QStringLiteral("brandName"));
    auto* brandSub = new QLabel(QStringLiteral("Audiloquy"), topBar_);
    brandSub->setObjectName(QStringLiteral("brandSub"));
    brandText->addWidget(brandName);
    brandText->addWidget(brandSub);
    topLayout->addWidget(brandMark);
    topLayout->addLayout(brandText);

    auto* titleDivider = new QFrame(topBar_);
    titleDivider->setObjectName(QStringLiteral("verticalDivider"));
    titleDivider->setFixedSize(1, 34);
    topLayout->addSpacing(8);
    topLayout->addWidget(titleDivider);
    projectTitleEdit_ = new QLineEdit(topBar_);
    projectTitleEdit_->setObjectName(QStringLiteral("projectTitleEdit"));
    projectTitleEdit_->setPlaceholderText(QStringLiteral("工程名称"));
    projectTitleEdit_->setMaximumWidth(285);
    topLayout->addWidget(projectTitleEdit_, 1);

    auto* offlineBadge = new QLabel(QStringLiteral("●  本地优先"), topBar_);
    offlineBadge->setObjectName(QStringLiteral("offlineBadge"));
    topLayout->addWidget(offlineBadge);

    auto* modeFrame = new QFrame(topBar_);
    modeFrame->setObjectName(QStringLiteral("modeSwitch"));
    auto* modeLayout = new QHBoxLayout(modeFrame);
    modeLayout->setContentsMargins(3, 3, 3, 3);
    modeLayout->setSpacing(2);
    simpleModeButton_ = new QPushButton(QStringLiteral("简洁"), modeFrame);
    professionalModeButton_ = new QPushButton(QStringLiteral("专业"), modeFrame);
    simpleModeButton_->setCheckable(true);
    professionalModeButton_->setCheckable(true);
    simpleModeButton_->setProperty("modeButton", true);
    professionalModeButton_->setProperty("modeButton", true);
    simpleModeButton_->setChecked(true);
    modeLayout->addWidget(simpleModeButton_);
    modeLayout->addWidget(professionalModeButton_);
    auto* modeGroup = new QButtonGroup(this);
    modeGroup->setExclusive(true);
    modeGroup->addButton(simpleModeButton_);
    modeGroup->addButton(professionalModeButton_);
    topLayout->addWidget(modeFrame);

    openButton_ = new QPushButton(QStringLiteral("打开"), topBar_);
    openButton_->setProperty("quietButton", true);
    saveButton_ = new QPushButton(QStringLiteral("保存"), topBar_);
    saveButton_->setProperty("primaryButton", true);
    topLayout->addWidget(openButton_);
    topLayout->addWidget(saveButton_);
    minimizeButton_ = new QPushButton(QStringLiteral("—"), topBar_);
    maximizeButton_ = new QPushButton(QStringLiteral("□"), topBar_);
    closeButton_ = new QPushButton(QStringLiteral("×"), topBar_);
    for (auto* button : {minimizeButton_, maximizeButton_, closeButton_}) {
        button->setProperty("windowButton", true);
        button->setFixedSize(42, 42);
    }
    closeButton_->setProperty("closeButton", true);
    topLayout->addWidget(minimizeButton_);
    topLayout->addWidget(maximizeButton_);
    topLayout->addWidget(closeButton_);
    rootLayout->addWidget(topBar_);

    auto* workflowNav = new QFrame(root);
    workflowNav->setObjectName(QStringLiteral("workflowNav"));
    workflowNav->setFixedHeight(58);
    auto* workflowLayout = new QHBoxLayout(workflowNav);
    workflowLayout->setContentsMargins(22, 7, 22, 7);
    workflowLayout->setSpacing(7);
    auto makeStepButton = [workflowNav](const QString& number, const QString& label) {
        auto* button = new QPushButton(
            QStringLiteral("%1  %2").arg(number, label), workflowNav);
        button->setCheckable(true);
        button->setProperty("workflowStep", true);
        button->setMinimumWidth(116);
        return button;
    };
    manuscriptStepButton_ = makeStepButton(QStringLiteral("01"), QStringLiteral("文稿"));
    voiceStepButton_ = makeStepButton(QStringLiteral("02"), QStringLiteral("声音"));
    rhythmStepButton_ = makeStepButton(QStringLiteral("03"), QStringLiteral("节奏"));
    generateStepButton_ = makeStepButton(QStringLiteral("04"), QStringLiteral("生成"));
    classroomStepButton_ = makeStepButton(QStringLiteral("05"), QStringLiteral("课堂"));
    manuscriptStepButton_->setChecked(true);
    auto* workflowGroup = new QButtonGroup(this);
    workflowGroup->setExclusive(true);
    for (auto* button : {manuscriptStepButton_, voiceStepButton_, rhythmStepButton_,
                         generateStepButton_, classroomStepButton_}) {
        workflowGroup->addButton(button);
        workflowLayout->addWidget(button);
        if (button != classroomStepButton_) {
            auto* chevron = new QLabel(QStringLiteral("›"), workflowNav);
            chevron->setObjectName(QStringLiteral("workflowChevron"));
            workflowLayout->addWidget(chevron);
        }
    }
    workflowLayout->addStretch();
    auto* workflowHint = new QLabel(QStringLiteral("五步完成一套听力"), workflowNav);
    workflowHint->setObjectName(QStringLiteral("workflowHint"));
    workflowLayout->addWidget(workflowHint);
    rootLayout->addWidget(workflowNav);

    auto* bodySplitter = new QSplitter(Qt::Horizontal, root);
    bodySplitter->setObjectName(QStringLiteral("bodySplitter"));
    bodySplitter->setChildrenCollapsible(false);
    bodySplitter->setHandleWidth(1);

    auto* sidebar = new QFrame(bodySplitter);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setMinimumWidth(275);
    sidebar->setMaximumWidth(370);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(18, 14, 18, 10);
    sidebarLayout->setSpacing(8);

    auto* groupHeading = new QHBoxLayout;
    auto* groupTitle = sectionTitle(QStringLiteral("题组编排"), sidebar);
    groupCountLabel_ = new QLabel(sidebar);
    groupCountLabel_->setObjectName(QStringLiteral("countBadge"));
    groupCountLabel_->setAlignment(Qt::AlignCenter);
    groupHeading->addWidget(groupTitle);
    groupHeading->addStretch();
    groupHeading->addWidget(groupCountLabel_);
    sidebarLayout->addLayout(groupHeading);
    sidebarLayout->addWidget(mutedLabel(QStringLiteral("选择题组后在右侧编辑文本与播放规则。"), sidebar));

    newProjectButton_ = new QPushButton(QStringLiteral("新建空白工程"), sidebar);
    newProjectButton_->setObjectName(QStringLiteral("newProjectButton"));
    newProjectButton_->setProperty("quietButton", true);
    exampleButton_ = new QPushButton(QStringLiteral("重置示例"), sidebar);
    exampleButton_->setObjectName(QStringLiteral("exampleProjectButton"));
    exampleButton_->setProperty("quietButton", true);
    auto* projectActions = new QHBoxLayout;
    projectActions->setSpacing(6);
    projectActions->addWidget(newProjectButton_);
    projectActions->addWidget(exampleButton_);
    sidebarLayout->addLayout(projectActions);

    auto* groupActions = new QGridLayout;
    groupActions->setHorizontalSpacing(6);
    groupActions->setVerticalSpacing(6);
    addSegmentButton_ = new QPushButton(QStringLiteral("＋ 新增"), sidebar);
    duplicateSegmentButton_ = new QPushButton(QStringLiteral("复制"), sidebar);
    deleteSegmentButton_ = new QPushButton(QStringLiteral("删除"), sidebar);
    moveUpButton_ = new QPushButton(QStringLiteral("↑ 上移"), sidebar);
    moveDownButton_ = new QPushButton(QStringLiteral("↓ 下移"), sidebar);
    undoButton_ = new QPushButton(QStringLiteral("撤销"), sidebar);
    redoButton_ = new QPushButton(QStringLiteral("重做"), sidebar);
    addSegmentButton_->setObjectName(QStringLiteral("addSegmentButton"));
    duplicateSegmentButton_->setObjectName(QStringLiteral("duplicateSegmentButton"));
    deleteSegmentButton_->setObjectName(QStringLiteral("deleteSegmentButton"));
    moveUpButton_->setObjectName(QStringLiteral("moveSegmentUpButton"));
    moveDownButton_->setObjectName(QStringLiteral("moveSegmentDownButton"));
    undoButton_->setObjectName(QStringLiteral("undoEditButton"));
    redoButton_->setObjectName(QStringLiteral("redoEditButton"));
    for (auto* button : {addSegmentButton_, duplicateSegmentButton_, deleteSegmentButton_,
                         moveUpButton_, moveDownButton_, undoButton_, redoButton_}) {
        button->setProperty("quietButton", true);
        button->setProperty("compactButton", true);
    }
    groupActions->addWidget(addSegmentButton_, 0, 0);
    groupActions->addWidget(duplicateSegmentButton_, 0, 1);
    groupActions->addWidget(deleteSegmentButton_, 0, 2);
    groupActions->addWidget(moveUpButton_, 1, 0);
    groupActions->addWidget(moveDownButton_, 1, 1);
    groupActions->addWidget(undoButton_, 1, 2);
    groupActions->addWidget(redoButton_, 1, 3);
    sidebarLayout->addLayout(groupActions);

    segmentList_ = new QListWidget(sidebar);
    segmentList_->setObjectName(QStringLiteral("segmentList"));
    segmentList_->setSelectionMode(QAbstractItemView::SingleSelection);
    segmentList_->setDragDropMode(QAbstractItemView::InternalMove);
    segmentList_->setDefaultDropAction(Qt::MoveAction);
    segmentList_->setDragEnabled(true);
    segmentList_->setAcceptDrops(true);
    segmentList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    segmentList_->setSpacing(5);
    sidebarLayout->addWidget(segmentList_, 1);

    auto* generationLabel = mutedLabel(
        QStringLiteral("生成成功后自动播放 · PCM WAV"), sidebar);
    generationLabel->setAlignment(Qt::AlignCenter);
    sidebarLayout->addWidget(generationLabel);
    generateSelectedButton_ =
        new QPushButton(QStringLiteral("生成并试听选中题组"), sidebar);
    generateSelectedButton_->setObjectName(QStringLiteral("generateSelectedButton"));
    generateSelectedButton_->setToolTip(QStringLiteral("生成 WAV 后立即非循环播放一次"));
    generateSelectedButton_->setProperty("accentButton", true);
    generateAllButton_ = new QPushButton(QStringLiteral("生成并播放整套"), sidebar);
    generateAllButton_->setToolTip(QStringLiteral("生成整套 WAV 后立即播放"));
    generateAllButton_->setProperty("primaryButton", true);
    sidebarLayout->addWidget(generateSelectedButton_);
    sidebarLayout->addWidget(generateAllButton_);

    auto* workspace = new QWidget(bodySplitter);
    workspace->setObjectName(QStringLiteral("workspace"));
    auto* workspaceLayout = new QVBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(0);
    workspaceScroll_ = new QScrollArea(workspace);
    workspaceScroll_->setObjectName(QStringLiteral("workspaceScroll"));
    workspaceScroll_->setWidgetResizable(true);
    workspaceScroll_->setFrameShape(QFrame::NoFrame);
    auto* scrollContents = new QWidget(workspaceScroll_);
    scrollContents->setObjectName(QStringLiteral("scrollContents"));
    auto* contentLayout = new QVBoxLayout(scrollContents);
    contentLayout->setContentsMargins(34, 28, 38, 32);
    contentLayout->setSpacing(18);

    pageEyebrowLabel_ = new QLabel(QStringLiteral("简洁工作流 · 第 1 步"), scrollContents);
    pageEyebrowLabel_->setObjectName(QStringLiteral("eyebrow"));
    contentLayout->addWidget(pageEyebrowLabel_);
    auto* titleRow = new QHBoxLayout;
    pageTitleLabel_ = new QLabel(QStringLiteral("准备听力文稿"), scrollContents);
    pageTitleLabel_->setObjectName(QStringLiteral("pageTitle"));
    currentGroupBadge_ = new QLabel(scrollContents);
    currentGroupBadge_->setObjectName(QStringLiteral("currentGroupBadge"));
    titleRow->addWidget(pageTitleLabel_);
    titleRow->addWidget(currentGroupBadge_);
    titleRow->addStretch();
    contentLayout->addLayout(titleRow);
    pageDescriptionLabel_ = mutedLabel(
        QStringLiteral("直接编辑题组文稿，或从一个教学情景开始构思。"), scrollContents);
    contentLayout->addWidget(pageDescriptionLabel_);

    scenarioGeneratorCard_ = makeCard(scrollContents);
    scenarioGeneratorCard_->setObjectName(QStringLiteral("scenarioGeneratorContainer"));
    auto* scenarioLayout = new QVBoxLayout(scenarioGeneratorCard_);
    scenarioLayout->setContentsMargins(22, 18, 22, 18);
    scenarioLayout->setSpacing(10);
    auto* scenarioHeader = new QHBoxLayout;
    auto* scenarioText = new QVBoxLayout;
    scenarioText->setSpacing(3);
    auto* scenarioEyebrow = new QLabel(QStringLiteral("01  文稿"), scenarioGeneratorCard_);
    scenarioEyebrow->setObjectName(QStringLiteral("cardStepLabel"));
    scenarioText->addWidget(scenarioEyebrow);
    scenarioText->addWidget(
        sectionTitle(QStringLiteral("从一个情景开始"), scenarioGeneratorCard_));
    scenarioText->addWidget(mutedLabel(
        QStringLiteral("输入英文题干、三个选项与答案，快速得到男女对话、答案证据和可编辑初稿。"),
        scenarioGeneratorCard_));
    smartScenarioButton_ =
        new QPushButton(QStringLiteral("✦  智能生成情景"), scenarioGeneratorCard_);
    smartScenarioButton_->setObjectName(QStringLiteral("smartScenarioButton"));
    smartScenarioButton_->setProperty("primaryButton", true);
    smartScenarioButton_->setMinimumWidth(178);
    scenarioHeader->addLayout(scenarioText, 1);
    scenarioHeader->addWidget(smartScenarioButton_, 0, Qt::AlignVCenter);
    scenarioLayout->addLayout(scenarioHeader);
    contentLayout->addWidget(scenarioGeneratorCard_);

    globalSettingsCard_ = makeCard(scrollContents);
    auto* globalLayout = new QVBoxLayout(globalSettingsCard_);
    globalLayout->setContentsMargins(22, 20, 22, 20);
    globalLayout->setSpacing(14);
    globalLayout->addWidget(
        sectionTitle(QStringLiteral("02–03  声音与节奏"), globalSettingsCard_));
    globalLayout->addWidget(mutedLabel(
        QStringLiteral("口音地区与角色性别会逐项核验；自然度取决于本机安装的真实音色，修改后需重新生成。"),
        globalSettingsCard_));
    auto* globalGrid = new QGridLayout;
    globalGrid->setHorizontalSpacing(22);
    globalGrid->setVerticalSpacing(8);
    auto* accentLabel = new QLabel(QStringLiteral("英语口音"), globalSettingsCard_);
    accentLabel->setObjectName(QStringLiteral("fieldLabel"));
    auto* wpmLabel = new QLabel(QStringLiteral("目标语速"), globalSettingsCard_);
    wpmLabel->setObjectName(QStringLiteral("fieldLabel"));
    globalGrid->addWidget(accentLabel, 0, 0, 1, 2);
    accentCombo_ = new QComboBox(globalSettingsCard_);
    accentCombo_->addItem(QStringLiteral("美式英语  en-US"));
    accentCombo_->addItem(QStringLiteral("英式英语  en-GB"));
    auto* wpmRow = new QHBoxLayout;
    wpmSlider_ = new QSlider(Qt::Horizontal, globalSettingsCard_);
    wpmSlider_->setRange(40, 300);
    wpmSlider_->setSingleStep(5);
    wpmSpin_ = new QSpinBox(globalSettingsCard_);
    wpmSpin_->setRange(40, 300);
    wpmSpin_->setSuffix(QStringLiteral(" WPM"));
    wpmSpin_->setMinimumWidth(108);
    wpmRow->addWidget(wpmSlider_, 1);
    wpmRow->addWidget(wpmSpin_);
    globalGrid->addWidget(accentCombo_, 1, 0, 1, 2);
    globalGrid->addWidget(wpmLabel, 2, 0, 1, 2);
    globalGrid->addLayout(wpmRow, 3, 0, 1, 2);

    auto* maleVoiceLabel = new QLabel(QStringLiteral("男声音色"), globalSettingsCard_);
    auto* femaleVoiceLabel = new QLabel(QStringLiteral("女声音色"), globalSettingsCard_);
    maleVoiceLabel->setObjectName(QStringLiteral("fieldLabel"));
    femaleVoiceLabel->setObjectName(QStringLiteral("fieldLabel"));
    globalGrid->addWidget(maleVoiceLabel, 4, 0, 1, 2);
    auto* maleVoiceRow = new QHBoxLayout;
    auto* femaleVoiceRow = new QHBoxLayout;
    maleVoiceCombo_ = new QComboBox(globalSettingsCard_);
    femaleVoiceCombo_ = new QComboBox(globalSettingsCard_);
    for (auto* combo : {accentCombo_, maleVoiceCombo_, femaleVoiceCombo_}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(8);
        combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    maleVoiceCombo_->setObjectName(QStringLiteral("maleVoiceCombo"));
    femaleVoiceCombo_->setObjectName(QStringLiteral("femaleVoiceCombo"));
    previewMaleVoiceButton_ = new QPushButton(QStringLiteral("试听"), globalSettingsCard_);
    previewFemaleVoiceButton_ = new QPushButton(QStringLiteral("试听"), globalSettingsCard_);
    previewMaleVoiceButton_->setProperty("quietButton", true);
    previewFemaleVoiceButton_->setProperty("quietButton", true);
    previewMaleVoiceButton_->setToolTip(QStringLiteral("使用当前男声音色朗读一条标准句"));
    previewFemaleVoiceButton_->setToolTip(QStringLiteral("使用当前女声音色朗读一条标准句"));
    maleVoiceRow->addWidget(maleVoiceCombo_, 1);
    maleVoiceRow->addWidget(previewMaleVoiceButton_);
    femaleVoiceRow->addWidget(femaleVoiceCombo_, 1);
    femaleVoiceRow->addWidget(previewFemaleVoiceButton_);
    globalGrid->addLayout(maleVoiceRow, 5, 0, 1, 2);
    globalGrid->addWidget(femaleVoiceLabel, 6, 0, 1, 2);
    globalGrid->addLayout(femaleVoiceRow, 7, 0, 1, 2);

    strictAccentCheck_ = new QCheckBox(QStringLiteral("严格校验口音地区，不冒充英音/美音"),
                                       globalSettingsCard_);
    strictAccentCheck_->setChecked(true);
    allowGenderFallbackCheck_ = new QCheckBox(
        QStringLiteral("目标性别缺失时允许回退（会明确标注）"), globalSettingsCard_);
    allowGenderFallbackCheck_->setObjectName(QStringLiteral("allowGenderFallbackCheck"));
    allowGenderFallbackCheck_->setChecked(false);
    globalGrid->addWidget(strictAccentCheck_, 8, 0, 1, 2);
    globalGrid->addWidget(allowGenderFallbackCheck_, 9, 0, 1, 2);

    voiceAuditLabel_ = mutedLabel(QString(), globalSettingsCard_);
    voiceAuditLabel_->setObjectName(QStringLiteral("voiceAuditLabel"));
    voiceAuditLabel_->setMinimumWidth(0);
    voiceAuditLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    globalGrid->addWidget(voiceAuditLabel_, 10, 0, 1, 2);
    auto* voicePackTools = new QHBoxLayout;
    auto* openVoicePacksButton = new QPushButton(QStringLiteral("打开声音包目录"), globalSettingsCard_);
    auto* rescanVoicePacksButton = new QPushButton(QStringLiteral("重新扫描音色"), globalSettingsCard_);
    openVoicePacksButton->setProperty("quietButton", true);
    rescanVoicePacksButton->setProperty("quietButton", true);
    voicePackTools->addWidget(openVoicePacksButton);
    voicePackTools->addWidget(rescanVoicePacksButton);
    voicePackTools->addStretch();
    globalGrid->addLayout(voicePackTools, 11, 0, 1, 2);
    connect(openVoicePacksButton, &QPushButton::clicked, this, [this] {
        const QString path = QDir(qEnvironmentVariable("LOCALAPPDATA"))
                                 .filePath(QStringLiteral("Audiloquy/voice-packs"));
        QDir().mkpath(path);
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        showStatus(QStringLiteral("已打开本地声音包目录；清单与运行时不会随工程上传"));
    });
    connect(rescanVoicePacksButton, &QPushButton::clicked, this, [this] {
        refreshVoiceOptions();
        commitProjectSettings();
        showStatus(QStringLiteral("已重新扫描 Windows 音色与本地声音包"));
    });
    auto* durationCaption = mutedLabel(QStringLiteral("预估整套时长"), globalSettingsCard_);
    totalDurationLabel_ = new QLabel(globalSettingsCard_);
    totalDurationLabel_->setObjectName(QStringLiteral("metricPill"));
    globalGrid->addWidget(durationCaption, 12, 0);
    globalGrid->addWidget(totalDurationLabel_, 12, 1, Qt::AlignRight);
    globalGrid->setColumnStretch(0, 1);
    globalGrid->setColumnStretch(1, 1);
    globalLayout->addLayout(globalGrid);
    editorCard_ = makeCard(scrollContents);
    auto* editorLayout = new QVBoxLayout(editorCard_);
    editorLayout->setContentsMargins(22, 20, 22, 22);
    editorLayout->setSpacing(13);
    editorLayout->addWidget(sectionTitle(QStringLiteral("选中题组文稿"), editorCard_));
    editorLayout->addWidget(mutedLabel(QStringLiteral("题号是播放与循环的语义锚点；角色会随工程一起保存。"), editorCard_));

    auto* fieldsGrid = new QGridLayout;
    fieldsGrid->setHorizontalSpacing(18);
    fieldsGrid->setVerticalSpacing(8);
    auto* questionLabelWidget = new QLabel(QStringLiteral("题号范围"), editorCard_);
    questionLabelWidget->setObjectName(QStringLiteral("fieldLabel"));
    auto* speakerLabelWidget = new QLabel(QStringLiteral("朗读角色"), editorCard_);
    speakerLabelWidget->setObjectName(QStringLiteral("fieldLabel"));
    fieldsGrid->addWidget(questionLabelWidget, 0, 0);
    fieldsGrid->addWidget(speakerLabelWidget, 0, 1);
    auto* questionRow = new QHBoxLayout;
    questionStartSpin_ = new QSpinBox(editorCard_);
    questionStartSpin_->setRange(1, 100000);
    questionEndSpin_ = new QSpinBox(editorCard_);
    questionEndSpin_->setRange(1, 100000);
    questionRow->addWidget(new QLabel(QStringLiteral("第"), editorCard_));
    questionRow->addWidget(questionStartSpin_);
    questionRow->addWidget(new QLabel(QStringLiteral("至"), editorCard_));
    questionRow->addWidget(questionEndSpin_);
    questionRow->addWidget(new QLabel(QStringLiteral("题"), editorCard_));
    speakerCombo_ = new QComboBox(editorCard_);
    speakerCombo_->setEditable(true);
    speakerCombo_->addItems({QStringLiteral("Narrator"), QStringLiteral("Woman"),
                             QStringLiteral("Man"), QStringLiteral("Student"),
                             QStringLiteral("Teacher")});
    fieldsGrid->addLayout(questionRow, 1, 0);
    fieldsGrid->addWidget(speakerCombo_, 1, 1);
    fieldsGrid->setColumnStretch(0, 1);
    fieldsGrid->setColumnStretch(1, 1);
    editorLayout->addLayout(fieldsGrid);

    auto* textHeader = new QHBoxLayout;
    auto* textLabel = new QLabel(QStringLiteral("英文台词"), editorCard_);
    textLabel->setObjectName(QStringLiteral("fieldLabel"));
    wordCountLabel_ = mutedLabel(QString(), editorCard_);
    textHeader->addWidget(textLabel);
    textHeader->addStretch();
    textHeader->addWidget(wordCountLabel_);
    editorLayout->addLayout(textHeader);
    scriptEdit_ = new QTextEdit(editorCard_);
    scriptEdit_->setObjectName(QStringLiteral("scriptEdit"));
    scriptEdit_->setAcceptRichText(false);
    scriptEdit_->setPlaceholderText(QStringLiteral("Paste or type the English listening script here..."));
    scriptEdit_->setMinimumHeight(175);
    editorLayout->addWidget(scriptEdit_);

    auto* ruleGrid = new QGridLayout;
    ruleGrid->setHorizontalSpacing(18);
    ruleGrid->setVerticalSpacing(8);
    auto* pauseLabel = new QLabel(QStringLiteral("每遍后停顿"), editorCard_);
    pauseLabel->setObjectName(QStringLiteral("fieldLabel"));
    auto* repeatLabel = new QLabel(QStringLiteral("朗读次数"), editorCard_);
    repeatLabel->setObjectName(QStringLiteral("fieldLabel"));
    auto* estimateLabel = new QLabel(QStringLiteral("题组预估时长"), editorCard_);
    estimateLabel->setObjectName(QStringLiteral("fieldLabel"));
    ruleGrid->addWidget(pauseLabel, 0, 0);
    ruleGrid->addWidget(repeatLabel, 0, 1);
    ruleGrid->addWidget(estimateLabel, 0, 2);
    pauseSpin_ = new QDoubleSpinBox(editorCard_);
    pauseSpin_->setRange(0.0, 3600.0);
    pauseSpin_->setDecimals(1);
    pauseSpin_->setSingleStep(0.5);
    pauseSpin_->setSuffix(QStringLiteral(" 秒"));
    repeatSpin_ = new QSpinBox(editorCard_);
    repeatSpin_->setRange(1, 100);
    repeatSpin_->setSuffix(QStringLiteral(" 遍"));
    segmentDurationLabel_ = new QLabel(editorCard_);
    segmentDurationLabel_->setObjectName(QStringLiteral("metricPill"));
    ruleGrid->addWidget(pauseSpin_, 1, 0);
    ruleGrid->addWidget(repeatSpin_, 1, 1);
    ruleGrid->addWidget(segmentDurationLabel_, 1, 2, Qt::AlignLeft);
    ruleGrid->setColumnStretch(0, 1);
    ruleGrid->setColumnStretch(1, 1);
    ruleGrid->setColumnStretch(2, 1);
    editorLayout->addLayout(ruleGrid);
    contentLayout->addWidget(editorCard_);
    contentLayout->addWidget(globalSettingsCard_);

    professionalCard_ = makeCard(scrollContents);
    professionalCard_->setObjectName(QStringLiteral("professionalCard"));
    auto* professionalLayout = new QVBoxLayout(professionalCard_);
    professionalLayout->setContentsMargins(22, 20, 22, 22);
    professionalLayout->setSpacing(12);
    professionalLayout->addWidget(sectionTitle(QStringLiteral("专业检查器"), professionalCard_));
    professionalLayout->addWidget(mutedLabel(QStringLiteral("查看稳定 ID、合成约束与最近输出。简洁/专业模式共用同一份工程数据。"), professionalCard_));
    auto* professionalGrid = new QGridLayout;
    professionalGrid->setHorizontalSpacing(22);
    professionalGrid->setVerticalSpacing(9);
    professionalGrid->addWidget(mutedLabel(QStringLiteral("题组稳定 ID"), professionalCard_), 0, 0);
    professionalGrid->addWidget(mutedLabel(QStringLiteral("合成格式"), professionalCard_), 1, 0);
    professionalGrid->addWidget(mutedLabel(QStringLiteral("声音选择"), professionalCard_), 2, 0);
    professionalGrid->addWidget(mutedLabel(QStringLiteral("生成审阅"), professionalCard_), 3, 0);
    professionalGrid->addWidget(mutedLabel(QStringLiteral("最近输出"), professionalCard_), 4, 0);
    segmentIdLabel_ = new QLabel(professionalCard_);
    pcmFormatLabel_ = new QLabel(QStringLiteral("PCM16 · 44,100 Hz · Mono"), professionalCard_);
    voiceHintLabel_ = new QLabel(professionalCard_);
    voiceHintLabel_->setMinimumWidth(0);
    voiceHintLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    voiceHintLabel_->setWordWrap(true);
    generationAuditLabel_ = new QLabel(QStringLiteral("手工文稿 · 无生成记录"), professionalCard_);
    generationAuditLabel_->setMinimumWidth(0);
    generationAuditLabel_->setWordWrap(true);
    outputPathLabel_ = new QLabel(QStringLiteral("尚未生成"), professionalCard_);
    outputPathLabel_->setObjectName(QStringLiteral("outputPathLabel"));
    outputPathLabel_->setMinimumWidth(0);
    outputPathLabel_->setWordWrap(true);
    professionalGrid->addWidget(segmentIdLabel_, 0, 1);
    professionalGrid->addWidget(pcmFormatLabel_, 1, 1);
    professionalGrid->addWidget(voiceHintLabel_, 2, 1);
    professionalGrid->addWidget(generationAuditLabel_, 3, 1);
    professionalGrid->addWidget(outputPathLabel_, 4, 1);
    professionalGrid->setColumnStretch(1, 1);
    professionalLayout->addLayout(professionalGrid);
    teacherReviewedCheck_ = new QCheckBox(
        QStringLiteral("我已核对题干、原文证据与答案唯一性"), professionalCard_);
    teacherReviewedCheck_->setObjectName(QStringLiteral("teacherReviewedCheck"));
    teacherReviewedCheck_->setEnabled(false);
    professionalLayout->addWidget(teacherReviewedCheck_);
    contentLayout->addWidget(professionalCard_);
    professionalCard_->setVisible(false);

    outputCard_ = makeCard(scrollContents);
    auto* outputLayout = new QHBoxLayout(outputCard_);
    outputLayout->setContentsMargins(22, 18, 22, 18);
    outputLayout->setSpacing(14);
    auto* outputText = new QVBoxLayout;
    outputText->setSpacing(3);
    outputText->addWidget(sectionTitle(QStringLiteral("04  生成与试听"), outputCard_));
    outputText->addWidget(mutedLabel(
        QStringLiteral("生成完成后会自动播放；未保存工程默认写入当前目录的 ListeningStudioResults。"),
        outputCard_));
    outputLayout->addLayout(outputText, 1);
    auto* openOutputButton = new QPushButton(QStringLiteral("打开输出文件夹"), outputCard_);
    packageButton_ = new QPushButton(QStringLiteral("打包带走"), outputCard_);
    cancelRenderButton_ = new QPushButton(QStringLiteral("取消生成"), outputCard_);
    cancelRenderButton_->setObjectName(QStringLiteral("cancelRenderButton"));
    packageButton_->setObjectName(QStringLiteral("packageProjectButton"));
    openOutputButton->setProperty("quietButton", true);
    packageButton_->setProperty("primaryButton", true);
    cancelRenderButton_->setProperty("quietButton", true);
    cancelRenderButton_->setEnabled(false);
    outputLayout->addWidget(openOutputButton);
    outputLayout->addWidget(packageButton_);
    outputLayout->addWidget(cancelRenderButton_);
    connect(openOutputButton, &QPushButton::clicked, this, [this] {
        try {
            const auto path = outputDirectory();
            std::filesystem::create_directories(path);
            QDesktopServices::openUrl(QUrl::fromLocalFile(qPath(path)));
        } catch (const std::exception& error) {
            showFailure(QStringLiteral("无法打开输出文件夹"), error);
        }
    });
    contentLayout->addWidget(outputCard_);
    contentLayout->addStretch();

    workspaceScroll_->setWidget(scrollContents);
    workspaceLayout->addWidget(workspaceScroll_);

    inspectorScroll_ = new QScrollArea(bodySplitter);
    inspectorScroll_->setObjectName(QStringLiteral("inspectorScroll"));
    inspectorScroll_->setWidgetResizable(true);
    inspectorScroll_->setFrameShape(QFrame::NoFrame);
    inspectorScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    inspectorScroll_->setMinimumWidth(320);
    inspectorScroll_->setMaximumWidth(430);
    auto* inspectorContents = new QWidget(inspectorScroll_);
    inspectorContents->setObjectName(QStringLiteral("inspectorContents"));
    inspectorContents->setMinimumWidth(0);
    inspectorContents->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* inspectorLayout = new QVBoxLayout(inspectorContents);
    inspectorLayout->setContentsMargins(14, 18, 18, 18);
    inspectorLayout->setSpacing(14);
    auto* inspectorTitle = sectionTitle(QStringLiteral("题组设置与声音核验"), inspectorContents);
    inspectorTitle->setObjectName(QStringLiteral("inspectorTitle"));
    inspectorLayout->addWidget(inspectorTitle);
    for (QFrame* card : {globalSettingsCard_, professionalCard_, outputCard_}) {
        card->setMinimumWidth(0);
        card->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    inspectorLayout->addWidget(globalSettingsCard_);
    inspectorLayout->addWidget(professionalCard_);
    inspectorLayout->addWidget(outputCard_);
    inspectorLayout->addStretch();
    inspectorScroll_->setWidget(inspectorContents);

    bodySplitter->addWidget(sidebar);
    bodySplitter->addWidget(workspace);
    bodySplitter->addWidget(inspectorScroll_);
    bodySplitter->setStretchFactor(0, 0);
    bodySplitter->setStretchFactor(1, 1);
    bodySplitter->setStretchFactor(2, 0);
    bodySplitter->setSizes({270, 720, 350});
    rootLayout->addWidget(bodySplitter, 1);

    playerBar_ = new QFrame(root);
    playerBar_->setObjectName(QStringLiteral("playerBar"));
    playerBar_->setProperty("activeStep", false);
    playerBar_->setFocusPolicy(Qt::StrongFocus);
    playerBar_->setFixedHeight(112);
    auto* playerLayout = new QVBoxLayout(playerBar_);
    playerLayout->setContentsMargins(24, 10, 24, 9);
    playerLayout->setSpacing(5);
    auto* playbackTopRow = new QHBoxLayout;
    playbackTopRow->setSpacing(8);
    auto* playbackText = new QVBoxLayout;
    playbackText->setSpacing(2);
    playbackTitleLabel_ = new QLabel(QStringLiteral("课堂播放尚未开始"), playerBar_);
    playbackTitleLabel_->setObjectName(QStringLiteral("playbackTitle"));
    playbackDetailLabel_ =
        mutedLabel(QStringLiteral("05  课堂 · 生成后可按题组播放、循环或播放整套"), playerBar_);
    playbackText->addWidget(playbackTitleLabel_);
    playbackText->addWidget(playbackDetailLabel_);
    playbackTopRow->addLayout(playbackText, 1);
    previousButton_ = new QPushButton(QStringLiteral("上一组"), playerBar_);
    playSelectedButton_ = new QPushButton(QStringLiteral("▶  播放选中"), playerBar_);
    loopSelectedButton_ = new QPushButton(QStringLiteral("↻  循环选中"), playerBar_);
    playAllButton_ = new QPushButton(QStringLiteral("播放整套"), playerBar_);
    pauseButton_ = new QPushButton(QStringLiteral("Ⅱ  暂停"), playerBar_);
    pauseButton_->setObjectName(QStringLiteral("pausePlaybackButton"));
    stopButton_ = new QPushButton(QStringLiteral("■  停止"), playerBar_);
    stopButton_->setObjectName(QStringLiteral("stopPlaybackButton"));
    nextButton_ = new QPushButton(QStringLiteral("下一组"), playerBar_);
    playSelectedButton_->setProperty("primaryButton", true);
    loopSelectedButton_->setProperty("accentButton", true);
    for (auto* button : {previousButton_, playAllButton_, pauseButton_, stopButton_, nextButton_}) {
        button->setProperty("quietButton", true);
    }
    playbackTopRow->addWidget(previousButton_);
    playbackTopRow->addWidget(playSelectedButton_);
    playbackTopRow->addWidget(loopSelectedButton_);
    playbackTopRow->addWidget(playAllButton_);
    playbackTopRow->addWidget(pauseButton_);
    playbackTopRow->addWidget(stopButton_);
    playbackTopRow->addWidget(nextButton_);
    playerLayout->addLayout(playbackTopRow);

    auto* progressRow = new QHBoxLayout;
    progressRow->setSpacing(10);
    playbackTimeLabel_ = new QLabel(QStringLiteral("0 秒 / 0 秒"), playerBar_);
    playbackTimeLabel_->setObjectName(QStringLiteral("playbackTime"));
    playbackTimeLabel_->setMinimumWidth(96);
    playbackSlider_ = new QSlider(Qt::Horizontal, playerBar_);
    playbackSlider_->setObjectName(QStringLiteral("playbackSlider"));
    playbackSlider_->setRange(0, 0);
    playbackSlider_->setEnabled(false);
    progressRow->addWidget(playbackTimeLabel_);
    progressRow->addWidget(playbackSlider_, 1);
    playerLayout->addLayout(progressRow);
    rootLayout->addWidget(playerBar_);

    auto* statusBarFrame = new QFrame(root);
    statusBarFrame->setObjectName(QStringLiteral("statusBarFrame"));
    statusBarFrame->setFixedHeight(30);
    auto* statusLayout = new QHBoxLayout(statusBarFrame);
    statusLayout->setContentsMargins(24, 0, 24, 0);
    statusLabel_ = new QLabel(QStringLiteral("●  就绪"), statusBarFrame);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    statusLayout->addWidget(statusLabel_);
    statusLayout->addStretch();
    auto* localOnly = new QLabel(
        QStringLiteral("本地优先  ·  音色来源与回退可审计"), statusBarFrame);
    localOnly->setObjectName(QStringLiteral("statusMeta"));
    statusLayout->addWidget(localOnly);
    rootLayout->addWidget(statusBarFrame);

    setCentralWidget(root);
}

void MainWindow::connectUi() {
    connect(minimizeButton_, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(maximizeButton_, &QPushButton::clicked, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
        maximizeButton_->setText(isMaximized() ? QStringLiteral("❐") : QStringLiteral("□"));
    });
    connect(closeButton_, &QPushButton::clicked, this, &QWidget::close);
    connect(manuscriptStepButton_, &QPushButton::clicked, this,
            [this] { activateWorkflowStep(0); });
    connect(voiceStepButton_, &QPushButton::clicked, this,
            [this] { activateWorkflowStep(1); });
    connect(rhythmStepButton_, &QPushButton::clicked, this,
            [this] { activateWorkflowStep(2); });
    connect(generateStepButton_, &QPushButton::clicked, this,
            [this] { activateWorkflowStep(3); });
    connect(classroomStepButton_, &QPushButton::clicked, this,
            [this] { activateWorkflowStep(4); });
    connect(smartScenarioButton_, &QPushButton::clicked, this,
            [this] { openScenarioGenerator(); });
    connect(simpleModeButton_, &QPushButton::clicked, this,
            [this] { setDisplayMode(DisplayMode::Simple); });
    connect(professionalModeButton_, &QPushButton::clicked, this,
            [this] { setDisplayMode(DisplayMode::Professional); });
    connect(openButton_, &QPushButton::clicked, this, [this] { openProjectFromDisk(); });
    connect(saveButton_, &QPushButton::clicked, this,
            [this] { (void)saveProjectToDisk(false); });
    connect(exampleButton_, &QPushButton::clicked, this,
            [this] { loadExampleProject(true); });
    connect(newProjectButton_, &QPushButton::clicked, this,
            [this] { newBlankProject(true); });
    connect(addSegmentButton_, &QPushButton::clicked, this,
            [this] { addSegment(); });
    connect(duplicateSegmentButton_, &QPushButton::clicked, this,
            [this] { duplicateSegment(); });
    connect(deleteSegmentButton_, &QPushButton::clicked, this,
            [this] { deleteSegment(); });
    connect(moveUpButton_, &QPushButton::clicked, this,
            [this] { moveSegment(-1); });
    connect(moveDownButton_, &QPushButton::clicked, this,
            [this] { moveSegment(1); });
    connect(undoButton_, &QPushButton::clicked, this,
            [this] { undoEdit(); });
    connect(redoButton_, &QPushButton::clicked, this,
            [this] { redoEdit(); });
    connect(packageButton_, &QPushButton::clicked, this,
            [this] { packageProjectToDisk(); });

    connect(segmentList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (loadingUi_ || row < 0) {
            return;
        }
        commitCurrentSegment();
        if (auto* item = segmentList_->item(row)) {
            currentSegmentId_ = utf8(item->data(Qt::UserRole).toString());
        }
        loadCurrentSegment();
        updateMetrics();
        updateAudioAvailability();
    });
    connect(segmentList_, &QListWidget::itemPressed, this, [this](QListWidgetItem*) {
        if (loadingUi_ || busy_) {
            return;
        }
        commitProjectSettings();
        commitCurrentSegment();
        dragBeforeProject_ = project_;
        dragBeforeSelection_ = currentSegmentId_;
    });
    connect(segmentList_->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex&, int, int, const QModelIndex&, int) {
                if (loadingUi_ || busy_) {
                    dragBeforeProject_.reset();
                    return;
                }
                const Project before = dragBeforeProject_.has_value()
                                           ? *dragBeforeProject_
                                           : project_;
                const std::string beforeSelection = dragBeforeProject_.has_value()
                                                        ? dragBeforeSelection_
                                                        : currentSegmentId_;
                commitListOrderFromUi();
                dragBeforeProject_.reset();
                dragBeforeSelection_.clear();
                recordStructuralEdit(before, beforeSelection);
            });

    connect(projectTitleEdit_, &QLineEdit::textEdited, this, [this] {
        if (!loadingUi_) {
            commitProjectSettings();
            scheduleHistoryBaseline();
            setDirty();
        }
    });
    connect(accentCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (loadingUi_) {
            return;
        }
        commitProjectSettings();
        refreshVoiceOptions();
        invalidateAllRenderedAudio();
        scheduleHistoryBaseline();
        setDirty();
        updateMetrics();
        updateAudioAvailability();
    });
    const auto voiceSelectionChanged = [this] {
        if (loadingUi_) {
            return;
        }
        commitProjectSettings();
        invalidateAllRenderedAudio();
        scheduleHistoryBaseline();
        setDirty();
        updateAudioAvailability();
    };
    connect(maleVoiceCombo_, &QComboBox::currentIndexChanged, this,
            [voiceSelectionChanged](int) { voiceSelectionChanged(); });
    connect(femaleVoiceCombo_, &QComboBox::currentIndexChanged, this,
            [voiceSelectionChanged](int) { voiceSelectionChanged(); });
    connect(strictAccentCheck_, &QCheckBox::toggled, this, [this, voiceSelectionChanged](bool) {
        refreshVoiceOptions();
        voiceSelectionChanged();
    });
    connect(allowGenderFallbackCheck_, &QCheckBox::toggled, this,
            [this, voiceSelectionChanged](bool) {
                refreshVoiceOptions();
                voiceSelectionChanged();
            });
    connect(previewMaleVoiceButton_, &QPushButton::clicked, this,
            [this] { startVoicePreview(platform::windows::VoiceGender::Male); });
    connect(previewFemaleVoiceButton_, &QPushButton::clicked, this,
            [this] { startVoicePreview(platform::windows::VoiceGender::Female); });
    connect(wpmSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        if (loadingUi_) {
            return;
        }
        const QSignalBlocker blocker(wpmSlider_);
        wpmSlider_->setValue(value);
        commitProjectSettings();
        invalidateAllRenderedAudio();
        scheduleHistoryBaseline();
        setDirty();
        updateMetrics();
        updateAudioAvailability();
    });
    connect(wpmSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (loadingUi_) {
            return;
        }
        wpmSpin_->setValue(value);
    });

    const auto segmentChanged = [this] {
        if (loadingUi_) {
            return;
        }
        commitCurrentSegment();
        if (!currentSegmentId_.empty()) {
            invalidateCurrentRenderedAudio();
        }
        scheduleHistoryBaseline();
        setDirty();
        updateSelectedListItem();
        updateMetrics();
        updateAudioAvailability();
    };
    connect(questionStartSpin_, &QSpinBox::valueChanged, this, [this, segmentChanged](int value) {
        if (!loadingUi_ && questionEndSpin_->value() < value) {
            questionEndSpin_->setValue(value);
        }
        segmentChanged();
    });
    connect(questionEndSpin_, &QSpinBox::valueChanged, this, [segmentChanged](int) {
        segmentChanged();
    });
    connect(speakerCombo_, &QComboBox::currentTextChanged, this, [segmentChanged](const QString&) {
        segmentChanged();
    });
    connect(scriptEdit_, &QTextEdit::textChanged, this, segmentChanged);
    connect(pauseSpin_, &QDoubleSpinBox::valueChanged, this, [segmentChanged](double) {
        segmentChanged();
    });
    connect(repeatSpin_, &QSpinBox::valueChanged, this, [segmentChanged](int) {
        segmentChanged();
    });
    connect(teacherReviewedCheck_, &QCheckBox::toggled, this, [this](bool reviewed) {
        if (loadingUi_) {
            return;
        }
        Segment* segment = currentSegment();
        if (segment == nullptr || !segment->generation.has_value()) {
            return;
        }
        segment->generation->teacherReviewed = reviewed;
        segment->generation->requiresTeacherReview = !reviewed;
        history_.adoptCurrent(project_, currentSegmentId_);
        setDirty();
        loadCurrentSegment();
    });

    connect(generateSelectedButton_, &QPushButton::clicked, this, [this] {
        activateWorkflowStep(3);
        startRender(RenderScope::Selected);
    });
    connect(generateAllButton_, &QPushButton::clicked, this, [this] {
        activateWorkflowStep(3);
        startRender(RenderScope::All);
    });
    connect(cancelRenderButton_, &QPushButton::clicked, this,
            [this] { cancelRender(); });
    connect(&renderJob_, &RenderJobController::progressChanged, this,
            [this](const RenderJobProgress& progress) { handleRenderProgress(progress); });
    connect(&renderJob_, &RenderJobController::finished, this,
            [this](const RenderJobResult& result) { handleRenderFinished(result); });
    connect(playSelectedButton_, &QPushButton::clicked, this,
            [this] {
                activateWorkflowStep(4);
                playSelected(false);
            });
    connect(loopSelectedButton_, &QPushButton::clicked, this,
            [this] {
                activateWorkflowStep(4);
                playSelected(true);
            });
    connect(playAllButton_, &QPushButton::clicked, this, [this] {
        activateWorkflowStep(4);
        playAll();
    });
    connect(pauseButton_, &QPushButton::clicked, this, [this] { togglePlaybackPause(); });
    connect(stopButton_, &QPushButton::clicked, this, [this] { stopPlayback(); });
    connect(previousButton_, &QPushButton::clicked, this, [this] { moveSelection(-1); });
    connect(nextButton_, &QPushButton::clicked, this, [this] { moveSelection(1); });
    connect(playbackSlider_, &QSlider::sliderPressed, this,
            [this] { playbackScrubbing_ = true; });
    connect(playbackSlider_, &QSlider::sliderReleased, this, [this] {
        seekPlaybackFromSlider();
        playbackScrubbing_ = false;
    });
    playbackTimer_ = new QTimer(this);
    playbackTimer_->setInterval(120);
    connect(playbackTimer_, &QTimer::timeout, this, [this] { pollPlayback(); });
    playbackTimer_->start();
    recoveryTimer_ = new QTimer(this);
    recoveryTimer_->setInterval(3000);
    connect(recoveryTimer_, &QTimer::timeout, this, [this] { (void)autosaveRecovery(); });
    recoveryTimer_->start();
    historyTimer_ = new QTimer(this);
    historyTimer_->setSingleShot(true);
    historyTimer_->setInterval(350);
    connect(historyTimer_, &QTimer::timeout, this, [this] {
        if (!loadingUi_ && !busy_) {
            commitProjectSettings();
            commitCurrentSegment();
            history_.adoptCurrent(project_, currentSegmentId_);
            updateAudioAvailability();
        }
    });

    auto* openShortcut = new QShortcut(QKeySequence::Open, this);
    connect(openShortcut, &QShortcut::activated, this, [this] { openProjectFromDisk(); });
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this,
            [this] { (void)saveProjectToDisk(false); });
    auto* saveAsShortcut = new QShortcut(QKeySequence::SaveAs, this);
    connect(saveAsShortcut, &QShortcut::activated, this,
            [this] { (void)saveProjectToDisk(true); });
    auto* stopShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(stopShortcut, &QShortcut::activated, this, [this] { stopPlayback(); });
}

void MainWindow::applyTheme() {
    setStyleSheet(QStringLiteral(R"CSS(
        * {
            font-family: "Microsoft YaHei UI", "Segoe UI";
            font-size: 10pt;
            color: #273047;
        }
        QWidget#appRoot, QWidget#workspace, QWidget#scrollContents,
        QWidget#inspectorContents, QScrollArea#inspectorScroll {
            background: #F7F5EF;
        }
        QWidget#appRoot {
            border: 1px solid #DCE2EC;
        }
        QFrame#topBar {
            background: #F7F5EF;
            border-bottom: 1px solid #DCE2EC;
        }
        QLabel#brandMark {
            color: #FFFFFF;
            background: transparent;
            border: none;
            font-size: 14pt;
            font-weight: 700;
        }
        QLabel#brandName { font-size: 14pt; font-weight: 700; color: #14213D; }
        QLabel#brandSub { font-size: 11pt; font-weight: 700; color: #14213D; }
        QLabel#brandTagline { font-size: 7.5pt; color: #8a8177; }
        QLabel#offlineBadge {
            color: #0E7F77;
            background: #E6F5F1;
            border: 1px solid #B9DED6;
            border-radius: 11px;
            padding: 4px 9px;
            font-size: 8.5pt;
            font-weight: 600;
        }
        QFrame#verticalDivider { background: #e7e1d7; }
        QLineEdit#projectTitleEdit {
            border: 1px solid transparent;
            background: transparent;
            font-weight: 600;
            padding: 8px 10px;
        }
        QLineEdit#projectTitleEdit:hover, QLineEdit#projectTitleEdit:focus {
            background: #faf8f3;
            border: 1px solid #e2ddd4;
            border-radius: 8px;
        }
        QFrame#modeSwitch { background: #efede8; border-radius: 9px; }
        QPushButton[modeButton="true"] {
            min-width: 54px;
            padding: 7px 12px;
            border: none;
            border-radius: 7px;
            background: transparent;
            color: #6d7180;
            font-weight: 600;
        }
        QPushButton[modeButton="true"]:checked {
            background: white;
            color: #4E5AC7;
        }
        QPushButton[windowButton="true"] {
            min-width: 42px;
            min-height: 42px;
            padding: 0;
            border: none;
            border-radius: 8px;
            background: transparent;
            color: #14213D;
            font-size: 13pt;
            font-weight: 400;
        }
        QPushButton[windowButton="true"]:hover { background: #E9ECF3; }
        QPushButton[closeButton="true"]:hover { color: white; background: #C94A55; }
        QFrame#workflowNav {
            background: #fffefa;
            border-bottom: 1px solid #e7e1d7;
        }
        QPushButton[workflowStep="true"] {
            min-height: 27px;
            padding: 7px 13px;
            border: 1px solid transparent;
            border-radius: 9px;
            background: transparent;
            color: #777b89;
            font-weight: 600;
        }
        QPushButton[workflowStep="true"]:hover {
            background: #f3f1ec;
            border-color: #e4dfd6;
            color: #4652b8;
        }
        QPushButton[workflowStep="true"]:checked {
            background: #eef0fc;
            border-color: #cfd4f5;
            color: #3f4baa;
        }
        QLabel#workflowChevron { color: #bbb5aa; font-size: 16pt; }
        QLabel#workflowHint { color: #948d82; font-size: 8.5pt; }
        QPushButton {
            border: 1px solid #ded9d0;
            border-radius: 8px;
            background: #fffefa;
            padding: 9px 14px;
            font-weight: 600;
        }
        QPushButton:hover { border-color: #b9bfe8; background: #f7f6fb; }
        QPushButton:pressed { background: #eceefa; }
        QPushButton:disabled { color: #a8b0bf; background: #f2f4f8; border-color: #e4e7ed; }
        QPushButton[compactButton="true"] {
            min-height: 26px;
            padding: 5px 4px;
            font-size: 8.5pt;
        }
        QPushButton[primaryButton="true"] {
            background: #4652b8;
            border-color: #4652b8;
            color: white;
        }
        QPushButton[primaryButton="true"]:hover { background: #38449f; border-color: #38449f; }
        QPushButton[accentButton="true"] {
            background: #e6f5f1;
            border-color: #b9ded6;
            color: #117c6e;
        }
        QPushButton[quietButton="true"] { background: #fffefa; color: #4d5569; }
        QPushButton[primaryButton="true"]:disabled,
        QPushButton[accentButton="true"]:disabled,
        QPushButton[quietButton="true"]:disabled {
            color: #a8b0bf;
            background: #f2f4f8;
            border-color: #e4e7ed;
        }
        QFrame#sidebar {
            background: #fffefa;
            border-right: 1px solid #e7e1d7;
        }
        QScrollArea#inspectorScroll { border-left: 1px solid #DCE2EC; }
        QLabel#inspectorTitle { color: #14213D; padding: 0 2px 2px 2px; }
        QSplitter#bodySplitter::handle { background: #e7e1d7; }
        QLabel#sectionTitle { font-size: 12pt; font-weight: 700; color: #29314d; }
        QLabel#mutedLabel { color: #7e7d80; font-size: 9pt; }
        QLabel#countBadge, QLabel#currentGroupBadge {
            background: #eef0fc;
            color: #4652b8;
            border: 1px solid #d5d8f2;
            border-radius: 9px;
            padding: 3px 9px;
            font-size: 9pt;
            font-weight: 600;
        }
        QListWidget#segmentList {
            background: transparent;
            border: none;
            outline: none;
        }
        QListWidget#segmentList::item {
            background: #f8f6f1;
            border: 1px solid #e8e2d9;
            border-radius: 10px;
            padding: 10px 11px;
            color: #455269;
        }
        QListWidget#segmentList::item:hover { background: #f3f2f8; border-color: #d9d7e6; }
        QListWidget#segmentList::item:selected {
            background: #eef0fc;
            border: 1px solid #bfc5ec;
            color: #3f4baa;
        }
        QScrollArea#workspaceScroll { border: none; background: #f7f5ef; }
        QLabel#eyebrow {
            color: #4652b8;
            font-size: 8pt;
            font-weight: 700;
            letter-spacing: 1px;
        }
        QLabel#pageTitle { font-size: 22pt; font-weight: 700; color: #20283e; }
        QFrame#card, QFrame#professionalCard, QFrame#scenarioGeneratorContainer {
            background: #fffefa;
            border: 1px solid #e5dfd5;
            border-radius: 13px;
        }
        QFrame#scenarioGeneratorContainer { border-color: #ccd0ed; background: #fbfbff; }
        QLabel#cardStepLabel { color: #117c6e; font-size: 8pt; font-weight: 700; letter-spacing: 1px; }
        QFrame#professionalCard { border-color: #cdd1ef; background: #fbfbff; }
        QLabel#fieldLabel { color: #465269; font-size: 9pt; font-weight: 600; }
        QLabel#voiceAuditLabel {
            color: #536078;
            background: #F3F7F6;
            border: 1px solid #D7E8E4;
            border-radius: 8px;
            padding: 8px 10px;
        }
        QCheckBox { color: #536078; spacing: 8px; font-size: 8.5pt; }
        QCheckBox::indicator {
            width: 16px; height: 16px; border: 1px solid #BCC5D5;
            border-radius: 4px; background: white;
        }
        QCheckBox::indicator:checked { background: #4E5AC7; border-color: #4E5AC7; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QTextEdit {
            background: #fbfaf7;
            border: 1px solid #dfdbd4;
            border-radius: 8px;
            padding: 8px 10px;
            selection-background-color: #4652b8;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QTextEdit:focus {
            background: white;
            border: 1px solid #7b84cf;
        }
        QComboBox { min-height: 20px; }
        QComboBox::drop-down { border: none; width: 26px; }
        QComboBox QAbstractItemView { background: #fffefa; border: 1px solid #dfdbd4; selection-background-color: #eef0fc; }
        QTextEdit#scriptEdit { font-family: "Segoe UI"; font-size: 11pt; line-height: 145%; }
        QSlider::groove:horizontal { height: 5px; background: #ddd9d1; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: #4652b8; border-radius: 2px; }
        QSlider::handle:horizontal { width: 16px; margin: -6px 0; background: #fffefa; border: 2px solid #4652b8; border-radius: 8px; }
        QLabel#metricPill {
            background: #eef0fc;
            color: #3c467d;
            border-radius: 8px;
            padding: 6px 10px;
            font-weight: 700;
        }
        QFrame#playerBar {
            background: #fffefa;
            border-top: 1px solid #ded8ce;
        }
        QFrame#playerBar[activeStep="true"] { background: #f3f8f6; border-top: 2px solid #159485; }
        QLabel#playbackTitle { color: #29314d; font-size: 11pt; font-weight: 700; }
        QLabel#playbackTime { color: #667187; font-size: 8pt; font-variant-numeric: tabular-nums; }
        QFrame#statusBarFrame { background: #F7F5EF; border-top: 1px solid #DCE2EC; }
        QLabel#statusLabel { color: #0E7F77; font-size: 8pt; }
        QLabel#statusMeta { color: #68748B; font-size: 8pt; }
        QScrollBar:vertical { background: transparent; width: 10px; margin: 3px; }
        QScrollBar::handle:vertical { background: #cbd2de; min-height: 36px; border-radius: 4px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )CSS"));
}

void MainWindow::loadExampleProject(bool askBeforeDiscard) {
    if (askBeforeDiscard && !confirmDiscardChanges()) {
        return;
    }
    stopPlayback();
    if (historyTimer_ != nullptr) {
        historyTimer_->stop();
    }

    Project example;
    example.id = "classroom-listening-demo";
    example.title = utf8(QStringLiteral("高中英语听力训练 · 示例"));
    example.accent = Accent::American;
    example.targetWpm = 130.0;
    example.segments = {
        Segment{"questions-01-02", {1, 2}, "Woman",
                "Good morning. I would like to book two tickets for the science museum this "
                "Saturday. Is the ten o'clock tour still available?",
                5.0, 2},
        Segment{"questions-03-04", {3, 4}, "Man",
                "I'm afraid that tour is full, but there are places on the eleven thirty tour. "
                "It lasts about ninety minutes and includes the new space exhibition.",
                6.0, 2},
        Segment{"questions-05-06", {5, 6}, "Narrator",
                "You now have ten seconds to check your answers to Questions Five and Six. "
                "After that, the conversation will be played one more time.",
                10.0, 1},
    };

    project_ = std::move(example);
    projectPath_.clear();
    currentSegmentId_ = project_.segments.front().id;
    history_.reset(project_, currentSegmentId_);
    dragBeforeProject_.reset();
    dragBeforeSelection_.clear();
    invalidateAllRenderedAudio();
    dirty_ = false;
    refreshProjectUi();
    setDirty(false);
    showStatus(QStringLiteral("已加载可编辑的示例工程"));
}

void MainWindow::newBlankProject(bool askBeforeDiscard) {
    if (askBeforeDiscard && !confirmDiscardChanges()) {
        return;
    }
    stopPlayback();
    if (historyTimer_ != nullptr) {
        historyTimer_->stop();
    }
    Project blank;
    blank.id = freshStableId("project");
    blank.title = utf8(QStringLiteral("未命名听力"));
    blank.accent = Accent::American;
    blank.targetWpm = 130.0;
    blank.segments.push_back(Segment{
        freshStableId("segment"), {1, 1}, "Narrator", {}, 5.0, 1});
    project_ = std::move(blank);
    projectPath_.clear();
    currentSegmentId_ = project_.segments.front().id;
    history_.reset(project_, currentSegmentId_);
    dragBeforeProject_.reset();
    dragBeforeSelection_.clear();
    invalidateAllRenderedAudio();
    dirty_ = askBeforeDiscard;
    refreshProjectUi();
    setDirty(dirty_);
    showStatus(QStringLiteral("已创建空白工程；填写文稿后即可生成"));
}

void MainWindow::recordStructuralEdit(const Project& before,
                                      const std::string& beforeSelection) {
    if (applyingHistory_ || project_ == before) {
        updateAudioAvailability();
        return;
    }
    const std::string baselineSelection = beforeSelection.empty()
                                              ? currentSegmentId_
                                              : beforeSelection;
    history_.recordTransition(before, baselineSelection, project_, currentSegmentId_);
    setDirty();
    refreshSegmentList();
    updateMetrics();
    updateAudioAvailability();
}

void MainWindow::scheduleHistoryBaseline() {
    history_.invalidateRedo();
    if (historyTimer_ != nullptr) {
        historyTimer_->start();
    }
}

void MainWindow::addSegment() {
    if (busy_) {
        return;
    }
    commitProjectSettings();
    commitCurrentSegment();
    const Project before = project_;
    const std::string beforeSelection = currentSegmentId_;
    int nextQuestion = 1;
    for (const Segment& segment : project_.segments) {
        nextQuestion = std::max(nextQuestion, segment.questions.last + 1);
    }
    Segment segment{freshStableId("segment"), {nextQuestion, nextQuestion},
                    "Narrator", {}, 5.0, 1};
    const std::string id = segment.id;
    project_.segments.push_back(std::move(segment));
    currentSegmentId_ = id;
    invalidateCurrentRenderedAudio();
    recordStructuralEdit(before, beforeSelection);
    showStatus(QStringLiteral("已新增第 %1 题组").arg(nextQuestion));
}

void MainWindow::duplicateSegment() {
    if (busy_) {
        return;
    }
    commitProjectSettings();
    commitCurrentSegment();
    Segment* selected = currentSegment();
    if (selected == nullptr) {
        showStatus(QStringLiteral("请先选择要复制的题组"), true);
        return;
    }
    const Project before = project_;
    const std::string beforeSelection = currentSegmentId_;
    Segment duplicate = *selected;
    duplicate.id = freshStableId("segment");
    duplicate.renderedAudioFile.clear();
    if (duplicate.generation.has_value()) {
        duplicate.generation->teacherReviewed = false;
        duplicate.generation->requiresTeacherReview = true;
    }
    const std::string id = duplicate.id;
    const auto position = std::find_if(
        project_.segments.begin(), project_.segments.end(),
        [selected](const Segment& segment) { return &segment == selected; });
    const auto insertAt = position == project_.segments.end()
                              ? project_.segments.end()
                              : std::next(position);
    project_.segments.insert(insertAt, std::move(duplicate));
    currentSegmentId_ = id;
    programWav_.clear();
    project_.renderedProgramFile.clear();
    recordStructuralEdit(before, beforeSelection);
    showStatus(QStringLiteral("已复制题组；稳定 ID 已重新生成，题号范围保持不变"));
}

void MainWindow::deleteSegment() {
    if (busy_) {
        return;
    }
    commitProjectSettings();
    commitCurrentSegment();
    if (project_.segments.empty()) {
        return;
    }
    const Project before = project_;
    const std::string beforeSelection = currentSegmentId_;
    const auto position = std::find_if(
        project_.segments.begin(), project_.segments.end(),
        [this](const Segment& segment) { return segment.id == currentSegmentId_; });
    if (position == project_.segments.end()) {
        return;
    }
    const std::size_t deletedIndex = static_cast<std::size_t>(
        std::distance(project_.segments.begin(), position));
    const std::string deletedId = position->id;
    project_.segments.erase(position);
    segmentWavs_.erase(deletedId);
    if (project_.segments.empty()) {
        currentSegmentId_.clear();
    } else {
        const std::size_t replacement = std::min(deletedIndex, project_.segments.size() - 1);
        currentSegmentId_ = project_.segments[replacement].id;
    }
    programWav_.clear();
    project_.renderedProgramFile.clear();
    recordStructuralEdit(before, beforeSelection);
    showStatus(QStringLiteral("已删除题组；可用撤销恢复"));
}

void MainWindow::moveSegment(int delta) {
    if (busy_ || project_.segments.empty()) {
        return;
    }
    commitProjectSettings();
    commitCurrentSegment();
    const auto position = std::find_if(
        project_.segments.begin(), project_.segments.end(),
        [this](const Segment& segment) { return segment.id == currentSegmentId_; });
    if (position == project_.segments.end()) {
        return;
    }
    const auto current = static_cast<std::ptrdiff_t>(
        std::distance(project_.segments.begin(), position));
    const auto destination = current + static_cast<std::ptrdiff_t>(delta);
    if (destination < 0 || destination >= static_cast<std::ptrdiff_t>(project_.segments.size())) {
        return;
    }
    const Project before = project_;
    const std::string beforeSelection = currentSegmentId_;
    std::iter_swap(project_.segments.begin() + current,
                   project_.segments.begin() + destination);
    programWav_.clear();
    project_.renderedProgramFile.clear();
    recordStructuralEdit(before, beforeSelection);
    showStatus(QStringLiteral("题组已排序；题号范围未改变"));
}

void MainWindow::commitListOrderFromUi() {
    if (segmentList_ == nullptr || project_.segments.empty()) {
        return;
    }
    std::vector<Segment> previous = std::move(project_.segments);
    std::vector<Segment> reordered;
    reordered.reserve(previous.size());
    for (int row = 0; row < segmentList_->count(); ++row) {
        const auto id = utf8(segmentList_->item(row)->data(Qt::UserRole).toString());
        const auto found = std::find_if(
            previous.begin(), previous.end(), [&id](const Segment& segment) {
                return segment.id == id;
            });
        if (found != previous.end()) {
            reordered.push_back(std::move(*found));
        }
    }
    if (reordered.size() != previous.size()) {
        project_.segments = std::move(previous);
        return;
    }
    project_.segments = std::move(reordered);
    programWav_.clear();
    project_.renderedProgramFile.clear();
    refreshSegmentList();
}

void MainWindow::applyHistoryProject(Project project,
                                     std::string selectedSegmentId,
                                     const QString& status) {
    if (historyTimer_ != nullptr) {
        historyTimer_->stop();
    }
    applyingHistory_ = true;
    stopPlayback();
    project_ = std::move(project);
    if (!selectedSegmentId.empty() && project_.findSegment(selectedSegmentId) != nullptr) {
        currentSegmentId_ = std::move(selectedSegmentId);
    } else if (project_.findSegment(currentSegmentId_) == nullptr) {
        currentSegmentId_ = project_.segments.empty() ? std::string{} : project_.segments.front().id;
    }
    restoreRenderedAudio();
    refreshProjectUi();
    applyingHistory_ = false;
    setDirty();
    showStatus(status);
}

void MainWindow::undoEdit() {
    if (busy_) {
        return;
    }
    commitProjectSettings();
    commitCurrentSegment();
    Project restored;
    std::string selected;
    if (!history_.undo(project_, currentSegmentId_, &restored, &selected)) {
        showStatus(QStringLiteral("没有可撤销的题组操作"));
        return;
    }
    applyHistoryProject(std::move(restored), std::move(selected),
                        QStringLiteral("已撤销上一次题组操作"));
}

void MainWindow::redoEdit() {
    if (busy_) {
        return;
    }
    commitProjectSettings();
    commitCurrentSegment();
    Project restored;
    std::string selected;
    if (!history_.redo(project_, currentSegmentId_, &restored, &selected)) {
        showStatus(QStringLiteral("没有可重做的题组操作"));
        return;
    }
    applyHistoryProject(std::move(restored), std::move(selected),
                        QStringLiteral("已重做题组操作"));
}

void MainWindow::checkRecoveryOnStartup() {
    if (!recoveryEnabled_) {
        return;
    }
    const auto candidates = recoveryStore_.candidates();
    for (const recovery::Candidate& candidate : candidates) {
        Project recovered;
        try {
            recovered = recoveryStore_.load(candidate);
        } catch (const std::exception& error) {
            showStatus(QStringLiteral("发现无法读取的自动恢复草稿：%1")
                           .arg(qString(error.what())), true);
            continue;
        }
        const QString title = recovered.title.empty()
                                  ? QStringLiteral("未命名工程")
                                  : qString(recovered.title);
        const MessageChoice choice = askAppMessage(
            this, QStringLiteral("发现未完成草稿"),
            QStringLiteral("检测到上次未正常结束的工程：%1\n是否恢复这份草稿？\n\n"
                           "恢复后会以未保存状态打开，原工程文件不会被自动覆盖。")
                .arg(title),
            QStringLiteral("恢复草稿"), QStringLiteral("丢弃草稿"), QStringLiteral("稍后处理"));
        if (choice == MessageChoice::Primary) {
            stopPlayback();
            project_ = std::move(recovered);
            projectPath_ = candidate.originalProjectPath;
            currentSegmentId_ = project_.segments.empty() ? std::string{}
                                                           : project_.segments.front().id;
            history_.reset(project_, currentSegmentId_);
            dragBeforeProject_.reset();
            dragBeforeSelection_.clear();
            restoreRenderedAudio();
            dirty_ = true;
            refreshProjectUi();
            setDirty(true);
            bool migrated = false;
            try {
                recoveryStore_.write(project_, projectPath_);
                recoveryStore_.discard(candidate);
                migrated = true;
            } catch (const std::exception& error) {
                showStatus(QStringLiteral("草稿已恢复，但恢复记录未能迁移：%1")
                               .arg(qString(error.what())), true);
            }
            if (migrated) {
                showStatus(QStringLiteral("草稿已恢复，原文件尚未改动；请保存"));
            }
        } else if (choice == MessageChoice::Secondary) {
            try {
                recoveryStore_.discard(candidate);
                showStatus(QStringLiteral("已丢弃未完成草稿"));
            } catch (const std::exception& error) {
                showStatus(QStringLiteral("丢弃草稿失败：%1").arg(qString(error.what())), true);
            }
        }
        break;
    }
}

bool MainWindow::autosaveRecovery() {
    if (!recoveryEnabled_ || busy_ || !dirty_) {
        return true;
    }
    try {
        commitProjectSettings();
        commitCurrentSegment();
        recoveryStore_.write(project_, projectPath_);
        return true;
    } catch (const std::exception& error) {
        showStatus(QStringLiteral("自动保存草稿失败：%1").arg(qString(error.what())), true);
        return false;
    }
}

void MainWindow::clearRecovery() {
    if (!recoveryEnabled_) {
        return;
    }
    try {
        recoveryStore_.clear();
    } catch (const std::exception& error) {
        showStatus(QStringLiteral("清理自动恢复草稿失败：%1").arg(qString(error.what())), true);
    }
}

void MainWindow::packageProjectToDisk() {
    if (busy_) {
        return;
    }
    commitProjectSettings();
    commitCurrentSegment();
    const QString selected = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择打包目标文件夹"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (selected.isEmpty()) {
        return;
    }
    try {
        const storage::PackageResult result = storage::package(
            project_, projectPath_, nativePath(selected));
        showStatus(QStringLiteral("工程已打包：%1（复制 %2 个音频）")
                       .arg(qPath(result.directory))
                       .arg(static_cast<qulonglong>(result.copiedResourceCount)));
        QDesktopServices::openUrl(QUrl::fromLocalFile(qPath(result.directory)));
    } catch (const std::exception& error) {
        showFailure(QStringLiteral("打包工程失败"), error);
    }
}

void MainWindow::setEditorWidgetsEnabled(bool enabled) {
    globalSettingsCard_->setEnabled(enabled);
    for (QWidget* widget : {static_cast<QWidget*>(newProjectButton_),
                            static_cast<QWidget*>(addSegmentButton_),
                            static_cast<QWidget*>(duplicateSegmentButton_),
                            static_cast<QWidget*>(deleteSegmentButton_),
                            static_cast<QWidget*>(moveUpButton_),
                            static_cast<QWidget*>(moveDownButton_),
                            static_cast<QWidget*>(undoButton_),
                            static_cast<QWidget*>(redoButton_),
                            static_cast<QWidget*>(exampleButton_),
                            static_cast<QWidget*>(segmentList_),
                            static_cast<QWidget*>(projectTitleEdit_),
                            static_cast<QWidget*>(accentCombo_),
                            static_cast<QWidget*>(maleVoiceCombo_),
                            static_cast<QWidget*>(femaleVoiceCombo_),
                            static_cast<QWidget*>(previewMaleVoiceButton_),
                            static_cast<QWidget*>(previewFemaleVoiceButton_),
                            static_cast<QWidget*>(strictAccentCheck_),
                            static_cast<QWidget*>(allowGenderFallbackCheck_),
                            static_cast<QWidget*>(wpmSpin_),
                            static_cast<QWidget*>(wpmSlider_),
                            static_cast<QWidget*>(questionStartSpin_),
                            static_cast<QWidget*>(questionEndSpin_),
                            static_cast<QWidget*>(speakerCombo_),
                            static_cast<QWidget*>(scriptEdit_),
                            static_cast<QWidget*>(pauseSpin_),
                            static_cast<QWidget*>(repeatSpin_),
                            static_cast<QWidget*>(smartScenarioButton_),
                            static_cast<QWidget*>(packageButton_)}) {
        if (widget != nullptr) {
            widget->setEnabled(enabled);
        }
    }
}

void MainWindow::refreshProjectUi() {
    loadingUi_ = true;
    {
        const QSignalBlocker titleBlocker(projectTitleEdit_);
        const QSignalBlocker accentBlocker(accentCombo_);
        const QSignalBlocker wpmBlocker(wpmSpin_);
        const QSignalBlocker sliderBlocker(wpmSlider_);
        const QSignalBlocker strictBlocker(strictAccentCheck_);
        const QSignalBlocker genderFallbackBlocker(allowGenderFallbackCheck_);
        projectTitleEdit_->setText(qString(project_.title));
        accentCombo_->setCurrentIndex(project_.accent == Accent::American ? 0 : 1);
        const int wpm = static_cast<int>(std::lround(project_.targetWpm));
        wpmSpin_->setValue(wpm);
        wpmSlider_->setValue(wpm);
        strictAccentCheck_->setChecked(project_.voiceSettings.strictAccent);
        allowGenderFallbackCheck_->setChecked(project_.voiceSettings.allowGenderFallback);
    }
    refreshVoiceOptions();
    {
        const QSignalBlocker maleBlocker(maleVoiceCombo_);
        const QSignalBlocker femaleBlocker(femaleVoiceCombo_);
        const int maleIndex = maleVoiceCombo_->findData(
            qString(project_.voiceSettings.maleVoiceTokenId));
        const int femaleIndex = femaleVoiceCombo_->findData(
            qString(project_.voiceSettings.femaleVoiceTokenId));
        if (maleIndex >= 0) {
            maleVoiceCombo_->setCurrentIndex(maleIndex);
        }
        if (femaleIndex >= 0) {
            femaleVoiceCombo_->setCurrentIndex(femaleIndex);
        }
    }
    loadingUi_ = false;
    refreshSegmentList();
    updateMetrics();
    updateAudioAvailability();
    const QString baseTitle = qString(project_.title).isEmpty()
                                  ? QStringLiteral("语澜")
                                  : qString(project_.title);
    setWindowTitle(QStringLiteral("%1 — 语澜").arg(baseTitle));
}

void MainWindow::refreshSegmentList() {
    loadingUi_ = true;
    const QSignalBlocker blocker(segmentList_);
    segmentList_->clear();
    int selectedRow = -1;
    for (std::size_t index = 0; index < project_.segments.size(); ++index) {
        const Segment& segment = project_.segments[index];
        const auto wav = segmentWavs_.find(segment.id);
        const bool generated = wav != segmentWavs_.end() && std::filesystem::exists(wav->second);
        auto* item = new QListWidgetItem(segmentListText(segment, generated), segmentList_);
        item->setData(Qt::UserRole, qString(segment.id));
        item->setSizeHint(QSize(0, 70));
        item->setToolTip(qString(segment.text));
        if (segment.id == currentSegmentId_) {
            selectedRow = static_cast<int>(index);
        }
    }
    groupCountLabel_->setText(QString::number(project_.segments.size()));
    if (selectedRow < 0 && !project_.segments.empty()) {
        selectedRow = 0;
        currentSegmentId_ = project_.segments.front().id;
    }
    segmentList_->setCurrentRow(selectedRow);
    loadingUi_ = false;
    loadCurrentSegment();
}

void MainWindow::loadCurrentSegment() {
    loadingUi_ = true;
    Segment* segment = currentSegment();
    const bool enabled = segment != nullptr && !busy_;
    for (QWidget* widget : {static_cast<QWidget*>(questionStartSpin_),
                            static_cast<QWidget*>(questionEndSpin_),
                            static_cast<QWidget*>(speakerCombo_),
                            static_cast<QWidget*>(scriptEdit_),
                            static_cast<QWidget*>(pauseSpin_),
                            static_cast<QWidget*>(repeatSpin_)}) {
        widget->setEnabled(enabled);
    }
    if (segment) {
        const QSignalBlocker startBlocker(questionStartSpin_);
        const QSignalBlocker endBlocker(questionEndSpin_);
        const QSignalBlocker speakerBlocker(speakerCombo_);
        const QSignalBlocker scriptBlocker(scriptEdit_);
        const QSignalBlocker pauseBlocker(pauseSpin_);
        const QSignalBlocker repeatBlocker(repeatSpin_);
        const QSignalBlocker reviewBlocker(teacherReviewedCheck_);
        questionStartSpin_->setValue(segment->questions.first);
        questionEndSpin_->setValue(segment->questions.last);
        speakerCombo_->setCurrentText(qString(segment->speaker));
        scriptEdit_->setPlainText(qString(segment->text));
        pauseSpin_->setValue(segment->pauseAfterSeconds);
        repeatSpin_->setValue(segment->repeatCount);
        currentGroupBadge_->setText(questionLabel(*segment));
        segmentIdLabel_->setText(qString(segment->id));
        voiceHintLabel_->setText(project_.accent == Accent::American
                                     ? QStringLiteral("优先匹配 en-US 系统声音")
                                     : QStringLiteral("优先匹配 en-GB 系统声音"));
        if (segment->generation.has_value()) {
            const GenerationRecord& record = *segment->generation;
            generationAuditLabel_->setText(
                QStringLiteral("%1%2 · 结构约束已校验 · %3")
                    .arg(qString(record.provider),
                         record.model.empty() ? QString()
                                              : QStringLiteral(" / %1").arg(qString(record.model)),
                         record.teacherReviewed ? QStringLiteral("教师已确认")
                                                : QStringLiteral("待教师语义复核")));
            teacherReviewedCheck_->setEnabled(true);
            teacherReviewedCheck_->setChecked(record.teacherReviewed);
        } else {
            generationAuditLabel_->setText(QStringLiteral("手工文稿 · 无生成记录"));
            teacherReviewedCheck_->setChecked(false);
            teacherReviewedCheck_->setEnabled(false);
        }
    } else {
        currentGroupBadge_->setText(QStringLiteral("未选择"));
        segmentIdLabel_->setText(QStringLiteral("—"));
        generationAuditLabel_->setText(QStringLiteral("—"));
        teacherReviewedCheck_->setChecked(false);
        teacherReviewedCheck_->setEnabled(false);
        scriptEdit_->clear();
    }
    loadingUi_ = false;
    updateMetrics();
    updateAudioAvailability();
}

void MainWindow::commitProjectSettings() {
    if (loadingUi_) {
        return;
    }
    project_.title = utf8(projectTitleEdit_->text().trimmed());
    project_.accent = accentCombo_->currentIndex() == 0 ? Accent::American : Accent::British;
    project_.targetWpm = static_cast<double>(wpmSpin_->value());
    project_.voiceSettings.maleVoiceTokenId = selectedVoiceToken(
        platform::windows::VoiceGender::Male);
    project_.voiceSettings.femaleVoiceTokenId = selectedVoiceToken(
        platform::windows::VoiceGender::Female);
    project_.voiceSettings.strictAccent = strictAccentCheck_->isChecked();
    project_.voiceSettings.allowGenderFallback = allowGenderFallbackCheck_->isChecked();
    setWindowTitle(QStringLiteral("%1%2 — 语澜")
                       .arg(dirty_ ? QStringLiteral("• ") : QString())
                       .arg(projectTitleEdit_->text().trimmed()));
}

void MainWindow::commitCurrentSegment() {
    if (loadingUi_) {
        return;
    }
    if (Segment* segment = currentSegment()) {
        const std::string newText = utf8(scriptEdit_->toPlainText().trimmed());
        if (segment->text != newText) {
            segment->generation.reset();
            generationAuditLabel_->setText(QStringLiteral("文稿已手工修改 · 原生成审阅记录已失效"));
            const QSignalBlocker reviewBlocker(teacherReviewedCheck_);
            teacherReviewedCheck_->setChecked(false);
            teacherReviewedCheck_->setEnabled(false);
        }
        segment->questions.first = questionStartSpin_->value();
        segment->questions.last = questionEndSpin_->value();
        segment->speaker = utf8(speakerCombo_->currentText().trimmed());
        segment->text = newText;
        segment->pauseAfterSeconds = pauseSpin_->value();
        segment->repeatCount = repeatSpin_->value();
    }
}

void MainWindow::updateSelectedListItem() {
    const Segment* segment = currentSegment();
    if (!segment) {
        return;
    }
    for (int row = 0; row < segmentList_->count(); ++row) {
        QListWidgetItem* item = segmentList_->item(row);
        if (utf8(item->data(Qt::UserRole).toString()) == segment->id) {
            const auto wav = segmentWavs_.find(segment->id);
            const bool generated = wav != segmentWavs_.end() && std::filesystem::exists(wav->second);
            item->setText(segmentListText(*segment, generated));
            item->setToolTip(qString(segment->text));
            break;
        }
    }
    currentGroupBadge_->setText(questionLabel(*segment));
}

void MainWindow::updateMetrics() {
    try {
        totalDurationLabel_->setText(durationText(estimateProjectDuration(project_)));
    } catch (...) {
        totalDurationLabel_->setText(QStringLiteral("—"));
    }
    const Segment* segment = currentSegment();
    if (!segment) {
        wordCountLabel_->setText(QStringLiteral("0 词"));
        segmentDurationLabel_->setText(QStringLiteral("—"));
        return;
    }
    wordCountLabel_->setText(QStringLiteral("%1 词 · 单遍 %2")
                                 .arg(static_cast<qulonglong>(countReadableWords(segment->text)))
                                 .arg(durationText(estimateReadingDuration(segment->text,
                                                                          project_.targetWpm))));
    try {
        segmentDurationLabel_->setText(
            durationText(estimateSegmentDuration(*segment, project_.targetWpm)));
    } catch (...) {
        segmentDurationLabel_->setText(QStringLiteral("—"));
    }
}

void MainWindow::updateAudioAvailability() {
    const Segment* segment = currentSegment();
    bool selectedGenerated = false;
    QString detail = QStringLiteral("待生成 · 点击“生成并试听选中题组”自动预览");
    if (segment) {
        const auto found = segmentWavs_.find(segment->id);
        if (found != segmentWavs_.end() && std::filesystem::exists(found->second)) {
            selectedGenerated = true;
            outputPathLabel_->setText(qPath(found->second));
            detail = QStringLiteral("已生成选中题组 · 可直接播放或循环");
        } else {
            outputPathLabel_->setText(QStringLiteral("尚未生成或内容已更改"));
        }
    }
    playbackDetailLabel_->setText(detail);
    const bool hasSegment = segment != nullptr;
    newProjectButton_->setEnabled(!busy_);
    addSegmentButton_->setEnabled(!busy_);
    duplicateSegmentButton_->setEnabled(hasSegment && !busy_);
    deleteSegmentButton_->setEnabled(hasSegment && !busy_);
    moveUpButton_->setEnabled(hasSegment && segmentList_->currentRow() > 0 && !busy_);
    moveDownButton_->setEnabled(hasSegment &&
                                 segmentList_->currentRow() + 1 < segmentList_->count() &&
                                 !busy_);
    undoButton_->setEnabled(history_.canUndo() && !busy_);
    redoButton_->setEnabled(history_.canRedo() && !busy_);
    packageButton_->setEnabled(!busy_);
    cancelRenderButton_->setEnabled(renderJob_.isRunning());
    const bool programGenerated =
        !programWav_.empty() && std::filesystem::exists(programWav_);
    generateSelectedButton_->setEnabled(hasSegment && !busy_);
    playSelectedButton_->setEnabled(selectedGenerated && !busy_);
    loopSelectedButton_->setEnabled(selectedGenerated && !busy_);
    generateAllButton_->setEnabled(!project_.segments.empty() && !busy_);
    playAllButton_->setEnabled(programGenerated && !busy_);
    const auto playback = player_.snapshot();
    const bool playbackActive = playback.state != platform::windows::WavPlayer::State::Stopped;
    pauseButton_->setEnabled(playbackActive && !busy_);
    stopButton_->setEnabled(playbackActive && !busy_);
    playbackSlider_->setEnabled(playbackActive && playback.durationMilliseconds > 0 && !busy_);
    previousButton_->setEnabled(segmentList_->currentRow() > 0 && !busy_);
    nextButton_->setEnabled(segmentList_->currentRow() >= 0 &&
                            segmentList_->currentRow() + 1 < segmentList_->count() && !busy_);
}

void MainWindow::invalidateAllRenderedAudio() {
    segmentWavs_.clear();
    programWav_.clear();
    project_.renderedProgramFile.clear();
    for (Segment& segment : project_.segments) {
        segment.renderedAudioFile.clear();
    }
}

void MainWindow::invalidateCurrentRenderedAudio() {
    if (Segment* segment = currentSegment()) {
        segmentWavs_.erase(segment->id);
        segment->renderedAudioFile.clear();
    }
    programWav_.clear();
    project_.renderedProgramFile.clear();
}

void MainWindow::restoreRenderedAudio() {
    segmentWavs_.clear();
    programWav_.clear();
    for (const Segment& segment : project_.segments) {
        if (segment.renderedAudioFile.empty()) {
            continue;
        }
        const auto path = storage::resolveResource(projectPath_, segment.renderedAudioFile);
        audio::WavInfo info;
        std::string error;
        if (std::filesystem::is_regular_file(path) &&
            audio::inspectPcmWav(path, &info, &error)) {
            segmentWavs_[segment.id] = path;
        }
    }
    if (!project_.renderedProgramFile.empty()) {
        const auto path = storage::resolveResource(projectPath_, project_.renderedProgramFile);
        audio::WavInfo info;
        std::string error;
        if (std::filesystem::is_regular_file(path) &&
            audio::inspectPcmWav(path, &info, &error)) {
            programWav_ = path;
        }
    }
}

void MainWindow::refreshVoiceOptions() {
    const QString previousMale = maleVoiceCombo_->currentData().toString();
    const QString previousFemale = femaleVoiceCombo_->currentData().toString();
    const QSignalBlocker maleBlocker(maleVoiceCombo_);
    const QSignalBlocker femaleBlocker(femaleVoiceCombo_);
    maleVoiceCombo_->clear();
    femaleVoiceCombo_->clear();
    installedVoices_.clear();

    std::string sapiError;
    (void)platform::windows::listVoices(&installedVoices_, &sapiError);
    localVoicePacks_.discover();
    for (const auto& voice : localVoicePacks_.voices()) {
        installedVoices_.push_back(platform::windows::VoiceInfo{
            voice.tokenId,
            voice.name,
            voice.locale,
            {voice.locale},
            voice.gender,
            false,
            true,
        });
    }
    if (installedVoices_.empty()) {
        const QString message = sapiError.empty()
                                    ? QStringLiteral("未发现可用的系统音色或本地声音包")
                                    : QStringLiteral("无法读取系统音色：%1").arg(qString(sapiError));
        maleVoiceCombo_->addItem(message, QString());
        femaleVoiceCombo_->addItem(message, QString());
        maleVoiceCombo_->setEnabled(false);
        femaleVoiceCombo_->setEnabled(false);
        previewMaleVoiceButton_->setEnabled(false);
        previewFemaleVoiceButton_->setEnabled(false);
        voiceAuditLabel_->setText(message);
        return;
    }

    const QString wantedLocale = accentCombo_->currentIndex() == 0
                                     ? QStringLiteral("en-US")
                                     : QStringLiteral("en-GB");
    const auto hasLocale = [&](const platform::windows::VoiceInfo& voice) {
        return std::any_of(voice.locales.begin(), voice.locales.end(), [&](const auto& locale) {
            return QString::compare(qString(locale), wantedLocale, Qt::CaseInsensitive) == 0;
        });
    };
    const auto isEnglish = [](const platform::windows::VoiceInfo& voice) {
        return std::any_of(voice.locales.begin(), voice.locales.end(), [](const auto& locale) {
            const std::string normalized = lowerAsciiCopy(locale);
            return normalized == "en" || normalized.rfind("en-", 0) == 0;
        });
    };

    auto populate = [&](QComboBox* combo,
                        platform::windows::VoiceGender wantedGender,
                        const QString& previous) {
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& voice : installedVoices_) {
                if (voice.gender != wantedGender) {
                    continue;
                }
                const bool exact = hasLocale(voice);
                if ((pass == 0 && !exact) || (pass == 1 && exact)) {
                    continue;
                }
                if (!exact && (strictAccentCheck_->isChecked() || !isEnglish(voice))) {
                    continue;
                }
                const QString label = QStringLiteral("%1 · %2%3")
                                          .arg(qString(voice.name), qString(voice.locale),
                                               exact ? QString() : QStringLiteral(" · 地区回退")) +
                                      (localVoicePacks_.ownsToken(voice.tokenId)
                                           ? QStringLiteral(" · 本地神经包")
                                           : QStringLiteral(" · Windows"));
                combo->addItem(label, qString(voice.tokenId));
            }
        }
        if (combo->count() == 0) {
            combo->addItem(QStringLiteral("未安装 %1 %2音色")
                               .arg(wantedLocale,
                                    wantedGender == platform::windows::VoiceGender::Male
                                        ? QStringLiteral("男声")
                                        : QStringLiteral("女声")),
                           QString());
        }
        const int previousIndex = combo->findData(previous);
        if (previousIndex >= 0) {
            combo->setCurrentIndex(previousIndex);
        }
        combo->setEnabled(true);
    };

    populate(maleVoiceCombo_, platform::windows::VoiceGender::Male, previousMale);
    populate(femaleVoiceCombo_, platform::windows::VoiceGender::Female, previousFemale);
    const bool maleReady = !maleVoiceCombo_->currentData().toString().isEmpty();
    const bool femaleReady = !femaleVoiceCombo_->currentData().toString().isEmpty();
    previewMaleVoiceButton_->setEnabled(maleReady || allowGenderFallbackCheck_->isChecked());
    previewFemaleVoiceButton_->setEnabled(femaleReady || allowGenderFallbackCheck_->isChecked());

    if (maleReady && femaleReady) {
        const bool usesPack =
            localVoicePacks_.ownsToken(selectedVoiceToken(platform::windows::VoiceGender::Male)) ||
            localVoicePacks_.ownsToken(selectedVoiceToken(platform::windows::VoiceGender::Female));
        voiceAuditLabel_->setText(
            QStringLiteral("✓ 已找到 %1 的男女两路本地音色；口音地区与角色性别均可核验。%2")
                .arg(wantedLocale,
                     usesPack ? QStringLiteral(" 神经声音来自用户安装的外置声音包。")
                              : QString()));
    } else {
        QStringList missing;
        if (!maleReady) {
            missing.push_back(QStringLiteral("男声"));
        }
        if (!femaleReady) {
            missing.push_back(QStringLiteral("女声"));
        }
        voiceAuditLabel_->setText(
            QStringLiteral("⚠ 本机缺少 %1 %2。严格模式会阻止不准确合成；可安装 Windows 音色，或把经许可核验的声音包放入 LocalAppData/Audiloquy/voice-packs。")
                .arg(wantedLocale, missing.join(QStringLiteral("、"))));
    }
}

std::string MainWindow::selectedVoiceToken(platform::windows::VoiceGender gender) const {
    const QComboBox* combo = gender == platform::windows::VoiceGender::Male
                                 ? maleVoiceCombo_
                                 : femaleVoiceCombo_;
    return combo == nullptr ? std::string{} : utf8(combo->currentData().toString());
}

void MainWindow::setDisplayMode(DisplayMode mode) {
    displayMode_ = mode;
    const bool professional = mode == DisplayMode::Professional;
    if (auto* inspectorLayout = qobject_cast<QVBoxLayout*>(
            professionalCard_->parentWidget()->layout())) {
        inspectorLayout->removeWidget(professionalCard_);
        const int targetIndex = professional
                                    ? 1
                                    : std::max(1, inspectorLayout->indexOf(globalSettingsCard_) + 1);
        inspectorLayout->insertWidget(targetIndex, professionalCard_);
    }
    professionalCard_->setVisible(professional);
    simpleModeButton_->setChecked(!professional);
    professionalModeButton_->setChecked(professional);
    if (professional) {
        inspectorScroll_->ensureWidgetVisible(professionalCard_, 12, 12);
    }
    updatePageContext();
    showStatus(professional ? QStringLiteral("已切换到专业检查视图")
                            : QStringLiteral("已返回简洁编辑视图"));
}

void MainWindow::openScenarioGenerator() {
    activateWorkflowStep(0);
    commitCurrentSegment();
    Segment* segment = currentSegment();
    if (segment == nullptr) {
        showStatus(QStringLiteral("请先选择一个要接收文稿的题组"), true);
        return;
    }

    ScenarioDialog dialog(!segment->text.empty(), this);
    if (dialog.exec() != QDialog::Accepted || !dialog.adoptedDraft()) {
        showStatus(QStringLiteral("未采用情景初稿，当前文稿保持不变"));
        return;
    }

    const ScenarioDraft& draft = *dialog.adoptedDraft();
    segment->speaker = "Man + Woman";
    segment->text = renderDialogueScript(draft);
    GenerationRecord generation;
    generation.provider = dialog.adoptedProvider();
    generation.model = dialog.adoptedModel();
    generation.questionStem = draft.sourceRequest.questionStem;
    generation.options = draft.sourceRequest.options;
    generation.correctAnswer = std::string(answerLabelCode(draft.supportedAnswer));
    generation.requiresTeacherReview = draft.requiresTeacherReview;
    generation.teacherReviewed = false;
    generation.evidence.reserve(draft.evidence.size());
    for (const ScenarioEvidence& evidence : draft.evidence) {
        generation.evidence.push_back(GenerationEvidence{
            std::string(answerLabelCode(evidence.option)),
            evidence.role == EvidenceRole::Supports ? "supports" : "rejects",
            evidence.turnId,
            evidence.quote,
        });
    }
    segment->generation = std::move(generation);
    invalidateCurrentRenderedAudio();
    player_.stop();
    setDirty();
    refreshSegmentList();
    updateMetrics();
    updateAudioAvailability();
    workspaceScroll_->ensureWidgetVisible(editorCard_, 24, 24);
    scriptEdit_->setFocus(Qt::OtherFocusReason);
    showStatus(QStringLiteral("情景初稿已采用到当前题组；可继续修改后生成试听"));
}

void MainWindow::activateWorkflowStep(int step) {
    workflowStep_ = std::clamp(step, 0, 4);
    const std::array<QPushButton*, 5> buttons{
        manuscriptStepButton_, voiceStepButton_, rhythmStepButton_, generateStepButton_,
        classroomStepButton_};
    for (int index = 0; index < static_cast<int>(buttons.size()); ++index) {
        buttons[static_cast<std::size_t>(index)]->setChecked(index == workflowStep_);
    }

    QWidget* scrollTarget = nullptr;
    QWidget* focusTarget = nullptr;
    QString stepName;
    switch (workflowStep_) {
        case 0:
            stepName = QStringLiteral("文稿");
            scrollTarget = scenarioGeneratorCard_;
            focusTarget = smartScenarioButton_;
            break;
        case 1:
            stepName = QStringLiteral("声音");
            scrollTarget = globalSettingsCard_;
            focusTarget = accentCombo_;
            break;
        case 2:
            stepName = QStringLiteral("节奏");
            scrollTarget = globalSettingsCard_;
            focusTarget = wpmSpin_;
            break;
        case 3:
            stepName = QStringLiteral("生成");
            scrollTarget = outputCard_;
            focusTarget = generateSelectedButton_;
            break;
        case 4:
            stepName = QStringLiteral("课堂");
            focusTarget = playerBar_;
            break;
        default:
            break;
    }
    if (scrollTarget != nullptr) {
        const bool inspectorTarget = scrollTarget == globalSettingsCard_ ||
                                     scrollTarget == outputCard_ ||
                                     scrollTarget == professionalCard_;
        (inspectorTarget ? inspectorScroll_ : workspaceScroll_)
            ->ensureWidgetVisible(scrollTarget, 24, 24);
    }
    if (focusTarget != nullptr) {
        focusTarget->setFocus(Qt::OtherFocusReason);
    }
    playerBar_->setProperty("activeStep", workflowStep_ == 4);
    playerBar_->style()->unpolish(playerBar_);
    playerBar_->style()->polish(playerBar_);
    playerBar_->update();
    updatePageContext();
    showStatus(QStringLiteral("已定位到第 %1 步：%2").arg(workflowStep_ + 1).arg(stepName));
}

void MainWindow::updatePageContext() {
    QString title;
    QString description;
    switch (workflowStep_) {
        case 0:
            title = QStringLiteral("准备听力文稿");
            description = QStringLiteral("直接编辑题组文稿，或从一个教学情景开始构思。");
            break;
        case 1:
            title = QStringLiteral("选择英语声音");
            description = QStringLiteral("为整套材料选择美式或英式英语，系统会优先匹配对应声音。");
            break;
        case 2:
            title = QStringLiteral("安排朗读节奏");
            description = QStringLiteral("设定目标 WPM、题组停顿和重复次数，同时查看预估时长。");
            break;
        case 3:
            title = QStringLiteral("生成并自动试听");
            description = QStringLiteral("可先试听选中题组，确认后再生成整套 WAV 节目。");
            break;
        case 4:
            title = QStringLiteral("进入课堂播放");
            description = QStringLiteral("按题组播放、循环或播放整套；按 Esc 可随时停止。");
            break;
        default:
            break;
    }
    const bool professional = displayMode_ == DisplayMode::Professional;
    pageEyebrowLabel_->setText(
        QStringLiteral("%1 · 第 %2 步")
            .arg(professional ? QStringLiteral("专业模式") : QStringLiteral("简洁工作流"))
            .arg(workflowStep_ + 1));
    pageTitleLabel_->setText(professional ? title + QStringLiteral(" · 专业检查") : title);
    pageDescriptionLabel_->setText(
        professional
            ? description + QStringLiteral("专业检查器与简洁模式共用同一份工程数据。")
            : description);
}

void MainWindow::setDirty(bool dirty) {
    dirty_ = dirty;
    saveButton_->setText(dirty_ ? QStringLiteral("保存  •") : QStringLiteral("保存"));
    const QString title = projectTitleEdit_->text().trimmed().isEmpty()
                              ? QStringLiteral("未命名工程")
                              : projectTitleEdit_->text().trimmed();
    setWindowTitle(QStringLiteral("%1%2 — 语澜")
                       .arg(dirty_ ? QStringLiteral("• ") : QString())
                       .arg(title));
}

Segment* MainWindow::currentSegment() noexcept {
    return project_.findSegment(currentSegmentId_);
}

const Segment* MainWindow::currentSegment() const noexcept {
    return project_.findSegment(currentSegmentId_);
}

bool MainWindow::validateForAction(const QString& action) {
    commitProjectSettings();
    commitCurrentSegment();
    const auto issues = validate(project_);
    if (issues.empty()) {
        return true;
    }
    const ValidationIssue& issue = issues.front();
    const QString detail = QStringLiteral("%1\n\n位置：%2\n原因：%3")
                               .arg(action)
                               .arg(qString(issue.path))
                               .arg(qString(issue.message));
    showAppMessage(this, QStringLiteral("工程内容尚不完整"), detail,
                   MessageTone::Warning);
    showStatus(QStringLiteral("请先修正工程内容：%1").arg(qString(issue.path)), true);
    return false;
}

bool MainWindow::confirmDiscardChanges() {
    if (!dirty_) {
        return true;
    }
    const MessageChoice choice = askAppMessage(
        this, QStringLiteral("工程尚未保存"), QStringLiteral("要先保存当前更改吗？"),
        QStringLiteral("保存"), QStringLiteral("不保存"), QStringLiteral("取消"));
    if (choice == MessageChoice::Primary) {
        return saveProjectToDisk(false);
    }
    if (choice == MessageChoice::Secondary) {
        clearRecovery();
        return true;
    }
    return false;
}

bool MainWindow::saveProjectToDisk(bool forceChoosePath) {
    if (busy_) {
        return false;
    }
    commitProjectSettings();
    commitCurrentSegment();
    std::filesystem::path path = projectPath_;
    if (forceChoosePath || path.empty()) {
        QString initial;
        if (!path.empty()) {
            initial = qPath(path);
        } else {
            const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
            const QString title = projectTitleEdit_->text().trimmed().isEmpty()
                                      ? QStringLiteral("语澜工程")
                                      : projectTitleEdit_->text().trimmed();
            initial = documents + QLatin1Char('/') + title +
                      QStringLiteral(".json");
        }
        QString selected = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存听力工程"), initial,
            QStringLiteral("听力工程 (*.json);;所有文件 (*.*)"));
        if (selected.isEmpty()) {
            return false;
        }
        if (QFileInfo(selected).suffix().isEmpty()) {
            selected += QStringLiteral(".json");
        }
        path = nativePath(selected);
    }
    try {
        const bool saveAs = !projectPath_.empty() && projectPath_ != path;
        Project prepared = storage::prepareForSave(project_, projectPath_, path);
        storage::save(prepared, path, ValidationPurpose::Draft);
        project_ = std::move(prepared);
        projectPath_ = path;
        if (saveAs) {
            // Existing snapshots contain paths relative to the old project
            // location; restart structural history after a cross-directory
            // Save As so undo cannot restore stale resource references.
            history_.reset(project_, currentSegmentId_);
        } else {
            history_.adoptCurrent(project_, currentSegmentId_);
        }
        restoreRenderedAudio();
        clearRecovery();
        setDirty(false);
        showStatus(QStringLiteral("工程已保存：%1").arg(qPath(path)));
        return true;
    } catch (const std::exception& error) {
        showFailure(QStringLiteral("保存工程失败"), error);
        return false;
    }
}

void MainWindow::openProjectFromDisk() {
    if (busy_) {
        return;
    }
    if (!confirmDiscardChanges()) {
        return;
    }
    const QString selected = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开听力工程"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStringLiteral("听力工程 (*.json);;所有文件 (*.*)"));
    if (selected.isEmpty()) {
        return;
    }
    try {
        const auto selectedPath = nativePath(selected);
        const storage::LoadResult loadedResult =
            storage::load(selectedPath, ValidationPurpose::Draft);
        stopPlayback();
        if (historyTimer_ != nullptr) {
            historyTimer_->stop();
        }
        project_ = loadedResult.project;
        projectPath_ = selectedPath;
        currentSegmentId_ = project_.segments.empty() ? std::string() : project_.segments.front().id;
        history_.reset(project_, currentSegmentId_);
        dragBeforeProject_.reset();
        dragBeforeSelection_.clear();
        restoreRenderedAudio();
        dirty_ = false;
        refreshProjectUi();
        setDirty(false);
        clearRecovery();
        if (loadedResult.missingResources.empty()) {
            showStatus(QStringLiteral("工程已打开：%1").arg(selected));
        } else {
            showStatus(QStringLiteral("工程已打开，但有 %1 个音频资源缺失或无效；请重新生成。")
                           .arg(static_cast<qulonglong>(loadedResult.missingResources.size())),
                       true);
        }
    } catch (const std::exception& error) {
        showFailure(QStringLiteral("打开工程失败"), error);
    }
}

std::filesystem::path MainWindow::outputDirectory() const {
    return defaultOutputDirectory(projectPath_, std::filesystem::current_path(), project_.id);
}

void MainWindow::startRender(RenderScope scope) {
    if (busy_ || renderJob_.isRunning()) {
        return;
    }
    commitProjectSettings();
    commitCurrentSegment();

    Project snapshot = project_;
    if (scope == RenderScope::Selected) {
        const Segment* selected = snapshot.findSegment(currentSegmentId_);
        if (selected == nullptr) {
            showStatus(QStringLiteral("请先选择要生成的题组"), true);
            return;
        }
        Project selectedValidation = snapshot;
        selectedValidation.segments = {*selected};
        const auto issues = validate(selectedValidation);
        if (!issues.empty()) {
            const ValidationIssue& issue = issues.front();
            showAppMessage(
                this, QStringLiteral("工程内容尚不完整"),
                QStringLiteral("生成前需要修正以下内容。\n\n位置：%1\n原因：%2")
                    .arg(qString(issue.path), qString(issue.message)),
                MessageTone::Warning);
            showStatus(QStringLiteral("请先修正工程内容：%1").arg(qString(issue.path)), true);
            return;
        }
    } else {
        const auto issues = validate(snapshot);
        if (!issues.empty()) {
            const ValidationIssue& issue = issues.front();
            showAppMessage(
                this, QStringLiteral("工程内容尚不完整"),
                QStringLiteral("生成前需要修正以下内容。\n\n位置：%1\n原因：%2")
                    .arg(qString(issue.path), qString(issue.message)),
                MessageTone::Warning);
            showStatus(QStringLiteral("请先修正工程内容：%1").arg(qString(issue.path)), true);
            return;
        }
    }

    if (recoveryEnabled_ && !autosaveRecovery()) {
        showStatus(QStringLiteral("自动保存草稿失败，仍继续生成；请及时手动保存"), true);
    }

    RenderJobRequest request;
    request.project = snapshot;
    request.outputDirectory = outputDirectory();
    request.scope = scope;
    request.selectedSegmentId = currentSegmentId_;
    request.localVoicePacks = localVoicePacks_.snapshot();
    renderIntent_ = scope == RenderScope::Selected ? RenderIntent::Selected : RenderIntent::All;
    beginBusy(scope == RenderScope::Selected
                  ? QStringLiteral("正在后台生成选中题组…")
                  : QStringLiteral("正在后台生成整套听力…"));
    if (!renderJob_.start(std::move(request))) {
        renderIntent_ = RenderIntent::None;
        endBusy();
        showStatus(QStringLiteral("已有生成任务正在运行"), true);
        return;
    }
    updateAudioAvailability();
}

void MainWindow::startVoicePreview(platform::windows::VoiceGender gender) {
    if (busy_ || renderJob_.isRunning()) {
        return;
    }
    commitProjectSettings();
    Project preview;
    preview.id = freshStableId("voice-preview");
    preview.title = "Audiloquy voice preview";
    preview.accent = project_.accent;
    preview.targetWpm = project_.targetWpm;
    preview.voiceSettings = project_.voiceSettings;
    const bool male = gender == platform::windows::VoiceGender::Male;
    const std::string id = freshStableId("preview");
    preview.segments.emplace_back(
        id, QuestionRange{1, 1}, male ? "Man" : "Woman",
        "Good afternoon. Please listen carefully and choose the best answer.",
        0.0, 1);
    RenderJobRequest request;
    request.project = std::move(preview);
    request.outputDirectory = outputDirectory() / ".cache" / "preview";
    request.scope = RenderScope::Selected;
    request.selectedSegmentId = id;
    request.localVoicePacks = localVoicePacks_.snapshot();
    renderIntent_ = male ? RenderIntent::PreviewMale : RenderIntent::PreviewFemale;
    beginBusy(male ? QStringLiteral("正在生成男声试听…") : QStringLiteral("正在生成女声试听…"));
    if (!renderJob_.start(std::move(request))) {
        renderIntent_ = RenderIntent::None;
        endBusy();
        showStatus(QStringLiteral("已有生成任务正在运行"), true);
        return;
    }
    updateAudioAvailability();
}

void MainWindow::cancelRender() {
    if (!renderJob_.isRunning()) {
        return;
    }
    renderJob_.cancel();
    cancelRenderButton_->setEnabled(false);
    showStatus(QStringLiteral("正在取消后台生成…"));
}

void MainWindow::handleRenderProgress(const RenderJobProgress& progress) {
    if (!renderJob_.isRunning()) {
        return;
    }
    QString message = renderPhaseText(progress.phase);
    if (progress.totalSegments > 0) {
        message = QStringLiteral("%1 · 第 %2/%3 组")
                      .arg(message)
                      .arg(static_cast<qulonglong>(progress.currentSegmentIndex + 1))
                      .arg(static_cast<qulonglong>(progress.totalSegments));
    }
    if (progress.totalTurns > 0) {
        message += QStringLiteral(" · 第 %1/%2 轮")
                       .arg(static_cast<qulonglong>(progress.currentTurnIndex + 1))
                       .arg(static_cast<qulonglong>(progress.totalTurns));
    }
    if (progress.cacheHit) {
        message += QStringLiteral(" · 使用缓存");
    }
    showStatus(message);
    updateAudioAvailability();
}

void MainWindow::handleRenderFinished(const RenderJobResult& result) {
    const RenderIntent intent = renderIntent_;
    renderIntent_ = RenderIntent::None;
    const bool preview = intent == RenderIntent::PreviewMale ||
                         intent == RenderIntent::PreviewFemale;
    bool appliedToProject = false;
    if (!preview) {
        for (const RenderedSegment& rendered : result.completedSegments) {
            if (project_.findSegment(rendered.id) == nullptr) {
                continue;
            }
            segmentWavs_[rendered.id] = rendered.path;
            if (Segment* segment = project_.findSegment(rendered.id)) {
                segment->renderedAudioFile = utf8(qPath(rendered.path));
                appliedToProject = true;
            }
        }
    }
    if (intent == RenderIntent::All && result.success && !result.programPath.empty()) {
        programWav_ = result.programPath;
        project_.renderedProgramFile = utf8(qPath(result.programPath));
    }
    if (appliedToProject) {
        history_.adoptCurrent(project_, currentSegmentId_);
        setDirty();
        refreshSegmentList();
    }
    if (closePending_) {
        endBusy();
        closePending_ = false;
        showStatus(QStringLiteral("后台生成已结束，正在关闭窗口…"));
        QTimer::singleShot(0, this, [this] { close(); });
        return;
    }
    if (!result.success) {
        endBusy();
        if (result.cancelled) {
            showStatus(result.completedSegments.empty()
                           ? QStringLiteral("后台生成已取消")
                           : QStringLiteral("后台生成已取消；已保留已完成的题组音频"));
        } else {
            showFailure(QStringLiteral("后台生成失败"),
                        std::runtime_error(result.error.empty()
                                                ? "Render job failed"
                                                : result.error));
        }
        return;
    }

    if (intent == RenderIntent::PreviewMale || intent == RenderIntent::PreviewFemale) {
        endBusy();
        if (result.completedSegments.empty()) {
            showStatus(QStringLiteral("试听没有生成有效音频"), true);
            return;
        }
        std::string error;
        if (!player_.playAsync(result.completedSegments.front().path, false, &error)) {
            showFailure(QStringLiteral("音色试听不可用"), std::runtime_error(error));
            return;
        }
        outputPathLabel_->setText(qPath(result.completedSegments.front().path));
        playbackTitleLabel_->setText(QStringLiteral("正在播放 · 音色试听"));
        playbackDetailLabel_->setText(QStringLiteral("试听完成后可继续编辑工程"));
        QStringList voices;
        for (const RenderedVoiceUse& use : result.voiceUses) {
            const QString note = renderVoiceUseText(use);
            if (!voices.contains(note)) {
                voices.push_back(note);
            }
        }
        voiceHintLabel_->setText(QStringLiteral("试听：%1")
                                     .arg(voices.join(QStringLiteral("；"))));
        showStatus(QStringLiteral("正在试听后台生成的本机音色"));
        pollPlayback();
        return;
    }

    QStringList voices;
    for (const RenderedVoiceUse& use : result.voiceUses) {
        const QString note = renderVoiceUseText(use);
        if (!voices.contains(note)) {
            voices.push_back(note);
        }
    }
    if (!voices.isEmpty()) {
        voiceHintLabel_->setText(QStringLiteral("实际音色 · %1")
                                     .arg(voices.join(QStringLiteral("；"))));
    }
    history_.adoptCurrent(project_, currentSegmentId_);
    endBusy();
    refreshSegmentList();
    setDirty();
    updateMetrics();
    updateAudioAvailability();
    if (intent == RenderIntent::Selected) {
        const Segment* segment = currentSegment();
        showStatus(segment == nullptr
                       ? QStringLiteral("选中题组已生成")
                       : QStringLiteral("已生成 %1，正在自动试听").arg(questionLabel(*segment)));
        playSelected(false);
    } else {
        showStatus(QStringLiteral("整套听力已生成，正在自动播放"));
        playAll();
    }
}


void MainWindow::playSelected(bool loop) {
    const Segment* segment = currentSegment();
    if (!segment) {
        return;
    }
    const auto found = segmentWavs_.find(segment->id);
    if (found == segmentWavs_.end() || !std::filesystem::exists(found->second)) {
        playbackTitleLabel_->setText(QStringLiteral("选中题组尚未生成"));
        playbackDetailLabel_->setText(
            QStringLiteral("请先点击“生成并试听选中题组”"));
        showStatus(QStringLiteral("选中题组没有可播放的 WAV，请先生成"), true);
        return;
    }
    std::string error;
    if (!player_.playAsync(found->second, loop, &error)) {
        showFailure(QStringLiteral("播放失败"), std::runtime_error(error));
        return;
    }
    playbackTitleLabel_->setText(
        QStringLiteral("%1%2").arg(loop ? QStringLiteral("循环播放 · ")
                                      : QStringLiteral("正在播放 · "),
                                 questionLabel(*segment)));
    playbackDetailLabel_->setText(
        QStringLiteral("%1 · %2 WPM · %3")
            .arg(qString(segment->speaker))
            .arg(static_cast<int>(std::lround(project_.targetWpm)))
            .arg(project_.accent == Accent::American ? QStringLiteral("en-US")
                                                     : QStringLiteral("en-GB")));
    showStatus(loop ? QStringLiteral("已开始循环选中题组，按 Esc 停止")
                    : QStringLiteral("已开始播放选中题组"));
    pollPlayback();
}

void MainWindow::playAll() {
    if (programWav_.empty() || !std::filesystem::exists(programWav_)) {
        playbackTitleLabel_->setText(QStringLiteral("整套听力尚未生成"));
        playbackDetailLabel_->setText(QStringLiteral("请先点击“生成并播放整套”"));
        showStatus(QStringLiteral("当前没有可播放的整套 WAV，请先生成"), true);
        return;
    }
    std::string error;
    if (!player_.playAsync(programWav_, false, &error)) {
        showFailure(QStringLiteral("播放失败"), std::runtime_error(error));
        return;
    }
    playbackTitleLabel_->setText(QStringLiteral("正在播放 · 整套听力"));
    playbackDetailLabel_->setText(
        QStringLiteral("%1 个题组 · 预估 %2")
            .arg(project_.segments.size())
            .arg(durationText(estimateProjectDuration(project_))));
    showStatus(QStringLiteral("已开始播放整套听力"));
    pollPlayback();
}

void MainWindow::togglePlaybackPause() {
    std::string error;
    const auto playback = player_.snapshot(&error);
    bool succeeded = false;
    if (playback.state == platform::windows::WavPlayer::State::Playing) {
        succeeded = player_.pause(&error);
    } else if (playback.state == platform::windows::WavPlayer::State::Paused) {
        succeeded = player_.resume(&error);
    }
    if (!succeeded) {
        if (!error.empty()) {
            showStatus(QStringLiteral("切换播放状态失败：%1").arg(qString(error)), true);
        }
        return;
    }
    pollPlayback();
}

void MainWindow::seekPlaybackFromSlider() {
    std::string error;
    if (!player_.seek(static_cast<std::uint64_t>(playbackSlider_->value()), &error)) {
        if (!error.empty()) {
            showStatus(QStringLiteral("定位播放位置失败：%1").arg(qString(error)), true);
        }
        return;
    }
    pollPlayback();
}

void MainWindow::pollPlayback() {
    std::string error;
    const auto playback = player_.snapshot(&error);
    if (!error.empty()) {
        showStatus(QStringLiteral("读取播放状态失败：%1").arg(qString(error)), true);
        player_.stop();
    }

    const auto cappedDuration = static_cast<int>(std::min<std::uint64_t>(
        playback.durationMilliseconds,
        static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
    const auto cappedPosition = static_cast<int>(std::min<std::uint64_t>(
        playback.positionMilliseconds,
        static_cast<std::uint64_t>(std::max(0, cappedDuration))));
    if (!playbackScrubbing_) {
        const QSignalBlocker blocker(playbackSlider_);
        playbackSlider_->setRange(0, cappedDuration);
        playbackSlider_->setValue(cappedPosition);
    }
    playbackTimeLabel_->setText(
        QStringLiteral("%1 / %2")
            .arg(durationText(std::chrono::milliseconds(playback.positionMilliseconds)))
            .arg(durationText(std::chrono::milliseconds(playback.durationMilliseconds))));

    const bool active = playback.state != platform::windows::WavPlayer::State::Stopped;
    playbackSlider_->setEnabled(active && playback.durationMilliseconds > 0 && !busy_);
    pauseButton_->setEnabled(active && !busy_);
    stopButton_->setEnabled(active && !busy_);
    pauseButton_->setText(playback.state == platform::windows::WavPlayer::State::Paused
                              ? QStringLiteral("▶  继续")
                              : QStringLiteral("Ⅱ  暂停"));
    if (playback.state == platform::windows::WavPlayer::State::Paused) {
        playbackTitleLabel_->setText(QStringLiteral("已暂停"));
    } else if (!active && playback.durationMilliseconds > 0) {
        playbackTitleLabel_->setText(QStringLiteral("播放完成"));
    }
}

void MainWindow::stopPlayback() {
    std::string error;
    if (!player_.stop(&error) && !error.empty()) {
        showStatus(QStringLiteral("停止播放时出现问题：%1").arg(qString(error)), true);
        return;
    }
    if (playbackTitleLabel_) {
        playbackTitleLabel_->setText(QStringLiteral("已停止"));
        playbackSlider_->setRange(0, 0);
        playbackSlider_->setValue(0);
        playbackTimeLabel_->setText(QStringLiteral("0 秒 / 0 秒"));
        pauseButton_->setText(QStringLiteral("Ⅱ  暂停"));
        updateAudioAvailability();
    }
}

void MainWindow::moveSelection(int delta) {
    const int next = segmentList_->currentRow() + delta;
    if (next >= 0 && next < segmentList_->count()) {
        segmentList_->setCurrentRow(next);
    }
}

void MainWindow::beginBusy(const QString& message) {
    busy_ = true;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    openButton_->setEnabled(false);
    saveButton_->setEnabled(false);
    setEditorWidgetsEnabled(false);
    updateAudioAvailability();
    showStatus(message);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::endBusy() {
    busy_ = false;
    QApplication::restoreOverrideCursor();
    openButton_->setEnabled(true);
    saveButton_->setEnabled(true);
    setEditorWidgetsEnabled(true);
    loadCurrentSegment();
    updateAudioAvailability();
}

void MainWindow::showStatus(const QString& message, bool error) {
    statusLabel_->setText(QStringLiteral("%1  %2")
                              .arg(error ? QStringLiteral("●") : QStringLiteral("●"), message));
    statusLabel_->setStyleSheet(error ? QStringLiteral("color: #ffb4b4;") : QString());
}

void MainWindow::showFailure(const QString& title, const std::exception& error) {
    const QString detail = qString(error.what());
    showAppMessage(this, title, detail, MessageTone::Critical);
    showStatus(QStringLiteral("%1：%2").arg(title, detail), true);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == topBar_ && event != nullptr) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            isMaximized() ? showNormal() : showMaximized();
            maximizeButton_->setText(isMaximized() ? QStringLiteral("❐")
                                                   : QStringLiteral("□"));
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && windowHandle() != nullptr) {
                return windowHandle()->startSystemMove();
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef _WIN32
    Q_UNUSED(eventType);
    auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage != nullptr && nativeMessage->message == WM_NCHITTEST && result != nullptr) {
        const QPoint local = mapFromGlobal(
            QPoint(GET_X_LPARAM(nativeMessage->lParam), GET_Y_LPARAM(nativeMessage->lParam)));
        const int border = std::max(6, static_cast<int>(std::lround(7.0 * devicePixelRatioF())));
        if (!isMaximized()) {
            const bool left = local.x() >= 0 && local.x() < border;
            const bool right = local.x() < width() && local.x() >= width() - border;
            const bool top = local.y() >= 0 && local.y() < border;
            const bool bottom = local.y() < height() && local.y() >= height() - border;
            if (top && left) {
                *result = HTTOPLEFT;
                return true;
            }
            if (top && right) {
                *result = HTTOPRIGHT;
                return true;
            }
            if (bottom && left) {
                *result = HTBOTTOMLEFT;
                return true;
            }
            if (bottom && right) {
                *result = HTBOTTOMRIGHT;
                return true;
            }
            if (left) {
                *result = HTLEFT;
                return true;
            }
            if (right) {
                *result = HTRIGHT;
                return true;
            }
            if (top) {
                *result = HTTOP;
                return true;
            }
            if (bottom) {
                *result = HTBOTTOM;
                return true;
            }
        }
        if (topBar_ != nullptr && local.y() >= topBar_->y() &&
            local.y() < topBar_->y() + topBar_->height()) {
            QWidget* hit = childAt(local);
            const bool interactive = qobject_cast<QPushButton*>(hit) != nullptr ||
                                     qobject_cast<QLineEdit*>(hit) != nullptr ||
                                     qobject_cast<QComboBox*>(hit) != nullptr;
            if (!interactive) {
                *result = HTCAPTION;
                return true;
            }
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (renderJob_.isRunning()) {
        closePending_ = true;
        renderJob_.cancel();
        showStatus(QStringLiteral("正在取消生成，完成后再关闭窗口…"));
        event->ignore();
        return;
    }
    if (confirmDiscardChanges()) {
        clearRecovery();
        recoveryStore_.shutdown();
        player_.stop();
        event->accept();
    } else {
        event->ignore();
    }
}

}  // namespace listening::app
