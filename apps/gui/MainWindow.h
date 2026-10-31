#pragma once
#include <QMainWindow>
class QtTcpTransport;
class QLineEdit;
class QSpinBox;
class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    QtTcpTransport* transport() const { return transport_; }
    void connectToLocalServer(quint16 port);
private:
    QtTcpTransport* transport_;
    QLineEdit* host_;
    QSpinBox* port_;
};
