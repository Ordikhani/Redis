#pragma once
#include <QTcpServer>
#include <QHostAddress>
#include <QUdpSocket>
#include <QTcpSocket>
#include <QSet>
#include <QHash>
#include <QList>
#include <QVector>
#include <QPointer>
#include <QTimer>
#include "HotColdStore.h"

class Server : public QObject {
    Q_OBJECT
public:
    explicit Server(QObject* parent = nullptr);
    ~Server() override;
    bool start();
private:
    struct Peer { QByteArray buffer; bool initialized = false; QSet<QString> channels; };
    void readPeer(QTcpSocket* socket);
    void readUdp();
    void sendUdp(const QHostAddress& sender, quint16 senderPort, const QJsonObject& message);
    void handleRequest(QTcpSocket* socket, const QHostAddress& sender, quint16 senderPort, const QJsonObject& request);
    void respond(QTcpSocket* socket, const QHostAddress& sender, quint16 senderPort, const QJsonObject& message);
    void housekeepingUdp();
    QString udpKey(const QHostAddress& address, quint16 port) const;
    void broadcastLive(const QString& channel, const QJsonObject& record, const QJsonObject& result);
    void write(QTcpSocket* socket, const QJsonObject& message);
    // A save/publish for a cold ID is held (not acknowledged) until the worker has restored
    // the ID's archive, then stored in arrival order and acknowledged. ok:true therefore
    // always means "stored", for old and new clients alike.
    struct DeferredWrite {
        QPointer<QTcpSocket> socket;
        bool udp = false;
        QHostAddress udpAddress;
        quint16 udpPort = 0; QString requestId; QString op; QString channel;
        QJsonObject record; qint64 receivedAtMs = 0; qint64 deadlineMs = 0;
    };
    struct DeferredId { QString jobId; QList<DeferredWrite> writes; };
    bool deferWrite(const QString& id, const DeferredWrite& write);
    void processDeferred();
    void finishWrite(const DeferredWrite& w, const QJsonObject& result);
    void failWrite(const DeferredWrite& w, const QString& error, const QString& code,
                   const QJsonObject& extra = QJsonObject());
    QTcpServer m_listener;
    QUdpSocket m_udpListener;
    bool m_udpEnabled = false;
    int m_udpMaxDatagramBytes = 1200;
    struct UdpPeer { QSet<QString> channels; qint64 lastSeenMs = 0; };
    struct UdpAssembly { QVector<QByteArray> chunks; int received = 0; int totalBytes = 0; qint64 startedMs = 0; };
    struct UdpCache { QJsonObject response; qint64 storedMs = 0; };
    struct UdpLiveEvent { QHostAddress address; quint16 port = 0; QJsonObject event;
                          qint64 sentAtMs = 0; int attempts = 0; };
    QHash<QString,UdpLiveEvent> m_udpLive;
    QHash<QString, UdpPeer> m_udpPeers;
    QHash<QString, UdpAssembly> m_udpAssemblies;
    QHash<QString, UdpCache> m_udpCache;
    QSet<QString> m_udpPending;
    QTimer m_udpCleanupTimer;
    QHash<QTcpSocket*, Peer> m_peers;
    HotColdStore m_store;
    int m_maxClients = 64;
    QHash<QString, DeferredId> m_deferred; // by record id; FIFO per id
    int m_deferredCount = 0;
    int m_recoveryAckTimeoutMs = 10000;
    QTimer m_deferredTimer;
    QTimer m_postgresTimer;
};
