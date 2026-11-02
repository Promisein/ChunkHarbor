#include "ServerFilesModel.h"
namespace {
QString text(const std::string &s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
}
ServerFilesModel::ServerFilesModel(QObject *parent) : QAbstractTableModel(parent) { setObjectName("filesModel"); }
int ServerFilesModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : static_cast<int>(page_.files.size()); }
int ServerFilesModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : 4; }
QVariant ServerFilesModel::headerData(int col, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    const QStringList headers{tr("逻辑路径"), tr("大小（字节）"), tr("当前版本"), "SHA-256"};
    return col >= 0 && col < headers.size() ? QVariant(headers[col]) : QVariant{};
}
QVariant ServerFilesModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() || index.column() < 0 || index.column() >= 4) return {};
    const auto &f = page_.files[static_cast<std::size_t>(index.row())];
    if (role == Qt::UserRole) return QVariant::fromValue<qulonglong>(f.size);
    if (role == Qt::ToolTipRole || role == Qt::UserRole + 1) {
        auto detail = tr("逻辑路径：%1\n大小：%2 字节\n当前版本：%3\nSHA-256：%4")
            .arg(text(f.path), QString::number(static_cast<qulonglong>(f.size)), text(f.versionId), text(f.sha256));
        // QToolTip auto-detects rich text. Escape every server field, then add
        // only our own markup; QLabel details use the separate plain-text role.
        return role == Qt::ToolTipRole ? "<qt>" + detail.toHtmlEscaped().replace("\n", "<br/>") + "</qt>" : detail;
    }
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case 0: return text(f.path);
    case 1: return QString::number(static_cast<qulonglong>(f.size));
    case 2: return text(f.versionId);
    default: return text(f.sha256);
    }
}
void ServerFilesModel::setPage(chunkharbor::protocol::FilesResult page) {
    chunkharbor::protocol::encodeFilesResult(page);
    beginResetModel();
    page_ = std::move(page);
    endResetModel();
}
