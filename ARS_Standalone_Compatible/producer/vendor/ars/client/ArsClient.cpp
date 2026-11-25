#include "ArsClient.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>
#include <QJsonParseError>
#include <QDebug>

ArsClient::ArsClient(QObject* parent) : QObject(parent) {
    connect(&m_socket, &QTcpSocket::connected, this, [this] {
        send("hello");
    });
    connect(&m_socket, &QTcpSocket::readyRead, this, &ArsClient::receive);
    connect(&m_udpSocket, &QUdpSocket::connected, this, [this] { send("hello"); });
    connect(&m_udpSocket, &QUdpSocket::readyRead, this, &ArsClient::receiveUdp);
    connect(&m_socket, &QTcpSocket::disconnected, this, [this] {
        m_ready = false;
        m_buffer.clear();
        failPending("Connection closed; a submitted write may already have committed");
        if (!m_manualDisconnect && m_transport == Transport::Tcp) emit disconnected();
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
#else
    connect(&m_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this, [this](QAbstractSocket::SocketError) {
#endif
        const QString target = QString("%1:%2").arg(m_host).arg(m_port);
        if (m_socket.error() == QAbstractSocket::ConnectionRefusedError) {
            emit connectionError("ARS connection refused at " + target +
                ". Start ars_server and wait for [READY]. Use ars_server --check on the server machine. "
                "Redis/PostgreSQL running does not mean the ARS port is open. Lab port=7093; default ARS port=7090.");
        } else emit connectionError("ARS " + target + ": " + m_socket.errorString());
    });
    m_timeoutTimer.setInterval(250);
    connect(&m_timeoutTimer, &QTimer::timeout, this, [this] {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        for (const auto& id : m_pending.keys()) {
            auto& pending = m_pending[id];
            if (m_transport == Transport::Udp && now >= pending.retryAt && now < pending.deadline) {
                writeUdpMessage(pending.bytes);
                pending.retries++;
                pending.retryAt = now + qMin(4000, 700 * (1 << qMin(3, pending.retries)));
            }
            if (pending.deadline <= now) {
                const auto p = m_pending.take(id);
                emit requestFailed(id, p.operation, "Request timeout; write outcome may be unknown");
            }
        }
        if (!m_manualDisconnect && m_transport == Transport::Udp) {
            // UDP has no socket-level connection state: a heartbeat renews the server's lease.
            if (m_ready && now - m_lastHeartbeatMs > 15000) {
                m_lastHeartbeatMs = now;
                send("heartbeat");
            }
            if (m_ready && now - m_lastReceivedMs > 45000) {
                m_ready = false;
                failPending("UDP heartbeat lost; trying to reconnect");
                emit disconnected();
            }
        }
        if (!m_manualDisconnect && !m_ready && now-m_lastConnectAttemptMs > 4000) {
            if (m_transport == Transport::Udp && m_udpSocket.state()==QAbstractSocket::ConnectedState) {
                m_lastConnectAttemptMs = now;
                bool hasHello = false;
                for (const auto& p : m_pending) if (p.operation=="hello") {hasHello=true;break;}
                if (!hasHello) send("hello");
            } else if (m_transport == Transport::Tcp && m_socket.state() == QAbstractSocket::UnconnectedState && !m_host.isEmpty()) {
                performConnection();
            }
        }
        for (auto it=m_assemblies.begin();it!=m_assemblies.end();) {
            if (now-it.value().startedMs>12000) it=m_assemblies.erase(it);
            else ++it;
        }
    });
    m_timeoutTimer.start();
}
ArsClient::~ArsClient() {
    // Disconnect callbacks before members used by them are destroyed.
    m_timeoutTimer.stop();
    QObject::disconnect(&m_socket, nullptr, this, nullptr);
    QObject::disconnect(&m_udpSocket, nullptr, this, nullptr);
    m_socket.abort();
    m_udpSocket.close();
}
void ArsClient::connectToServer(const QString& host, quint16 port, Transport transport) {
    m_manualDisconnect = false;
    m_ready = false;
    m_socket.abort();
    m_udpSocket.close();
    failPending("Reconnecting");
    m_buffer.clear();
    m_assemblies.clear();
    m_host = host;
    m_port = port;
    m_transport = transport;
    m_lastHeartbeatMs = QDateTime::currentMSecsSinceEpoch();
    m_lastReceivedMs = m_lastHeartbeatMs;
    performConnection();
}
void ArsClient::performConnection() {
    if (m_host.isEmpty() || !m_port || m_manualDisconnect) return;
    m_lastConnectAttemptMs=QDateTime::currentMSecsSinceEpoch();
    if (m_transport == Transport::Udp) m_udpSocket.connectToHost(m_host,m_port);
    else m_socket.connectToHost(m_host,m_port);
}
void ArsClient::disconnectFromServer() {
    if (m_transport==Transport::Udp && m_ready) send("disconnect");
    m_manualDisconnect = true;
    m_ready=false;
    m_socket.disconnectFromHost();
    m_udpSocket.close();
    failPending("Client disconnected");
}
void ArsClient::failPending(const QString& reason) {
    const auto pending = m_pending;
    m_pending.clear();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        emit requestFailed(it.key(), it.value().operation, reason);
}
void ArsClient::writeUdpMessage(const QByteArray& bytes) {
    if (m_udpSocket.state()!=QAbstractSocket::ConnectedState) return;
    const int limit=m_udpLimit;
    if (bytes.size()<=limit) { m_udpSocket.write(bytes); return; }
    const QString frameId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    const int chunkSize=qMax(180,(limit-330)*3/4);
    const int count=(bytes.size()+chunkSize-1)/chunkSize;
    for (int i=0; i<count; ++i) {
        const QJsonObject fragment{{"udpFrame",1},{"frameId",frameId},
            {"index",i},{"count",count},
            {"payload",QString::fromLatin1(bytes.mid(i*chunkSize,chunkSize).toBase64())}};
        m_udpSocket.write(QJsonDocument(fragment).toJson(QJsonDocument::Compact));
    }
}
QString ArsClient::send(const QString& operation, QJsonObject args) {
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto reject = [this, id, operation](const QString& error) {
        QTimer::singleShot(0, this, [this, id, operation, error] { emit requestFailed(id, operation, error); });
    };
    const bool connected = m_transport==Transport::Udp ? m_udpSocket.state()==QAbstractSocket::ConnectedState
                                                      : m_socket.state()==QAbstractSocket::ConnectedState;
    if ((!m_ready && operation != "hello") || !connected) {
        reject("Client is not ready/connected"); return id;
    }
    QJsonObject request{{"requestId", id}, {"op", operation}, {"args", args}};
    const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    if (bytes.size()>1024*1024 || m_pending.size()>=256 ||
        (m_transport==Transport::Tcp && m_socket.bytesToWrite()>4*1024*1024)) {
        reject("Client request/buffer limit reached"); return id;
    }
    const qint64 now=QDateTime::currentMSecsSinceEpoch();
    m_pending.insert(id,{operation,now+15000,now+700,0,bytes});
    if (m_transport == Transport::Udp) writeUdpMessage(bytes);
    else if (m_socket.write(bytes+'\n')<0) {m_pending.remove(id); reject(m_socket.errorString());}
    return id;
}
void ArsClient::processMessage(const QJsonObject& obj) {
    m_lastReceivedMs=QDateTime::currentMSecsSinceEpoch();
    if (obj.value("event").toString()=="record") {
        const QString channel=obj.value("channel").toString();
        const ArsDataRecord record=ArsDataRecord::fromJsonObject(obj.value("record").toObject());
        const QString eventId=obj.value("eventId").toString();
        bool duplicate=false;
        if (m_transport==Transport::Udp && !eventId.isEmpty()) {
            duplicate=m_liveIds.contains(eventId);
            if (!duplicate) {
                if (m_liveIds.size()>=4096) m_liveIds.clear();
                m_liveIds.insert(eventId);
            }
            send("ack_event",{{"eventId",eventId}});
        }
        if (!duplicate) {
            emit liveRecordReceived(channel,record);
            emit liveRecordDetailed(channel,record,obj.value("sequence").toString(),
                                    qint64(obj.value("receivedAtMs").toDouble()));
        }
        return;
    }
    const auto id=obj.value("requestId").toString();
    if (!m_pending.contains(id)) return;
    const auto pending=m_pending.take(id);
    if (!obj.value("ok").toBool()) {
        const QString error=obj.value("error").toString();
        emit requestFailed(id,pending.operation,error);
        emit requestFailedDetailed(id,pending.operation,error,obj.value("code").toString());
        if (pending.operation=="hello" && m_transport==Transport::Tcp) m_socket.disconnectFromHost();
    } else {
        if (pending.operation=="hello") {
            const QSet<QString> restore = m_subscriptions;
            m_ready=true;
            for (const QString& channel : restore) send("subscribe",{{"channel",channel}});
            emit ready();
        }
        if (pending.operation!="heartbeat" && pending.operation!="ack_event")
            emit responseReceived(id,pending.operation,obj.value("result").toObject());
    }
}
void ArsClient::receive() {
    m_buffer+=m_socket.readAll();
    while (true) {
        const int end=m_buffer.indexOf('\n');
        if (end<0) break;
        if (end>8*1024*1024) {m_socket.abort();return;}
        const auto line=m_buffer.left(end); m_buffer.remove(0,end+1);
        QJsonParseError error;
        const auto doc=QJsonDocument::fromJson(line,&error);
        if (error.error!=QJsonParseError::NoError || !doc.isObject()) {
            emit connectionError("Invalid server response");m_socket.abort();return;
        }
        processMessage(doc.object());
    }
    if (m_buffer.size()>8*1024*1024) m_socket.abort();
}
void ArsClient::receiveUdp() {
    while (m_udpSocket.hasPendingDatagrams()) {
        const qint64 size=m_udpSocket.pendingDatagramSize();
        if (size<1 || size>65507) {
            QByteArray ignored(65507,'\0');m_udpSocket.readDatagram(ignored.data(),ignored.size());continue;
        }
        QByteArray datagram(int(size),'\0');
        if (m_udpSocket.readDatagram(datagram.data(),datagram.size())<0) continue;
        QJsonParseError error;
        auto doc=QJsonDocument::fromJson(datagram,&error);
        if (error.error!=QJsonParseError::NoError || !doc.isObject()) continue;
        QJsonObject obj=doc.object();
        if (obj.value("udpFrame").toInt()==1) {
            const QString frameId=obj.value("frameId").toString();
            const int index=obj.value("index").toInt(-1);
            const int count=obj.value("count").toInt(-1);
            if (frameId.size()>64 || frameId.isEmpty() || count<1 || count>2000 || index<0 || index>=count) continue;
            const QByteArray chunk=QByteArray::fromBase64(obj.value("payload").toString().toLatin1());
            if (chunk.isEmpty() || chunk.size()>m_udpLimit) continue;
            if (!m_assemblies.contains(frameId)) {
                if (m_assemblies.size()>=32) continue;
                Assembly a;
                a.chunks.resize(count);
                a.startedMs=QDateTime::currentMSecsSinceEpoch();
                m_assemblies.insert(frameId,a);
            }
            auto& a=m_assemblies[frameId];
            if (a.chunks.size()!=count) {m_assemblies.remove(frameId);continue;}
            if (a.chunks[index].isEmpty()) {a.chunks[index]=chunk;a.totalBytes+=chunk.size();a.received++;}
            if (a.totalBytes>1024*1024) {m_assemblies.remove(frameId);continue;}
            if (a.received!=count) continue;
            QByteArray merged;
            for (const auto& c:a.chunks) merged+=c;
            m_assemblies.remove(frameId);
            doc=QJsonDocument::fromJson(merged,&error);
            if (error.error!=QJsonParseError::NoError || !doc.isObject()) continue;
            obj=doc.object();
        }
        processMessage(obj);
    }
}
QString ArsClient::saveRecord(const ArsDataRecord& r) { return send("save", {{"record", r.toJsonObject()}}); }
QString ArsClient::publishLive(const QString& c, const ArsDataRecord& r) { return send("publish", {{"channel", c}, {"record", r.toJsonObject()}}); }
QString ArsClient::getLatest(const QString& id) { return send("latest", {{"id", id}}); }
QString ArsClient::queryHistory(const QString& id, qint64 from, qint64 to, int limit, const QString& field, const QString& after, const QString& until) {
    return send("history", {{"id", id}, {"fromMs", double(from)}, {"toMs", double(to)}, {"limit", limit},
        {"timeField", field}, {"afterSequence", after}, {"untilSequence", until}});
}
QString ArsClient::subscribeLive(const QString& c) { m_subscriptions.insert(c); return send("subscribe", {{"channel", c}}); }
QString ArsClient::unsubscribeLive(const QString& c) { m_subscriptions.remove(c); return send("unsubscribe", {{"channel", c}}); }
QString ArsClient::deleteById(const QString& id, bool disk) { return send("delete", {{"id", id}, {"removeFromDisk", disk}}); }
QString ArsClient::hydrateToCache(const QString& id) { return send("hydrate", {{"id", id}}); }
QString ArsClient::recoverHistory(const QString& id) { return send("recover", {{"id", id}}); }
QString ArsClient::recoveryStatus(const QString& jobId) { return send("recovery_status", {{"jobId", jobId}}); }
QString ArsClient::status() { return send("status"); }

QString ArsClient::searchHot(const QString& mode, const QJsonArray& filters,
                             const QString& idContains, const QString& cursor,
                             int limit, int scanBudget) {
    return send("search_hot", {{"mode",mode},{"filters",filters},{"idContains",idContains},
                               {"cursor",cursor.isEmpty()?"0:0":cursor},
                               {"limit",limit},{"scanBudget",scanBudget}});
}
QString ArsClient::recentHistory(const QString& id, int lookbackSec, int limit) {
    return send("recent",{{"id",id},{"lookbackSec",lookbackSec},{"limit",limit},{"offset",0}});
}
QString ArsClient::recentHistoryPage(const QString& id,qint64 fromMs,qint64 toMs,
                                     qint64 offset,int limit) {
    return send("recent",{{"id",id},{"fromMs",double(fromMs)}, {"toMs",double(toMs)},
                           {"offset",double(offset)},{"limit",limit}});
}
QString ArsClient::recentHistoryToday(const QString& id, int limit) {
    const auto now = QDateTime::currentDateTime();
    const QDateTime midnight(now.date(), QTime(0, 0), now.timeSpec());
    return recentHistoryPage(id, midnight.toMSecsSinceEpoch(), now.toMSecsSinceEpoch(), 0, limit);
}
