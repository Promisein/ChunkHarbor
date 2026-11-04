#pragma once
#include "InfoCard.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QSplitter>
#include <QVBoxLayout>

namespace ui {
inline constexpr int BrowserRowHeight = 42;
inline QString plainTooltip(const QString &value) {
    return "<qt>" + value.toHtmlEscaped().replace("\n", "<br/>") + "</qt>";
}
class ElidedLabel final : public QLabel {
  public:
    explicit ElidedLabel(QWidget *parent) : QLabel(parent) {
        setTextFormat(Qt::PlainText);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    void setValue(const QString &value) {
        setProperty("fullText", value);
        setToolTip(plainTooltip(value));
        setText(fontMetrics().elidedText(value, Qt::ElideMiddle, contentsRect().width()));
    }
  protected:
    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        setText(fontMetrics().elidedText(property("fullText").toString(), Qt::ElideMiddle, contentsRect().width()));
    }
};
inline void browserPage(QVBoxLayout *layout) {
    layout->setContentsMargins(24, 18, 24, 18);
    layout->setSpacing(10);
}
inline void browserSplitter(QSplitter *splitter) {
    splitter->setProperty("visualRole", "browserSplitter");
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(12);
}
inline void browserList(QAbstractItemView *view) {
    view->setProperty("visualRole", "browserList");
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setTextElideMode(Qt::ElideMiddle);
    view->setAlternatingRowColors(true);
}
inline QLabel *browserEmptyState(QAbstractItemView *view, const QString &text) {
    auto *layout = new QVBoxLayout(view->viewport());
    auto *label = new QLabel(text, view->viewport());
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setProperty("role", "browserEmptyState");
    layout->addWidget(label);
    return label;
}
inline void detailPanel(InfoCard *panel) {
    panel->setProperty("visualRole", "detailPanel");
    panel->setGraphicsEffect(nullptr);
    panel->setMinimumWidth(280);
    panel->setMaximumWidth(400);
    panel->body()->setContentsMargins(18, 16, 18, 16);
    panel->body()->setSpacing(8);
    panel->body()->itemAt(0)->widget()->setProperty("role", "detailPanelTitle");
}
inline ElidedLabel *detailField(InfoCard *panel, const QString &title, const char *name, bool copy = false) {
    auto *caption = new QLabel(title, panel);
    caption->setProperty("role", "muted");
    panel->body()->addWidget(caption);
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    auto *value = new ElidedLabel(panel);
    value->setObjectName(name);
    row->addWidget(value, 1);
    if (copy) {
        auto *button = new QPushButton(QObject::tr("复制"), panel);
        button->setObjectName(QString(name) + "Copy");
        button->setAccessibleName(QObject::tr("复制%1").arg(title));
        button->setProperty("visualRole", "copyAction");
        row->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, panel, [value] {
            QApplication::clipboard()->setText(value->property("fullText").toString());
        });
    }
    panel->body()->addLayout(row);
    return value;
}
} // namespace ui
