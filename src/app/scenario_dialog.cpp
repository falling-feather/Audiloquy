#include "app/scenario_dialog.h"

#include "app/deepseek_scenario_polisher.h"

#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScreen>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStringList>
#include <QStyle>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#endif

#include <array>
#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <utility>

namespace listening::app {
namespace {

std::string utf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString qString(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString questionKindName(QuestionKind kind) {
    switch (kind) {
        case QuestionKind::When:
            return QStringLiteral("时间（When）");
        case QuestionKind::Where:
            return QStringLiteral("地点（Where）");
        case QuestionKind::Why:
            return QStringLiteral("原因（Why）");
        case QuestionKind::What:
            return QStringLiteral("事物/行为（What）");
        case QuestionKind::Unsupported:
            return QStringLiteral("暂不支持");
    }
    return QStringLiteral("未知");
}

QString evidenceRoleName(EvidenceRole role) {
    return role == EvidenceRole::Supports ? QStringLiteral("支持")
                                          : QStringLiteral("排除");
}

QString verdictName(OptionVerdict verdict) {
    return verdict == OptionVerdict::Supported ? QStringLiteral("正确")
                                               : QStringLiteral("干扰项");
}

QString judgmentExplanation(const OptionJudgment& judgment) {
    const QString original = qString(judgment.explanation);
    if (original == QStringLiteral("The dialogue explicitly states and confirms this answer.")) {
        return QStringLiteral("对话明确陈述并确认了该答案。");
    }
    if (original == QStringLiteral("The dialogue explicitly considers and rejects this answer.")) {
        return QStringLiteral("对话明确讨论并排除了该选项。");
    }
    return original;
}

QString htmlEscaped(std::string_view value) {
    return qString(value).toHtmlEscaped();
}

QString targetLengthHint(const std::optional<std::size_t>& targetWords,
                        std::size_t actualWords) {
    if (!targetWords.has_value()) {
        return {};
    }
    const std::size_t target = *targetWords;
    const std::size_t lower = target * 75 / 100;
    const std::size_t upper = target * 125 / 100;
    if (actualWords >= lower && actualWords <= upper) {
        return {};
    }
    return QStringLiteral("⚠ 目标约 %1 词，实际 %2 词，已超出目标±25%；建议调整目标或修改文稿，证据未删减。")
        .arg(static_cast<qulonglong>(target))
        .arg(static_cast<qulonglong>(actualWords));
}

GenerationQuestion generationQuestionFromDraft(const ScenarioDraft& draft) {
    GenerationQuestion question;
    question.questionStem = draft.sourceRequest.questionStem;
    question.options = draft.sourceRequest.options;
    question.correctAnswer = std::string(answerLabelCode(draft.supportedAnswer));
    question.evidence.reserve(draft.evidence.size());
    for (const ScenarioEvidence& evidence : draft.evidence) {
        question.evidence.push_back(GenerationEvidence{
            std::string(answerLabelCode(evidence.option)),
            evidence.role == EvidenceRole::Supports ? "supports" : "rejects",
            evidence.turnId,
            evidence.quote,
        });
    }
    return question;
}

GenerationRecord generationRecordFromDraft(const ScenarioDraft& draft,
                                           std::string provider,
                                           std::string model) {
    GenerationRecord record;
    record.provider = std::move(provider);
    record.model = std::move(model);
    static_cast<GenerationQuestion&>(record) = generationQuestionFromDraft(draft);
    record.requiresTeacherReview = draft.requiresTeacherReview;
    record.teacherReviewed = false;
    return record;
}

GenerationRecord generationRecordFromMulti(const MultiQuestionDraft& draft,
                                           std::string provider,
                                           std::string model) {
    GenerationRecord record;
    record.provider = std::move(provider);
    record.model = std::move(model);
    if (!draft.questions.empty()) {
        const MultiQuestionItem& first = draft.questions.front();
        record.questionStem = first.sourceRequest.questionStem;
        record.options = first.sourceRequest.options;
        record.correctAnswer = std::string(answerLabelCode(first.supportedAnswer));
        record.evidence.reserve(first.evidence.size());
        for (const ScenarioEvidence& evidence : first.evidence) {
            record.evidence.push_back(GenerationEvidence{
                std::string(answerLabelCode(evidence.option)),
                evidence.role == EvidenceRole::Supports ? "supports" : "rejects",
                evidence.turnId,
                evidence.quote,
            });
        }
        for (std::size_t index = 1; index < draft.questions.size(); ++index) {
            const MultiQuestionItem& item = draft.questions[index];
            GenerationQuestion question;
            question.questionStem = item.sourceRequest.questionStem;
            question.options = item.sourceRequest.options;
            question.correctAnswer = std::string(answerLabelCode(item.supportedAnswer));
            question.evidence.reserve(item.evidence.size());
            for (const ScenarioEvidence& evidence : item.evidence) {
                question.evidence.push_back(GenerationEvidence{
                    std::string(answerLabelCode(evidence.option)),
                    evidence.role == EvidenceRole::Supports ? "supports" : "rejects",
                    evidence.turnId,
                    evidence.quote,
                });
            }
            record.additionalQuestions.push_back(std::move(question));
        }
    }
    record.requiresTeacherReview = draft.requiresTeacherReview;
    record.teacherReviewed = false;
    return record;
}

QLabel* sectionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("scenarioSectionTitle"));
    return label;
}

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setObjectName(QStringLiteral("scenarioPanel"));
    panel->setFrameShape(QFrame::NoFrame);
    return panel;
}

}  // namespace

