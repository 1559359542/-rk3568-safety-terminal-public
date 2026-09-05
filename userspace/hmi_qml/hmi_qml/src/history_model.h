#pragma once

#include <QAbstractListModel>
#include <QSqlDatabase>

class HistoryModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
public:
    enum Roles { EventIdRole = Qt::UserRole + 1, TimeRole, LevelRole, SourceRole, DurationRole, RecoveredRole, PreviewRole };
    explicit HistoryModel(QString databasePath, QObject *parent = nullptr);
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
    struct Event { qlonglong id; QString time, level, source; qlonglong duration; bool recovered; QString preview; };
    QString databasePath_; QVector<Event> events_; bool loading_ = false; QString error_;
};
