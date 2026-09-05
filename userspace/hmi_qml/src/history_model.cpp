#include "history_model.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QDebug>
#include <utility>

HistoryModel::HistoryModel(QString databasePath, bool activeOnly, QObject *parent) : QAbstractListModel(parent), databasePath_(std::move(databasePath)), activeOnly_(activeOnly) {
    refreshTimer_.setInterval(1000);
    connect(&refreshTimer_, &QTimer::timeout, this, [this]() { reload(20); });
    refreshTimer_.start();
    reload();
}
int HistoryModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : events_.size(); }
QVariant HistoryModel::data(const QModelIndex &index, int role) const { if (!index.isValid() || index.row() >= events_.size()) return {}; const Event &e = events_.at(index.row()); switch(role) { case EventIdRole:return e.id; case TimeRole:return e.time; case LevelRole:return e.level; case SourceRole:return e.source; case EventTypeRole:return e.type; case ActiveRole:return e.active; case DetailRole:return e.detail; case ValueRole:return e.value; case ThresholdRole:return e.threshold; case EvidenceRole:return e.evidence; default:return {}; } }
QHash<int,QByteArray> HistoryModel::roleNames() const { return {{EventIdRole,"eventId"},{TimeRole,"time"},{LevelRole,"level"},{SourceRole,"source"},{EventTypeRole,"eventType"},{ActiveRole,"active"},{DetailRole,"detail"},{ValueRole,"value"},{ThresholdRole,"threshold"},{EvidenceRole,"evidencePath"}}; }
void HistoryModel::reload(int limit)
{
    if (loading_) return; loading_ = true; emit loadingChanged(); error_.clear(); emit errorChanged();
    const QString name = QStringLiteral("hmi_history_%1").arg(reinterpret_cast<quintptr>(this));
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=1000"));
    db.setDatabaseName(databasePath_);
    if (!db.open()) { error_ = db.lastError().text(); emit errorChanged(); db = {}; QSqlDatabase::removeDatabase(name); loading_ = false; emit loadingChanged(); return; }
    /* evidence_path is optional in databases created by older eventd builds;
       it is not needed by the current HMI list, so do not make history
       rendering depend on that column being present. */
    // event_time_ms originates from the producer snapshot's monotonic clock;
    // event_id is SQLite's insertion order and therefore the correct HMI order.
    const QString sql = activeOnly_
        ? QStringLiteral("SELECT event_id, event_datetime, source, event_type, level, active, value, threshold, detail FROM events WHERE active = 1 AND level IN ('alarm', 'warning') ORDER BY event_id DESC LIMIT %1").arg(qBound(1, limit, 100))
        : QStringLiteral("SELECT event_id, event_datetime, source, event_type, level, active, value, threshold, detail FROM events ORDER BY event_id DESC LIMIT %1").arg(qBound(1, limit, 100));
    QSqlQuery q(db); q.prepare(sql);
    QVector<Event> next; if (!q.exec()) { error_ = q.lastError().text(); qWarning() << "HistoryModel query failed:" << error_; } else while (q.next()) next.push_back({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString(), q.value(4).toString(), q.value(8).toString(), QString(), q.value(5).toInt() != 0, q.value(6).toDouble(), q.value(7).toDouble()});
    q.finish(); db.close(); db = {}; QSqlDatabase::removeDatabase(name);
    if (error_.isEmpty() && events_ != next) {
        beginResetModel();
        events_ = std::move(next);
        endResetModel();
    }
    emit errorChanged(); loading_ = false; emit loadingChanged();
}