ScenarioDialog::ScenarioDialog(bool replacesExistingText, QWidget* parent)
    : QDialog(parent), replacesExistingText_(replacesExistingText) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setObjectName(QStringLiteral("scenarioDialog"));
    setWindowTitle(QStringLiteral("语澜 · 智能生成情景"));
    setModal(true);
    const QSize available = screen() != nullptr ? screen()->availableGeometry().size()
                                                : QSize(1280, 800);
    resize(std::min(1380, std::max(960, available.width() - 64)),
           std::min(880, std::max(620, available.height() - 64)));
    setMinimumSize(920, 600);
    buildUi();
    connectUi();
    questionRequests_.push_back(currentRequest());
    refreshQuestionList();
    clearResult();
    const bool scenarioSmoke =
        QCoreApplication::arguments().contains(QStringLiteral("--scenario-smoke-test"));
    const bool multiScenarioSmoke =
        QCoreApplication::arguments().contains(QStringLiteral("--scenario-multi-smoke-test"));
    const bool deepSeekSmoke =
        QCoreApplication::arguments().contains(QStringLiteral("--deepseek-live-smoke-test"));
    if (scenarioSmoke || multiScenarioSmoke || deepSeekSmoke) {
        QTimer::singleShot(100, this, [this] {
            populateExample();
            const bool requireMulti = QCoreApplication::arguments().contains(
                QStringLiteral("--scenario-multi-smoke-test"));
            if (requireMulti) {
                addQuestion();
                questionStemEdit_->setText(QStringLiteral("Where did the woman leave her bag?"));
                optionAEdit_->setText(QStringLiteral("At the station."));
                optionBEdit_->setText(QStringLiteral("In the classroom."));
                optionCEdit_->setText(QStringLiteral("At the cafe."));
                answerCombo_->setCurrentIndex(2);
                saveCurrentQuestion();
            }
            const bool requireDeepSeek = QCoreApplication::arguments().contains(
                QStringLiteral("--deepseek-live-smoke-test"));
            if (requireDeepSeek && deepSeekProviderButton_->isEnabled()) {
                deepSeekProviderButton_->click();
            }
            generateDraft();
            QDir directory(QDir::current());
            directory.mkpath(QStringLiteral("tmp"));
            const bool captured = grab().save(
                directory.filePath(requireDeepSeek
                                       ? QStringLiteral("tmp/deepseek-live-smoke.png")
                                       : requireMulti
                                       ? QStringLiteral("tmp/scenario-multi-smoke.png")
                                       : QStringLiteral("tmp/scenario-generator-smoke.png")),
                "PNG");
            if (generatedDraft_ && captured && adoptButton_->isEnabled() &&
                (!requireDeepSeek || usedDeepSeek_)) {
                adoptDraft();
            } else if (generatedMultiDraft_ && captured && adoptButton_->isEnabled()) {
                adoptDraft();
            } else {
                reject();
            }
        });
    }
}

const std::optional<ScenarioDraft>& ScenarioDialog::adoptedDraft() const noexcept {
    return adoptedDraft_;
}

std::optional<GenerationRecord> ScenarioDialog::adoptedGeneration() const {
    return adoptedGeneration_;
}

std::string ScenarioDialog::adoptedScript() const {
    return adoptedScript_;
}

std::string ScenarioDialog::adoptedProvider() const {
    return usedDeepSeek_ ? "deepseek" : "local-template";
}

std::string ScenarioDialog::adoptedModel() const {
    return utf8(deepSeekModel_);
}

