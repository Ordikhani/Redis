#include "HotColdStore.h"
#include "HotColdScripts.h"
#include <QDateTime>
#include "ServerConfig.h"
#include <QJsonDocument>
#include <QDebug>
#include <algorithm>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <limits>

namespace {
QByteArray json(const QJsonObject& obj) { return QJsonDocument(obj).toJson(QJsonDocument::Compact); }
QJsonObject parse(const QByteArray& bytes) { return QJsonDocument::fromJson(bytes).object(); }
qint64 seq(const QJsonObject& obj) { return obj.value("sequence").toString().toLongLong(); }
}

HotColdStore::~HotColdStore() {
    if (m_worker) { m_worker->requestStop(); m_worker->wait(); delete m_worker; m_worker=nullptr; }
}

QByteArray HotColdStore::key(const char* kind, const QString& id) const {
    QByteArray value=m_prefix+":"+kind;
    if (!id.isNull()) value += ":" + id.toUtf8().toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals);
    return value;
}
void HotColdStore::disableRedis(const QString& why) {
    if (m_hot) qWarning() << "Redis hot store disabled; subsequent operations use PostgreSQL:" << why;
    m_hot=false; m_redis.disconnectFromHost(); m_error=why;
}
ArsRedisReply HotColdStore::command(const QList<QByteArray>& args) {
    auto r=m_redis.sendCommand(args, 2500);
    if (!r.isOk()) disableRedis(r.error);
    return r;
}
bool HotColdStore::open(const ServerConfig& s) {
    if (!m_postgres.open(s)) { m_error=m_postgres.error(); return false; }
    m_idleSeconds=qBound(1,s.value("lifecycle/inactiveAfterSec",60).toInt(),86400*30);
    m_backupSeconds=qBound(1,s.value("lifecycle/checkpointEverySec",300).toInt(),86400);
    m_batchSize=qBound(1,s.value("lifecycle/batchSize",100).toInt(),500);
    m_maxActiveIds=qBound(0,s.value("memory/maxActiveIds",0).toInt(),10000000);
    m_maxBatchesPerSweep=qBound(1,s.value("lifecycle/maxBatchesPerSweep",4).toInt(),100);
    m_prefix=s.value("redis/keyPrefix","arscentral").toString().toUtf8()+":hotv1";
    m_hot=s.value("redis/enabled",true).toBool() && m_redis.connectToHost(
        s.value("redis/host","127.0.0.1").toString(),
        quint16(s.value("redis/port",6379).toUInt()),s.value("redis/password").toString(),
        s.value("redis/db",0).toInt());
    if (!m_hot) qWarning() << "Redis unavailable: PostgreSQL-only fallback";
    if (m_hot) {
        auto readConfig=[this](const QByteArray& name)->QByteArray {
            auto r=m_redis.sendCommand({"CONFIG","GET",name},2500);
            if (r.type==ArsRedisReply::Array && r.elements.size()==2 &&
                r.elements.at(1).type==ArsRedisReply::String) return r.elements.at(1).str;
            return {};
        };
        const auto eviction=readConfig("maxmemory-policy");
        const auto aof=readConfig("appendonly");
        const auto sync=readConfig("appendfsync");
        if (eviction!="noeviction") {
            qWarning() << "Redis maxmemory-policy must be noeviction, otherwise unsynced records may disappear:" << eviction;
            if (s.value("redis/requireDurability",false).toBool()) disableRedis("Redis eviction policy unsafe");
        }
        if (aof!="yes" || sync!="always") {
            qWarning() << "Redis AOF appendfsync always is required for strict power-failure durability; current:" << aof << sync;
            if (s.value("redis/requireDurability",false).toBool()) disableRedis("Redis AOF policy insufficient");
        }
    }
    if (s.value("redis/enabled",true).toBool() && s.value("redis/required",true).toBool() && !m_hot) {
        m_error="Redis hot store is required but not ready. Check startup diagnostics or explicitly set redis/required=false for SQL-only fallback.";
        return false;
    }
    // Maintenance uses independent Redis and PostgreSQL connections in a bounded
    // worker thread. Client reads/writes remain on the server thread and never share
    // QTcpSocket or QSqlDatabase instances with this worker.
    if (m_hot) {
        m_worker = new HotColdWorker();
        m_worker->start();
        QElapsedTimer startupTimer; startupTimer.start();
        while (!m_worker->isHealthy() && startupTimer.elapsed()<10000) {
            if (startupTimer.elapsed()>200 && !m_worker->isRunning()) break;
            QThread::msleep(20);
        }
        if (!m_worker->isHealthy()) {
            m_error = m_worker->lastError().isEmpty()
                ? QStringLiteral("Maintenance worker did not become ready within 10 seconds")
                : m_worker->lastError();
            return false;
        }
    }
    m_lastBackupMs=0;
    return true;
}
bool HotColdStore::nextSequence(qint64& value) {
    if (m_sequenceOffset>=m_sequencePool.size()) {
        m_sequencePool.clear(); m_sequenceOffset=0;
        if (!m_postgres.reserveSequences(1024,m_sequencePool)) {m_error=m_postgres.error();return false;}
    }
    value=m_sequencePool.at(m_sequenceOffset++); return true;
}
bool HotColdStore::active(const QString& id, bool& found) {
    auto r=command({"ZSCORE",key("lastSeen"),id.toUtf8()});
    if (!r.isOk()) return false;
    found=r.type!=ArsRedisReply::Nil;
    return true;
}
bool HotColdStore::append(const QJsonObject& item,qint64 received,qint64 sequence,bool requireActive) {
    const auto id=item.value("record").toObject().value("id").toString();
    const auto r=command({"EVAL",HotColdScripts::append,"4",key("history",id),key("latest",id),key("lastSeen"),key("recvIndex",id),
                          json(item),QByteArray::number(qint64(item.value("record").toObject().value("timestamp").toDouble())),
                          QByteArray::number(received),id.toUtf8(),requireActive?QByteArray("1"):QByteArray("0"),
                          QByteArray::number(m_maxActiveIds)});
    Q_UNUSED(sequence);
    if (!r.isOk() || r.type!=ArsRedisReply::Integer) return false;
    if (r.integer==-1) {
        m_errorCode="HOT_ID_EVICTED_DURING_APPEND";
        m_error="ID became cold between the active check and append; restore is required";
        return false;
    }
    if (r.integer==-2) {
        m_errorCode="HOT_CAPACITY_REACHED";
        m_error="Hot ID capacity reached at atomic append admission";
        return false;
    }
    return r.integer>0;
}
bool HotColdStore::save(const QJsonObject& record,QJsonObject& result,qint64 receivedAtMs) {
    m_errorCode.clear();
    // A write held by the server while its ID was recovered keeps its arrival time.
    const qint64 received=receivedAtMs>0?receivedAtMs:QDateTime::currentMSecsSinceEpoch();
    auto directSql=[&]() -> bool {
        if (!m_postgres.save(record,result,received)) {m_error=m_postgres.error();return false;}
        result.insert("storage","postgres");result.insert("checkpointed",true);
        result.insert("cacheAvailable",false);
        qWarning().noquote() << "[SQL_FALLBACK] id=" << record.value("id").toString()
                             << "storage=PostgreSQL (Redis unavailable)";
        return true;
    };
    if (!m_hot) return directSql();
    const auto id=record.value("id").toString();
    bool existing=false;
    if (!active(id,existing)) return directSql();

    auto capacityAvailable=[&]() -> bool {
        if (m_maxActiveIds<=0) return true;
        auto activeCount=command({"ZCARD",key("lastSeen")});
        if (!activeCount.isOk() || activeCount.type!=ArsRedisReply::Integer) return false;
        if (activeCount.integer>=m_maxActiveIds) {
            m_errorCode="HOT_CAPACITY_REACHED";
            m_error="Hot ID capacity reached; retry after an inactive ID has been archived and evicted";
            return false;
        }
        return true;
    };
    if (!existing && !capacityAvailable()) return false;

    auto queueRestore=[&]() -> bool {
        QString jobId, queueError;
        if (!m_worker || !m_worker->enqueueRecovery(id,QString(),jobId,queueError)) {
            m_errorCode="RECOVERY_UNAVAILABLE";
            m_error=queueError.isEmpty()?QStringLiteral("Recovery worker unavailable"):queueError;
            return false;
        }
        m_errorCode.clear();
        result={{"stored",false},{"storage","pending_recovery"},{"checkpointed",false},
                {"cacheAvailable",false},{"recoveryPending",true},{"recoveryJobId",jobId},
                {"retryAfterMs",250}};
        return true;
    };

    if (!existing) {
        QJsonObject archivedLatest;
        if (!m_postgres.latest(id,archivedLatest)) { m_error=m_postgres.error(); return false; }
        if (archivedLatest.value("found").toBool()) return queueRestore();
    }

    auto writeRecord=[&](bool requireActive,qint64& sequence)->bool {
        if (!nextSequence(sequence)) return false;
        const QJsonObject item{{"record",record},{"sequence",QString::number(sequence)},
                               {"receivedAtMs",double(received)}};
        return append(item,received,sequence,requireActive);
    };
    qint64 sequence=0;
    if (!writeRecord(existing,sequence)) {
        if (m_errorCode!="HOT_ID_EVICTED_DURING_APPEND") {
            if (m_error.isEmpty()) m_error="Redis append failed or acknowledgement was lost; verify history before retrying";
            return false;
        }
        // The worker won the race and evicted this ID after the ZSCORE check.
        // Since eviction is allowed only after SQL checkpoint completion, queue
        // a full restore and explicitly do not acknowledge this new record.
        QJsonObject archivedLatest;
        if (!m_postgres.latest(id,archivedLatest)) {m_error=m_postgres.error();m_errorCode="POSTGRES_UNAVAILABLE";return false;}
        if (archivedLatest.value("found").toBool()) return queueRestore();

        // Defensive recovery for an ID whose Redis active marker disappeared
        // but which has no SQL history: retry as a new ID, without active guard.
        m_errorCode.clear();m_error.clear();
        if (!writeRecord(false,sequence)) {
            if (m_error.isEmpty()) m_error="Redis append failed after cold-ID race recovery";
            return false;
        }
    }
    if (!existing)
        qInfo().noquote() << "[NEW_HOT] atUtc=" << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
                          << "id=" << id << "storage=Redis";
    m_errorCode.clear();
    result={{"sequence",QString::number(sequence)},{"receivedAtMs",double(received)},
            {"stored",true},{"storage","redis"},{"checkpointed",false},
            {"cacheAvailable",true},{"restoredFromPostgres",false},{"restoredHistoryCount",0}};
    return true;
}

