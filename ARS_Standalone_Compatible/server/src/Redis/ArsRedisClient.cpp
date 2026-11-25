#include "ArsRedisClient.h"
#include <QEventLoop>
#include <QDateTime>
#include <QDebug>

ArsRedisClient::ArsRedisClient(QObject* parent)
    : QObject(parent)
{
}

ArsRedisClient::~ArsRedisClient()
{
    disconnectFromHost();
}

bool ArsRedisClient::connectToHost(const QString& host, quint16 port, const QString& password, int db)
{
    if (m_socket.state() == QAbstractSocket::ConnectedState)
        disconnectFromHost();

    m_socket.connectToHost(host, port);
    if (!m_socket.waitForConnected(3000)) {
        m_lastError = m_socket.errorString();
        return false;
    }

    // ارسال رمز عبور در صورت وجود
    if (!password.isEmpty()) {
        auto reply = sendCommand({"AUTH", password.toUtf8()});
        if (!reply.isOk()) {
            m_lastError = "Auth failed: " + reply.error;
            m_socket.abort();
            return false;
        }
    }

    // انتخاب دیتابیس در صورت نیاز
    if (db > 0) {
        auto reply = sendCommand({"SELECT", QByteArray::number(db)});
        if (!reply.isOk()) {
            m_lastError = "Select DB failed: " + reply.error;
            m_socket.abort();
            return false;
        }
    }

    return true;
}

void ArsRedisClient::disconnectFromHost()
{
    // A timed-out RESP command may have a late reply buffered in QTcpSocket.
    // Graceful disconnect can leave the socket alive while queued bytes drain;
    // abort() closes immediately and discards all unread/late response bytes so
    // the next connection can never consume a reply belonging to an old command.
    if (m_socket.state() != QAbstractSocket::UnconnectedState)
        m_socket.abort();
}

bool ArsRedisClient::isConnected() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

QByteArray ArsRedisClient::formatCommand(const QList<QByteArray>& args)
{
    QByteArray cmd;
    cmd.append("*" + QByteArray::number(args.size()) + "\r\n");
    for (const auto& arg : args) {
        cmd.append("$" + QByteArray::number(arg.size()) + "\r\n");
        cmd.append(arg);
        cmd.append("\r\n");
    }
    return cmd;
}

ArsRedisReply ArsRedisClient::sendCommand(const QList<QByteArray>& args, int timeoutMs)
{
    ArsRedisReply reply;
    if (!isConnected()) {
        reply.type = ArsRedisReply::Error;
        reply.error = "Socket not connected";
        return reply;
    }

    const QByteArray cmd = formatCommand(args);
    if (m_socket.write(cmd) == -1) {
        reply.type = ArsRedisReply::Error;
        reply.error = m_socket.errorString();
        return reply;
    }
    m_socket.flush();

    QByteArray buffer;
    qint64 startTime = QDateTime::currentMSecsSinceEpoch();

    while (true) {
        if (m_socket.bytesAvailable() > 0) {
            buffer.append(m_socket.readAll());
            QByteArray parseBuf = buffer;
            reply = parseReply(parseBuf);
            if (reply.type != ArsRedisReply::Nil || parseBuf.isEmpty()) {
                if (reply.type == ArsRedisReply::Error && reply.error == "INCOMPLETE") {
                    // ادامه خواندن تا تکمیل پاسخ
                } else {
                    return reply;
                }
            }
        }

        if (QDateTime::currentMSecsSinceEpoch() - startTime > timeoutMs) {
            reply.type = ArsRedisReply::Error;
            reply.error = "Command timeout (" + QString::number(timeoutMs) + " ms)";
            return reply;
        }

        if (!m_socket.waitForReadyRead(50)) {
            if (m_socket.state() != QAbstractSocket::ConnectedState) {
                reply.type = ArsRedisReply::Error;
                reply.error = "Connection lost during command";
                return reply;
            }
        }
    }
}

ArsRedisReply ArsRedisClient::parseReply(QByteArray& buffer)
{
    ArsRedisReply reply;
    if (buffer.isEmpty()) {
        reply.type = ArsRedisReply::Error;
        reply.error = "INCOMPLETE";
        return reply;
    }

    char prefix = buffer.at(0);
    int crlfPos = buffer.indexOf("\r\n");
    if (crlfPos < 0) {
        reply.type = ArsRedisReply::Error;
        reply.error = "INCOMPLETE";
        return reply;
    }

    QByteArray line = buffer.mid(1, crlfPos - 1);
    buffer.remove(0, crlfPos + 2);

    switch (prefix) {
    case '+': // Simple String
        reply.type = ArsRedisReply::String;
        reply.str = line;
        break;
    case '-': // Error
        reply.type = ArsRedisReply::Error;
        reply.error = QString::fromUtf8(line);
        break;
    case ':': // Integer
        reply.type = ArsRedisReply::Integer;
        reply.integer = line.toLongLong();
        break;
    case '$': { // Bulk String
        qint64 len = line.toLongLong();
        if (len == -1) {
            reply.type = ArsRedisReply::Nil;
        } else {
            if (buffer.size() < len + 2) {
                reply.type = ArsRedisReply::Error;
                reply.error = "INCOMPLETE";
                return reply;
            }
            reply.type = ArsRedisReply::String;
            reply.str = buffer.left(len);
            buffer.remove(0, len + 2); // محتوا + \r\n
        }
        break;
    }
    case '*': { // Array
        qint64 count = line.toLongLong();
        if (count == -1) {
            reply.type = ArsRedisReply::Nil;
        } else {
            reply.type = ArsRedisReply::Array;
            for (qint64 i = 0; i < count; ++i) {
                ArsRedisReply elem = parseReply(buffer);
                if (elem.type == ArsRedisReply::Error && elem.error == "INCOMPLETE") {
                    reply.type = ArsRedisReply::Error;
                    reply.error = "INCOMPLETE";
                    return reply;
                }
                reply.elements.append(elem);
            }
        }
        break;
    }
    default:
        reply.type = ArsRedisReply::Error;
        reply.error = "Unknown RESP prefix";
        break;
    }

    return reply;
}

