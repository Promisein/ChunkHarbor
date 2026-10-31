#include "MainWindow.h"
#include "QtTcpTransport.h"
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), transport_(new QtTcpTransport(this)) {
    setWindowTitle(tr("ChunkHarbor - Connection prototype"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    auto* heading = new QLabel(tr("ChunkHarbor"), content);
    auto font = heading->font(); font.setPointSize(22); font.setBold(true); heading->setFont(font);
    layout->addWidget(heading);
    auto* description = new QLabel(tr("M2 connection prototype\nCheck the server connection and protocol handshake."), content);
    description->setWordWrap(true); layout->addWidget(description);
    auto* form = new QFormLayout;
    host_ = new QLineEdit(QStringLiteral("127.0.0.1"), content);
    port_ = new QSpinBox(content); port_->setRange(1, 65535); port_->setValue(18787);
    form->addRow(tr("Server address"), host_); form->addRow(tr("Port"), port_); layout->addLayout(form);
    auto* button = new QPushButton(tr("Connect"), content); layout->addWidget(button);
    auto* status = new QLabel(tr("Disconnected"), content); status->setWordWrap(true); layout->addWidget(status);
    layout->addStretch();
    auto* note = new QLabel(tr("Use localhost or an SSH tunnel. File backup is available through the Linux CLI."), content);
    note->setWordWrap(true); layout->addWidget(note);
    connect(button, &QPushButton::clicked, this, [this] { transport_->connectToServer(host_->text().trimmed(), static_cast<quint16>(port_->value())); });
    connect(transport_, &QtTcpTransport::statusChanged, status, &QLabel::setText);
    setCentralWidget(content); resize(500, 350);
}
void MainWindow::connectToLocalServer(quint16 port) {
    host_->setText(QStringLiteral("127.0.0.1")); port_->setValue(port);
    transport_->connectToServer(host_->text(), port);
}
