#include "history_model.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <utility>

HistoryModel::HistoryModel(QString databasePath, QObject *parent) : QAbstractListModel(parent), databasePath_(std::move(databasePath)) { reload(); }
int HistoryModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : events_.size(); }
QVariant HistoryModel::data(const QModelIndex &index, int role) const { if (!index.isValid() || index.row() >= events_.size()) return {}; const Event &e = events_.at(index.row()); switch(role) { case EventIdRole:return e.id; case TimeRole:return e.time; case LevelRole:return e.level; case SourceRole:return e.source; case DurationRole:return e.duration; case RecoveredRole:return e.recovered; case PreviewRole:return e.preview; default:return {}; } }
QHash<int,QByteArray> HistoryModel::roleNames() const { return {{EventIdRole,"eventId"},{TimeRole,"time"},{LevelRole,"level"},{SourceRole,"source"},{DurationRole,"durationMs"},{RecoveredRole,"recovered"},{PreviewRole,"preview"}}; }
void HistoryModel::reload(int limit)
{
    if (loading_) return; loading_ = true; emit loadingChanged(); error_.clear(); emit errorChanged();
    const QString name = QStringLiteral("hmi_history_%1").arg(reinterpret_cast<quintptr>(this));
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name); db.setDatabaseName(databasePath_);
    if (!db.open()) { error_ = db.lastError().text(); emit errorChanged(); db = {}; QSqlDatabase::removeDatabase(name); loading_ = false; emit loadingChanged(); return; }
    QSqlQuery q(db); q.prepare(QStringLiteral("SELECT id, timestamp, level, source, duration_ms, recovered, preview_path FROM events ORDER BY id DESC LIMIT :limit")); q.bindValue(QStringLiteral(":limit"), qBound(1, limit, 100));
    QVector<Event> next; if (!q.exec()) error_ = q.lastError().text(); else while (q.next()) next.push_back({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString(), q.value(4).toLongLong(), q.value(5).toBool(), q.value(6).toString()});
    q.finish(); db.close(); db = {}; QSqlDatabase::removeDatabase(name); if (error_.isEmpty()) { beginResetModel(); events_ = std::move(next); endResetModel(); } emit errorChanged(); loading_ = false; emit loadingChanged();
}