void ScenarioDialog::buildUi() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 14, 18, 16);
    rootLayout->setSpacing(12);

    headerFrame_ = new QFrame(this);
    headerFrame_->setObjectName(QStringLiteral("scenarioHeader"));
    headerFrame_->installEventFilter(this);
    auto* headingRow = new QHBoxLayout(headerFrame_);
    headingRow->setContentsMargins(8, 4, 4, 4);
    headingRow->setSpacing(16);
    auto* backButton = new QPushButton(QStringLiteral("←"), headerFrame_);
    backButton->setObjectName(QStringLiteral("scenarioBackButton"));
    backButton->setProperty("windowButton", true);
    backButton->setFixedSize(38, 38);
    connect(backButton, &QPushButton::clicked, this, &QDialog::reject);
    headingRow->addWidget(backButton, 0, Qt::AlignTop);
    auto* headingBlock = new QVBoxLayout;
    headingBlock->setSpacing(4);
    auto* title = new QLabel(QStringLiteral("从题目反推听力情景"), headerFrame_);
    title->setObjectName(QStringLiteral("scenarioDialogTitle"));
    auto* description = new QLabel(
        QStringLiteral("填写英文题干、选项、答案与英文主题，生成一男一女的短对话，并同步给出答案证据与干扰项判断。"),
        headerFrame_);
    description->setObjectName(QStringLiteral("scenarioDescription"));
    description->setWordWrap(true);
    headingBlock->addWidget(title);
    headingBlock->addWidget(description);
    headingRow->addLayout(headingBlock, 1);
    closeButton_ = new QPushButton(QStringLiteral("×"), headerFrame_);
    closeButton_->setObjectName(QStringLiteral("scenarioCloseButton"));
    closeButton_->setProperty("windowButton", true);
    closeButton_->setProperty("closeButton", true);
    closeButton_->setFixedSize(38, 38);
    headingRow->addWidget(closeButton_, 0, Qt::AlignTop);
    rootLayout->addWidget(headerFrame_);

    auto* providerBar = new QFrame(this);
    providerBar->setObjectName(QStringLiteral("scenarioProviderBar"));
    auto* providerLayout = new QHBoxLayout(providerBar);
    providerLayout->setContentsMargins(14, 10, 14, 10);
    providerLayout->setSpacing(8);
    auto* providerLabel = new QLabel(QStringLiteral("生成策略："), providerBar);
    providerLabel->setObjectName(QStringLiteral("scenarioFieldLabel"));
    localProviderButton_ = new QPushButton(QStringLiteral("本地生成 · 离线"), providerBar);
    deepSeekProviderButton_ =
        new QPushButton(QStringLiteral("DeepSeek 润色 · 可选"), providerBar);
    for (auto* button : {localProviderButton_, deepSeekProviderButton_}) {
        button->setCheckable(true);
        button->setProperty("providerButton", true);
    }
    localProviderButton_->setChecked(true);
    auto* providerGroup = new QButtonGroup(this);
    providerGroup->setExclusive(true);
    providerGroup->addButton(localProviderButton_);
    providerGroup->addButton(deepSeekProviderButton_);
    deepSeekProviderButton_->setEnabled(DeepSeekScenarioPolisher::credentialAvailable());
    deepSeekProviderButton_->setToolTip(
        deepSeekProviderButton_->isEnabled()
            ? QStringLiteral("会把题干、选项、答案、主题和完整本地初稿发送给 DeepSeek；密钥与结果不写入发行包")
            : QStringLiteral("未找到私有密钥；本地生成不受影响"));
    providerLayout->addWidget(providerLabel);
    providerLayout->addWidget(localProviderButton_);
    providerLayout->addWidget(deepSeekProviderButton_);
    providerLayout->addStretch();
    auto* trustNote = new QLabel(
        QStringLiteral("ⓘ 始终保留本地原始稿；API 失败或校验不通过不会覆盖。"), providerBar);
    trustNote->setObjectName(QStringLiteral("scenarioTinyLabel"));
    providerLayout->addWidget(trustNote);
    rootLayout->addWidget(providerBar);

    auto* contentHost = new QWidget(this);
    contentHost->setObjectName(QStringLiteral("scenarioContentHost"));
    contentHost->setMinimumWidth(1160);
    auto* contentLayout = new QGridLayout(contentHost);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setHorizontalSpacing(16);
    contentLayout->setVerticalSpacing(16);
    contentLayout->setColumnStretch(0, 8);
    contentLayout->setColumnStretch(1, 12);
    contentLayout->setColumnStretch(2, 8);

    auto* requestPanel = makePanel(this);
    auto* requestLayout = new QVBoxLayout(requestPanel);
    requestLayout->setContentsMargins(18, 16, 18, 18);
    requestLayout->setSpacing(11);
    requestLayout->addWidget(sectionLabel(QStringLiteral("01 题目信息"), requestPanel));

    auto* questionToolbar = new QHBoxLayout;
    questionToolbar->setSpacing(6);
    auto* questionListLabel = new QLabel(QStringLiteral("共享材料中的题目"), requestPanel);
    questionListLabel->setObjectName(QStringLiteral("scenarioFieldLabel"));
    questionToolbar->addWidget(questionListLabel);
    questionToolbar->addStretch();
    addQuestionButton_ = new QPushButton(QStringLiteral("＋ 新增"), requestPanel);
    addQuestionButton_->setObjectName(QStringLiteral("scenarioAddQuestionButton"));
    removeQuestionButton_ = new QPushButton(QStringLiteral("删除"), requestPanel);
    removeQuestionButton_->setObjectName(QStringLiteral("scenarioRemoveQuestionButton"));
    removeQuestionButton_->setEnabled(false);
    questionToolbar->addWidget(addQuestionButton_);
    questionToolbar->addWidget(removeQuestionButton_);
    requestLayout->addLayout(questionToolbar);
    questionList_ = new QListWidget(requestPanel);
    questionList_->setObjectName(QStringLiteral("scenarioQuestionList"));
    questionList_->setSelectionMode(QAbstractItemView::SingleSelection);
    questionList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    questionList_->setMaximumHeight(86);
    questionList_->setMinimumHeight(48);
    requestLayout->addWidget(questionList_);

    auto* stemLabel = new QLabel(QStringLiteral("英文题干"), requestPanel);
    stemLabel->setObjectName(QStringLiteral("scenarioFieldLabel"));
    questionStemEdit_ = new QLineEdit(requestPanel);
    questionStemEdit_->setObjectName(QStringLiteral("scenarioStemEdit"));
    questionStemEdit_->setPlaceholderText(
        QStringLiteral("When did the woman go to the convenience store?"));
    requestLayout->addWidget(stemLabel);
    requestLayout->addWidget(questionStemEdit_);

    auto* optionsGrid = new QGridLayout;
    optionsGrid->setHorizontalSpacing(8);
    optionsGrid->setVerticalSpacing(8);
    const std::array<QString, 3> optionLabels{
        QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")};
    const std::array<QString, 3> placeholders{
        QStringLiteral("In the morning."), QStringLiteral("In the afternoon."),
        QStringLiteral("Tomorrow.")};
    std::array<QLineEdit**, 3> optionTargets{&optionAEdit_, &optionBEdit_, &optionCEdit_};
    const std::array<QString, 3> objectNames{
        QStringLiteral("scenarioOptionAEdit"), QStringLiteral("scenarioOptionBEdit"),
        QStringLiteral("scenarioOptionCEdit")};
    for (std::size_t index = 0; index < optionTargets.size(); ++index) {
        auto* optionLabel = new QLabel(optionLabels[index], requestPanel);
        optionLabel->setObjectName(QStringLiteral("scenarioOptionLabel"));
        optionLabel->setAlignment(Qt::AlignCenter);
        auto* edit = new QLineEdit(requestPanel);
        edit->setObjectName(objectNames[index]);
        edit->setPlaceholderText(placeholders[index]);
        *optionTargets[index] = edit;
        optionsGrid->addWidget(optionLabel, static_cast<int>(index), 0);
        optionsGrid->addWidget(edit, static_cast<int>(index), 1);
    }
    requestLayout->addLayout(optionsGrid);

    auto* compactForm = new QFormLayout;
    compactForm->setHorizontalSpacing(12);
    compactForm->setVerticalSpacing(9);
    compactForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    answerCombo_ = new QComboBox(requestPanel);
    answerCombo_->setObjectName(QStringLiteral("scenarioAnswerCombo"));
    answerCombo_->addItem(QStringLiteral("请选择正确答案"), -1);
    answerCombo_->addItem(QStringLiteral("A"), 0);
    answerCombo_->addItem(QStringLiteral("B"), 1);
    answerCombo_->addItem(QStringLiteral("C"), 2);
    compactForm->addRow(QStringLiteral("正确答案"), answerCombo_);

    topicEdit_ = new QLineEdit(requestPanel);
    topicEdit_->setObjectName(QStringLiteral("scenarioTopicEdit"));
    topicEdit_->setPlaceholderText(QStringLiteral("如：daily errands / school clubs"));
    topicEdit_->setToolTip(QStringLiteral("当前离线模板会把英文主题写入对话开场"));
    compactForm->addRow(QStringLiteral("英文主题 / 场景"), topicEdit_);

    difficultyCombo_ = new QComboBox(requestPanel);
    difficultyCombo_->setObjectName(QStringLiteral("scenarioDifficultyCombo"));
    difficultyCombo_->addItem(QStringLiteral("基础 A2"), QStringLiteral("A2"));
    difficultyCombo_->addItem(QStringLiteral("标准 B1"), QStringLiteral("B1"));
    difficultyCombo_->addItem(QStringLiteral("进阶 B2"), QStringLiteral("B2"));
    difficultyCombo_->setCurrentIndex(1);
    difficultyCombo_->setToolTip(QStringLiteral("当前离线模板会按层级调整开场表达，全文仍需教师复核"));
    compactForm->addRow(QStringLiteral("语言难度"), difficultyCombo_);

    targetWordsSpin_ = new QSpinBox(requestPanel);
    targetWordsSpin_->setObjectName(QStringLiteral("scenarioTargetWordsSpin"));
    targetWordsSpin_->setRange(50, 240);
    targetWordsSpin_->setSingleStep(5);
    targetWordsSpin_->setValue(55);
    targetWordsSpin_->setSuffix(QStringLiteral(" 词左右"));
    compactForm->addRow(QStringLiteral("目标长度"), targetWordsSpin_);
    requestLayout->addLayout(compactForm);
    requestLayout->addStretch();

    auto* requestActions = new QHBoxLayout;
    exampleButton_ = new QPushButton(QStringLiteral("填入示例"), requestPanel);
    exampleButton_->setObjectName(QStringLiteral("scenarioSampleButton"));
    exampleButton_->setProperty("quietButton", true);
    generateButton_ = new QPushButton(QStringLiteral("生成情景初稿"), requestPanel);
    generateButton_->setObjectName(QStringLiteral("scenarioGenerateButton"));
    generateButton_->setProperty("primaryButton", true);
    requestActions->addWidget(exampleButton_);
    requestActions->addStretch();
    requestActions->addWidget(generateButton_);
    requestLayout->addLayout(requestActions);
    contentLayout->addWidget(requestPanel, 0, 0, 2, 1);

    auto* previewPanel = makePanel(this);
    auto* previewLayout = new QVBoxLayout(previewPanel);
    previewLayout->setContentsMargins(18, 16, 18, 18);
    previewLayout->setSpacing(10);
    auto* previewHeader = new QHBoxLayout;
    previewHeader->addWidget(sectionLabel(QStringLiteral("02 情景文稿"), previewPanel));
    previewHeader->addStretch();
    auto* previewHint = new QLabel(QStringLiteral("男女轮次分别选声 · 采用后可继续编辑"), previewPanel);
    previewHint->setObjectName(QStringLiteral("scenarioTinyLabel"));
    previewHeader->addWidget(previewHint);
    previewLayout->addLayout(previewHeader);
    previewList_ = new QListWidget(previewPanel);
    previewList_->setObjectName(QStringLiteral("scenarioPreviewList"));
    previewList_->setSelectionMode(QAbstractItemView::NoSelection);
    previewList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    previewList_->setSpacing(0);
    previewLayout->addWidget(previewList_, 1);
    contentLayout->addWidget(previewPanel, 0, 1, 2, 1);

    auto* evidencePanel = makePanel(this);
    auto* evidenceLayout = new QVBoxLayout(evidencePanel);
    evidenceLayout->setContentsMargins(18, 16, 18, 18);
    evidenceLayout->setSpacing(8);
    evidenceLayout->addWidget(sectionLabel(QStringLiteral("03 答案核验"), evidencePanel));
    evidenceBrowser_ = new QTextBrowser(evidencePanel);
    evidenceBrowser_->setObjectName(QStringLiteral("scenarioEvidenceBrowser"));
    evidenceBrowser_->setOpenExternalLinks(false);
    evidenceBrowser_->setPlaceholderText(QStringLiteral("系统会列出正确答案证据，以及 A / B / C 的判断依据。"));
    evidenceLayout->addWidget(evidenceBrowser_, 1);
    contentLayout->addWidget(evidencePanel, 0, 2);

    auto* sourcePanel = makePanel(this);
    auto* sourceLayout = new QVBoxLayout(sourcePanel);
    sourceLayout->setContentsMargins(18, 16, 18, 18);
    sourceLayout->setSpacing(9);
    sourceLayout->addWidget(sectionLabel(QStringLiteral("生成来源"), sourcePanel));
    generationSourceLabel_ = new QLabel(sourcePanel);
    generationSourceLabel_->setObjectName(QStringLiteral("scenarioSourceLabel"));
    generationSourceLabel_->setWordWrap(true);
    generationSourceLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    generationSourceLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    generationSourceLabel_->setMinimumHeight(112);
    sourceLayout->addWidget(generationSourceLabel_);
    sourceLayout->addStretch();
    contentLayout->addWidget(sourcePanel, 1, 2);
    contentLayout->setRowStretch(0, 5);
    contentLayout->setRowStretch(1, 3);
    auto* contentScroll = new QScrollArea(this);
    contentScroll->setObjectName(QStringLiteral("scenarioContentScroll"));
    contentScroll->setWidgetResizable(true);
    contentScroll->setFrameShape(QFrame::NoFrame);
    contentScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    contentScroll->setWidget(contentHost);
    rootLayout->addWidget(contentScroll, 1);

    auto* footer = new QHBoxLayout;
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("scenarioStatusLabel"));
    statusLabel_->setWordWrap(true);
    footer->addWidget(statusLabel_, 1);
    cancelButton_ = new QPushButton(QStringLiteral("取消"), this);
    cancelButton_->setObjectName(QStringLiteral("scenarioCancelButton"));
    cancelButton_->setProperty("quietButton", true);
    adoptButton_ = new QPushButton(QStringLiteral("采用到当前题组"), this);
    adoptButton_->setObjectName(QStringLiteral("scenarioAdoptButton"));
    adoptButton_->setProperty("primaryButton", true);
    if (replacesExistingText_) {
        adoptButton_->setText(QStringLiteral("替换当前题组文稿"));
        adoptButton_->setToolTip(QStringLiteral("当前题组已有内容；采用后将由本次初稿替换"));
    }
    footer->addWidget(cancelButton_);
    footer->addWidget(adoptButton_);
    rootLayout->addLayout(footer);

    setStyleSheet(QStringLiteral(R"(
        QDialog#scenarioDialog {
            background: #F7F5EF;
            color: #14213D;
            border: 1px solid #DCE2EC;
            font-family: "Microsoft YaHei UI", "Segoe UI";
            font-size: 13px;
        }
        QFrame#scenarioHeader { background: #F7F5EF; border: none; }
        QFrame#scenarioProviderBar {
            background: #FFFFFF;
            border: 1px solid #DCE2EC;
            border-radius: 11px;
        }
        QScrollArea#scenarioContentScroll, QWidget#scenarioContentHost {
            background: transparent;
        }
        QLabel#scenarioDialogTitle {
            color: #14213D;
            font-size: 24px;
            font-weight: 700;
        }
        QLabel#scenarioDescription {
            color: #68708C;
            font-size: 13px;
        }
        QLabel#scenarioLocalBadge {
            color: #087D78;
            background: #E7F7F4;
            border: 1px solid #BCE9E3;
            border-radius: 13px;
            padding: 5px 10px;
            font-weight: 600;
        }
        QFrame#scenarioPanel {
            background: #FFFFFF;
            border: 1px solid #E1E5F0;
            border-radius: 13px;
        }
        QLabel#scenarioSectionTitle {
            color: #3541A5;
            font-size: 14px;
            font-weight: 700;
        }
        QLabel#scenarioFieldLabel {
            color: #3E455F;
            font-weight: 600;
        }
        QLabel#scenarioOptionLabel {
            color: white;
            background: #4E59C6;
            border-radius: 11px;
            min-width: 22px;
            min-height: 22px;
            max-width: 22px;
            max-height: 22px;
            font-weight: 700;
        }
        QLabel#scenarioTinyLabel {
            color: #8A91A8;
            font-size: 11px;
        }
        QLabel#scenarioSourceLabel {
            color: #536078;
            background: #F3F7F6;
            border: 1px solid #D7E8E4;
            border-radius: 8px;
            padding: 10px;
        }
        QLineEdit, QComboBox, QSpinBox, QTextBrowser {
            color: #1D2340;
            background: #FBFCFF;
            border: 1px solid #D8DDEA;
            border-radius: 8px;
            padding: 7px 9px;
            selection-background-color: #D9DDFB;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus,
        QTextBrowser:focus {
            border: 1px solid #5964D5;
            background: #FFFFFF;
        }
        QListWidget#scenarioPreviewList {
            background: #FBFCFF;
            border: 1px solid #D8DDEA;
            border-radius: 8px;
            outline: none;
        }
        QListWidget#scenarioPreviewList::item {
            border-bottom: 1px solid #E8EBF2;
            background: #FFFFFF;
        }
        QListWidget#scenarioQuestionList {
            color: #1D2340;
            background: #FBFCFF;
            border: 1px solid #D8DDEA;
            border-radius: 8px;
            outline: none;
            selection-background-color: #4E59C6;
            selection-color: #FFFFFF;
        }
        QListWidget#scenarioQuestionList::item {
            color: #1D2340;
            background: #FFFFFF;
            border-bottom: 1px solid #E8EBF2;
            padding: 5px 8px;
        }
        QListWidget#scenarioQuestionList::item:hover {
            background: #EEF0FF;
            color: #1D2340;
        }
        QListWidget#scenarioQuestionList::item:selected {
            background: #4E59C6;
            color: #FFFFFF;
        }
        QLabel#scenarioMaleChip, QLabel#scenarioFemaleChip {
            min-width: 30px; max-width: 30px;
            min-height: 25px; max-height: 25px;
            border-radius: 7px;
            font-weight: 700;
        }
        QLabel#scenarioMaleChip { color: #3856C8; background: #E9EEFF; }
        QLabel#scenarioFemaleChip { color: #C94A55; background: #FDEBED; }
        QLabel#scenarioTurnText { color: #14213D; font-size: 13px; }
        QLabel#scenarioTurnMeta { color: #8A91A8; font-size: 10px; }
        QPushButton {
            min-height: 34px;
            border-radius: 8px;
            padding: 0 14px;
            color: #343B59;
            background: #EEF0F7;
            border: 1px solid #D9DEEA;
            font-weight: 600;
        }
        QPushButton:hover { background: #E4E7F1; }
        QPushButton[providerButton="true"] {
            min-height: 32px;
            background: #FFFFFF;
            color: #536078;
            border: 1px solid #D8DDEA;
        }
        QPushButton[providerButton="true"]:checked {
            background: #4E5AC7;
            color: #FFFFFF;
            border-color: #4E5AC7;
        }
        QPushButton[providerButton="true"]:disabled {
            background: #F3F4F7;
            color: #A1A8B5;
        }
        QPushButton[windowButton="true"] {
            min-width: 38px;
            min-height: 38px;
            padding: 0;
            background: transparent;
            border: none;
            color: #14213D;
            font-size: 17px;
        }
        QPushButton[windowButton="true"]:hover { background: #E9ECF3; }
        QPushButton[closeButton="true"]:hover { background: #C94A55; color: white; }
        QPushButton[primaryButton="true"] {
            color: #FFFFFF;
            background: #4E59C6;
            border: 1px solid #4E59C6;
        }
        QPushButton[primaryButton="true"]:hover { background: #3F49B2; }
        QPushButton[primaryButton="true"]:disabled {
            color: #E4E6F4;
            background: #AEB4DA;
            border-color: #AEB4DA;
        }
        QPushButton[quietButton="true"] { background: #FFFFFF; }
        QLabel#scenarioStatusLabel { color: #087D78; }
        QLabel#scenarioStatusLabel[error="true"] { color: #B43E48; }
    )"));
}

void ScenarioDialog::connectUi() {
    connect(exampleButton_, &QPushButton::clicked, this, [this] { populateExample(); });
    connect(addQuestionButton_, &QPushButton::clicked, this, [this] { addQuestion(); });
    connect(removeQuestionButton_, &QPushButton::clicked, this, [this] { removeQuestion(); });
    connect(questionList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || static_cast<std::size_t>(row) >= questionRequests_.size() ||
            static_cast<std::size_t>(row) == activeQuestionIndex_) {
            return;
        }
        saveCurrentQuestion();
        loadQuestion(static_cast<std::size_t>(row));
    });
    connect(generateButton_, &QPushButton::clicked, this, [this] { generateDraft(); });
    connect(adoptButton_, &QPushButton::clicked, this, [this] { adoptDraft(); });
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::reject);
    connect(localProviderButton_, &QPushButton::clicked, this, [this] {
        generateButton_->setText(QStringLiteral("生成本地初稿"));
        if (localDraft_) {
            generatedDraft_ = localDraft_;
            usedDeepSeek_ = false;
            refreshPreview();
            adoptButton_->setEnabled(true);
            showStatus(QStringLiteral("已切回保留的本地初稿。"));
        } else {
            showStatus(QStringLiteral("本地模板是默认路径，不会发送任何网络请求。"));
        }
    });
    connect(deepSeekProviderButton_, &QPushButton::clicked, this, [this] {
        generateButton_->setText(QStringLiteral("本地生成并可选润色"));
        showStatus(QStringLiteral("将先生成并保留本地初稿；仅在点击生成后发送非证据句进行润色。"));
    });

    const auto invalidate = [this] {
        saveCurrentQuestion();
        clearResult();
    };
    connect(questionStemEdit_, &QLineEdit::textChanged, this, invalidate);
    connect(optionAEdit_, &QLineEdit::textChanged, this, invalidate);
    connect(optionBEdit_, &QLineEdit::textChanged, this, invalidate);
    connect(optionCEdit_, &QLineEdit::textChanged, this, invalidate);
    connect(topicEdit_, &QLineEdit::textChanged, this, invalidate);
    connect(answerCombo_, &QComboBox::currentIndexChanged, this, invalidate);
    connect(difficultyCombo_, &QComboBox::currentIndexChanged, this, invalidate);
    connect(targetWordsSpin_, &QSpinBox::valueChanged, this, invalidate);
}

