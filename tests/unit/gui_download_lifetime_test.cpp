#include "MainWindow.h"
#include "FileListController.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <memory>
#include <stdexcept>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir temp;
    QCoreApplication::setOrganizationName("ChunkHarborTests");
    QCoreApplication::setApplicationName("download-lifetime");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    QSettings().setValue("storage/logDirectory", temp.filePath("logs"));
    try {
        for (int attempt = 0; attempt < 10; ++attempt) {
            QTcpServer server;
            if (!server.listen(QHostAddress::LocalHost, 0)) throw std::runtime_error("listen failed");
            auto window = std::make_unique<MainWindow>(nullptr, temp.filePath(QString::number(attempt)));
            window->findChild<QSpinBox *>("serverPort")->setValue(server.serverPort());
            window->fileListController()->model()->setPage({{{"a", std::string(32, '1'), 0,
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}}, {}});
            window->findChild<QTableView *>("filesTable")->selectRow(0);
            if (attempt < 5) window->connectToLocalServer(server.serverPort());
            else window->fileListController()->refresh();
            QElapsedTimer timer;
            timer.start();
            QTcpSocket *peer = nullptr;
            while ((!peer || !peer->bytesAvailable()) && timer.elapsed() < 3000) {
                QApplication::processEvents();
                if (!peer && server.hasPendingConnections()) peer = server.nextPendingConnection();
                QThread::msleep(1);
            }
            if (!peer || !peer->bytesAvailable()) throw std::runtime_error("pending HELLO fixture failed");
            window.reset(); // Selected-file views and an in-flight health probe coexist.
            QApplication::processEvents();
        }
        std::cout << "selected download page plus pending health/list HELLO destruction passed ten times\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
