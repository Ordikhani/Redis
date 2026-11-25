#pragma once
#include <QObject>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QSet>
#include <QVector>
#include <QTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <ARS/ArsDataRecord.h>

// Use in its owning Qt thread. No Redis/SQL or API-token dependency in clients.
class ArsClient : public QObject {
    Q_OBJECT
public:
    enum class Transport { Tcp, Udp };
    explicit ArsClient(QObject* parent = nullptr);
    ~ArsClient() override;
    void connectToServer(const QString& host, quint16 port, Transport transport = Transport::Tcp);
    void disconnectFromServer();
    bool isReady() const { return m_ready; }
    // Match [udp] maxDatagramBytes in server.ini; default 1200 avoids IP fragmentation.
    void setUdpDatagramSize(int bytes) { if (bytes>=512 && bytes<=4096) m_udpLimit=bytes; }
    QString saveRecord(const ArsDataRecord& record);
    QString publishLive(const QString& channel, const ArsDataRecord& record);
    QString getLatest(const QString& id);
    QString queryHistory(const QString& id, qint64 fromMs = 0, qint64 toMs = -1,
                         int limit = 100, const QString& timeField = "timestamp",
                         const QString& afterSequence = QString(), const QString& untilSequence = QString());
    QString subscribeLive(const QString& channel);
    QString unsubscribeLive(const QString& channel);
    QString deleteById(const QString& id, bool removeFromDisk = true);
    QString hydrateToCache(const QString& id); // Asynchronous: returns queued/completed status.
    QString recoverHistory(const QString& id); // Explicit alias for asynchronous cold-history recovery.
    QString recoveryStatus(const QString& jobId);
    QString status();
    // Search only hot (RAM) data. Cursor is returned in nextCursor.
    QString searchHot(const QString& mode = "latest", const QJsonArray& filters = {},
                      const QString& idContains = {}, const QString& cursor = {},
                      int limit = 20, int scanBudget = 120);
    // Recent history by SERVER reception time; cold IDs are queried from SQL.
    QString recentHistory(const QString& id, int lookbackSec = 1800, int limit = 50);
    // Follow a recent-history page using the returned fromMs/toMs/nextOffset.
    QString recentHistoryPage(const QString& id, qint64 fromMs, qint64 toMs,
                              qint64 offset, int limit = 50);
    // Calendar day in the CLIENT's local timezone; sent as an exact UTC epoch range.
    QString recentHistoryToday(const QString& id, int limit = 50);
signals:
    void ready();
    void disconnected();
    void responseReceived(QString requestId, QString operation, QJsonObject result);
    void requestFailed(QString requestId, QString operation, QString error);
    // Structured protocol failures expose machine-readable codes such as HOT_CAPACITY_REACHED.
    void requestFailedDetailed(QString requestId, QString operation, QString error, QString code);
    void liveRecordReceived(QString channel, ArsDataRecord record);
    // Optional richer event. Old liveRecordReceived remains unchanged for compatibility.
    void liveRecordDetailed(QString channel, ArsDataRecord record,
                            QString sequence, qint64 receivedAtMs);
    void connectionError(QString message);
private:
    struct Pending { QString operation; qint64 deadline; qint64 retryAt; int retries = 0; QByteArray bytes; };
    struct Assembly { QVector<QByteArray> chunks; int received = 0; int totalBytes = 0; qint64 startedMs = 0; };
    QString send(const QString& operation, QJsonObject args = QJsonObject());
    void receive();
    void receiveUdp();
    void processMessage(const QJsonObject& response);
    void writeUdpMessage(const QByteArray& bytes);
    void performConnection();
    void failPending(const QString& reason);
    QTcpSocket m_socket;
    QUdpSocket m_udpSocket;
    QHash<QString,Assembly> m_assemblies;
    QSet<QString> m_subscriptions;
    QSet<QString> m_liveIds;
    Transport m_transport = Transport::Tcp;
    int m_udpLimit = 1200;
    bool m_manualDisconnect = false;
    qint64 m_lastReceivedMs = 0;
    qint64 m_lastConnectAttemptMs = 0;
    qint64 m_lastHeartbeatMs = 0;
    QTimer m_timeoutTimer;
    QByteArray m_buffer;
    QHash<QString, Pending> m_pending;
    QString m_host;
    quint16 m_port = 0;
    bool m_ready = false;
};
