#include "PostgresStore.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QUuid>
#include <QVariant>

PostgresStore::~PostgresStore() {
    const auto name = m_db.connectionName();
    m_db.close(); m_db = QSqlDatabase();
    if (!name.isEmpty()) QSqlDatabase::removeDatabase(name);
}
bool PostgresStore::reconnect(const ServerConfig& s, bool acquireWriterLock) {
    // All QSqlQuery instances are scoped to the calling operation and must have
    // been destroyed before reconnect() is called. Each thread calls it only on the
    // connection it owns: the maintenance worker without the writer lock, and the
    // client-facing thread with it (the session advisory lock is re-acquired, and
    // reconnect fails if another ARS took ownership of the database meanwhile).
    const QString oldName = m_db.connectionName();
    if (m_db.isValid()) m_db.close();
    m_db = QSqlDatabase();
    if (!oldName.isEmpty()) QSqlDatabase::removeDatabase(oldName);
    return open(s, acquireWriterLock);
}

bool PostgresStore::open(const ServerConfig& s, bool acquireWriterLock) {
    m_db = QSqlDatabase::addDatabase("QPSQL", "ars_" + QUuid::createUuid().toString());
    m_db.setHostName(s.value("postgres/host", "127.0.0.1").toString());
    m_db.setPort(s.value("postgres/port", 5432).toInt());
    m_db.setDatabaseName(s.value("postgres/database", "ars").toString());
    m_db.setUserName(s.value("postgres/user", "ars_app").toString());
    m_db.setPassword(s.value("postgres/password").toString());
    m_db.setConnectOptions("connect_timeout=5");
    if (!m_db.open()) { m_error = m_db.lastError().text(); return false; }
    QSqlQuery timeout(m_db);
    if (!timeout.exec("SET statement_timeout = '5000ms'")) { m_error = timeout.lastError().text(); return false; }
    QSqlQuery name(m_db);
    const QString applicationName=acquireWriterLock?QStringLiteral("ars_server"):QStringLiteral("ars_maintenance_worker");
    if (!name.exec(QString("SET application_name = '%1'").arg(applicationName))) {m_error=name.lastError().text();return false;}
    if (acquireWriterLock) {
        QSqlQuery lock(m_db);
        if (!lock.exec("SELECT pg_try_advisory_lock(187431220)") || !lock.next() || !lock.value(0).toBool()) {
            m_error="Another ARS server owns this database, or the ownership lock failed. Run only one ARS writer per database.";
            return false;
        }
        return migrate();
    }
    // Maintenance connections are created only after the primary server connection
    // has acquired the writer lock and completed schema migration.
    return true;
}
bool PostgresStore::migrate() {
    if (!m_db.transaction()) { m_error = m_db.lastError().text(); return false; }
    QSqlQuery q(m_db);
    auto run = [&](const QString& sql) {
        if (q.exec(sql)) return true;
        m_error = q.lastError().text(); m_db.rollback(); return false;
    };
    if (!run("SELECT pg_advisory_xact_lock(187431219)")) return false;
    if (!run("CREATE TABLE IF NOT EXISTS ars_schema_version (version INTEGER PRIMARY KEY)")) return false;
    if (!run("SELECT COALESCE(MAX(version),0) FROM ars_schema_version")) return false;
    q.next(); const int version = q.value(0).toInt(); q.finish();
    if (version > 1) { m_error = "Database schema is newer than this server"; m_db.rollback(); return false; }
    if (version == 0) {
        if (!run("CREATE TABLE ars_records (sequence BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY, "
                 "id TEXT NOT NULL, timestamp_ms BIGINT NOT NULL, received_at_ms BIGINT NOT NULL, record JSONB NOT NULL)")) return false;
        if (!run("CREATE INDEX ars_records_event ON ars_records(id,timestamp_ms DESC,sequence DESC)")) return false;
        if (!run("CREATE INDEX ars_records_received ON ars_records(id,received_at_ms,sequence)")) return false;
        if (!run("CREATE INDEX ars_records_sequence ON ars_records(id,sequence)")) return false;
        if (!run("INSERT INTO ars_schema_version VALUES (1)")) return false;
    }
    if (!m_db.commit()) { m_error = m_db.lastError().text(); m_db.rollback(); return false; }
    return true;
}
bool PostgresStore::save(const QJsonObject& record, QJsonObject& result, qint64 receivedAtMs) {
    const auto received = receivedAtMs < 0 ? QDateTime::currentMSecsSinceEpoch() : receivedAtMs;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO ars_records(id,timestamp_ms,received_at_ms,record) VALUES (?,?,?,CAST(? AS JSONB)) RETURNING sequence");
    q.addBindValue(record.value("id").toString());
    q.addBindValue(qint64(record.value("timestamp").toDouble()));
    q.addBindValue(received);
    q.addBindValue(QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact)));
    if (!q.exec() || !q.next()) { m_error = q.lastError().text(); return false; }
    result = {{"sequence", q.value(0).toString()}, {"receivedAtMs", double(received)}, {"stored", true}};
    return true;
}
bool PostgresStore::latest(const QString& id, QJsonObject& result) {
    QSqlQuery q(m_db);
    q.prepare("SELECT record::text,sequence,received_at_ms FROM ars_records WHERE id=? ORDER BY timestamp_ms DESC,sequence DESC LIMIT 1");
    q.addBindValue(id);
    if (!q.exec()) { m_error = q.lastError().text(); return false; }
    result = {{"found", false}};
    if (q.next()) result = {{"found", true}, {"record", QJsonDocument::fromJson(q.value(0).toByteArray()).object()},
        {"sequence", q.value(1).toString()}, {"receivedAtMs", double(q.value(2).toLongLong())}};
    return true;
}
bool PostgresStore::history(const QJsonObject& a, QJsonObject& result) {
    // The server validates the field and numeric bounds before calling this method.
    const auto field = a.value("timeField").toString("timestamp") == "receivedAt" ? "received_at_ms" : "timestamp_ms";
    qint64 until = a.value("untilSequence").toString().toLongLong();
    QSqlQuery q(m_db);
    if (until == 0) {
        q.prepare("SELECT COALESCE(MAX(sequence),0) FROM ars_records WHERE id=?");
        q.addBindValue(a.value("id").toString());
        if (!q.exec() || !q.next()) { m_error = q.lastError().text(); return false; }
        until = q.value(0).toLongLong(); q.finish();
    }
    QString sql = "SELECT record::text,sequence,received_at_ms FROM ars_records WHERE id=? AND sequence>? AND sequence<=?";
    const auto from = qint64(a.value("fromMs").toDouble(0));
    const auto to = qint64(a.value("toMs").toDouble(-1));
    if (from > 0) sql += QString(" AND %1>=?").arg(field);
    if (to >= 0) sql += QString(" AND %1<=?").arg(field);
    sql += " ORDER BY sequence ASC LIMIT ?";
    q.prepare(sql);
    q.addBindValue(a.value("id").toString());
    q.addBindValue(a.value("afterSequence").toString().toLongLong()); q.addBindValue(until);
    if (from > 0) q.addBindValue(from);
    if (to >= 0) q.addBindValue(to);
    const int limit = a.value("limit").toInt(100);
    q.addBindValue(limit+1);
    if (!q.exec()) { m_error = q.lastError().text(); return false; }
    QJsonArray items;
    QString next;
    bool more = false;
    int bytes = 0;
    while (q.next()) {
        const auto json = q.value(0).toByteArray();
        if (items.size() >= limit || (bytes + json.size() > 4*1024*1024 && !items.isEmpty())) { more = true; break; }
        bytes += json.size(); next = q.value(1).toString();
        items.append(QJsonObject{{"record", QJsonDocument::fromJson(json).object()}, {"sequence", next},
            {"receivedAtMs", double(q.value(2).toLongLong())}});
    }
    result = {{"items", items}, {"hasMore", more}, {"nextSequence", next}, {"untilSequence", QString::number(until)}};
    return true;
}
bool PostgresStore::remove(const QString& id, QJsonObject& result) {
    QSqlQuery q(m_db); q.prepare("DELETE FROM ars_records WHERE id=?"); q.addBindValue(id);
    if (!q.exec()) { m_error = q.lastError().text(); return false; }
    result = {{"deleted", double(q.numRowsAffected())}}; return true;
}
bool PostgresStore::healthy() { QSqlQuery q(m_db); return q.exec("SELECT 1"); }

