#include "MainWindow.h"
#include "widgets/ComboBox.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QListWidget>
#include <QPalette>
#include <QScreen>
#include <QScrollArea>
#include <QSignalSpy>
#include <QWindow>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
void windowClick(QWidget *widget, QPoint local) {
    auto *window = widget->window();
    const auto position = widget->mapTo(window, local);
    QTest::mouseClick(window->windowHandle(), Qt::LeftButton, Qt::NoModifier, position);
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QApplication::setStyle("Fusion");
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#f6f7f9"));
    palette.setColor(QPalette::WindowText, QColor("#252a34"));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Text, QColor("#252a34"));
    palette.setColor(QPalette::Button, Qt::white);
    palette.setColor(QPalette::ButtonText, QColor("#252a34"));
    palette.setColor(QPalette::Highlight, QColor("#dfeafb"));
    palette.setColor(QPalette::HighlightedText, QColor("#2167ba"));
    app.setPalette(palette);
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "temporary settings");
        QCoreApplication::setOrganizationName("ChunkHarborTests");
        QCoreApplication::setApplicationName("combo-popup");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
        QSettings().setValue("storage/logDirectory", temp.filePath("logs"));
        QSettings().setValue("connection/mode", "tcp");
        MainWindow window(nullptr, temp.filePath("data"), MainWindow::StartupMode::Manual);
        const QString label = app.arguments().value(1, "after");
        QDir().mkpath("combo-artifacts");
        auto *nav = window.findChild<QListWidget *>("navigation");
        auto open = [&](QComboBox *combo) {
            combo->setFocus();
            windowClick(combo, combo->rect().center());
            // QComboBox intentionally ignores the opening gesture's release.
            QTest::qWait(QApplication::doubleClickInterval()+50);
            check(combo->view()->window()->isVisible(), "mouse opens actual popup");
            check(QApplication::activePopupWidget() == combo->view()->window(), "native Qt popup grab retained");
        };
        for (bool maximized : {false, true}) {
            if (maximized) window.showMaximized(); else { window.showNormal(); window.resize(1280, 820); }
            window.activateWindow();
            QTest::qWait(200);
            for (int page : {1, 5, 6}) {
                nav->setCurrentRow(page);
                QTest::qWait(50);
                auto *pageWidget = window.findChild<QStackedWidget *>("pageStack")->currentWidget();
                for (auto *combo : pageWidget->findChildren<QComboBox *>()) {
                    for (auto *parent = combo->parentWidget(); parent; parent = parent->parentWidget())
                        if (auto *scroll = qobject_cast<QScrollArea *>(parent)) scroll->ensureWidgetVisible(combo);
                    QTest::qWait(30);
                    combo->setCurrentIndex(0);
                    // SSH/TCP selection changes visible form rows and label widths.
                    // Let the layout settle before sending real window coordinates.
                    QTest::qWait(50);
                    QSignalSpy activated(combo, &QComboBox::activated);
                    open(combo);
                    auto *popup = combo->view()->window();
                    auto pixels = popup->grab().toImage();
                    const auto dpr = pixels.devicePixelRatio();
                    std::cout << "page=" << page << " max=" << maximized << " dpr=" << dpr
                              << " control=" << combo->width() << " popup=" << popup->width() << std::endl;
                    check(pixels.pixelColor(0, 0).alpha() < 40 &&
                          pixels.pixelColor(pixels.width()-1, 0).alpha() < 40 &&
                          pixels.pixelColor(0, pixels.height()-1).alpha() < 40 &&
                          pixels.pixelColor(pixels.width()-1, pixels.height()-1).alpha() < 40,
                          "all popup outer corners must remain transparent");
                    const auto controlLeft = combo->mapToGlobal(QPoint()).x();
                    if (page == 6) std::cout << "native geometry control=" << controlLeft << " popup=" << popup->x()
                        << " window=" << window.x() << "," << window.width() << " available="
                        << popup->screen()->availableGeometry().left() << "," << popup->screen()->availableGeometry().right() << std::endl;
                    check(qAbs(popup->x() + 8 - controlLeft) <= 1, "rounded panel aligns with combo left edge");
                    check(popup->width()-16 >= combo->width(), "rounded panel covers combo width");
                    if (page == 1) {
                        const auto origin = popup->mapToGlobal(QPoint());
                        const auto name = QString("combo-artifacts/%1-%2-%3")
                            .arg(label).arg(dpr).arg(maximized ? "maximized" : "normal");
                        check(popup->screen()->grabWindow(0, origin.x()-15, origin.y()-15,
                            popup->width()+30, popup->height()+30).save(name+"-screen.png"), "native popup screenshot");
                        check(pixels.save(name+"-alpha.png"), "popup alpha screenshot");
                        const auto frame = window.frameGeometry();
                        check(window.screen()->grabWindow(0, frame.x(), frame.y(), frame.width(),
                            frame.height()).save(name+"-window.png"), "native window and popup screenshot");
                    }
                    QTest::keyClick(combo->view(), Qt::Key_Down);
                    QTest::keyClick(combo->view(), Qt::Key_Up);
                    QTest::keyClick(combo->view(), Qt::Key_Down);
                    QTest::keyClick(combo->view(), Qt::Key_Return);
                    QTest::qWait(30);
                    check(!popup->isVisible() && combo->currentIndex() == 1 && activated.count() == 1,
                          "arrow keys and Enter select exactly once and close");
                    check(combo->hasFocus(), "Enter restores focus to combo");
                    open(combo);
                    QTest::keyClick(combo->view(), Qt::Key_Down);
                    QTest::keyClick(combo->view(), Qt::Key_Escape);
                    QTest::qWait(30);
                    check(!popup->isVisible() && combo->currentIndex() == 1 && activated.count() == 1,
                          "Escape closes without committing preview selection");
                    check(combo->hasFocus(), "Escape restores focus");
                    open(combo);
                    auto *view = combo->view();
                    const auto target = view->model()->index(combo->count()-1, 0);
                    view->scrollTo(target);
                    QTest::qWait(30);
                    const auto point = view->visualRect(target).center();
                    windowClick(view->viewport(), point);
                    QTest::qWait(80);
                    std::cout << "mouse index=" << combo->currentIndex() << " wanted=" << combo->count()-1
                              << " visible=" << popup->isVisible() << " activated=" << activated.count()
                              << " target=" << point.x() << ',' << point.y() << std::endl;
                    check(!popup->isVisible() && combo->currentIndex() == combo->count()-1 && activated.count() == 2,
                          "mouse selects last item exactly once and closes");
                    open(combo);
                    windowClick(&window, QPoint(300, 30));
                    QTest::qWait(80);
                    check(!popup->isVisible() && activated.count() == 2, "outside click closes without activation");
                }
            }
        }
        std::cout << "native popup pixels, keyboard/mouse/cancel/outside/focus and all pages passed" << std::endl;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