bool ArsRedisClient::hsetRecord(const QString& key, const ArsDataRecord& record)
{
    const QByteArray data = record.toJson();
    auto reply = sendCommand({"HSET", key.toUtf8(), "data", data, "timestamp", QByteArray::number(record.timestamp)});
    return reply.isOk();
}

ArsDataRecord ArsRedisClient::hgetRecord(const QString& key, bool* found)
{
    if (found) *found = false;
    auto reply = sendCommand({"HGET", key.toUtf8(), "data"});
    if (reply.type == ArsRedisReply::String && !reply.str.isEmpty()) {
        if (found) *found = true;
        return ArsDataRecord::fromJson(reply.str);
    }
    return ArsDataRecord();
}

QString ArsRedisClient::xaddRecord(const QString& streamKey, const ArsDataRecord& record, int maxLen)
{
    // نکته حیاتی بهینه‌سازی رم: استفاده از MAXLEN ~ جهت جلوگیری از رشد بی‌رویه استریم در رم
    QList<QByteArray> args = {
        "XADD",
        streamKey.toUtf8(),
        "MAXLEN",
        "~",
        QByteArray::number(maxLen),
        "*", // تولید خودکار ID استریم بر اساس زمان ردیس
        "data",
        record.toJson()
    };

    auto reply = sendCommand(args);
    if (reply.type == ArsRedisReply::String) {
        return QString::fromUtf8(reply.str);
    }
    return QString();
}

QList<ArsDataRecord> ArsRedisClient::xrangeRecords(const QString& streamKey, qint64 fromMs, qint64 toMs, int count)
{
    QList<ArsDataRecord> list;
    QByteArray startId = (fromMs > 0) ? (QByteArray::number(fromMs) + "-0") : QByteArray("-");
    QByteArray endId   = (toMs > 0)   ? (QByteArray::number(toMs) + "-18446744073709551615") : QByteArray("+");

    QList<QByteArray> args = {
        "XRANGE",
        streamKey.toUtf8(),
        startId,
        endId,
        "COUNT",
        QByteArray::number(count)
    };

    auto reply = sendCommand(args);
    if (reply.type == ArsRedisReply::Array) {
        for (const auto& entry : reply.elements) {
            // ساختار entry در ردیس استریم: [id, [field1, val1, ...]]
            if (entry.type == ArsRedisReply::Array && entry.elements.size() >= 2) {
                const auto& fields = entry.elements[1];
                if (fields.type == ArsRedisReply::Array) {
                    for (int i = 0; i < fields.elements.size() - 1; i += 2) {
                        if (fields.elements[i].str == "data") {
                            list.append(ArsDataRecord::fromJson(fields.elements[i + 1].str));
                            break;
                        }
                    }
                }
            }
        }
    }
    return list;
}

bool ArsRedisClient::updateLastSeen(const QString& zsetKey, const QString& id, qint64 timestamp)
{
    auto reply = sendCommand({"ZADD", zsetKey.toUtf8(), QByteArray::number(timestamp), id.toUtf8()});
    return reply.isOk();
}

QStringList ArsRedisClient::getInactiveIds(const QString& zsetKey, qint64 beforeTimestamp)
{
    QStringList ids;
    auto reply = sendCommand({"ZRANGEBYSCORE", zsetKey.toUtf8(), "-inf", QByteArray::number(beforeTimestamp)});
    if (reply.type == ArsRedisReply::Array) {
        for (const auto& elem : reply.elements) {
            if (elem.type == ArsRedisReply::String)
                ids.append(QString::fromUtf8(elem.str));
        }
    }
    return ids;
}

bool ArsRedisClient::removeId(const QString& keyPrefix, const QString& id)
{
    const QString latestKey  = QString("%1:latest:%2").arg(keyPrefix, id);
    const QString historyKey = QString("%1:history:%2").arg(keyPrefix, id);
    const QString zsetKey    = QString("%1:last_seen").arg(keyPrefix);

    sendCommand({"DEL", latestKey.toUtf8(), historyKey.toUtf8()});
    sendCommand({"ZREM", zsetKey.toUtf8(), id.toUtf8()});
    return true;
}

bool ArsRedisClient::publish(const QString& channel, const QByteArray& message)
{
    auto reply = sendCommand({"PUBLISH", channel.toUtf8(), message});
    return reply.isOk();
}
