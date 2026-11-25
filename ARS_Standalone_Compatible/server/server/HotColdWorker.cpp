#include "HotColdWorker.h"
#include "HotColdScripts.h"
#include "PostgresStore.h"
#include "Redis/ArsRedisClient.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQueue>
#include "ServerConfig.h"
#include <QThread>
#include <QDebug>
#include <QMutexLocker>
#include <QJsonArray>
#include <QUuid>
#include <limits>

namespace {
QJsonObject decode(const QByteArray& b) { return QJsonDocument::fromJson(b).object(); }
}

HotColdWorker::HotColdWorker() = default;
HotColdWorker::~HotColdWorker() { requestStop(); wait(); }
void HotColdWorker::requestStop() { m_stop.store(true); }
void HotColdWorker::setError(const QString& e) { QMutexLocker lock(&m_errorMutex); m_error=e; }
QString HotColdWorker::lastError() const { QMutexLocker lock(&m_errorMutex); return m_error; }

bool HotColdWorker::enqueueRecovery(const QString& id, const QString& requestedJobId,
                                    QString& acceptedJobId, QString& error) {
    QMutexLocker lock(&m_jobsMutex);
    const auto existing = m_idJobs.constFind(id);
    if (existing != m_idJobs.cend()) {
        acceptedJobId = existing.value();
        return true;
    }
    if (!m_healthy.load()) {
        error = QStringLiteral("Recovery worker is not healthy");
        return false;
    }
    if (m_recoveryQueue.size() >= m_maxRecoveryQueue) {
        error = QStringLiteral("Recovery queue is full; retry later");
        return false;
    }
    acceptedJobId = requestedJobId.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces) : requestedJobId;
    m_idJobs.insert(id, acceptedJobId);
    m_jobStates.insert(acceptedJobId, QJsonObject{{"jobId", acceptedJobId}, {"id", id},
        {"status", "queued"}, {"createdAtMs", double(QDateTime::currentMSecsSinceEpoch())}});
    m_recoveryQueue.enqueue({id, acceptedJobId});
    return true;
}

bool HotColdWorker::recoveryStatus(const QString& jobId, QJsonObject& result) const {
    QMutexLocker lock(&m_jobsMutex);
    const auto it = m_jobStates.constFind(jobId);
    if (it == m_jobStates.cend()) return false;
    result = it.value();
    return true;
}

int HotColdWorker::recoveryQueueDepth() const {
    QMutexLocker lock(&m_jobsMutex);
    return m_recoveryQueue.size();
}

bool HotColdWorker::recoveryInProgress(const QString& id) const {
    QMutexLocker lock(&m_jobsMutex);
    return m_idJobs.contains(id);
}