bool HotColdStore::latest(const QString& id,QJsonObject& result) {
    if (m_hot) {
        auto r=command({"GET",key("latest",id)});
        if (r.isOk() && r.type==ArsRedisReply::String) {
            const auto item=parse(r.str);
            if (!item.isEmpty()) {
                result=item; result.insert("found",true); result.insert("source","redis");
                result.insert("cached",true); return true;
            }
        }
    }
    if (!m_postgres.latest(id,result)) {m_error=m_postgres.error();return false;}
    result.insert("source","postgres"); result.insert("cached",false); return true;
}
bool HotColdStore::hydrate(const QString& id,QJsonObject& result) {
    m_errorCode.clear();
    if (!m_hot) {
        result={{"status","unavailable"},{"error","Redis hot cache is disabled"}};
        return true;
    }
    bool existing=false;
    if (!active(id,existing)) return false;
    if (existing) {
        result={{"status","completed"},{"id",id},{"alreadyActive",true},{"restoredCount",0}};
        return true;
    }
    QJsonObject archivedLatest;
    if (!m_postgres.latest(id,archivedLatest)) {m_error=m_postgres.error();return false;}
    if (!archivedLatest.value("found").toBool()) {
        result={{"status","completed"},{"id",id},{"notFound",true},{"restoredCount",0}};
        return true;
    }
    if (m_maxActiveIds>0) {
        auto activeCount=command({"ZCARD",key("lastSeen")});
        if (!activeCount.isOk() || activeCount.type!=ArsRedisReply::Integer) return false;
        if (activeCount.integer>=m_maxActiveIds) {
            m_errorCode="HOT_CAPACITY_REACHED";
            m_error="Hot ID capacity reached; explicit hydration rejected before Redis memory admission";
            return false;
        }
    }
    QString jobId, queueError;
    if (!m_worker || !m_worker->enqueueRecovery(id,QString(),jobId,queueError)) {
        m_error=queueError.isEmpty()?QStringLiteral("Recovery worker unavailable"):queueError;
        return false;
    }
    result={{"status","queued"},{"id",id},{"jobId",jobId}};
    return true;
}

