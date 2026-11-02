#pragma once
#include <QComboBox>
#include <QFrame>
#include <QLayout>
#include <QListView>
#include <QStyledItemDelegate>
namespace ui {
// Use the same styled item view on every page instead of the platform menu delegate.
class ComboBox final : public QComboBox {
  public:
    explicit ComboBox(QWidget *parent = nullptr) : QComboBox(parent) {
        auto *list = new QListView;
        list->setFrameShape(QFrame::NoFrame);
        list->setUniformItemSizes(true);
        list->setSpacing(2);
        list->setItemDelegate(new QStyledItemDelegate(list));
        setView(list);
        auto *popup = list->window();
        popup->setObjectName("comboPopup");
        popup->setAttribute(Qt::WA_TranslucentBackground);
        popup->setWindowFlag(Qt::FramelessWindowHint);
        if (auto *frame = qobject_cast<QFrame *>(popup))
            frame->setFrameShape(QFrame::NoFrame);
        if (popup->layout())
            popup->layout()->setContentsMargins(0, 0, 0, 0);
        setMaxVisibleItems(8);
    }
    void showPopup() override {
        view()->setMinimumWidth(width());
        QComboBox::showPopup();
    }
};
} // namespace ui
