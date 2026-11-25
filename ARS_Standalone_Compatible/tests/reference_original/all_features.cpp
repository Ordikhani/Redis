#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>
#include "ArsClient.h"

// Sequential workflow: advance only after the server confirms each operation.
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    ArsClient client;
    const QString id = "demo_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString channel = "demo:" + id;
    bool liveReceived = false;
    int step = 0;
    QObject::connect(&client, &ArsClient::ready, [&] { client.subscribeLive(channel); });
    QObject::connect(&client, &ArsClient::liveRecordReceived,
        [&](const QString& c, const ArsDataRecord& r) {
            if (c == channel && r.id == id) liveReceived = true;
            qInfo().noquote() << "LIVE:" << r.toJson();
        });
    QObject::connect(&client, &ArsClient::responseReceived,
        [&](const QString&, const QString& op, const QJsonObject& result) {
            if (op == "auth") return;
            qInfo().noquote() << op << QJsonDocument(result).toJson(QJsonDocument::Compact);
            switch (step++) {
            case 0: {
                ArsDataRecord record(id);
                record.setCoordinates(35.7,51.4).setSource("AllFeaturesDemo")
                      .addFeature("temperature",28.5).setPayload(QByteArray::fromHex("0102a0"));
                client.publishLive(channel, record); break;
            }
            case 1: client.getLatest(id); break;
            case 2: client.queryHistory(id,0,-1,100,"receivedAt"); break;
            case 3: client.deleteById(id,false); break; // Evict only the cache.
            case 4: client.hydrateToCache(id); break;
            case 5: client.unsubscribeLive(channel); break;
            case 6: {
                ArsDataRecord record(id);
                record.addFeature("temperature",29.0);
                client.saveRecord(record); break; // Store without a live event.
            }
            case 7: client.deleteById(id,true); break; // Delete only this demo's unique ID.
            case 8: client.getLatest(id); break;
            case 9:
                if (result.value("found").toBool()) { qCritical() << "Delete verification failed"; app.exit(1); }
                else client.status();
                break;
            case 10:
                qInfo() << "Finished. Live event received:" << liveReceived;
                app.exit(liveReceived ? 0 : 1); break;
            }
        });
    QObject::connect(&client, &ArsClient::requestFailed,
        [&](const QString&, const QString& op, const QString& error) { qCritical() << op << error; app.exit(1); });
    QObject::connect(&client, &ArsClient::connectionError,
        [&](const QString& error) { qCritical() << error; app.exit(1); });
    QTimer::singleShot(30000, &app, [&] { qCritical() << "Demo timeout"; app.exit(1); });
    client.connectToServer(app.arguments().value(1,"127.0.0.1"), 7090, qEnvironmentVariable("ARS_API_TOKEN"));
    return app.exec();
}