bool HotColdStore::recoveryStatus(const QString& jobId,QJsonObject& result) {
    if (!m_worker) {m_error="Recovery worker unavailable";return false;}
    if (!m_worker->recoveryStatus(jobId,result)) {m_error="Unknown or expired recovery job";return false;}
    return true;
}
bool HotColdStore::itemAt(const QByteArray& listKey,qint64 index,QJsonObject& item) {
    auto r=command({"LINDEX",listKey,QByteArray::number(index)});
    if (!r.isOk() || r.type!=ArsRedisReply::String) return false;
    item=parse(r.str);return !item.isEmpty();
}
bool HotColdStore::redisHistory(const QJsonObject& a,QJsonObject& result) {
    const auto listKey=key("history",a.value("id").toString());
    auto lr=command({"LLEN",listKey});
    if (!lr.isOk() || lr.type!=ArsRedisReply::Integer) return false;
    const qint64 count=lr.integer;
    if (count==0) {result={{"items",QJsonArray()},{"hasMore",false},{"nextSequence",""},{"untilSequence","0"}};return true;}
    QJsonObject last;
    if (!itemAt(listKey,count-1,last)) return false;
    const auto untilText=a.value("untilSequence").toString();
    const qint64 until=untilText.isEmpty()?seq(last):untilText.toLongLong();
    const qint64 after=a.value("afterSequence").toString().toLongLong();
    // Binary search by monotonic server sequence; no scan from the beginning on each page.
    qint64 lo=0,hi=count;
    while(lo<hi) {
        const auto mid=lo+(hi-lo)/2;
        QJsonObject at;
        if (!itemAt(listKey,mid,at)) return false;
        if (seq(at)<=after) lo=mid+1; else hi=mid;
    }
    const auto from=qint64(a.value("fromMs").toDouble(0));
    const auto to=qint64(a.value("toMs").toDouble(-1));
    const auto limit=a.value("limit").toInt(100);
    const bool recv=a.value("timeField").toString("timestamp")=="receivedAt";
    QJsonArray items;
    QString next;
    int bytes=0;
    bool more=false;
    for (qint64 index=lo;index<count;) {
        const qint64 end=qMin(count-1,index+qint64(11));
        auto page=command({"LRANGE",listKey,QByteArray::number(index),QByteArray::number(end)});
        if (!page.isOk() || page.type!=ArsRedisReply::Array) return false;
        if (page.elements.isEmpty()) break;
        for (const auto& el:page.elements) {
            if (el.type!=ArsRedisReply::String) return false;
            const auto obj=parse(el.str);
            if (obj.isEmpty()) return false;
            if (seq(obj)>until) {index=count;break;}
            const auto value=recv?qint64(obj.value("receivedAtMs").toDouble()):
                 qint64(obj.value("record").toObject().value("timestamp").toDouble());
            if (value>=from && (to<0 || value<=to)) {
                if (items.size()>=limit || (bytes+el.str.size()>4*1024*1024 && !items.isEmpty())) {more=true;index=count;break;}
                bytes+=el.str.size();items.append(obj);next=obj.value("sequence").toString();
            }
        }
        if (index!=count) index=end+1;
    }
    result={{"items",items},{"hasMore",more},{"nextSequence",next},
            {"untilSequence",QString::number(until)}};
    return true;
}
// After a hot read, confirm the ID is still hot. The maintenance worker evicts only after
// every row is committed to PostgreSQL, so an ID evicted during the read is complete in SQL.
// Returns 1 = use the Redis result, 0 = read PostgreSQL instead (ID evicted, or Redis lost),
// -1 = the ID is still hot but its Redis read failed (do not serve a partial answer).
int HotColdStore::afterHotRead(const QString& id,bool readOk,const char* operation) {
    bool still=false;
    if (!m_hot || !active(id,still)) return 0;
    if (still) return readOk ? 1 : -1;
    qInfo().noquote() << "[READ_RACE] id=" << id << "op=" << operation << "evicted during read; served from PostgreSQL";
    return 0;
}
bool HotColdStore::history(const QJsonObject& args,QJsonObject& result) {
    if (m_hot) {
        const auto id=args.value("id").toString();
        bool existing=false;
        if (active(id,existing) && existing) {
            const int use=afterHotRead(id,redisHistory(args,result),"history");
            if (use==1) {result.insert("source","redis");return true;}
            if (use==-1) {m_error="Unable to read complete hot history";return false;}
        }
    }
    if (!m_postgres.history(args,result)) {m_error=m_postgres.error();return false;}
    result.insert("source","postgres");return true;
}
bool HotColdStore::flushId(const QString& id,bool evict,qint64 cutoff,int maxBatches,bool* complete) {
    if (complete) *complete=false;
    if (!m_hot) return false;
    const auto listKey=key("history",id);
    auto length=command({"LLEN",listKey});
    if (!length.isOk() || length.type!=ArsRedisReply::Integer) return false;
    auto saved=command({"HGET",key("synced"),id.toUtf8()});
    if (!saved.isOk()) return false;
    qint64 index=saved.type==ArsRedisReply::String?saved.str.toLongLong():0;
    if (index>length.integer) {m_error="Checkpoint offset exceeds history length";return false;}
    for (int n=0;n<maxBatches && index<length.integer;n++) {
        auto records=command({"LRANGE",listKey,QByteArray::number(index),
                              QByteArray::number(qMin(length.integer-1,index+m_batchSize-1))});
        if (!records.isOk() || records.type!=ArsRedisReply::Array || records.elements.isEmpty()) return false;
        QJsonArray batch;
        for (const auto& item:records.elements) {
            if (item.type!=ArsRedisReply::String) {m_error="Corrupt history item";return false;}
            auto obj=parse(item.str);
            if (obj.isEmpty()) {m_error="Invalid history JSON";return false;}
            batch.append(obj);
        }
        if (!m_postgres.archiveBatch(batch)) {m_error=m_postgres.error();qWarning()<<"Archive failed:"<<m_error;return false;}
        // Advance the offset only if Redis still holds exactly these rows at [index, index+n).
        const auto advanced=command({"EVAL",HotColdScripts::advanceSynced,"2",key("synced"),listKey,id.toUtf8(),
                                     QByteArray::number(index),QByteArray::number(index+batch.size()),
                                     batch.first().toObject().value("sequence").toString().toUtf8(),
                                     batch.last().toObject().value("sequence").toString().toUtf8()});
        if (!advanced.isOk() || advanced.type!=ArsRedisReply::Integer) return false;
        if (advanced.integer==-1) {
            // The maintenance worker advanced the same offset meanwhile. The rows are already
            // committed (inserts are idempotent by sequence), so continue from its offset.
            auto current=command({"HGET",key("synced"),id.toUtf8()});
            if (!current.isOk()) return false;
            index=current.type==ArsRedisReply::String?current.str.toLongLong():0;
            if (index>length.integer) {m_error="Checkpoint offset exceeds history length";return false;}
            continue;
        }
        if (advanced.integer!=1) {m_error="Checkpoint batch does not match Redis history; offset not advanced";return false;}
        index+=batch.size();
        qInfo().noquote() << "[CHECKPOINT] atUtc=" << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
                          << "id=" << id << "destination=PostgreSQL records="
                          << batch.size() << "syncedHistoryCount=" << index;
    }
    if (complete) *complete=(index==length.integer);
    if (index<length.integer && maxBatches>=100000) {
        m_error="Checkpoint requires too many batches; no data evicted";
        return false;
    }
    if (evict && index==length.integer) {
        auto r=command({"EVAL",HotColdScripts::evict,"5",listKey,key("latest",id),key("lastSeen"),key("synced"),key("recvIndex",id),
                        id.toUtf8(),QByteArray::number(cutoff)});
        if (!r.isOk()) return false;
        if (r.type==ArsRedisReply::Integer && r.integer==1)
            qInfo().noquote() << "[ARCHIVED] atUtc=" << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
                              << "id=" << id
                              << "source=Redis destination=PostgreSQL"
                              << "historyRecords=" << index
                              << "reason=inactivity cacheEvicted=true";
    }
    return true;
}
bool HotColdStore::remove(const QString& id,bool fromDisk,QJsonObject& result) {
    if (m_worker && m_worker->recoveryInProgress(id)) {
        m_error="Cannot delete an ID while its history recovery job is queued or running; wait for a terminal job state and retry";
        return false;
    }
    if (!fromDisk) {
        if (m_hot) {
            // Eviction must first archive all unsynced records, not silently drop them.
            if (!flushId(id,false,0,100000)) return false;
            const auto r=command({"EVAL",HotColdScripts::evict,"5",key("history",id),key("latest",id),
                                  key("lastSeen"),key("synced"),key("recvIndex",id),id.toUtf8(),
                                  QByteArray::number(std::numeric_limits<qint64>::max())});
            if (!r.isOk()) return false;
            result={{"cacheEvicted",r.integer==1},{"archivePreserved",true}};
            qInfo().noquote() << "[DELETE_CACHE] id=" << id << "archivePreserved=true";
        } else result={{"cacheEvicted",false},{"archivePreserved",true}};
        return true;
    }
    // Checkpoint first, remove Redis copy second, delete PostgreSQL copy last.
    // If SQL deletion fails, the checkpointed archive remains recoverable.
    if (m_hot) {
        if (!flushId(id,false,0,100000)) return false;
        auto r=command({"EVAL",HotColdScripts::evict,"5",key("history",id),key("latest",id),
                        key("lastSeen"),key("synced"),key("recvIndex",id),id.toUtf8(),
                        QByteArray::number(std::numeric_limits<qint64>::max())});
        if (!r.isOk()) return false;
    }
    if (!m_postgres.remove(id,result)) {m_error=m_postgres.error();return false;}
    result.insert("cacheAvailable",m_hot);
    qInfo().noquote() << "[DELETE_ALL] id=" << id << "source=Redis+PostgreSQL";
    return true;
}