void ScenarioDialog::populateExample() {
    questionStemEdit_->setText(
        QStringLiteral("When did the woman go to the convenience store?"));
    optionAEdit_->setText(QStringLiteral("In the morning."));
    optionBEdit_->setText(QStringLiteral("In the afternoon."));
    optionCEdit_->setText(QStringLiteral("Tomorrow."));
    answerCombo_->setCurrentIndex(2);
    topicEdit_->setText(QStringLiteral("daily errands and convenience-store shopping"));
    topicEdit_->setCursorPosition(0);
    difficultyCombo_->setCurrentIndex(1);
    targetWordsSpin_->setValue(55);
    saveCurrentQuestion();
    refreshQuestionList();
    showStatus(QStringLiteral("示例已填入；点击“生成情景初稿”即可查看结果。"));
}

void ScenarioDialog::saveCurrentQuestion() {
    if (questionRequests_.empty() || activeQuestionIndex_ >= questionRequests_.size() ||
        questionStemEdit_ == nullptr) {
        return;
    }
    questionRequests_[activeQuestionIndex_] = currentRequest();
    refreshQuestionList();
}

void ScenarioDialog::loadQuestion(std::size_t index) {
    if (index >= questionRequests_.size()) {
        return;
    }
    activeQuestionIndex_ = index;
    const ScenarioRequest& request = questionRequests_[index];
    const QSignalBlocker stemBlocker(questionStemEdit_);
    const QSignalBlocker optionABlocker(optionAEdit_);
    const QSignalBlocker optionBBlocker(optionBEdit_);
    const QSignalBlocker optionCBlocker(optionCEdit_);
    const QSignalBlocker answerBlocker(answerCombo_);
    const QSignalBlocker topicBlocker(topicEdit_);
    const QSignalBlocker difficultyBlocker(difficultyCombo_);
    const QSignalBlocker targetBlocker(targetWordsSpin_);
    questionStemEdit_->setText(qString(request.questionStem));
    optionAEdit_->setText(qString(request.options[0]));
    optionBEdit_->setText(qString(request.options[1]));
    optionCEdit_->setText(qString(request.options[2]));
    answerCombo_->setCurrentIndex(request.correctAnswer.has_value()
                                       ? static_cast<int>(*request.correctAnswer) + 1
                                       : 0);
    topicEdit_->setText(qString(request.topic));
    if (request.difficulty.has_value()) {
        const int difficultyIndex = difficultyCombo_->findData(qString(*request.difficulty));
        difficultyCombo_->setCurrentIndex(difficultyIndex >= 0 ? difficultyIndex : 1);
    } else {
        difficultyCombo_->setCurrentIndex(1);
    }
    if (request.targetWordCount.has_value()) {
        targetWordsSpin_->setValue(static_cast<int>(*request.targetWordCount));
    } else {
        targetWordsSpin_->setValue(55);
    }
}

