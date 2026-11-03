#pragma once
#include <QComboBox>
#include <QBoxLayout>
#include <QEvent>
#include <QFrame>
#include <QLayout>
#include <QListView>
#include <QPainter>
#include <QScreen>
#include <QStyledItemDelegate>
namespace ui {
// Use the same styled item view on every page instead of the platform menu delegate.
class ComboBox final : public QComboBox {
    static constexpr int ShadowMargin = 8;
    QWidget *popup_ = nullptr; // Owned by QComboBox; no replacement popup or input handlers.
  public:
    explicit ComboBox(QWidget *parent = nullptr) : QComboBox(parent) {
        auto *list = new QListView;
        list->setObjectName("comboPopupList");
        list->setFrameShape(QFrame::NoFrame);
        list->setUniformItemSizes(true);
        list->setSpacing(2);
        list->setItemDelegate(new QStyledItemDelegate(list));
        setView(list);
        auto *popup = popup_ = list->window();
        popup->setObjectName("comboPopup");
        popup->setAttribute(Qt::WA_TranslucentBackground);
        popup->setWindowFlag(Qt::FramelessWindowHint);
        popup->setWindowFlag(Qt::NoDropShadowWindowHint);
        popup->setAutoFillBackground(false);
        list->setAutoFillBackground(false);
        list->viewport()->setAutoFillBackground(false);
        popup->installEventFilter(this);
        if (auto *frame = qobject_cast<QFrame *>(popup))
            frame->setFrameShape(QFrame::NoFrame);
        if (popup->layout())
            popup->layout()->setContentsMargins(ShadowMargin, ShadowMargin, ShadowMargin, ShadowMargin);
        setMaxVisibleItems(8);
    }
    void showPopup() override {
        view()->setMinimumWidth(qMax(0, width() - 2 * ShadowMargin));
        // Styles can insert menu spacers. The painted panel owns its own insets.
        if (auto *layout = qobject_cast<QBoxLayout *>(popup_->layout())) {
            for (int i = 0; i < layout->count(); ++i)
                if (auto *spacer = layout->itemAt(i)->spacerItem())
                    spacer->changeSize(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
            layout->invalidate();
        }
        QComboBox::showPopup();
        if (popup_->isVisible()) {
            // Reserve shadow outside the control width, keeping the panel aligned.
            auto geometry = popup_->geometry();
            const auto available = popup_->screen()->availableGeometry();
            geometry.setWidth(qMin(available.width(), qMax(geometry.width(), width() + 2 * ShadowMargin)));
            const auto origin = mapToGlobal(QPoint());
            const int left = layoutDirection() == Qt::RightToLeft
                ? origin.x() + width() + ShadowMargin - geometry.width() : origin.x() - ShadowMargin;
            geometry.moveLeft(qBound(available.left(), left, available.right() - geometry.width() + 1));
            popup_->setGeometry(geometry);
        }
    }

  protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == popup_ && event->type() == QEvent::Paint) {
            // QComboBox's private container paints PE_PanelMenu even with NoFrame.
            // Paint only this surface; leave its mouse/keyboard/focus handling intact.
            QPainter painter(popup_);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(popup_->rect(), Qt::transparent);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            painter.setRenderHint(QPainter::Antialiasing);
            const QRectF panel = QRectF(popup_->rect()).adjusted(
                ShadowMargin, ShadowMargin, -ShadowMargin, -ShadowMargin);
            painter.setPen(Qt::NoPen);
            for (int spread = 6; spread > 0; --spread) {
                painter.setBrush(QColor(35, 49, 69, 5));
                painter.drawRoundedRect(panel.adjusted(-spread, 1-spread, spread, 1+spread),
                                        10+spread, 10+spread);
            }
            painter.setBrush(Qt::white);
            painter.setPen(QPen(QColor("#e3e7ef"), 1));
            painter.drawRoundedRect(panel.adjusted(.5, .5, -.5, -.5), 10, 10);
            return true;
        }
        return QComboBox::eventFilter(object, event);
    }
};
} // namespace ui