// Small, deliberately bounded in-memory query. "latest" scans the latest record
// of every hot ID; "records" scans every hot history record, across all IDs.
// The cursor is an index into a changing hot set, so it is best-effort rather
// than a transactional snapshot. PostgreSQL records are never searched here.
namespace {
QJsonValue fieldValue(const QJsonObject& item, const QString& field) {
    if (field=="receivedAtMs" || field=="sequence") return item.value(field);
    QJsonValue value=item.value("record");
    const auto parts=field.split('.');
    for (const auto& part:parts) {
        if (!value.isObject()) return QJsonValue();
        value=value.toObject().value(part);
    }
    return value;
}
bool matches(const QJsonObject& item, const QJsonArray& filters, const QString& idContains) {
    if (!item.value("record").toObject().value("id").toString().contains(idContains,Qt::CaseInsensitive)) return false;
    for (const auto& raw:filters) {
        const auto c=raw.toObject();
        const auto value=fieldValue(item,c.value("field").toString());
        const auto expected=c.value("value");
        const auto op=c.value("op").toString("eq");
        if (value.isUndefined() || value.isNull()) return false;
        bool matches=false;
        if (op=="contains") {
            matches=value.isString() && value.toString().contains(expected.toString(),Qt::CaseInsensitive);
        } else if (op=="eq" || op=="ne") {
            bool equals=false;
            if (value.isDouble() && expected.isDouble()) equals=value.toDouble()==expected.toDouble();
            else if (value.isBool() && expected.isBool()) equals=value.toBool()==expected.toBool();
            else if (value.isString() && expected.isString()) equals=value.toString()==expected.toString();
            matches=op=="eq"?equals:!equals;
        } else if (value.isDouble() && expected.isDouble()) {
            const double x=value.toDouble(), y=expected.toDouble();
            if (op=="gt") matches=x>y;
            if (op=="gte") matches=x>=y;
            if (op=="lt") matches=x<y;
            if (op=="lte") matches=x<=y;
        }
        if (!matches) return false;
    }
    return true;
}
}

