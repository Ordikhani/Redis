#include "Server.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QHostAddress>
#include <QTimer>
#include <QDebug>
#include "ServerConfig.h"
#include <QDateTime>
#include <QRegularExpression>
#include <QUuid>
#include <cmath>

namespace {
constexpr int FrameLimit = 1024*1024;
bool validName(const QString& s) { return !s.trimmed().isEmpty() && s.toUtf8().size() <= 256; }
bool integer(const QJsonValue& v, double min, double max) {
    const double n = v.toDouble();
    return v.isDouble() && std::isfinite(n) && std::floor(n) == n && n >= min && n <= max;
}
bool cursor(const QJsonValue& v) {
    if (v.isUndefined() || (v.isString() && v.toString().isEmpty())) return true;
    if (!v.isString()) return false;
    const auto s = v.toString();
    if (s.size() > 19) return false;
    for (auto c : s) if (c < QChar('0') || c > QChar('9')) return false;
    bool ok = false; s.toLongLong(&ok); return ok;
}
QString recordError(const QJsonObject& r) {
    if (!validName(r.value("id").toString())) return "Record id is required (max 256 UTF-8 bytes)";
    if (!integer(r.value("timestamp"), 1, 9007199254740991.0)) return "timestamp must be a positive integer in milliseconds";
    for (const auto& name : {"latitude", "longitude", "altitude", "speed", "heading"}) {
        if (r.contains(name) && (!r.value(name).isDouble() || !std::isfinite(r.value(name).toDouble()))) return "Optional numeric fields must be numbers";
    }
    if (r.contains("latitude") && std::abs(r.value("latitude").toDouble()) > 90) return "Invalid latitude";
    if (r.contains("longitude") && std::abs(r.value("longitude").toDouble()) > 180) return "Invalid longitude";
    if (r.contains("features") && !r.value("features").isObject()) return "features must be an object";
    for (const auto& name : {"source", "payload"}) if (r.contains(name) && !r.value(name).isString()) return "source/payload must be strings";
    if (QJsonDocument(r).toJson(QJsonDocument::Compact).size() > 256*1024) return "Record exceeds 256 KiB";
    return {};
}
}
namespace {
constexpr int MaxDeferredWrites = 4096;
constexpr int MaxDeferredPerId = 256;
}
Server::Server(QObject* parent) : QObject(parent) {
    m_deferredTimer.setInterval(25);
    connect(&m_deferredTimer, &QTimer::timeout, this, [this] { processDeferred(); });
    m_postgresTimer.setInterval(1000);
    connect(&m_postgresTimer, &QTimer::timeout, this, [this] { m_store.maintainPostgres(); });
    connect(&m_udpListener, &QUdpSocket::readyRead, this, &Server::readUdp);
    m_udpCleanupTimer.setInterval(1000);
    connect(&m_udpCleanupTimer, &QTimer::timeout, this, &Server::housekeepingUdp);
    connect(&m_listener, &QTcpServer::newConnection, this, [this] {
        while (auto* socket = m_listener.nextPendingConnection()) {
            if (m_peers.size() >= m_maxClients) { socket->abort(); socket->deleteLater(); continue; }
            socket->setReadBufferSize(2*FrameLimit);
            m_peers.insert(socket, Peer{});
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readPeer(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] { m_peers.remove(socket); socket->deleteLater(); });
            // Clients exchange a credential-free 'hello' before sending commands.
            // This allows the same connection lifecycle for TCP and UDP.
            QTimer::singleShot(10000, socket, [this, socket] {
                if (m_peers.contains(socket) && !m_peers.value(socket).initialized) socket->disconnectFromHost();
            });
        }
    });
}
Server::~Server() {
    QObject::disconnect(&m_listener, nullptr, this, nullptr);
    for (auto* peer : m_peers.keys()) {
        QObject::disconnect(peer, nullptr, this, nullptr);
        peer->abort();
    }
    m_udpListener.close();
    m_listener.close();
}
bool Server::start() {
    const ServerConfig& s = ServerConfig::instance();
    m_maxClients = qBound(1, s.value("server/maxClients", 64).toInt(), 1024);
    // Must stay below the client SDK request timeout (15 s) so every held write gets a definite answer.
    m_recoveryAckTimeoutMs = qBound(1000, s.value("recovery/ackTimeoutMs", 10000).toInt(), 14000);
    if (!m_store.open(s)) { qCritical() << "Storage initialization failed:" << m_store.error(); return false; }
    m_postgresTimer.start();
    QHostAddress address(s.value("server/host", "127.0.0.1").toString());
    const int port = s.value("server/port", 7090).toInt();
    if (address.isNull() || port < 1 || port > 65535 || !m_listener.listen(address, quint16(port))) {
        qCritical() << "Listen failed:" << m_listener.errorString(); return false;
    }
    m_udpEnabled = s.value("udp/enabled", false).toBool();
    if (m_udpEnabled) {
        const QHostAddress udpAddress(s.value("udp/host", s.value("server/host", "127.0.0.1")).toString());
        const int udpPort = s.value("udp/port", 7092).toInt();
        m_udpMaxDatagramBytes = s.value("udp/maxDatagramBytes", 1200).toInt();
        if (udpAddress.isNull() || udpPort < 1 || udpPort > 65535 ||
            m_udpMaxDatagramBytes < 512 || m_udpMaxDatagramBytes > 4096 ||
            !m_udpListener.bind(udpAddress, quint16(udpPort))) {
            qCritical() << "UDP bind/config failed:" << m_udpListener.errorString();
            m_listener.close();
            return false;
        }
        m_udpCleanupTimer.start();
        qInfo().noquote() << "[READY] ARS UDP on" << udpAddress.toString()
                          << "port=" << udpPort << "maxDatagramBytes=" << m_udpMaxDatagramBytes;
    }
    qInfo().noquote() << "[READY] ARS TCP listening on" << address.toString() << "port=" << port
                     << "config=compiled-in";
    return true;
}
bool Server::deferWrite(const QString& id, const DeferredWrite& w) {
    auto it = m_deferred.find(id);
    if (it == m_deferred.end() || m_deferredCount >= MaxDeferredWrites || it->writes.size() >= MaxDeferredPerId) return false;
    it->writes.append(w);
    ++m_deferredCount;
    if (!m_deferredTimer.isActive()) m_deferredTimer.start();
    return true;
}
void Server::broadcastLive(const QString& channel, const QJsonObject& record,
                           const QJsonObject& result) {
    if (!result.value("stored").toBool()) return;
    const QJsonObject event{{"event", "record"}, {"channel", channel}, {"record", record},
        {"sequence", result.value("sequence")}, {"receivedAtMs", result.value("receivedAtMs")}};
    for (auto* peer : m_peers.keys())
        if (m_peers.value(peer).initialized && m_peers.value(peer).channels.contains(channel))
            write(peer, event);
    for (auto it = m_udpPeers.cbegin(); it != m_udpPeers.cend(); ++it) {
        if (!it.value().channels.contains(channel)) continue;
        const QString endpoint = it.key();
        const int separator = endpoint.lastIndexOf('|');
        if (separator > 0 && m_udpLive.size() < 2048) {
            const QHostAddress address(endpoint.left(separator));
            const quint16 port = quint16(endpoint.mid(separator+1).toUShort());
            QJsonObject live = event;
            const QString eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            live.insert("eventId",eventId);
            const QString key=endpoint+"|"+eventId;
            m_udpLive.insert(key,{address,port,live,QDateTime::currentMSecsSinceEpoch(),1});
            sendUdp(address,port,live);
        }
    }
}
void Server::finishWrite(const DeferredWrite& w, const QJsonObject& result) {
    if (w.udp || (w.socket && m_peers.contains(w.socket)))
        respond(w.socket, w.udpAddress, w.udpPort,
                {{"requestId", w.requestId}, {"ok", true}, {"result", result}});
    if (w.op == "publish") broadcastLive(w.channel, w.record, result);
}
void Server::failWrite(const DeferredWrite& w, const QString& error, const QString& code, const QJsonObject& extra) {
    if (!w.udp && (!w.socket || !m_peers.contains(w.socket))) return;
    QJsonObject message{{"requestId", w.requestId}, {"ok", false}, {"error", error}, {"code", code}};
    for (auto it = extra.begin(); it != extra.end(); ++it) message.insert(it.key(), it.value());
    respond(w.socket, w.udpAddress, w.udpPort, message);
}
// Runs every 25 ms while writes are held. A held write whose client disconnected is still
// stored: the SDK reports a disconnect as "write may already have committed".
void Server::processDeferred() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_deferred.begin(); it != m_deferred.end();) {
        DeferredId& entry = it.value();
        QJsonObject state;
        const QString status = m_store.recoveryStatus(entry.jobId, state)
            ? state.value("status").toString() : QStringLiteral("failed");
        if (status == "completed") {
            bool waitingAgain = false;
            bool firstAfterRecovery = true;
            while (!entry.writes.isEmpty()) {
                const DeferredWrite w = entry.writes.takeFirst();
                --m_deferredCount;
                QJsonObject result;
                if (!m_store.save(w.record, result, w.receivedAtMs)) {
                    const QString code = m_store.takeErrorCode();
                    qWarning() << "Deferred write failed:" << m_store.error();
                    failWrite(w, code.isEmpty() ? QStringLiteral("Database operation failed; see server log") : m_store.error(),
                              code.isEmpty() ? QStringLiteral("STORE_FAILED") : code);
                    continue;
                }
                if (result.value("recoveryPending").toBool()) {
                    // Evicted again before this write landed: wait for the new recovery job.
                    entry.jobId = result.value("recoveryJobId").toString();
                    entry.writes.prepend(w);
                    ++m_deferredCount;
                    waitingAgain = true;
                    break;
                }
                // Same fields as the old synchronous restore, so existing clients keep working.
                result.insert("heldForRecoveryMs", double(now - w.receivedAtMs));
                if (firstAfterRecovery) {
                    result.insert("restoredFromPostgres", state.value("restoredCount").toInt() > 0);
                    result.insert("restoredHistoryCount", state.value("restoredCount").toInt());
                    firstAfterRecovery = false;
                }
                finishWrite(w, result);
            }
            if (!waitingAgain) { it = m_deferred.erase(it); continue; }
        } else if (status == "failed") {
            const QString code = state.value("code").toString().isEmpty() ? QStringLiteral("RECOVERY_FAILED")
                                                                           : state.value("code").toString();
            const QString why = state.value("error").toString();
            for (const auto& w : entry.writes)
                failWrite(w, QStringLiteral("History recovery failed; the write was not stored: ") + why, code,
                          {{"recoveryJobId", entry.jobId}});
            m_deferredCount -= entry.writes.size();
            it = m_deferred.erase(it);
            continue;
        } else {
            // Still queued/running: answer writes whose ACK deadline passed (oldest first).
            while (!entry.writes.isEmpty() && entry.writes.first().deadlineMs <= now) {
                const DeferredWrite w = entry.writes.takeFirst();
                --m_deferredCount;
                failWrite(w, QStringLiteral("History recovery is still running; the write was not stored. "
                                            "Retry it after recovery_status reports completed."),
                          QStringLiteral("RECOVERY_PENDING"), {{"recoveryJobId", entry.jobId}});
            }
            if (entry.writes.isEmpty()) { it = m_deferred.erase(it); continue; }
        }
        ++it;
    }
    if (m_deferred.isEmpty()) m_deferredTimer.stop();
}
void Server::write(QTcpSocket* socket, const QJsonObject& message) {
    if (socket->state() != QAbstractSocket::ConnectedState) return;
    const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (socket->bytesToWrite() + bytes.size() > 8*1024*1024) { socket->abort(); return; }
    socket->write(bytes);
}
void Server::readPeer(QTcpSocket* socket) {
    if (!m_peers.contains(socket)) return;
    m_peers[socket].buffer += socket->readAll();
    if (m_peers[socket].buffer.size() > 2*FrameLimit) { socket->abort(); return; }
    // Yield after 32 frames so one client cannot monopolize the event loop indefinitely.
    for (int i=0; i<32 && m_peers.contains(socket); ++i) {
        const int end = m_peers[socket].buffer.indexOf('\n');
        if (end < 0) break;
        if (end > FrameLimit) { socket->abort(); return; }
        const auto line = m_peers[socket].buffer.left(end);
        m_peers[socket].buffer.remove(0, end+1);
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            write(socket, {{"ok", false}, {"error", "Invalid JSON object"}}); socket->disconnectFromHost(); return;
        }
        handleRequest(socket, {}, 0, doc.object());
        if (socket->state() != QAbstractSocket::ConnectedState) return;
    }
    if (!m_peers.contains(socket)) return;
    if (m_peers[socket].buffer.indexOf('\n') >= 0) {
        QTimer::singleShot(0, socket, [this, socket] { readPeer(socket); });
    } else if (m_peers[socket].buffer.size() > FrameLimit) socket->abort();
}
void Server::handleRequest(QTcpSocket* socket, const QHostAddress& sender, quint16 senderPort, const QJsonObject& request) {
    const bool isUdp = socket == nullptr;
    const QString endpoint = isUdp ? udpKey(sender, senderPort) : QString();
    const auto requestId = request.value("requestId").toString();
    const auto op = request.value("op").toString();
    const auto args = request.value("args").toObject();
    auto fail = [&](const QString& error) { respond(socket, sender, senderPort, {{"requestId", requestId}, {"ok", false}, {"error", error}}); };
    auto failWithCode = [&](const QString& error, const QString& code) {
        respond(socket, sender, senderPort, {{"requestId", requestId}, {"ok", false}, {"error", error}, {"code", code}});
    };
    auto reply = [&](const QJsonObject& result) { respond(socket, sender, senderPort, {{"requestId", requestId}, {"ok", true}, {"result", result}}); };
    auto dbFailed = [&] {
        qWarning() << "Database operation failed:" << m_store.error();
        const QString code=m_store.takeErrorCode();
        if (!code.isEmpty()) failWithCode(m_store.error(),code);
        else fail("Database operation failed; see server log");
    };
    if (!validName(requestId) || !request.value("args").isObject()) { fail("requestId and args object required"); return; }
    if (op == "hello") {
        // No token: register this TCP connection / UDP endpoint for service use.
        if (isUdp) {
            if (m_udpPeers.size() >= 128 && !m_udpPeers.contains(endpoint)) {
                failWithCode("Maximum UDP clients reached", "CLIENT_LIMIT"); return;
            }
            m_udpPeers[endpoint].lastSeenMs = QDateTime::currentMSecsSinceEpoch();
        } else m_peers[socket].initialized = true;
        reply({{"protocol", 1}, {"transport", isUdp ? "udp" : "tcp"}}); return;
    }
    if (isUdp) {
        if (!m_udpPeers.contains(endpoint)) {
            failWithCode("Send hello first", "HELLO_REQUIRED"); return;
        }
        m_udpPeers[endpoint].lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    } else if (!m_peers.value(socket).initialized) { fail("Send hello first"); return; }
    if (op == "heartbeat") { reply({{"alive", true}}); return; }
    if (op == "ack_event") {
        const QString eventId=args.value("eventId").toString();
        if (!validName(eventId) || eventId.size()>64) { fail("Invalid eventId"); return; }
        m_udpLive.remove(endpoint+"|"+eventId);
        reply({{"acknowledged",true}}); return;
    }
    if (op == "disconnect") {
        if (isUdp) m_udpPeers.remove(endpoint);
        reply({{"disconnected", true}});
        if (!isUdp) socket->disconnectFromHost();
        return;
    }
    QJsonObject result;
    if (op == "status") {
        result = m_store.status();
        result.insert("udpEnabled", m_udpEnabled);
        result.insert("udpPort", m_udpEnabled ? int(m_udpListener.localPort()) : 0);
        reply(result); return;
    }
    if (op == "recovery_status") {
        const QString jobId=args.value("jobId").toString();
        if (!validName(jobId) || !m_store.recoveryStatus(jobId,result)) { dbFailed(); return; }
        reply(result); return;
    }
    const auto channel = args.value("channel").toString();
    if (op == "subscribe" || op == "unsubscribe" || op == "publish") {
        if (!validName(channel)) { fail("Valid channel required (max 256 bytes)"); return; }
    }
    if (op == "subscribe" || op == "unsubscribe") {
        if (op == "subscribe") {
            auto& channels = isUdp ? m_udpPeers[endpoint].channels : m_peers[socket].channels;
            if (channels.size() >= 64 && !channels.contains(channel)) { fail("Subscription limit reached"); return; }
            channels.insert(channel);
        } else {
            if (isUdp) m_udpPeers[endpoint].channels.remove(channel);
            else m_peers[socket].channels.remove(channel);
        }
        reply({{"channel", channel}, {"subscribed", op == "subscribe"}}); return;
    }
    if (op == "save" || op == "publish") {
        const auto record = args.value("record").toObject();
        const auto error = recordError(record);
        if (!error.isEmpty()) { fail(error); return; }
        const qint64 arrived = QDateTime::currentMSecsSinceEpoch();
        const DeferredWrite pending{socket, isUdp, sender, senderPort, requestId, op, channel,
                                    record, arrived, arrived + m_recoveryAckTimeoutMs};
        const auto id = record.value("id").toString();
        if (m_deferred.contains(id)) {
            // Keep per-ID order: writes queue behind earlier writes waiting for the same recovery.
            if (!deferWrite(id, pending))
                failWithCode("Too many writes are waiting for this ID's history recovery; nothing was stored, retry later",
                             "RECOVERY_PENDING");
            return;
        }
        if (!m_store.save(record, result, arrived)) { dbFailed(); return; }
        if (result.value("recoveryPending").toBool()) {
            m_deferred.insert(id, DeferredId{result.value("recoveryJobId").toString(), {}});
            if (!deferWrite(id, pending)) {
                m_deferred.remove(id);
                failWithCode("Too many writes are waiting for history recovery; nothing was stored, retry later",
                             "RECOVERY_PENDING");
            }
            return;
        }
        finishWrite(pending, result);
        return;
    }
    if (op == "search_hot") {
        if (args.contains("mode") && args.value("mode").toString()!="latest" && args.value("mode").toString()!="records") {
            fail("mode must be latest or records");return;
        }
        if ((args.contains("limit") && !integer(args.value("limit"),1,50)) ||
            (args.contains("scanBudget") && !integer(args.value("scanBudget"),1,200)) ||
            (args.contains("idContains") && (!args.value("idContains").isString() || args.value("idContains").toString().size()>256)) ||
            (args.contains("cursor") && !args.value("cursor").isString())) {
            fail("Invalid search options");return;
        }
        const auto filters=args.value("filters");
        if (!filters.isUndefined() && !filters.isArray()) {fail("filters must be an array");return;}
        const auto arr=filters.toArray();
        if (arr.size()>8) {fail("Maximum 8 search conditions");return;}
        static const QRegularExpression pathPattern("^[a-zA-Z_][a-zA-Z0-9_.]{0,127}$");
        for (const auto& v:arr) {
            if (!v.isObject()) {fail("Invalid search condition");return;}
            const auto f=v.toObject();
            const auto opName=f.value("op").toString("eq");
            if (!pathPattern.match(f.value("field").toString()).hasMatch() ||
                (opName!="eq" && opName!="ne" && opName!="gt" && opName!="gte" && opName!="lt" && opName!="lte" && opName!="contains") ||
                !f.contains("value") || (!f.value("value").isString() && !f.value("value").isDouble() && !f.value("value").isBool()) ||
                (opName=="contains" && !f.value("value").isString())) {
                fail("Invalid search field, operator or value");return;
            }
        }
        if (!m_store.searchHot(args,result)) {dbFailed();return;}
        reply(result);return;
    }
    if (op == "recent") {
        const auto id=args.value("id").toString();
        if (!validName(id) ||
            (args.contains("limit") && !integer(args.value("limit"),1,100)) ||
            (args.contains("offset") && !integer(args.value("offset"),0,100000000)) ||
            (args.contains("lookbackSec") && !integer(args.value("lookbackSec"),1,86400*30)) ||
            (args.contains("fromMs") && !integer(args.value("fromMs"),0,9007199254740991.0)) ||
            (args.contains("toMs") && !integer(args.value("toMs"),0,9007199254740991.0))) {
            fail("Invalid recent query; limit=1..100, lookbackSec=1..2592000");return;
        }
        const qint64 now=QDateTime::currentMSecsSinceEpoch();
        const qint64 to=args.contains("toMs")?qint64(args.value("toMs").toDouble()):now;
        const qint64 from=args.contains("fromMs")?qint64(args.value("fromMs").toDouble()):
                           qMax(qint64(0),to-qint64(args.value("lookbackSec").toInt(1800))*1000);
        if (from>to) {fail("fromMs exceeds toMs");return;}
        QJsonObject query=args;
        query.insert("fromMs",double(from));query.insert("toMs",double(to));
        if (!m_store.recent(query,result)) {dbFailed();return;}
        reply(result);return;
    }
    if (op != "latest" && op != "history" && op != "delete" && op != "hydrate" && op != "recover") { fail("Unknown operation"); return; }
    const auto id = args.value("id").toString();
    if (!validName(id)) { fail("Valid id required (max 256 bytes)"); return; }
    if (op == "latest" || op == "hydrate" || op == "recover") {
        const bool ok=op=="latest"?m_store.latest(id,result):m_store.hydrate(id,result);
        if (!ok) {dbFailed();return;}
        reply(result);return;
    }
    if (op == "history") {
        if ((args.contains("limit") && !integer(args.value("limit"),1,1000)) ||
            (args.contains("fromMs") && !integer(args.value("fromMs"),0,9007199254740991.0)) ||
            (args.contains("toMs") && !integer(args.value("toMs"),-1,9007199254740991.0)) ||
            !cursor(args.value("afterSequence")) || !cursor(args.value("untilSequence"))) { fail("Invalid history bounds/cursor; limit must be 1..1000"); return; }
        const auto field = args.value("timeField").toString("timestamp");
        if (field != "timestamp" && field != "receivedAt") { fail("timeField must be timestamp or receivedAt"); return; }
        if (args.value("toMs").toDouble(-1) >= 0 && args.value("fromMs").toDouble() > args.value("toMs").toDouble()) { fail("fromMs exceeds toMs"); return; }
        if (!m_store.history(args, result)) { dbFailed(); return; } reply(result); return;
    }
    if (op == "delete") {
        if (args.contains("removeFromDisk") && !args.value("removeFromDisk").isBool()) { fail("removeFromDisk must be boolean"); return; }
        if (!m_store.remove(id,args.value("removeFromDisk").toBool(true),result)) {dbFailed();return;}
        reply(result);
    }
}

