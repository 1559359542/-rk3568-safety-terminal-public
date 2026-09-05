#pragma once

#include <QAbstractListModel>
#include <QSqlDatabase>
#include <QTimer>

class HistoryModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
public:
    enum Roles { EventIdRole = Qt::UserRole + 1, TimeRole, LevelRole, SourceRole, EventTypeRole, ActiveRole, DetailRole, ValueRole, ThresholdRole, EvidenceRole };
    explicit HistoryModel(QString databasePath, bool activeOnly = false, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE void reload(int limit = 20);
    bool loading() const { return loading_; }
    QString error() const { return error_; }
signals:
    void loadingChanged();
    void errorChanged();
private:
    struct Event {
        qlonglong id;
        QString time, source, type, level, detail, evidence;
        bool active;
        double value;
        double threshold;

        bool operator==(const Event &other) const {
            return id == other.id && time == other.time && source == other.source &&
                   type == other.type && level == other.level && detail == other.detail &&
                   evidence == other.evidence && active == other.active &&
                   value == other.value && threshold == other.threshold;
        }
    };
    QString databasePath_; QVector<Event> events_; bool loading_ = false; QString error_; bool activeOnly_ = false;
    QTimer refreshTimer_;
};