bool HotColdStore::searchHot(const QJsonObject& args,QJsonObject& result) {
    if (!m_hot) {m_error="Hot search requires Redis; PostgreSQL fallback is not a RAM search";return false;}
    const QString mode=args.value("mode").toString("latest");
    const int limit=args.value("limit").toInt(20);
    const int budget=args.value("scanBudget").toInt(120);
    const QString cursor=args.value("cursor").toString("0:0");
    const auto parts=cursor.split(':');
    if (parts.size()!=2) {m_error="Invalid search cursor";return false;}
    bool firstOk=false,secondOk=false;
    qint64 idPos=parts.at(0).toLongLong(&firstOk), recordPos=parts.at(1).toLongLong(&secondOk);
    if (!firstOk || !secondOk || idPos<0 || recordPos<0 || idPos>100000000 || recordPos>100000000) {
        m_error="Invalid search cursor";return false;
    }
    const auto filters=args.value("filters").toArray();
    const auto idContains=args.value("idContains").toString();
    auto total=command({"ZCARD",key("lastSeen")});
    if (!total.isOk() || total.type!=ArsRedisReply::Integer) return false;
    const qint64 count=total.integer;
    QJsonArray items;
    int scanned=0,bytes=0;
    bool sizeLimit=false;
    while (idPos<count && scanned<budget && items.size()<limit && !sizeLimit) {
        if (mode=="latest") {
            // Two Redis commands for up to 12 IDs, instead of one GET per ID.
            const qint64 batchSize=qMin(qint64(12),qMin(count-idPos,qint64(budget-scanned)));
            auto ids=command({"ZREVRANGE",key("lastSeen"),QByteArray::number(idPos),
                              QByteArray::number(idPos+batchSize-1)});
            if (!ids.isOk() || ids.type!=ArsRedisReply::Array) return false;
            if (ids.elements.isEmpty()) break;
            QList<QByteArray> get={"MGET"};
            for (const auto& id:ids.elements) get.append(key("latest",QString::fromUtf8(id.str)));
            auto values=command(get);
            if (!values.isOk() || values.type!=ArsRedisReply::Array ||
                values.elements.size()!=ids.elements.size()) return false;
            for (const auto& value:values.elements) {
                ++scanned;++idPos;recordPos=0;
                if (value.type!=ArsRedisReply::String) continue;
                const auto item=parse(value.str);
                if (item.isEmpty()) {m_error="Invalid hot latest JSON";return false;}
                if (!matches(item,filters,idContains)) continue;
                if (bytes+value.str.size()>3*1024*1024 && !items.isEmpty()) {
                    --idPos;sizeLimit=true;break;
                }
                items.append(item);bytes+=value.str.size();
                if (items.size()>=limit) break;
            }
        } else {
            auto idReply=command({"ZREVRANGE",key("lastSeen"),QByteArray::number(idPos),QByteArray::number(idPos)});
            if (!idReply.isOk() || idReply.type!=ArsRedisReply::Array) return false;
            if (idReply.elements.isEmpty()) break;
            const QString id=QString::fromUtf8(idReply.elements.at(0).str);
            const qint64 fetch=qMin(qint64(12),qint64(budget-scanned));
            auto page=command({"LRANGE",key("history",id),QByteArray::number(recordPos),QByteArray::number(recordPos+fetch-1)});
            if (!page.isOk() || page.type!=ArsRedisReply::Array) return false;
            if (page.elements.isEmpty()) {++idPos;recordPos=0;continue;}
            for (const auto& el:page.elements) {
                if (el.type!=ArsRedisReply::String) {m_error="Bad Redis history item";return false;}
                auto item=parse(el.str);
                if (item.isEmpty()) {m_error="Bad Redis history JSON";return false;}
                if (matches(item,filters,idContains)) {
                    if (bytes+el.str.size()>3*1024*1024 && !items.isEmpty()) {sizeLimit=true;break;}
                    items.append(item);bytes+=el.str.size();
                }
                ++scanned;++recordPos;
                if (items.size()>=limit || scanned>=budget) break;
            }
            if (sizeLimit) break;
            if (page.elements.size()<fetch && items.size()<limit && scanned<budget) {
                ++idPos;recordPos=0;
            }
        }
    }
    const bool hasMore=idPos<count;
    result={{"items",items},{"source","redis"},{"mode",mode},
            {"scanned",scanned},{"hotIds",double(count)},
            {"hasMore",hasMore},{"nextCursor",hasMore?QString::number(idPos)+":"+QString::number(recordPos):QString()}};
    return true;
}

