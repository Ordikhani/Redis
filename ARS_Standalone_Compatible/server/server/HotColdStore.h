#pragma once
#include "PostgresStore.h"
#include "ServerConfig.h"
#include "Redis/ArsRedisClient.h"
#include "HotColdWorker.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>

// A single-server Redis hot / PostgreSQL cold store. The server is the only writer.
// Complete per-ID histories are held in Redis while active, and checkpointed to SQL.
class HotColdStore {
public:
    ~HotColdStore();
    bool open(const ServerConfig& settings);
    // receivedAtMs > 0 keeps the arrival time of a write the server held during recovery.
    bool save(const QJsonObject& record, QJsonObject& result, qint64 receivedAtMs = -1);
    bool latest(const QString& id, QJsonObject& result);
    bool history(const QJsonObject& request, QJsonObject& result);
    bool hydrate(const QString& id, QJsonObject& result);
    bool recoveryStatus(const QString& jobId, QJsonObject& result);
    bool searchHot(const QJsonObject& args, QJsonObject& result);
    bool recent(const QJsonObject& args, QJsonObject& result);
    bool remove(const QString& id, bool fromDisk, QJsonObject& result);
    QJsonObject status();
    // Called about once per second on the client-facing thread. Detects a lost PostgreSQL
    // connection and re-opens it (writer lock + schema check) with backoff up to 30 s.
    void maintainPostgres();
    QString error() const { return m_error; }
    QString takeErrorCode() { const QString code=m_errorCode; m_errorCode.clear(); return code; }
private:
    QByteArray key(const char* kind, const QString& id = QString()) const;
    void disableRedis(const QString& why);
    ArsRedisReply command(const QList<QByteArray>& args);
    bool active(const QString& id, bool& found);
    int afterHotRead(const QString& id, bool readOk, const char* operation);
    bool append(const QJsonObject& item, qint64 received, qint64 sequence, bool requireActive);
    bool flushId(const QString& id, bool evict, qint64 cutoffMs, int maxBatches, bool* complete = nullptr);
    bool redisHistory(const QJsonObject& request, QJsonObject& result);
    bool redisRecent(const QJsonObject& request, QJsonObject& result);
    bool legacyRecent(const QJsonObject& request, QJsonObject& result);
    bool itemAt(const QByteArray& listKey, qint64 index, QJsonObject& item);
    bool nextSequence(qint64& value);
    PostgresStore m_postgres;
    ArsRedisClient m_redis;
    HotColdWorker* m_worker = nullptr;
    QVector<qint64> m_sequencePool;
    int m_sequenceOffset = 0;
    QByteArray m_prefix;
    QString m_error;
    QString m_errorCode;
    bool m_hot = false;
    int m_idleSeconds = 60;
    int m_backupSeconds = 300;
    int m_batchSize = 100;
    int m_maxActiveIds = 0;
    int m_maxBatchesPerSweep = 4;
    qint64 m_lastBackupMs = 0;
    bool m_postgresDown = false;
    int m_postgresBackoffMs = 1000;
    qint64 m_postgresRetryAtMs = 0;
    int m_postgresReconnects = 0;
};
