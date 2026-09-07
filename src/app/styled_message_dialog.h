#pragma once

#include <QString>

class QWidget;

namespace listening::app {

enum class MessageTone {
    Information,
    Warning,
    Critical,
    Question,
};

enum class MessageChoice {
    Primary,
    Secondary,
    Cancel,
};

void showAppMessage(QWidget* parent,
                    const QString& title,
                    const QString& detail,
                    MessageTone tone);

[[nodiscard]] MessageChoice askAppMessage(QWidget* parent,
                                          const QString& title,
                                          const QString& detail,
                                          const QString& primaryText,
                                          const QString& secondaryText,
                                          const QString& cancelText);

}  // namespace listening::app
