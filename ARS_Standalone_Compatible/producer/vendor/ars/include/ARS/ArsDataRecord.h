#pragma once

#include "ArsGlobal.h"
#include <QString>
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonDocument>
#include <QVariantMap>
#include <optional>

/**
 * @brief ساختار انعطاف‌پذیر رکورد داده (ArsDataRecord)
 * پشتیبانی از فیلدهای اختیاری (مختصات، سرعت، زاویه، پیلود و فیچرهای دلخواه)
 */
struct ARS_EXPORT ArsDataRecord
{
    QString                     id;              ///< شناسه یکتا یا گروهی هدف / سنسور (اجباری)
    qint64                      timestamp = 0;   ///< زمان ثبت داده بر حسب میلی‌ثانیه (UTC)
    
    // فیلدهای اختیاری موقعیت و حرکت (می‌توانند مقداردهی شوند یا خالی بمانند)
    std::optional<double>       latitude;        ///< عرض جغرافیایی (اختیاری)
    std::optional<double>       longitude;       ///< طول جغرافیایی (اختیاری)
    std::optional<double>       altitude;        ///< ارتفاع به متر (اختیاری)
    std::optional<double>       speed;           ///< سرعت به نات یا متر بر ثانیه (اختیاری)
    std::optional<double>       heading;         ///< سمت یا زاویه به درجه (اختیاری)

    QString                     source;          ///< منبع داده (اختیاری: مثل Radar, AIS, GPS, IFF)
    QByteArray                  payload;         ///< داده خام باینری یا متن آزاد (اختیاری)
    QVariantMap                 customFeatures;  ///< مپ فیچرهای دلخواه اضافی (مثلاً توان، فرکانس، دما و...)

    ArsDataRecord()
    {
        timestamp = QDateTime::currentMSecsSinceEpoch();
    }

    explicit ArsDataRecord(const QString& recordId)
        : id(recordId)
    {
        timestamp = QDateTime::currentMSecsSinceEpoch();
    }

    // متدهای کمکی برای ست‌کردن سریع و تمیز فیلدها (Fluent Interface)
    ArsDataRecord& setCoordinates(double lat, double lon, std::optional<double> alt = std::nullopt) {
        latitude = lat;
        longitude = lon;
        altitude = alt;
        return *this;
    }

    ArsDataRecord& setMotion(double spd, double hdg) {
        speed = spd;
        heading = hdg;
        return *this;
    }

    ArsDataRecord& addFeature(const QString& key, const QVariant& value) {
        customFeatures.insert(key, value);
        return *this;
    }

    ArsDataRecord& setPayload(const QByteArray& data) {
        payload = data;
        return *this;
    }

    ArsDataRecord& setSource(const QString& src) {
        source = src;
        return *this;
    }

    ArsDataRecord& setTime(qint64 ms) {
        timestamp = ms;
        return *this;
    }

    // سریالایز به JSON (برای ذخیره در ردیس یا انتقال روی شبکه)
    QJsonObject toJsonObject() const;
    static ArsDataRecord fromJsonObject(const QJsonObject& json);

    QByteArray toJson() const;
    static ArsDataRecord fromJson(const QByteArray& data);
};