bool HotColdStore::legacyRecent(const QJsonObject& a, QJsonObject& result) {
    // Older hot IDs may predate the received-time ZSET. For those IDs we scan
    // their list in bounded Redis batches (exact, but O(history length)).
    const auto listKey=key("history",a.value("id").toString());
    auto n=command({"LLEN",listKey});
    if (!n.isOk() || n.type!=ArsRedisReply::Integer) return false;
    const qint64 from=qint64(a.value("fromMs").toDouble()),to=qint64(a.value("toMs").toDouble());
    const qint64 offset=qint64(a.value("offset").toDouble());
    const int limit=a.value("limit").toInt(50);
    qint64 eligible=0;
    QJsonArray items;
    int bytes=0;
    bool more=false;
    for (qint64 i=0;i<n.integer;i+=12) {
        auto batch=command({"LRANGE",listKey,QByteArray::number(i),QByteArray::number(qMin(n.integer-1,i+11))});
        if (!batch.isOk() || batch.type!=ArsRedisReply::Array) return false;
        for (const auto& el:batch.elements) {
            const auto item=parse(el.str);
            const qint64 t=qint64(item.value("receivedAtMs").toDouble());
            if (t<from || t>to) continue;
            if (eligible++<offset) continue;
            if (items.size()>=limit || (bytes+el.str.size()>3*1024*1024 && !items.isEmpty())) {more=true;break;}
            items.append(item);bytes+=el.str.size();
        }
        if (more) break;
    }
    result={{"items",items},{"hasMore",more},{"nextOffset",double(offset+items.size())},
            {"indexed",false},{"source","redis"}};
    return true;
}