void ScenarioDialog::refreshQuestionList() {
    if (questionList_ == nullptr) {
        return;
    }
    const QSignalBlocker blocker(questionList_);
    questionList_->clear();
    for (std::size_t index = 0; index < questionRequests_.size(); ++index) {
        const ScenarioRequest& request = questionRequests_[index];
        QString stem = qString(request.questionStem);
        if (stem.isEmpty()) {
            stem = QStringLiteral("待填写题目");
        }
        if (stem.size() > 42) {
            stem = stem.left(42) + QStringLiteral("…");
        }
        questionList_->addItem(QStringLiteral("第 %1 题　%2")
                                   .arg(static_cast<qulonglong>(index + 1), 1, 10, QLatin1Char('0'))
                                   .arg(stem));
    }
    if (!questionRequests_.empty()) {
        questionList_->setCurrentRow(static_cast<int>(std::min(
            activeQuestionIndex_, questionRequests_.size() - 1)));
    }
    removeQuestionButton_->setEnabled(questionRequests_.size() > 1);
    deepSeekProviderButton_->setEnabled(questionRequests_.size() == 1);
    if (questionRequests_.size() > 1) localProviderButton_->setChecked(true);
}

void ScenarioDialog::addQuestion() {
    saveCurrentQuestion();
    ScenarioRequest next;
    if (!questionRequests_.empty()) {
        next.topic = questionRequests_.front().topic;
        next.difficulty = questionRequests_.front().difficulty;
        next.targetWordCount = questionRequests_.front().targetWordCount;
    }
    questionRequests_.push_back(std::move(next));
    activeQuestionIndex_ = questionRequests_.size() - 1;
    refreshQuestionList();
    loadQuestion(activeQuestionIndex_);
    clearResult();
    questionStemEdit_->setFocus(Qt::OtherFocusReason);
    showStatus(QStringLiteral("已新增题目；填写完成后可与前面题目共用同一段听力。"));
}

