#include "MainWindow.h"
#include "FileListController.h"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <memory>

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
    if (argc < 3) return 64;
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 65;
    QCoreApplication::setOrganizationName("ChunkHarborTests");
    QCoreApplication::setApplicationName("files-probe");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings().setValue("storage/logDirectory", temporary.filePath("logs"));
    auto window = std::make_unique<MainWindow>(nullptr, temporary.filePath("workspace"));
    auto *controller = window->fileListController();
    auto *port = window->findChild<QSpinBox *>("serverPort");
    port->setValue(QString::fromLocal8Bit(argv[1]).toUInt());
    window->findChild<QListWidget *>("navigation")->setCurrentRow(2);
    const QString mode = QString::fromLocal8Bit(argv[2]);
    auto button = [&](const char *name) { return window->findChild<QPushButton *>(name); };
    auto *table = window->findChild<QTableView *>("filesTable");
    auto *search = window->findChild<QLineEdit *>("fileSearch");
    int heartbeat = 0;
    QTimer pulse;
    pulse.setInterval(10);
    QObject::connect(&pulse, &QTimer::timeout, &app, [&] { ++heartbeat; });
    pulse.start();
    QJsonArray pages;
    auto output = [&](bool success) {
        QJsonArray rows;
        if (window) {
            for (const auto &f : controller->model()->page().files)
                rows.append(QJsonObject{{"path", QString::fromUtf8(f.path)}, {"version", QString::fromStdString(f.versionId)},
                                        {"size", QString::number(static_cast<qulonglong>(f.size))},
                                        {"sha256", QString::fromStdString(f.sha256)}});
        }
        QJsonObject result{{"success", success}, {"rows", rows}, {"pages", pages}, {"heartbeat", heartbeat}};
        if (window) {
            result["busy"] = controller->busy();
            result["status"] = window->findChild<QLabel *>("filesStatus")->text();
            result["next"] = button("filesNext")->isEnabled();
            result["previous"] = button("filesPrevious")->isEnabled();
            result["restore"] = button("filesRestore")->isEnabled();
            result["visible"] = table->model()->rowCount();
            result["endpoint"] = controller->endpoint();
            if (controller->model()->rowCount()) {
                result["display_size"] = controller->model()->data(controller->model()->index(0, 1)).toString();
                table->selectRow(0);
                result["detail"] = window->findChild<QPlainTextEdit *>("fileDetails")->toPlainText();
                result["plain"] = window->findChild<QPlainTextEdit *>("fileDetails")->isReadOnly();
                result["tooltip"] = table->model()->data(table->model()->index(0, 0), Qt::ToolTipRole).toString();
            }
            if (argc > 3) {
                QApplication::processEvents();
                auto *detail = window->findChild<QPlainTextEdit *>("fileDetails");
                if (controller->model()->rowCount() && detail->viewport()->height() < 4 * detail->fontMetrics().lineSpacing()) {
                    std::cerr << "file details layout clipped" << std::endl;
                    app.exit(68);
                    return;
                }
                window->grab().save(QString::fromLocal8Bit(argv[3]));
            }
        }
        std::cout << "RESULT=" << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
        app.exit(0);
    };
    QObject::connect(controller, &FileListController::finished, &app, [&](bool success) {
        if (mode == "cancel" || mode == "endpoint" || mode == "close") return;
        QJsonArray paths;
        for (const auto &f : controller->model()->page().files) paths.append(QString::fromUtf8(f.path));
        pages.append(paths);
        if ((mode == "pages" || mode == "retain") && success && pages.size() == 1 && controller->hasNext()) {
            QTimer::singleShot(0, &app, [&] { button("filesNext")->click(); });
        } else if (mode == "pages" && success && pages.size() == 2) {
            QTimer::singleShot(0, &app, [&] { button("filesPrevious")->click(); });
        } else {
            if (mode == "pages") {
                search->setText("中文");
                int matches = table->model()->rowCount();
                if (!matches) { app.exit(66); return; }
            }
            output(success);
        }
    });
    window->show();
    QTimer::singleShot(0, &app, [&] {
        button("filesRefresh")->click();
        // Repeated activation while busy must not dispatch another request.
        button("filesRefresh")->click();
        if (mode == "cancel" || mode == "endpoint" || mode == "close" || mode == "restart") {
            QTimer::singleShot(75, &app, [&] {
                if (mode == "cancel") button("filesCancel")->click();
                else if (mode == "endpoint") port->setValue(port->value() == 65535 ? 1 : port->value() + 1);
                else if (mode == "restart") {
                    const auto original = port->value();
                    port->setValue(original == 65535 ? 1 : original + 1);
                    port->setValue(original);
                    button("filesRefresh")->click();
                    return;
                } else window.reset();
                QTimer::singleShot(350, &app, [&] { output(true); });
            });
        }
    });
    QTimer::singleShot(15000, &app, [&] { app.exit(67); });
    return app.exec();
}
