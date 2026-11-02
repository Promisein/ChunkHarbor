#include "WorkspacePage.h"
#include "widgets/InfoCard.h"
#include <QLabel>
#include <QVBoxLayout>
WorkspacePage::WorkspacePage(const QString &title, const QString &subtitle, QWidget *parent)
    : QWidget(parent), layout_(new QVBoxLayout(this)) {
    setObjectName("workspacePage");
    setAttribute(Qt::WA_StyledBackground, true);
    layout_->setContentsMargins(32, 28, 32, 32);
    layout_->setSpacing(24);
    auto *heading = new QLabel(title, this);
    heading->setProperty("role", "pageTitle");
    layout_->addWidget(heading);
    auto *detail = new QLabel(subtitle, this);
    detail->setProperty("role", "subtitle");
    detail->setWordWrap(true);
    layout_->addWidget(detail);
}
void WorkspacePage::placeholder(const QString &title, const QString &description) {
    auto *card = new InfoCard(title, this);
    auto *empty = new QLabel(description, card);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setProperty("role", "emptyState");
    card->body()->addStretch();
    card->body()->addWidget(empty);
    card->body()->addStretch();
    layout_->addWidget(card, 1);
}