void ScenarioDialog::removeQuestion() {
    if (questionRequests_.size() <= 1) {
        return;
    }
    saveCurrentQuestion();
    questionRequests_.erase(questionRequests_.begin() + static_cast<std::ptrdiff_t>(activeQuestionIndex_));
    activeQuestionIndex_ = std::min(activeQuestionIndex_, questionRequests_.size() - 1);
    refreshQuestionList();
    loadQuestion(activeQuestionIndex_);
    clearResult();
    showStatus(QStringLiteral("已删除当前题目。"));
}

ScenarioRequest ScenarioDialog::currentRequest() const {
    ScenarioRequest request;
    request.questionStem = utf8(questionStemEdit_->text().trimmed());
    request.options = {utf8(optionAEdit_->text().trimmed()),
                       utf8(optionBEdit_->text().trimmed()),
                       utf8(optionCEdit_->text().trimmed())};
    const int answer = answerCombo_->currentData().toInt();
    if (answer >= 0 && answer <= 2) {
        request.correctAnswer = static_cast<AnswerLabel>(answer);
    }
    request.topic = utf8(topicEdit_->text().trimmed());
    request.difficulty = utf8(difficultyCombo_->currentData().toString());
    request.targetWordCount = static_cast<std::size_t>(targetWordsSpin_->value());
    return request;
}

void ScenarioDialog::generateDraft() {
    saveCurrentQuestion();
    const bool multiQuestion = questionRequests_.size() >= 2;
    const ScenarioRequest request = currentRequest();
    generateButton_->setEnabled(false);
    try {
        adoptedGeneration_.reset();
        adoptedScript_.clear();
        generatedMultiDraft_.reset();
        if (multiQuestion) {
            MultiQuestionRequest multiRequest;
            multiRequest.questions = questionRequests_;
            multiRequest.topic = request.topic;
            multiRequest.difficulty = request.difficulty;
            multiRequest.targetWordCount = request.targetWordCount;
            generatedMultiDraft_ = generateLocalMultiQuestion(multiRequest);
            localDraft_.reset();
            generatedDraft_.reset();
        } else {
            localDraft_ = generateLocalScenario(request);
            generatedDraft_ = localDraft_;
        }
        adoptedDraft_.reset();
        usedDeepSeek_ = false;
        deepSeekModel_.clear();
        deepSeekPromptTokens_ = 0;
        deepSeekCompletionTokens_ = 0;

        QString resultStatus =
            QStringLiteral("本地初稿已生成并完成结构校验。采用前请核对答案证据。");
        bool resultIsWarning = false;
        if (deepSeekProviderButton_->isChecked() && !multiQuestion) {
            showStatus(QStringLiteral("本地初稿已保留；正在请求 DeepSeek 自然化对话并重建事实锚点……"));
            QApplication::processEvents();
            try {
                const DeepSeekPolishResult polished =
                    DeepSeekScenarioPolisher().polish(request, *localDraft_);
                generatedDraft_ = polished.draft;
                usedDeepSeek_ = true;
                deepSeekModel_ = polished.model;
                deepSeekPromptTokens_ = polished.promptTokens;
                deepSeekCompletionTokens_ = polished.completionTokens;
                resultStatus = QStringLiteral(
                    "DeepSeek 润色已完成，并再次通过本地结构与事实锚点校验；仍待教师确认语义唯一性。");
            } catch (const std::exception& error) {
                generatedDraft_ = localDraft_;
                resultStatus = QStringLiteral("可选润色未采用：%1 本地初稿仍可直接使用。")
                                   .arg(QString::fromUtf8(error.what()));
                resultIsWarning = true;
            }
        } else if (deepSeekProviderButton_->isChecked() && multiQuestion) {
            resultStatus = QStringLiteral(
                "多题共用材料已由本地模板生成；当前先不调用 API，确保所有题目共享同一组事实。请教师逐题复核。"
            );
            resultIsWarning = true;
        }
        refreshPreview();
        adoptButton_->setEnabled(true);
        showStatus(resultStatus, resultIsWarning);
    } catch (const ScenarioValidationError& error) {
        localDraft_.reset();
        generatedDraft_.reset();
        generatedMultiDraft_.reset();
        adoptedDraft_.reset();
        adoptedGeneration_.reset();
        adoptedScript_.clear();
        previewList_->clear();
        evidenceBrowser_->clear();
        adoptButton_->setEnabled(false);
        QStringList messages;
        for (const auto& issue : error.issues()) {
            messages.push_back(QStringLiteral("%1：%2")
                                   .arg(qString(issue.path), qString(issue.message)));
        }
        showStatus(messages.isEmpty() ? QStringLiteral("题目信息不完整，请检查后重试。")
                                      : messages.join(QStringLiteral("；")),
                   true);
    } catch (const std::exception& error) {
        clearResult();
        showStatus(QStringLiteral("生成失败：%1").arg(QString::fromUtf8(error.what())), true);
    }
    generateButton_->setEnabled(true);
}

