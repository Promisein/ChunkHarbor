#include "InfoCard.h"
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QVBoxLayout>
InfoCard::InfoCard(const QString &title, QWidget *parent)
    : QFrame(parent), body_(new QVBoxLayout(this)) {
    setObjectName("infoCard");
    body_->setContentsMargins(24, 22, 24, 24);
    body_->setSpacing(14);
    auto *label = new QLabel(title, this);
    label->setProperty("role", "cardTitle");
    body_->addWidget(label);
    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(22);
    shadow->setOffset(0, 3);
    shadow->setColor(QColor(24, 39, 60, 12));
    setGraphicsEffect(shadow);
}

void InfoCard::setSubtitle(const QString &text) {
    if (!subtitle_) {
        subtitle_ = new QLabel(this);
        subtitle_->setWordWrap(true);
        subtitle_->setProperty("role", "muted");
        body_->insertWidget(1, subtitle_);
    }
    subtitle_->setText(text);
    subtitle_->setVisible(!text.isEmpty());
}
