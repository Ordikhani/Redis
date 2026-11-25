#pragma once
#include <QSqlDatabase>
#include <QJsonObject>
#include "ServerConfig.h"
#include <QVector>
#include <QJsonArray>
#include <functional>

class PostgresStore {
public:
    ~PostgresStore();
    bool open(const ServerConfig& settings, bool acquireWriterLock = true);
    bool reconnect(const ServerConfig& settings, bool acquireWriterLock = false);
    bool save(const QJsonObject& record, QJsonObject& result, qint64 receivedAtMs = -1);
    bool reserveSequences(int count, QVector<qint64>& values);
    bool loadAll(const QString& id, const std::function<bool(const QJsonObject&)>& visitor);
    bool archiveBatch(const QJsonArray& items);
    bool latest(const QString& id, QJsonObject& result);
    bool history(const QJsonObject& args, QJsonObject& result);
    bool recent(const QJsonObject& args, QJsonObject& result);
    bool remove(const QString& id, QJsonObject& result);
    bool healthy();
    QString error() const { return m_error; }
private:
    bool migrate();
    QSqlDatabase m_db;
    QString m_error;
};