void ScenarioDialog::adoptDraft() {
    if (!generatedDraft_ && !generatedMultiDraft_) {
        showStatus(QStringLiteral("请先生成一份可采用的情景初稿。"), true);
        return;
    }
    adoptedGeneration_.reset();
    adoptedScript_.clear();
    if (generatedMultiDraft_) {
        adoptedScript_ = renderMultiDialogueScript(*generatedMultiDraft_);
        adoptedGeneration_ = generationRecordFromMulti(
            *generatedMultiDraft_, adoptedProvider(), adoptedModel());
        adoptedDraft_.reset();
    } else {
        adoptedDraft_ = generatedDraft_;
        adoptedScript_ = renderDialogueScript(*generatedDraft_);
        adoptedGeneration_ = generationRecordFromDraft(
            *generatedDraft_, adoptedProvider(), adoptedModel());
    }
    accept();
}

void ScenarioDialog::clearResult() {
    localDraft_.reset();
    generatedDraft_.reset();
    adoptedDraft_.reset();
    generatedMultiDraft_.reset();
    adoptedGeneration_.reset();
    adoptedScript_.clear();
    usedDeepSeek_ = false;
    deepSeekModel_.clear();
    previewList_->clear();
    evidenceBrowser_->clear();
    if (generationSourceLabel_ != nullptr) {
        generationSourceLabel_->setText(
            QStringLiteral("本地生成（离线）\n尚未向任何网络服务发送内容。"));
    }
    adoptButton_->setEnabled(false);
    showStatus(QStringLiteral("生成结果会保留题干、选项和证据，便于老师复核。"));
}

