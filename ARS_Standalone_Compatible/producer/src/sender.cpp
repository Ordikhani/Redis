// A small, deterministic Qt console traffic generator for the ARS central service.
// Clients never need direct Redis or PostgreSQL credentials.
#include "ArsClient.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QTimer>

static QString utc(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC).toString(Qt::ISODateWithMs);
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ars_test_sender");
    QCommandLineParser parser;
    parser.setApplicationDescription("Generate repeated, inactive, resumed and delayed ARS IDs");
    parser.addHelpOption();
    parser.addOption(QCommandLineOption("host", "ARS server address", "address", "127.0.0.1"));
    parser.addOption(QCommandLineOption("port", "ARS server TCP or UDP port", "number", "7090"));
    parser.addOption(QCommandLineOption("transport", "Transport: tcp or udp", "mode", "tcp"));
    parser.addOption(QCommandLineOption("udp-mtu", "Maximum UDP datagram size (match compiled server UDP setting)", "bytes", "1200"));
    parser.addOption(QCommandLineOption("channel", "Live event channel", "name", "ars:test:live"));
    parser.addOption(QCommandLineOption("prefix", "ID prefix", "text", "DEMO"));
    parser.addOption(QCommandLineOption("interval-ms", "Time between ticks", "ms", "1000"));
    parser.addOption(QCommandLineOption("pause-sec", "How long COLD ID is silent", "seconds", "145"));
    parser.addOption(QCommandLineOption("after-resume-sec", "Send more data after COLD returns", "seconds", "20"));
    parser.process(app);

    bool okPort=false, okInterval=false, okPause=false, okAfter=false;
    const int port=(parser.value("transport").trimmed().toLower()=="udp" &&
                     !parser.isSet("port")) ? 7092 : parser.value("port").toInt(&okPort);
    if (port==7092 && !parser.isSet("port") && parser.value("transport").trimmed().toLower()=="udp") okPort=true;
    const int interval=parser.value("interval-ms").toInt(&okInterval);
    const int pause=parser.value("pause-sec").toInt(&okPause);
    const int after=parser.value("after-resume-sec").toInt(&okAfter);
    if (!okPort || port<1 || port>65535 || !okInterval || interval<100 || interval>60000 ||
        !okPause || pause<1 || pause>86400 || !okAfter || after<1 || after>86400 ||
        parser.value("prefix").trimmed().isEmpty()) {
        qCritical() << "Invalid options; run with --help"; return 2;
    }
    const QString mode=parser.value("transport").trimmed().toLower();
    if (mode!="tcp" && mode!="udp") { qCritical()<<"--transport must be tcp or udp";return 2; }
    const ArsClient::Transport transport=mode=="udp" ? ArsClient::Transport::Udp : ArsClient::Transport::Tcp;
    const QString channel=parser.value("channel");
    const QString base=parser.value("prefix");
    const QString activeId=base+"-HOT-001";
    const QString coldId=base+"-COLD-001";
    const QString repeatedId=base+"-REPEAT-001";
    const QString lateId=base+"-LATE-001";
    const QString removableId=base+"-DELETE-001";
    const QString silentId=base+"-SILENT-001";

    qInfo().noquote() << "[SENDER] stream=" << channel << "IDs="
                      << activeId << coldId << repeatedId << lateId << removableId << silentId;
    qInfo() << "[SENDER] cold pause seconds:" << pause << "tick interval(ms):" << interval;

    ArsClient client;
    bool okMtu=false;
    const int mtu=parser.value("udp-mtu").toInt(&okMtu);
    if (!okMtu || mtu<512 || mtu>4096) {qCritical()<<"UDP MTU must be 512..4096"; return 2;}
    client.setUdpDatagramSize(mtu);
    QTimer timer;
    timer.setInterval(interval);
    int tick=0;
    qint64 pauseStarted=-1;
    qint64 resumeStarted=-1;
    int acknowledged=0;
    int failed=0;

    QObject::connect(&client,&ArsClient::connectionError,&app,[&](const QString& error){
        qWarning().noquote() << "[SENDER][CONNECTION_ERROR]" << error;
    });
    QObject::connect(&client,&ArsClient::disconnected,&app,[&]{
        timer.stop(); qWarning() << "[SENDER] Disconnected; retrying";
    });
    QObject::connect(&client,&ArsClient::requestFailedDetailed,&app,
                     [&](const QString&,const QString& op,const QString& error,const QString& code){
        ++failed;
        // RECOVERY_PENDING / RECOVERY_FAILED / HOT_CAPACITY_REACHED mean "nothing was stored".
        qWarning().noquote() << "[SENDER][FAILED] operation=" << op
                             << "code=" << (code.isEmpty()?QStringLiteral("-"):code) << "error=" << error;
    });
    QObject::connect(&client,&ArsClient::responseReceived,&app,
                     [&](const QString&,const QString& op,const QJsonObject& result){
        if (op=="publish" || op=="save") {
            ++acknowledged;
            if (result.value("restoredFromPostgres").toBool())
                qInfo().noquote() << "[SENDER][RESTORE_CONFIRMED] historyRecords="
                                  << result.value("restoredHistoryCount").toInt()
                                  << "newSequence=" << result.value("sequence").toString()
                                  << "heldForRecoveryMs=" << qint64(result.value("heldForRecoveryMs").toDouble());
            qInfo().noquote() << "[SENDER][ACK] seq=" << result.value("sequence").toString()
                              << "receivedAt=" << utc(qint64(result.value("receivedAtMs").toDouble()))
                              << "storage=" << result.value("storage").toString()
                              << "checkpointed=" << result.value("checkpointed").toBool();
        } else if (op=="status") {
            qInfo().noquote() << "[SENDER][STATUS]" << QString::fromUtf8(
                QJsonDocument(result).toJson(QJsonDocument::Compact));
            const int idle=result.value("inactiveAfterSec").toInt();
            if (idle>0 && pause<=idle+2)
                qWarning() << "[SENDER] Warning: pause-sec must exceed inactiveAfterSec to test archiving";
            if (!result.value("hotColdEnabled").toBool())
                qWarning() << "[SENDER] Redis hot mode is OFF; lifecycle test cannot pass";
        }
    });
    auto publish=[&](const QString& id,const QString& kind,bool oldTimestamp=false){
        ArsDataRecord r(id);
        r.timestamp=QDateTime::currentMSecsSinceEpoch()-(oldTimestamp?300000:0);
        r.setCoordinates(35.7000+(tick%30)*0.0001,51.4000+(tick%30)*0.0001)
         .setMotion(12.0+(tick%7),double(tick%360))
         .setSource("ARSTestSender")
         .addFeature("scenario",kind)
         .addFeature("tick",tick)
         .addFeature("temperature",25.0+(tick%10)*0.5)
         .setPayload(QByteArray("sample-data"));
        client.publishLive(channel,r);
        qInfo().noquote() << "[SENDER][SEND] id=" << id << "kind=" << kind
                          << "eventAt=" << utc(r.timestamp);
    };
    QObject::connect(&timer,&QTimer::timeout,&app,[&]{
        ++tick;
        const qint64 now=QDateTime::currentMSecsSinceEpoch();
        publish(activeId,"always-active");
        if (tick%3==0) publish(repeatedId,"duplicate-id");
        if (tick%5==0) publish(lateId,"old-event-time",true);
        if (tick<=3) publish(removableId,"test-delete");
        if (tick%7==0) {
            ArsDataRecord silent(silentId);
            silent.setSource("ARSTestSender").addFeature("mode","stored-without-live-event")
                  .addFeature("tick",tick);
            client.saveRecord(silent);
            qInfo().noquote() << "[SENDER][SAVE_ONLY] id=" << silentId
                              << "(available to history but NOT pushed to subscribers)";
        }
        if (tick<=5) publish(coldId,"before-silence");
        if (tick==6) {
            pauseStarted=now;
            qInfo().noquote() << "[SENDER][PAUSED] id=" << coldId
                              << "silenceSeconds=" << pause
                              << "(it should move from Redis to PostgreSQL)";
        }
        if (pauseStarted>=0 && resumeStarted<0 && now-pauseStarted>=qint64(pause)*1000) {
            resumeStarted=now;
            qInfo().noquote() << "[SENDER][RESUMED] id=" << coldId
                              << "(archive should hydrate to Redis before this new record)";
            publish(coldId,"reactivated-after-archive");
        } else if (resumeStarted>=0 && tick%2==0) {
            publish(coldId,"active-again");
        }
        if (resumeStarted>=0 && now-resumeStarted>=qint64(after)*1000) {
            timer.stop();
            qInfo().noquote() << "[SENDER][DONE] ticks=" << tick << "ACK=" << acknowledged
                              << "FAIL=" << failed << "(data intentionally kept for queries)";
            QTimer::singleShot(1000,&app,[&]{app.exit(failed?1:0);});
        }
    });
    QObject::connect(&client,&ArsClient::ready,&app,[&]{
        qInfo() << "[SENDER] Connected. Starting stream";
        client.status();
        timer.start();
    });
    QTimer::singleShot(12000,&app,[&]{
        if (!client.isReady()) {
            qCritical() << "[CONNECTION_TIMEOUT] Could not connect to ARS server";
            app.exit(1);
        }
    });
    client.connectToServer(parser.value("host"),quint16(port),transport);
    return app.exec();
}