// Reserve monotonically increasing server sequences in one SQL round-trip per 1024 writes.
// Reserved values need not be contiguous: gaps are harmless, and survive process restarts.
bool PostgresStore::reserveSequences(int count, QVector<qint64>& values) {
    QSqlQuery q(m_db);
    q.prepare("SELECT nextval(pg_get_serial_sequence('ars_records','sequence')) FROM generate_series(1,?)");
    q.addBindValue(count);
    if (!q.exec()) {m_error=q.lastError().text();return false;}
    values.clear();
    while(q.next()) values.append(q.value(0).toLongLong());
    if (values.size()!=count) {m_error="Sequence reservation returned wrong number of entries";return false;}
    return true;
}
// Forward-only database cursor: archive history is not loaded in one giant C++ array.
bool PostgresStore::loadAll(const QString& id,const std::function<bool(const QJsonObject&)>& visitor) {
    // Keyset pagination bounds both client memory and the lifetime of each SQL
    // statement/snapshot. A large recovery never holds one giant forward cursor.
    constexpr int pageSize=100;
    qint64 afterSequence=0;
    while (true) {
        QSqlQuery q(m_db);
        q.setForwardOnly(true);
        q.prepare("SELECT record::text,sequence,received_at_ms FROM ars_records "
                  "WHERE id=? AND sequence>? ORDER BY sequence ASC LIMIT ?");
        q.addBindValue(id);
        q.addBindValue(afterSequence);
        q.addBindValue(pageSize);
        if (!q.exec()) {m_error=q.lastError().text();return false;}
        int rows=0;
        while(q.next()) {
            const qint64 sequence=q.value(1).toLongLong();
            QJsonObject item{{"record",QJsonDocument::fromJson(q.value(0).toByteArray()).object()},
                {"sequence",QString::number(sequence)},
                {"receivedAtMs",double(q.value(2).toLongLong())}};
            if (!visitor(item)) {q.finish();m_error="Archive restoration interrupted";return false;}
            afterSequence=sequence;
            ++rows;
        }
        if (q.lastError().isValid()) {m_error=q.lastError().text();return false;}
        q.finish();
        if (rows<pageSize) return true;
    }
}
// Each checkpoint batch is all-or-nothing. Retrying after a lost ACK is safe by sequence.
bool PostgresStore::archiveBatch(const QJsonArray& items) {
    if (items.isEmpty()) return true;
    if (!m_db.transaction()) {m_error=m_db.lastError().text();return false;}
    QSqlQuery q(m_db);
    QStringList rows;
    for (int i = 0; i < items.size(); ++i) rows.append("(?,?,?,?,CAST(? AS JSONB))");
    if (!q.prepare("INSERT INTO ars_records(sequence,id,timestamp_ms,received_at_ms,record) "
                   "OVERRIDING SYSTEM VALUE VALUES " + rows.join(',') +
                   " ON CONFLICT (sequence) DO NOTHING")) {
        m_error=q.lastError().text();m_db.rollback();return false;
    }
    for (const auto& value:items) {
        const auto item=value.toObject();
        const auto record=item.value("record").toObject();
        bool validSequence=false;
        const qint64 sequence=item.value("sequence").toString().toLongLong(&validSequence);
        if (!validSequence || sequence<=0 || record.value("id").toString().isEmpty()) {
            m_error="Invalid record in checkpoint batch";m_db.rollback();return false;
        }
        q.addBindValue(sequence);
        q.addBindValue(record.value("id").toString());
        q.addBindValue(qint64(record.value("timestamp").toDouble()));
        q.addBindValue(qint64(item.value("receivedAtMs").toDouble()));
        q.addBindValue(QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact)));
    }
    if (!q.exec()) {m_error=q.lastError().text();m_db.rollback();return false;}
    if (!m_db.commit()) {m_error=m_db.lastError().text();m_db.rollback();return false;}
    return true;
}