void ScenarioDialog::refreshPreview() {
    const std::vector<DialogueTurn>* turns = nullptr;
    if (generatedMultiDraft_) {
        turns = &generatedMultiDraft_->turns;
    } else if (generatedDraft_) {
        turns = &generatedDraft_->turns;
    } else {
        return;
    }
    previewList_->clear();
    for (std::size_t index = 0; index < turns->size(); ++index) {
        const DialogueTurn& turn = (*turns)[index];
        auto* item = new QListWidgetItem(previewList_);
        auto* row = new QWidget(previewList_);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 7, 9, 7);
        rowLayout->setSpacing(10);
        auto* chip = new QLabel(turn.speaker == SpeakerGender::Male ? QStringLiteral("男")
                                                                    : QStringLiteral("女"),
                                row);
        chip->setObjectName(turn.speaker == SpeakerGender::Male
                                ? QStringLiteral("scenarioMaleChip")
                                : QStringLiteral("scenarioFemaleChip"));
        chip->setAlignment(Qt::AlignCenter);
        auto* textLabel = new QLabel(qString(turn.text), row);
        textLabel->setObjectName(QStringLiteral("scenarioTurnText"));
        textLabel->setWordWrap(true);
        auto* meta = new QLabel(QStringLiteral("%1 · %2 ms")
                                    .arg(index + 1)
                                    .arg(turn.pauseAfterMs),
                                row);
        meta->setObjectName(QStringLiteral("scenarioTurnMeta"));
        rowLayout->addWidget(chip, 0, Qt::AlignTop);
        rowLayout->addWidget(textLabel, 1);
        rowLayout->addWidget(meta, 0, Qt::AlignTop);
        item->setSizeHint(QSize(0, turn.text.size() > 90 ? 70 : 54));
        previewList_->setItemWidget(item, row);
    }
    if (generatedMultiDraft_) {
        const QString lengthHint = targetLengthHint(
            generatedMultiDraft_->sourceRequest.targetWordCount,
            generatedMultiDraft_->wordCount);
        QString sourceLabel =
            QStringLiteral("✓ 本地共享场景模板（离线）\n"
                           "✓ %1 道题共用同一段对话与事实\n"
                           "✓ 每题均已建立支持/排除证据\n"
                           "⚑ 语义唯一性：待教师逐题确认\n"
                           "实际词数：%2\n"
                           "未向任何网络服务发送内容")
                .arg(static_cast<qulonglong>(generatedMultiDraft_->questions.size()))
                .arg(static_cast<qulonglong>(generatedMultiDraft_->wordCount));
        if (!lengthHint.isEmpty()) {
            sourceLabel += QStringLiteral("\n") + lengthHint;
        }
        generationSourceLabel_->setText(sourceLabel);
        QString html;
        html += QStringLiteral("<div style='padding:8px 10px;margin:0 0 10px 0;"
                               "background:#EAF7F2;color:#087D78;border:1px solid #C7E8DD;"
                               "border-radius:8px;font-weight:700'>✓ 多题共用同一段材料 · ⚑ 待逐题复核</div>");
        html += QStringLiteral("<p style='margin:0 0 8px 0;color:#59617d'>场景：%1　词数：%2</p>")
                    .arg(htmlEscaped(generatedMultiDraft_->setting))
                    .arg(static_cast<qulonglong>(generatedMultiDraft_->wordCount));
        if (!lengthHint.isEmpty()) {
            html += QStringLiteral("<div style='padding:7px 9px;margin:0 0 8px 0;"
                                   "background:#FFF4DE;color:#9A6A24;border:1px solid #F0D39A;"
                                   "border-radius:7px'>%1</div>")
                        .arg(lengthHint.toHtmlEscaped());
        }
        for (std::size_t questionIndex = 0;
             questionIndex < generatedMultiDraft_->questions.size(); ++questionIndex) {
            const MultiQuestionItem& question = generatedMultiDraft_->questions[questionIndex];
            html += QStringLiteral("<p style='margin:10px 0 4px 0'><b>第%1题：</b>%2</p>")
                        .arg(static_cast<qulonglong>(questionIndex + 1))
                        .arg(htmlEscaped(question.sourceRequest.questionStem));
            html += QStringLiteral("<table cellspacing='0' cellpadding='4' width='100%'>");
            for (const auto& judgment : question.optionJudgments) {
                const bool supported = judgment.verdict == OptionVerdict::Supported;
                html += QStringLiteral("<tr><td width='28'><b>%1</b></td><td width='54' style='color:%2'>%3</td><td>%4</td></tr>")
                            .arg(qString(answerLabelCode(judgment.option)),
                                 supported ? QStringLiteral("#087D78") : QStringLiteral("#8A6070"),
                                 verdictName(judgment.verdict),
                                 judgment.explanation);
            }
            html += QStringLiteral("</table><ul style='margin-top:4px'>");
            for (const auto& evidence : question.evidence) {
                html += QStringLiteral("<li><b>%1 · %2</b>：&ldquo;%3&rdquo;</li>")
                            .arg(qString(answerLabelCode(evidence.option)),
                                 evidenceRoleName(evidence.role), htmlEscaped(evidence.quote));
            }
            html += QStringLiteral("</ul>");
        }
        evidenceBrowser_->setHtml(html);
        return;
    }
    const ScenarioDraft& draft = *generatedDraft_;
    const QString lengthHint = targetLengthHint(
        draft.sourceRequest.targetWordCount, draft.wordCount);
    if (usedDeepSeek_) {
        QString sourceLabel =
            QStringLiteral("① 本地确定性初稿：已通过\n"
                           "② DeepSeek 可选润色：%1\n"
                           "③ 本地结构与事实锚点复检：已通过\n"
                           "⚑ 语义唯一性：待教师确认\n"
                           "实际词数：%2\n"
                           "令牌：输入 %3 / 输出 %4")
                .arg(deepSeekModel_)
                .arg(static_cast<qulonglong>(draft.wordCount))
                .arg(deepSeekPromptTokens_)
                .arg(deepSeekCompletionTokens_);
        if (!lengthHint.isEmpty()) {
            sourceLabel += QStringLiteral("\n") + lengthHint;
        }
        generationSourceLabel_->setText(sourceLabel);
    } else {
        QString sourceLabel =
            QStringLiteral("✓ 本地确定性模板（离线）\n"
                           "✓ 结构与事实锚点校验完成\n"
                           "⚑ 语义唯一性待教师确认\n"
                           "实际词数：%1\n"
                           "未向任何网络服务发送内容")
                .arg(static_cast<qulonglong>(draft.wordCount));
        if (!lengthHint.isEmpty()) {
            sourceLabel += QStringLiteral("\n") + lengthHint;
        }
        generationSourceLabel_->setText(sourceLabel);
    }

    QString html;
    html += QStringLiteral("<div style='padding:8px 10px;margin:0 0 10px 0;"
                           "background:#EAF7F2;color:#087D78;border:1px solid #C7E8DD;"
                           "border-radius:8px;font-weight:700'>✓ 结构与事实锚点通过 · ⚑ 待教师语义复核</div>");
    html += QStringLiteral("<p style='margin:0 0 8px 0;color:#59617d'>");
    html += QStringLiteral("类型：<b>%1</b>　场景：%2　词数：%3")
                .arg(questionKindName(draft.questionKind), htmlEscaped(draft.setting))
                .arg(static_cast<qulonglong>(draft.wordCount));
    html += QStringLiteral("</p>");
    if (!lengthHint.isEmpty()) {
        html += QStringLiteral("<div style='padding:7px 9px;margin:0 0 8px 0;"
                               "background:#FFF4DE;color:#9A6A24;border:1px solid #F0D39A;"
                               "border-radius:7px'>%1</div>")
                    .arg(lengthHint.toHtmlEscaped());
    }
    html += QStringLiteral("<p style='margin:0 0 10px 0'>预设答案（结构匹配）：<b style='color:#087D78'>%1</b></p>")
                .arg(qString(answerLabelCode(draft.supportedAnswer)));
    html += QStringLiteral("<table cellspacing='0' cellpadding='4' width='100%'>");
    for (const auto& judgment : draft.optionJudgments) {
        const bool supported = judgment.verdict == OptionVerdict::Supported;
        html += QStringLiteral("<tr><td width='28'><b>%1</b></td><td width='54' style='color:%2'>%3</td><td>%4</td></tr>")
                    .arg(qString(answerLabelCode(judgment.option)),
                         supported ? QStringLiteral("#087D78") : QStringLiteral("#8A6070"),
                         verdictName(judgment.verdict),
                         judgmentExplanation(judgment).toHtmlEscaped());
    }
    html += QStringLiteral("</table>");
    html += QStringLiteral("<p style='margin:10px 0 4px 0'><b>原文证据</b></p><ul style='margin-top:0'>");
    for (const auto& evidence : draft.evidence) {
        html += QStringLiteral("<li><b>%1 · %2</b>：&ldquo;%3&rdquo;</li>")
                    .arg(qString(answerLabelCode(evidence.option)),
                         evidenceRoleName(evidence.role), htmlEscaped(evidence.quote));
    }
    html += QStringLiteral("</ul>");
    html += QStringLiteral("<p style='color:#9A6A24;margin:8px 0 0 0'>⚑ 规则与模型初稿仍需教师核对；男女标签会在主界面按轮次、口音地区与性别分别选声。</p>");
    evidenceBrowser_->setHtml(html);
}

void ScenarioDialog::showStatus(const QString& message, bool error) {
    statusLabel_->setText(message);
    statusLabel_->setProperty("error", error);
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
}

bool ScenarioDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == headerFrame_ && event != nullptr &&
        event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && windowHandle() != nullptr) {
            return windowHandle()->startSystemMove();
        }
    }
    return QDialog::eventFilter(watched, event);
}

bool ScenarioDialog::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef _WIN32
    Q_UNUSED(eventType);
    auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage != nullptr && nativeMessage->message == WM_NCHITTEST && result != nullptr) {
        const QPoint local = mapFromGlobal(
            QPoint(GET_X_LPARAM(nativeMessage->lParam), GET_Y_LPARAM(nativeMessage->lParam)));
        const int border = std::max(6, static_cast<int>(7.0 * devicePixelRatioF()));
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
        if (left || right || top || bottom) {
            *result = left ? HTLEFT : right ? HTRIGHT : top ? HTTOP : HTBOTTOM;
            return true;
        }
        if (headerFrame_ != nullptr && local.y() >= headerFrame_->y() &&
            local.y() < headerFrame_->y() + headerFrame_->height()) {
            QWidget* hit = childAt(local);
            if (qobject_cast<QPushButton*>(hit) == nullptr) {
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
    return QDialog::nativeEvent(eventType, message, result);
}

}  // namespace listening::app
