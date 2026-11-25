#include "ARS/ArsDataRecord.h"

QJsonObject ArsDataRecord::toJsonObject() const
{
    QJsonObject obj;
    obj["id"] = id;
    // UTC milliseconds fit exactly in IEEE-754 double for supported timestamps.
    // The explicit cast also avoids a Qt 5 qint64 -> QJsonValue overload ambiguity.
    obj["timestamp"] = double(timestamp);

    if (latitude.has_value())
        obj["latitude"] = *latitude;
    if (longitude.has_value())
        obj["longitude"] = *longitude;
    if (altitude.has_value())
        obj["altitude"] = *altitude;
    if (speed.has_value())
        obj["speed"] = *speed;
    if (heading.has_value())
        obj["heading"] = *heading;

    if (!source.isEmpty())
        obj["source"] = source;

    if (!payload.isEmpty())
        obj["payload"] = QString::fromLatin1(payload.toBase64());

    if (!customFeatures.isEmpty())
        obj["features"] = QJsonObject::fromVariantMap(customFeatures);

    return obj;
}

ArsDataRecord ArsDataRecord::fromJsonObject(const QJsonObject& json)
{
    ArsDataRecord r;
    r.id = json.value("id").toString();
    r.timestamp = json.value("timestamp").toVariant().toLongLong();
    if (r.timestamp == 0)
        r.timestamp = QDateTime::currentMSecsSinceEpoch();

    if (json.contains("latitude"))
        r.latitude = json.value("latitude").toDouble();
    if (json.contains("longitude"))
        r.longitude = json.value("longitude").toDouble();
    if (json.contains("altitude"))
        r.altitude = json.value("altitude").toDouble();
    if (json.contains("speed"))
        r.speed = json.value("speed").toDouble();
    if (json.contains("heading"))
        r.heading = json.value("heading").toDouble();

    if (json.contains("source"))
        r.source = json.value("source").toString();

    if (json.contains("payload")) {
        const QString b64 = json.value("payload").toString();
        r.payload = QByteArray::fromBase64(b64.toLatin1());
    }

    if (json.contains("features") && json.value("features").isObject()) {
        r.customFeatures = json.value("features").toObject().toVariantMap();
    }

    return r;
}

QByteArray ArsDataRecord::toJson() const
{
    return QJsonDocument(toJsonObject()).toJson(QJsonDocument::Compact);
}

ArsDataRecord ArsDataRecord::fromJson(const QByteArray& data)
{
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return ArsDataRecord();
    return fromJsonObject(doc.object());
}