// Query by server reception time. The composite index (id, received_at_ms,
// sequence) avoids scanning unrelated IDs or old records.
bool PostgresStore::recent(const QJsonObject& a,QJsonObject& result) {
    QSqlQuery q(m_db);
    q.prepare("SELECT record::text,sequence,received_at_ms FROM ars_records "
              "WHERE id=? AND received_at_ms>=? AND received_at_ms<=? "
              "ORDER BY received_at_ms ASC,sequence ASC LIMIT ? OFFSET ?");
    q.addBindValue(a.value("id").toString());
    q.addBindValue(qint64(a.value("fromMs").toDouble()));
    q.addBindValue(qint64(a.value("toMs").toDouble()));
    const int limit=a.value("limit").toInt(50);
    const qint64 offset=qint64(a.value("offset").toDouble());
    q.addBindValue(limit+1);
    q.addBindValue(offset);
    if (!q.exec()) {m_error=q.lastError().text();return false;}
    QJsonArray items;
    int bytes=0;
    bool more=false;
    while (q.next()) {
        const auto raw=q.value(0).toByteArray();
        if (items.size()>=limit || (bytes+raw.size()>3*1024*1024 && !items.isEmpty())) {more=true;break;}
        bytes+=raw.size();
        items.append(QJsonObject{{"record",QJsonDocument::fromJson(raw).object()},
                                 {"sequence",q.value(1).toString()},
                                 {"receivedAtMs",double(q.value(2).toLongLong())}});
    }
    if (q.lastError().isValid()) {m_error=q.lastError().text();return false;}
    result={{"items",items},{"hasMore",more},{"nextOffset",double(offset+items.size())}};
    return true;
}
