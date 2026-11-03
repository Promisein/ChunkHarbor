#include "TaskModel.h"
#include <QColor>
namespace gui {
QString strategyName(BackupStrategy strategy) {
    switch (strategy) {
    case BackupStrategy::Full: return "首次上传";
    case BackupStrategy::Incremental: return "增量备份";
    case BackupStrategy::Resume: return "断点续传";
    case BackupStrategy::Skip: return "无需上传";
    case BackupStrategy::Error: return "不可执行";
    }
    return "不可执行";
}
QString modeRecommendation(const QList<FileRecord> &records, bool automatic) {
    if (automatic) {
        int counts[5] = {};
        quint64 bytes = 0;
        for (const auto &r : records) {
            if (r.change == "已排除") continue;
            ++counts[static_cast<int>(r.strategy)];
            if (r.modeAllowed && r.strategy != BackupStrategy::Error) bytes += r.missingBytes;
        }
        return QString("自动备份计划：首次上传 %1、增量备份 %2、断点续传 %3、无需上传 %4、错误 %5。\n"
                       "预计上传：%6。确认后按表中策略串行执行；错误文件不会上传。")
            .arg(counts[1]).arg(counts[2]).arg(counts[3]).arg(counts[4]).arg(counts[0])
            .arg(formatBytes(bytes));
    }
    int full=0, incremental=0, resume=0, unchanged=0, unknown=0, blocked=0;
    for (const auto &r : records) {
        if (r.status == "已跳过") continue;
        if (!r.modeAllowed) ++blocked;
        switch (r.advice) {
        case BackupAdvice::Full: ++full; break;
        case BackupAdvice::Incremental: ++incremental; break;
        case BackupAdvice::Resume: ++resume; break;
        case BackupAdvice::Unchanged: ++unchanged; break;
        case BackupAdvice::Unknown: ++unknown; break;
        }
    }
    if (!(full+incremental+resume+unchanged+unknown)) return "没有可备份文件。";
    const int kinds = (full>0)+(incremental>0)+(resume>0)+(unchanged>0)+(unknown>0);
    QString text;
    if (kinds != 1 || unknown)
        text = QString("分类建议：首次上传 %1、增量备份 %2、断点续传 %3、无需上传 %4、待核验 %5。\n"
                       "目录状态不同或尚未核验，请逐项查看；不会自动切换模式。")
                   .arg(full).arg(incremental).arg(resume).arg(unchanged).arg(unknown);
    else if (resume)
        text = QString("推荐：断点续传（%1 个文件）。\n原因：源身份匹配，服务器已确认未完成会话。 ").arg(resume);
    else if (full)
        text = QString("推荐：首次完整上传（%1 个文件）。\n原因：服务器确认目标版本不存在。 ").arg(full);
    else if (incremental)
        text = QString("推荐：增量备份（%1 个文件）。\n原因：服务器已有对应版本，仅需传输变化块。 ").arg(incremental);
    else
        text = QString("无需上传：%1 个文件与服务器当前版本一致。\n执行时仍会复核源文件和固定版本。 ").arg(unchanged);
    if (blocked) text += QString("\n当前模式不适用于 %1 个文件；请查看说明，调整后重新扫描。 ").arg(blocked);
    return text;
}
QString formatBytes(quint64 n) {
    if (n < 1024)
        return QString::number(n) + " B";
    if (n < 1024 * 1024)
        return QString::number(double(n) / 1024, 'f', 1) + " KiB";
    if (n < 1024ULL * 1024 * 1024)
        return QString::number(double(n) / (1024 * 1024), 'f', 1) + " MiB";
    return QString::number(double(n) / (1024ULL * 1024 * 1024), 'f', 1) + " GiB";
}
} // namespace gui
TaskModel::TaskModel(QObject *parent) : QAbstractTableModel(parent) {
    setObjectName("backupPreviewModel");
}
int TaskModel::rowCount(const QModelIndex &p) const { return p.isValid() ? 0 : records_.size(); }
int TaskModel::columnCount(const QModelIndex &p) const { return p.isValid() ? 0 : 6; }
QVariant TaskModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid() || i.row() >= records_.size() || i.column() >= 6)
        return {};
    const auto &f = records_[i.row()];
    if (role == Qt::ToolTipRole)
        return f.localPath + "\n执行策略：" + gui::strategyName(f.strategy) + "\n" + f.note;
    if (role == Qt::ForegroundRole && (i.column() == 0 || i.column() == 3)) {
        return QColor(!f.modeAllowed || f.status == "失败" || f.change == "错误" ? "#c45656"
                      : f.change == "新增"                     ? "#278258"
                      : f.change == "已修改"                   ? "#2979dc"
                                                               : "#748399");
    }
    if (role != Qt::DisplayRole)
        return {};
    return QStringList{f.status, f.logicalPath, gui::formatBytes(f.size), f.change,
                       f.change == "已排除" ? QString("已排除") : gui::strategyName(f.strategy), f.note}[i.column()];
}
QVariant TaskModel::headerData(int i, Qt::Orientation o, int role) const {
    if (role != Qt::DisplayRole)
        return {};
    if (o == Qt::Vertical)
        return i + 1;
    return QStringList{"状态", "文件名", "大小", "变更类型", "执行策略", "说明"}.value(i);
}
void TaskModel::clear() {
    beginResetModel();
    records_.clear();
    endResetModel();
}
void TaskModel::upsert(const gui::FileRecord &f) {
    for (int i = 0; i < records_.size(); ++i)
        if (records_[i].logicalPath == f.logicalPath) {
            records_[i] = f;
            emit dataChanged(index(i, 0), index(i, 5));
            return;
        }
    if (records_.size() >= 10000)
        return;
    int r = records_.size();
    beginInsertRows({}, r, r);
    records_.append(f);
    endInsertRows();
}
