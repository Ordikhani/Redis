#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include "ARS/ArsDataRecord.h"

/**
 * @brief پاسخ پارس‌شده از سرور ردیس
 */
struct ArsRedisReply {
    enum Type { Nil, String, Integer, Array, Error };
    Type type = Nil;
    QByteArray str;
    qint64 integer = 0;
    QList<ArsRedisReply> elements;
    QString error;

    bool isOk() const { return type != Error; }
};

/**
 * @brief کلاینت بومی و سبک ردیس مبتنی بر سوکت‌های کیوت (بدون نیاز به کتابخانه‌های خارجی)
 * پیاده‌سازی پروتکل استاندارد RESP با پشتیبانی از دستورات، استریم‌ها و Pub/Sub
 */
class ArsRedisClient : public QObject
{
    Q_OBJECT
public:
    explicit ArsRedisClient(QObject* parent = nullptr);
    ~ArsRedisClient() override;

    bool connectToHost(const QString& host, quint16 port, const QString& password = "", int db = 0);
    void disconnectFromHost();
    bool isConnected() const;

    // اجرای دستور همگام (Synchronous execution with event loop)
    ArsRedisReply sendCommand(const QList<QByteArray>& args, int timeoutMs = 3000);

    // توابع کاربردی سطح بالا برای ساختارهای ردیس
    bool hsetRecord(const QString& key, const ArsDataRecord& record);
    ArsDataRecord hgetRecord(const QString& key, bool* found = nullptr);

    // افزودن به استریم با سقف حافظه (RAM Cap via MAXLEN ~ N)
    QString xaddRecord(const QString& streamKey, const ArsDataRecord& record, int maxLen = 500);

    // دریافت تاریخچه از استریم
    QList<ArsDataRecord> xrangeRecords(const QString& streamKey, qint64 fromMs, qint64 toMs, int count = 500);

    // ردگیری زمان آخرین فعالیت (برای جارو کردن رم)
    bool updateLastSeen(const QString& zsetKey, const QString& id, qint64 timestamp);
    QStringList getInactiveIds(const QString& zsetKey, qint64 beforeTimestamp);
    bool removeId(const QString& keyPrefix, const QString& id);

    // ارتباط زنده Pub/Sub
    bool publish(const QString& channel, const QByteArray& message);

    QString lastError() const { return m_lastError; }

signals:
    void connected();
    void disconnected();
    void errorOccurred(QString err);

private:
    QByteArray formatCommand(const QList<QByteArray>& args);
    ArsRedisReply parseReply(QByteArray& buffer);

    QTcpSocket m_socket;
    QString m_lastError;
    bool m_authenticated = false;
};