bool HotColdStore::redisRecent(const QJsonObject& a,QJsonObject& result) {
    const auto id=a.value("id").toString();
    const auto index=key("recvIndex",id);
    auto n=command({"LLEN",key("history",id)});
    if (!n.isOk() || n.type!=ArsRedisReply::Integer) return false;
    auto z=command({"ZCARD",index});
    if (!z.isOk() || z.type!=ArsRedisReply::Integer) return false;
    if (z.integer!=n.integer) return legacyRecent(a,result);
    const auto from=QByteArray::number(qint64(a.value("fromMs").toDouble()));
    const auto to=QByteArray::number(qint64(a.value("toMs").toDouble()));
    const qint64 offset=qint64(a.value("offset").toDouble());
    const int limit=a.value("limit").toInt(50);
    // Fetch only requested time range: O(log N + returned items).
    auto r=command({"ZRANGEBYSCORE",index,from,to,"LIMIT",QByteArray::number(offset),QByteArray::number(limit+1)});
    if (!r.isOk() || r.type!=ArsRedisReply::Array) return false;
    QJsonArray items;
    int bytes=0;
    // At most 12 record payloads in one Redis response, even with limit=100.
    for (int start=0; start<r.elements.size() && start<limit && items.size()<limit;start+=12) {
        QList<QByteArray> argv={"EVAL",R"LUA(
            local out={}
            for i=1,#ARGV do out[#out+1]=redis.call('LINDEX',KEYS[1],tonumber(ARGV[i])) end
            return out
        )LUA","1",key("history",id)};
        for (int i=start;i<r.elements.size() && i<limit && i<start+12;i++) argv.append(r.elements.at(i).str);
        auto rows=command(argv);
        if (!rows.isOk() || rows.type!=ArsRedisReply::Array) return false;
        bool sizeLimit=false;
        for (const auto& row:rows.elements) {
            if (row.type!=ArsRedisReply::String) {m_error="Missing time-index entry";return false;}
            if (bytes+row.str.size()>3*1024*1024 && !items.isEmpty()) {sizeLimit=true;break;}
            auto item=parse(row.str);
            if (item.isEmpty()) {m_error="Invalid time-index JSON";return false;}
            items.append(item);bytes+=row.str.size();
        }
        if (sizeLimit) break;
    }
    const bool more=r.elements.size()>items.size();
    result={{"items",items},{"hasMore",more},{"nextOffset",double(offset+items.size())},
            {"indexed",true},{"source","redis"}};
    return true;
}

