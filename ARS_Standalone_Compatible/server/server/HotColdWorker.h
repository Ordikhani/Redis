#pragma once

#include <QThread>
#include <QMutex>
#include <QString>
#include <QJsonObject>
#include <atomic>
#include <QQueue>
#include <QHash>

// Owns its Redis socket and PostgreSQL connection inside run().
// The client-facing server thread never shares Qt SQL/socket objects with it.
class HotColdWorker final : public QThread {
public:
    HotColdWorker();
    ~HotColdWorker() override;

    void requestStop();
    bool isHealthy() const { return m_healthy.load(); }
    bool checkpointRunning() const { return m_checkpointRunning.load(); }
    qint64 lastCheckpointAtMs() const { return m_lastCheckpointAtMs.load(); }
    bool lastCheckpointSucceeded() const { return m_lastCheckpointSucceeded.load(); }
    QString lastError() const;
    // Bounded asynchronous rehydration queue. Returns the existing job for an ID
    // if a recovery is already queued/running.
    bool enqueueRecovery(const QString& id, const QString& jobId, QString& acceptedJobId, QString& error);
    bool recoveryStatus(const QString& jobId, QJsonObject& result) const;
    int recoveryQueueDepth() const;
    bool recoveryInProgress(const QString& id) const;

protected:
    void run() override;

private:
    std::atomic_bool m_stop{false};
    std::atomic_bool m_healthy{false};
    std::atomic_bool m_checkpointRunning{false};
    std::atomic_bool m_lastCheckpointSucceeded{false};
    std::atomic<qint64> m_lastCheckpointAtMs{0};
    mutable QMutex m_errorMutex;
    QString m_error;
    void setError(const QString& error);
    struct RecoveryJob { QString id; QString jobId; };
    mutable QMutex m_jobsMutex;
    QQueue<RecoveryJob> m_recoveryQueue;
    QHash<QString, QJsonObject> m_jobStates;
    QHash<QString, QString> m_idJobs;
    int m_maxRecoveryQueue = 64;
};