// A UDP client uses exactly the same application protocol as TCP. Unlike TCP,
// UDP requires application-level request retry, deduplication and fragmentation.
QString Server::udpKey(const QHostAddress& address, quint16 port) const {
    return address.toString() + "|" + QString::number(port);
}
void Server::sendUdp(const QHostAddress& address, quint16 port, const QJsonObject& message) {
    const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (bytes.size() > FrameLimit) {
        const auto tooLarge = QJsonDocument(QJsonObject{{"requestId", message.value("requestId")},
            {"ok", false}, {"code", "RESPONSE_TOO_LARGE"},
            {"error", "Use smaller history/search page size"}}).toJson(QJsonDocument::Compact);
        m_udpListener.writeDatagram(tooLarge,address,port);
        return;
    }
    if (bytes.size() <= m_udpMaxDatagramBytes) {
        m_udpListener.writeDatagram(bytes,address,port);
        return;
    }
    const QByteArray frameId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
    const int payloadSize = qMax(200, (m_udpMaxDatagramBytes-330)*3/4);
    const int count = (bytes.size()+payloadSize-1)/payloadSize;
    for (int i=0; i<count; ++i) {
        const auto chunk = bytes.mid(i*payloadSize,payloadSize);
        const QJsonObject frame{{"udpFrame", 1}, {"frameId", QString::fromLatin1(frameId)},
            {"index", i}, {"count", count}, {"payload", QString::fromLatin1(chunk.toBase64())}};
        const auto encoded = QJsonDocument(frame).toJson(QJsonDocument::Compact);
        if (encoded.size() <= m_udpMaxDatagramBytes) m_udpListener.writeDatagram(encoded,address,port);
    }
}
void Server::respond(QTcpSocket* socket, const QHostAddress& address, quint16 port, const QJsonObject& message) {
    if (socket) { write(socket,message); return; }
    const QString requestId = message.value("requestId").toString();
    if (!requestId.isEmpty()) {
        const QString key = udpKey(address,port) + "|" + requestId;
        m_udpPending.remove(key);
        if (m_udpCache.size() < 2048) m_udpCache.insert(key,{message,QDateTime::currentMSecsSinceEpoch()});
    }
    sendUdp(address,port,message);
}
void Server::housekeepingUdp() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it=m_udpLive.begin();it!=m_udpLive.end();) {
        auto& item=it.value();
        if (item.attempts>=5 || !m_udpPeers.contains(udpKey(item.address,item.port))) {
            it=m_udpLive.erase(it);continue;
        }
        if (now-item.sentAtMs>=1000) {
            sendUdp(item.address,item.port,item.event);
            item.sentAtMs=now;
            ++item.attempts;
        }
        ++it;
    }
    for (auto it = m_udpPeers.begin(); it != m_udpPeers.end();) {
        if (now - it.value().lastSeenMs > 60000) it=m_udpPeers.erase(it);
        else ++it;
    }
    for (auto it = m_udpAssemblies.begin(); it != m_udpAssemblies.end();) {
        if (now-it.value().startedMs > 12000) it=m_udpAssemblies.erase(it);
        else ++it;
    }
    for (auto it = m_udpCache.begin(); it != m_udpCache.end();) {
        if (now-it.value().storedMs > 120000) it=m_udpCache.erase(it);
        else ++it;
    }
    // Async requests are normally completed before the existing recovery timeout.
    // Clear only abandoned in-flight entries if their endpoint has expired.
    for (auto it = m_udpPending.begin(); it != m_udpPending.end();) {
        const QString key = *it;
        const QString endpoint = key.left(key.lastIndexOf('|'));
        if (!m_udpPeers.contains(endpoint)) it=m_udpPending.erase(it);
        else ++it;
    }
}
void Server::readUdp() {
    int processed=0;
    while (m_udpListener.hasPendingDatagrams() && processed++ < 64) {
        const qint64 size=m_udpListener.pendingDatagramSize();
        if (size <= 0 || size > m_udpMaxDatagramBytes) {
            QByteArray junk;
            junk.resize(int(qMin(qint64(65536),qMax(qint64(1),size))));
            m_udpListener.readDatagram(junk.data(),junk.size());
            continue;
        }
        QByteArray raw;
        raw.resize(int(size));
        QHostAddress address;
        quint16 port=0;
        if (m_udpListener.readDatagram(raw.data(),raw.size(),&address,&port) < 0) continue;
        QJsonParseError error;
        QJsonDocument document=QJsonDocument::fromJson(raw,&error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) continue;
        QJsonObject packet=document.object();
        if (packet.value("udpFrame").toInt()==1) {
            const QString frameId=packet.value("frameId").toString();
            const int index=packet.value("index").toInt(-1);
            const int count=packet.value("count").toInt(-1);
            if (!validName(frameId) || frameId.size()>64 || count<1 || count>2000 || index<0 || index>=count) continue;
            const QByteArray payload=QByteArray::fromBase64(packet.value("payload").toString().toLatin1());
            if (payload.isEmpty() || payload.size() > m_udpMaxDatagramBytes) continue;
            const QString key=udpKey(address,port)+"|"+frameId;
            if (!m_udpAssemblies.contains(key)) {
                if (m_udpAssemblies.size() >= 64) continue;
                UdpAssembly entry;
                entry.chunks.resize(count);
                entry.startedMs=QDateTime::currentMSecsSinceEpoch();
                m_udpAssemblies.insert(key,entry);
            }
            auto& entry=m_udpAssemblies[key];
            if (entry.chunks.size()!=count) { m_udpAssemblies.remove(key); continue; }
            if (entry.chunks[index].isEmpty()) {
                entry.chunks[index]=payload;
                entry.totalBytes+=payload.size();
                ++entry.received;
            }
            if (entry.totalBytes > FrameLimit) { m_udpAssemblies.remove(key); continue; }
            if (entry.received != count) continue;
            QByteArray joined;
            for (const auto& chunk:entry.chunks) joined+=chunk;
            m_udpAssemblies.remove(key);
            document=QJsonDocument::fromJson(joined,&error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) continue;
            packet=document.object();
        }
        const QString requestId=packet.value("requestId").toString();
        if (!validName(requestId) || !packet.value("args").isObject()) continue;
        // hello is always processed to refresh a peer's lease (even after expiry).
        // Other commands require an established UDP endpoint before replay-cache access.
        const bool isHello = packet.value("op").toString() == "hello";
        if (!isHello && !m_udpPeers.contains(udpKey(address,port))) continue;
        const QString key=udpKey(address,port)+"|"+requestId;
        if (!isHello && m_udpCache.contains(key)) {
            sendUdp(address,port,m_udpCache[key].response);
            continue;
        }
        if (m_udpPending.contains(key)) continue;
        m_udpPending.insert(key);
        handleRequest(nullptr,address,port,packet);
        // Requests that synchronously failed always get a cached response.
        // Deferred requests stay m_udpPending until finishWrite/failWrite.
    }
    if (m_udpListener.hasPendingDatagrams()) QTimer::singleShot(0,this,[this]{readUdp();});
}