bool HotColdStore::recent(const QJsonObject& a,QJsonObject& result) {
    if (m_hot) {
        bool found=false;
        if (!active(a.value("id").toString(),found)) return false;
        if (found) {
            const int use=afterHotRead(a.value("id").toString(),redisRecent(a,result),"recent");
            if (use==1) {
                result.insert("fromMs",a.value("fromMs"));
                result.insert("toMs",a.value("toMs"));
                return true;
            }
            if (use==-1) return false;
        }
    }
    if (!m_postgres.recent(a,result)) {m_error=m_postgres.error();return false;}
    result.insert("source","postgres");result.insert("indexed",true);
    result.insert("fromMs",a.value("fromMs"));result.insert("toMs",a.value("toMs"));
    return true;
}

void HotColdStore::maintainPostgres() {
    const qint64 now=QDateTime::currentMSecsSinceEpoch();
    if (!m_postgresDown) {
        if (m_postgres.healthy()) return;
        m_postgresDown=true;
        m_postgresBackoffMs=1000;
        m_postgresRetryAtMs=now;
        qWarning().noquote() << "[POSTGRES_LOST] client-thread connection failed; reconnecting:" << m_postgres.error();
    }
    if (now<m_postgresRetryAtMs) return;
    // Re-acquire the single-writer advisory lock: it is a session lock and was released with
    // the old connection. If another ARS took the database meanwhile, keep refusing to write.
    const ServerConfig& settings = ServerConfig::instance();
    if (m_postgres.reconnect(settings,true)) {
        m_postgresDown=false;
        ++m_postgresReconnects;
        m_sequencePool.clear(); m_sequenceOffset=0; // reserve fresh values from the live sequence
        qInfo().noquote() << "[POSTGRES_RECONNECTED] client-thread connection restored; writer lock re-acquired";
        return;
    }
    qWarning().noquote() << "[POSTGRES_RECONNECT_FAILED] retry in" << m_postgresBackoffMs << "ms:" << m_postgres.error();
    m_postgresRetryAtMs=now+m_postgresBackoffMs;
    m_postgresBackoffMs=qMin(30000,m_postgresBackoffMs*2);
}
QJsonObject HotColdStore::status() {
    const bool workerHealthy=m_worker && m_worker->isHealthy();
    return {{"postgres",!m_postgresDown && m_postgres.healthy()},
            {"postgresReconnecting",m_postgresDown},{"postgresReconnects",m_postgresReconnects},{"redisCache",m_hot&&m_redis.isConnected()},
            {"hotColdEnabled",m_hot},{"inactiveAfterSec",m_idleSeconds},
            {"checkpointEverySec",m_backupSeconds},
            {"maxActiveIds",m_maxActiveIds},
            {"lastCheckpointAtMs",double(m_worker?m_worker->lastCheckpointAtMs():m_lastBackupMs)},
            {"checkpointRunning",m_worker?m_worker->checkpointRunning():false},
            {"lastCheckpointSucceeded",m_worker?m_worker->lastCheckpointSucceeded():false},
            {"maintenanceWorker",workerHealthy},
            {"recoveryQueueDepth",m_worker?m_worker->recoveryQueueDepth():0},
            {"maintenanceWorkerError",m_worker?m_worker->lastError():QString()},
            {"protocol",1}};
}