void HotColdWorker::run() {
    const ServerConfig& settings = ServerConfig::instance();
    const QString redisHost=settings.value("redis/host","127.0.0.1").toString();
    const quint16 redisPort=quint16(settings.value("redis/port",6379).toUInt());
    const QString redisPassword=settings.value("redis/password").toString();
    const int redisDb=settings.value("redis/db",0).toInt();
    const QByteArray prefix=settings.value("redis/keyPrefix","arscentral").toString().toUtf8()+":hotv1";
    const int batchSize=qBound(1,settings.value("lifecycle/batchSize",100).toInt(),500);
    const int maxBatches=qBound(1,settings.value("lifecycle/maxBatchesPerSweep",4).toInt(),100);
    const int idleSec=qBound(1,settings.value("lifecycle/inactiveAfterSec",60).toInt(),86400*30);
    const int checkpointSec=qBound(1,settings.value("lifecycle/checkpointEverySec",300).toInt(),86400);
    const int scanMs=qBound(200,settings.value("lifecycle/scanEveryMs",1000).toInt(),60000);
    const int workerBatchMs=qBound(10,settings.value("lifecycle/workerIntervalMs",100).toInt(),5000);
    m_maxRecoveryQueue=qBound(1,settings.value("recovery/maxQueuedJobs",64).toInt(),4096);
    const int maxActiveIds=qBound(0,settings.value("memory/maxActiveIds",0).toInt(),10000000);

    // This second SQL connection deliberately does not acquire the singleton writer lock
    // or run migrations. The foreground server has already completed both at startup.
    PostgresStore postgres;
    if (!postgres.open(settings, false)) {
        setError(QStringLiteral("Maintenance PostgreSQL connection failed: ")+postgres.error());
        qCritical().noquote() << "[WORKER_FAIL]" << lastError();
        return;
    }
    ArsRedisClient redis; // Constructed in this worker thread; its QTcpSocket stays thread-local.
    if (!redis.connectToHost(redisHost,redisPort,redisPassword,redisDb)) {
        setError(QStringLiteral("Maintenance Redis connection failed: ")+redis.lastError());
        qCritical().noquote() << "[WORKER_FAIL]" << lastError();
        return;
    }
    auto key=[&](const char* kind,const QString& id=QString())->QByteArray {
        QByteArray k=prefix+":"+kind;
        if (!id.isNull()) k += ":"+id.toUtf8().toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals);
        return k;
    };
    auto command=[&](const QList<QByteArray>& args)->ArsRedisReply {
        auto r=redis.sendCommand(args,2500);
        if (!r.isOk()) {
            setError(QStringLiteral("Redis maintenance command failed: ")+r.error);
            m_healthy.store(false);
            redis.disconnectFromHost(); // Force a clean reconnect on the next worker iteration.
        }
        return r;
    };
    auto flushId=[&](const QString& id,bool evict,qint64 cutoff,int batches)->bool {
        const auto list=key("history",id);
        auto len=command({"LLEN",list});
        if (!len.isOk() || len.type!=ArsRedisReply::Integer) return false;
        auto saved=command({"HGET",key("synced"),id.toUtf8()});
        if (!saved.isOk()) return false;
        qint64 index=saved.type==ArsRedisReply::String?saved.str.toLongLong():0;
        if (index>len.integer) { setError("Checkpoint offset exceeds Redis history length"); return false; }
        for (int n=0;n<batches && index<len.integer;n++) {
            auto rows=command({"LRANGE",list,QByteArray::number(index),QByteArray::number(qMin(len.integer-1,index+batchSize-1))});
            if (!rows.isOk() || rows.type!=ArsRedisReply::Array || rows.elements.isEmpty()) return false;
            QJsonArray payload;
            for (const auto& row:rows.elements) {
                if (row.type!=ArsRedisReply::String) { setError("Invalid Redis history row during checkpoint"); return false; }
                const auto obj=decode(row.str);
                if (obj.isEmpty()) { setError("Malformed Redis history JSON during checkpoint"); return false; }
                payload.append(obj);
            }
            if (!postgres.archiveBatch(payload)) { setError(QStringLiteral("PostgreSQL archive batch failed: ")+postgres.error()); return false; }
            // SQL commit precedes the Redis progress marker, and the marker only moves if Redis
            // still holds exactly the committed rows at [index, index+n). Retries are idempotent.
            auto advanced=command({"EVAL",HotColdScripts::advanceSynced,"2",key("synced"),list,id.toUtf8(),
                                   QByteArray::number(index),QByteArray::number(index+payload.size()),
                                   payload.first().toObject().value("sequence").toString().toUtf8(),
                                   payload.last().toObject().value("sequence").toString().toUtf8()});
            if (!advanced.isOk() || advanced.type!=ArsRedisReply::Integer) return false;
            if (advanced.integer==-1) {
                // The client thread (delete) advanced the offset meanwhile: resume from it.
                auto current=command({"HGET",key("synced"),id.toUtf8()});
                if (!current.isOk()) return false;
                index=current.type==ArsRedisReply::String?current.str.toLongLong():0;
                if (index>len.integer) { setError("Checkpoint offset exceeds Redis history length"); return false; }
                continue;
            }
            if (advanced.integer!=1) {
                setError(QStringLiteral("Checkpoint batch does not match Redis history; offset not advanced for id=")+id);
                qWarning().noquote() << "[WORKER_BATCH_MISMATCH] id=" << id << "code=" << advanced.integer;
                return false;
            }
            index+=payload.size();
        }
        if (evict && index==len.integer) {
            auto result=command({"EVAL",HotColdScripts::evict,"5",list,key("latest",id),key("lastSeen"),key("synced"),key("recvIndex",id),
                                 id.toUtf8(),QByteArray::number(cutoff)});
            if (!result.isOk()) return false;
        }
        return true;
    };

    auto recoverId = [&](const QString& id, const QString& jobId) -> QJsonObject {
        const auto staging=key("staging",jobId);
        const auto stagingIndex=key("staging_index",jobId);
        auto cleanup=[&] { command({"DEL",staging,stagingIndex}); };
        // A second check makes duplicate/retried jobs harmless.
        auto already=command({"ZSCORE",key("lastSeen"),id.toUtf8()});
        if (!already.isOk()) return {{"ok",false},{"error",lastError()}};
        if (already.type!=ArsRedisReply::Nil) return {{"ok",true},{"restoredCount",0},{"alreadyActive",true}};

        int count=0;
        int batchBytes=0;
        QList<QByteArray> listArgs={"RPUSH",staging};
        QList<QByteArray> indexArgs={"ZADD",stagingIndex};
        QJsonObject best;
        qint64 bestTimestamp=std::numeric_limits<qint64>::min(), bestSequence=-1;
        bool redisOk=true;
        auto flushStage=[&]() -> bool {
            const int batchCount=listArgs.size()-2;
            if (batchCount<=0) return true;
            QList<QByteArray> args={"EVAL",HotColdScripts::stageBatch,"2",staging,stagingIndex,
                "3600",QByteArray::number(batchCount),QByteArray::number(count-batchCount)};
            for (int i=0;i<batchCount;++i) {
                args.append(listArgs.at(i+2));
                args.append(indexArgs.at(2+i*2)); // receivedAtMs score
            }
            const auto staged=command(args);
            if (!staged.isOk()) return false;
            listArgs={"RPUSH",staging}; indexArgs={"ZADD",stagingIndex}; batchBytes=0;
            QThread::yieldCurrentThread();
            return true;
        };
        const bool sqlOk=postgres.loadAll(id,[&](const QJsonObject& item) {
            if (!redisOk) return false;
            const QByteArray encoded=QJsonDocument(item).toJson(QJsonDocument::Compact);
            if (encoded.size()>1024*1024) { setError("Recovery row exceeds 1 MiB safety limit"); redisOk=false; return false; }
            if (listArgs.size()>2 && (listArgs.size()>=qMax(2,batchSize+2) || batchBytes+encoded.size()>1024*1024)) {
                if (!flushStage()) { redisOk=false; return false; }
            }
            listArgs.append(encoded);
            const qint64 received=qint64(item.value("receivedAtMs").toDouble());
            indexArgs.append(QByteArray::number(received));
            indexArgs.append(QByteArray::number(count));
            batchBytes+=encoded.size();
            ++count;
            const qint64 timestamp=qint64(item.value("record").toObject().value("timestamp").toDouble());
            bool seqOk=false;
            const qint64 sequence=item.value("sequence").toString().toLongLong(&seqOk);
            if (timestamp>bestTimestamp || (timestamp==bestTimestamp && seqOk && sequence>bestSequence)) {
                best=item; bestTimestamp=timestamp; bestSequence=sequence;
            }
            return true;
        });
        if (!sqlOk || !redisOk) {
            cleanup();
            return {{"ok",false},{"error",redisOk?postgres.error():lastError()}};
        }
        if (!flushStage()) { const QString why=lastError(); cleanup(); return {{"ok",false},{"error",why}}; }
        if (count==0) { cleanup(); return {{"ok",true},{"restoredCount",0},{"notFound",true}}; }
        auto published=command({"EVAL",HotColdScripts::publishRecovery,"7",staging,key("history",id),key("latest",id),
            key("synced"),key("lastSeen"),stagingIndex,key("recvIndex",id),QByteArray::number(count),
            QJsonDocument(best).toJson(QJsonDocument::Compact),id.toUtf8(),
            QByteArray::number(QDateTime::currentMSecsSinceEpoch()),QByteArray::number(maxActiveIds)});
        if (!published.isOk() || published.type!=ArsRedisReply::Integer) {
            cleanup(); return {{"ok",false},{"error",lastError()}};
        }
        if (published.integer==-1) {
            cleanup(); return {{"ok",false},{"error","Hot ID capacity reached before recovery publication"},{"capacityRejected",true}};
        }
        if (published.integer==0) { cleanup(); return {{"ok",true},{"restoredCount",0},{"alreadyActive",true}}; }
        return {{"ok",true},{"restoredCount",count}};
    };

    QQueue<QString> checkpointQueue;
    QByteArray cursor="0";
    bool checkpointActive=false, checkpointFailed=false;
    qint64 nextCheckpoint=QDateTime::currentMSecsSinceEpoch()+qint64(checkpointSec)*1000;
    qint64 nextSweep=QDateTime::currentMSecsSinceEpoch()+scanMs;
    qint64 nextRedisRetryMs=0, nextPostgresProbeMs=0, nextPostgresRetryMs=0;
    int postgresBackoffMs=1000;
    bool postgresReady=true;
    m_healthy.store(true);
    qInfo() << "[WORKER_READY] checkpoint/sweep worker running; batchSize=" << batchSize;

    while (!m_stop.load()) {
        const qint64 loopNow=QDateTime::currentMSecsSinceEpoch();
        if (!redis.isConnected() && loopNow>=nextRedisRetryMs) {
            if (redis.connectToHost(redisHost,redisPort,redisPassword,redisDb)) {
                qInfo() << "[WORKER_REDIS_RECONNECTED]";
                nextRedisRetryMs=0;
            } else {
                setError(QStringLiteral("Maintenance Redis reconnect failed: ")+redis.lastError());
                nextRedisRetryMs=loopNow+qMax(500,workerBatchMs*5);
            }
        }
        if (loopNow>=nextPostgresProbeMs) {
            nextPostgresProbeMs=loopNow+1000;
            if (postgres.healthy()) {
                postgresReady=true;
                postgresBackoffMs=1000;
                nextPostgresRetryMs=0;
            } else {
                postgresReady=false;
                m_healthy.store(false);
                if (loopNow>=nextPostgresRetryMs) {
                    if (postgres.reconnect(settings,false)) {
                        postgresReady=true;
                        postgresBackoffMs=1000;
                        nextPostgresRetryMs=0;
                        qInfo() << "[WORKER_POSTGRES_RECONNECTED]";
                    } else {
                        setError(QStringLiteral("Maintenance PostgreSQL reconnect failed: ")+postgres.error());
                        nextPostgresRetryMs=loopNow+postgresBackoffMs;
                        postgresBackoffMs=qMin(30000,postgresBackoffMs*2);
                    }
                }
            }
        }
        const bool dependenciesReady=redis.isConnected() && postgresReady;
        m_healthy.store(dependenciesReady);
        if (dependenciesReady) setError(QString());
        if (!dependenciesReady) {
            QThread::msleep(static_cast<unsigned long>(workerBatchMs));
            continue; // Keep queued recovery jobs pending until both stores are healthy.
        }

        RecoveryJob recovery;
        bool hasRecovery=false;
        {
            QMutexLocker lock(&m_jobsMutex);
            if (!m_recoveryQueue.isEmpty()) {
                recovery=m_recoveryQueue.dequeue(); hasRecovery=true;
                auto state=m_jobStates.value(recovery.jobId);
                state.insert("status","running");
                state.insert("startedAtMs",double(loopNow));
                m_jobStates.insert(recovery.jobId,state);
            }
        }
        if (hasRecovery && redis.isConnected()) {
            const QJsonObject outcome=recoverId(recovery.id,recovery.jobId);
            QMutexLocker lock(&m_jobsMutex);
            auto state=m_jobStates.value(recovery.jobId);
            const bool ok=outcome.value("ok").toBool();
            state.insert("status",ok?"completed":"failed");
            state.insert("completedAtMs",double(QDateTime::currentMSecsSinceEpoch()));
            state.insert("restoredCount",outcome.value("restoredCount"));
            if (!ok) state.insert("error",outcome.value("error"));
            if (outcome.value("capacityRejected").toBool()) state.insert("code","HOT_CAPACITY_REACHED");
            if (outcome.value("notFound").toBool()) state.insert("notFound",true);
            if (outcome.value("alreadyActive").toBool()) state.insert("alreadyActive",true);
            m_jobStates.insert(recovery.jobId,state);
            m_idJobs.remove(recovery.id);
            // Retain a bounded history of terminal job states for polling clients.
            if (m_jobStates.size()>512) {
                for (auto it=m_jobStates.begin();it!=m_jobStates.end() && m_jobStates.size()>384;) {
                    const QString st=it.value().value("status").toString();
                    if (st=="completed" || st=="failed") it=m_jobStates.erase(it); else ++it;
                }
            }
        } else if (hasRecovery) {
            QMutexLocker lock(&m_jobsMutex);
            auto state=m_jobStates.value(recovery.jobId);
            state.insert("status","failed"); state.insert("error","Redis connection unavailable");
            state.insert("completedAtMs",double(QDateTime::currentMSecsSinceEpoch()));
            m_jobStates.insert(recovery.jobId,state); m_idJobs.remove(recovery.id);
        }
        const qint64 now=QDateTime::currentMSecsSinceEpoch();
        if (now>=nextCheckpoint && !checkpointActive) {
            checkpointActive=true; checkpointFailed=false; cursor="0"; checkpointQueue.clear();
            m_checkpointRunning.store(true);
        }
        if (checkpointActive) {
            if (checkpointQueue.isEmpty()) {
                auto scan=command({"ZSCAN",key("lastSeen"),cursor,"COUNT","10"});
                if (!scan.isOk() || scan.type!=ArsRedisReply::Array || scan.elements.size()!=2 ||
                    scan.elements[0].type!=ArsRedisReply::String || scan.elements[1].type!=ArsRedisReply::Array) {
                    checkpointFailed=true; checkpointActive=false;
                } else {
                    cursor=scan.elements[0].str;
                    const auto entries=scan.elements[1].elements;
                    for (int i=0;i+1<entries.size();i+=2)
                        if (entries[i].type==ArsRedisReply::String) checkpointQueue.enqueue(QString::fromUtf8(entries[i].str));
                    if (checkpointQueue.isEmpty() && cursor=="0") checkpointActive=false;
                }
            }
            if (checkpointActive && !checkpointQueue.isEmpty()) {
                const QString id=checkpointQueue.dequeue();
                if (!flushId(id,false,0,maxBatches)) checkpointFailed=true;
                else {
                    auto length=command({"LLEN",key("history",id)});
                    auto saved=command({"HGET",key("synced"),id.toUtf8()});
                    if (!length.isOk() || !saved.isOk() || length.type!=ArsRedisReply::Integer) checkpointFailed=true;
                    else {
                        const qint64 done=saved.type==ArsRedisReply::String?saved.str.toLongLong():0;
                        if (done<length.integer) checkpointQueue.enqueue(id);
                    }
                }
            }
            if (checkpointActive && checkpointQueue.isEmpty() && cursor=="0") checkpointActive=false;
            if (!checkpointActive) {
                m_checkpointRunning.store(false);
                m_lastCheckpointSucceeded.store(!checkpointFailed);
                if (!checkpointFailed) m_lastCheckpointAtMs.store(now);
                qInfo() << "[WORKER_CHECKPOINT_DONE] success=" << !checkpointFailed;
                nextCheckpoint=now+qint64(checkpointSec)*1000;
            }
        }
        if (now>=nextSweep) {
            const qint64 cutoff=now-qint64(idleSec)*1000;
            auto inactive=command({"ZRANGEBYSCORE",key("lastSeen"),"-inf",QByteArray::number(cutoff),"LIMIT","0","10"});
            if (inactive.isOk() && inactive.type==ArsRedisReply::Array) {
                for (const auto& value:inactive.elements) {
                    if (value.type!=ArsRedisReply::String) continue;
                    const QString id=QString::fromUtf8(value.str);
                    if (!flushId(id,true,cutoff,maxBatches))
                        qWarning().noquote() << "[WORKER_SWEEP_RETRY] id=" << id << "reason=" << lastError();
                }
            }
            nextSweep=now+scanMs;
        }
        QThread::msleep(static_cast<unsigned long>(workerBatchMs));
    }
    m_checkpointRunning.store(false);
    m_healthy.store(false);
    redis.disconnectFromHost();
    qInfo() << "[WORKER_STOPPED]";
}
