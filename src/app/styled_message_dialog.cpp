#include "app/styled_message_dialog.h"

#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace listening::app {
namespace {

class StyledMessageDialog final : public QDialog {
public:
    StyledMessageDialog(QWidget* parent,
                        const QString& title,
                        const QString& detail,
                        MessageTone tone,
                        const QString& primaryText,
                        const QString& secondaryText,
                        const QString& cancelText)
        : QDialog(parent) {
        setObjectName(QStringLiteral("appMessageDialog"));
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setModal(true);
        setMinimumWidth(440);
        setMaximumWidth(620);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        auto* content = new QFrame(this);
        content->setObjectName(QStringLiteral("messageContent"));
        auto* contentLayout = new QHBoxLayout(content);
        contentLayout->setContentsMargins(24, 24, 24, 20);
        contentLayout->setSpacing(16);

        auto* icon = new QLabel(content);
        icon->setObjectName(QStringLiteral("messageIcon"));
        icon->setProperty("tone", static_cast<int>(tone));
        icon->setAlignment(Qt::AlignCenter);
        icon->setFixedSize(42, 42);
        switch (tone) {
        case MessageTone::Critical:
            icon->setText(QStringLiteral("×"));
            break;
        case MessageTone::Question:
            icon->setText(QStringLiteral("?"));
            break;
        case MessageTone::Information:
            icon->setText(QStringLiteral("i"));
            break;
        case MessageTone::Warning:
            icon->setText(QStringLiteral("!"));
            break;
        }
        contentLayout->addWidget(icon, 0, Qt::AlignTop);

        auto* textLayout = new QVBoxLayout;
        textLayout->setSpacing(7);
        auto* titleLabel = new QLabel(title, content);
        titleLabel->setObjectName(QStringLiteral("messageTitle"));
        auto* detailLabel = new QLabel(detail, content);
        detailLabel->setObjectName(QStringLiteral("messageDetail"));
        detailLabel->setWordWrap(true);
        detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        textLayout->addWidget(titleLabel);
        textLayout->addWidget(detailLabel);
        contentLayout->addLayout(textLayout, 1);
        root->addWidget(content);

        auto* footer = new QFrame(this);
        footer->setObjectName(QStringLiteral("messageFooter"));
        auto* buttons = new QHBoxLayout(footer);
        buttons->setContentsMargins(24, 12, 24, 16);
        buttons->setSpacing(9);
        buttons->addStretch();
        if (!cancelText.isEmpty()) {
            auto* cancel = new QPushButton(cancelText, footer);
            cancel->setProperty("quietButton", true);
            connect(cancel, &QPushButton::clicked, this, [this] {
                choice_ = MessageChoice::Cancel;
                reject();
            });
            buttons->addWidget(cancel);
        }
        if (!secondaryText.isEmpty()) {
            auto* secondary = new QPushButton(secondaryText, footer);
            secondary->setProperty("secondaryButton", true);
            connect(secondary, &QPushButton::clicked, this, [this] {
                choice_ = MessageChoice::Secondary;
                accept();
            });
            buttons->addWidget(secondary);
        }
        auto* primary = new QPushButton(primaryText, footer);
        primary->setProperty("primaryButton", true);
        primary->setDefault(true);
        connect(primary, &QPushButton::clicked, this, [this] {
            choice_ = MessageChoice::Primary;
            accept();
        });
        buttons->addWidget(primary);
        root->addWidget(footer);

        setStyleSheet(QStringLiteral(R"CSS(
            QDialog#appMessageDialog {
                background: #F7F5EF;
                border: 1px solid #DCE2EC;
                font-family: "Microsoft YaHei UI", "Segoe UI";
                color: #14213D;
            }
            QFrame#messageContent { background: #FFFFFF; }
            QFrame#messageFooter { background: #F7F5EF; border-top: 1px solid #E2E6EE; }
            QLabel#messageTitle { color: #14213D; font-size: 16px; font-weight: 700; }
            QLabel#messageDetail { color: #5F6B81; font-size: 13px; line-height: 1.4; }
            QLabel#messageIcon {
                border-radius: 21px; font-size: 22px; font-weight: 700;
                color: #4E5AC7; background: #E9EEFF;
            }
            QLabel#messageIcon[tone="1"] { color: #A56A13; background: #FFF2D8; }
            QLabel#messageIcon[tone="2"] { color: #B73D4A; background: #FDEBED; }
            QLabel#messageIcon[tone="3"] { color: #0E7F77; background: #E6F5F1; }
            QPushButton {
                min-height: 36px; padding: 0 16px; border-radius: 8px;
                border: 1px solid #D8DDEA; background: #FFFFFF;
                color: #344158; font-weight: 600;
            }
            QPushButton:hover { background: #F0F2F7; border-color: #BCC4D7; }
            QPushButton[primaryButton="true"] {
                color: white; background: #4E5AC7; border-color: #4E5AC7;
            }
            QPushButton[primaryButton="true"]:hover { background: #3F49B2; }
            QPushButton[secondaryButton="true"] { color: #B43E48; }
        )CSS"));
    }

    [[nodiscard]] MessageChoice choice() const noexcept { return choice_; }

private:
    MessageChoice choice_{MessageChoice::Cancel};
};

}  // namespace

void showAppMessage(QWidget* parent,
                    const QString& title,
                    const QString& detail,
                    MessageTone tone) {
    StyledMessageDialog(parent, title, detail, tone, QStringLiteral("知道了"), {}, {}).exec();
}

MessageChoice askAppMessage(QWidget* parent,
                           const QString& title,
                           const QString& detail,
                           const QString& primaryText,
                           const QString& secondaryText,
                           const QString& cancelText) {
    StyledMessageDialog dialog(parent, title, detail, MessageTone::Question,
                               primaryText, secondaryText, cancelText);
    dialog.exec();
    return dialog.choice();
}

}  // namespace listening::app
