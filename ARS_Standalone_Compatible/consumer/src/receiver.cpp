// Interactive read-only/live/history/delete console for the ARS central protocol.
// Linux/POSIX stdin is handled by QSocketNotifier without blocking incoming events.
#include "ArsClient.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDateTime>
#include <QDate>
#include <QTime>
#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QStringList>
#include <QTimer>
#include <unistd.h>

struct Page {
    QString after;
    QString until;
    qint64 fromMs=0;
    QString timeField="receivedAt";
    int limit=100;
};
struct RecentPage { qint64 fromMs=0; qint64 toMs=0; qint64 offset=0; int limit=50; bool more=false; };
struct SearchPage { QString mode="latest"; QString idContains; QJsonArray filters; QString cursor="0:0"; bool more=false; };
static int durationSeconds(const QString& value) {
    if (value=="30m") return 1800;
    if (value=="1h") return 3600;
    if (value=="2h") return 7200;
    if (value=="24h") return 86400;
    if (value.endsWith('m')) return value.left(value.size()-1).toInt()*60;
    if (value.endsWith('h')) return value.left(value.size()-1).toInt()*3600;
    if (value.endsWith('s')) return value.left(value.size()-1).toInt();
    return -1;
}
static QString utc(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms,Qt::UTC).toString(Qt::ISODateWithMs);
}
static void help() {
    qInfo().noquote() << "\nCOMMANDS:\n"
                      "  history ID [limit]  - full history, legacy sequence order\n"
                      "  recent ID [30m|1h|2h|24h] - time-indexed recent history\n"
                      "  recentnext ID       - next recent-history page\n"
                      "  today ID            - history since local midnight\n"
                      "  hot [latest|records] [id_part] - search all hot RAM IDs / rows\n"
                      "  filter MODE FIELD OP VALUE - filter RAM (eq,gt,gte,lt,lte,contains)\n"
                      "  hotnext             - next hot search page\n"
                      "  next ID             - next history page\n"
                      "  last5 ID            - received in last five minutes\n"
                      "  latest ID           - most recent record (by event timestamp)\n"
                      "  delete ID           - permanently delete this ID from Redis AND PostgreSQL\n"
                      "  evict ID            - remove from Redis after archiving; keep PostgreSQL\n"
                      "  hydrate ID          - load full ID history from PostgreSQL to Redis\n"
                      "  sub CHANNEL         - subscribe to another live channel\n"
                      "  unsub CHANNEL       - unsubscribe from a live channel\n"
                      "  status              - service status\n"
                      "  help, quit\n";
}
int main(int argc, char* argv[]) {
    QCoreApplication app(argc,argv);
    QCoreApplication::setApplicationName("ars_test_receiver");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(QCommandLineOption("host","ARS server address","address","127.0.0.1"));
    parser.addOption(QCommandLineOption("port","ARS server TCP or UDP port","number","7090"));
    parser.addOption(QCommandLineOption("transport", "Transport: tcp or udp", "mode", "tcp"));
    parser.addOption(QCommandLineOption("udp-mtu", "Maximum UDP datagram size (match compiled server UDP setting)", "bytes", "1200"));
    parser.addOption(QCommandLineOption("channel","Live event channel","name","ars:test:live"));
    parser.process(app);
    bool validPort=false;
    const int port=(parser.value("transport").trimmed().toLower()=="udp" &&
                     !parser.isSet("port")) ? 7092 : parser.value("port").toInt(&validPort);
    if (port==7092 && !parser.isSet("port") && parser.value("transport").trimmed().toLower()=="udp") validPort=true;
    if (!validPort || port<1 || port>65535) {
        qCritical() << "Invalid --port";
        return 2;
    }
    const QString mode=parser.value("transport").trimmed().toLower();
    if (mode!="tcp" && mode!="udp") { qCritical()<<"--transport must be tcp or udp";return 2; }
    const ArsClient::Transport transport=mode=="udp" ? ArsClient::Transport::Udp : ArsClient::Transport::Tcp;
    const QString channel=parser.value("channel");
    ArsClient client;
    bool okMtu=false;
    const int mtu=parser.value("udp-mtu").toInt(&okMtu);
    if (!okMtu || mtu<512 || mtu>4096) {qCritical()<<"UDP MTU must be 512..4096"; return 2;}
    client.setUdpDatagramSize(mtu);
    QHash<QString,Page> pages;
    QHash<QString,QString> requestToId;
    QHash<QString,RecentPage> recentPages;
    SearchPage searchPage;
    QObject::connect(&client,&ArsClient::liveRecordDetailed,&app,
                     [&](const QString& ch,const ArsDataRecord& r,const QString& seq,qint64 received){
        qInfo().noquote() << "[LIVE] channel=" << ch << "id=" << r.id
                          << "seq=" << seq
                          << "receivedAt=" << utc(received)
                          << "eventAt=" << utc(r.timestamp)
                          << "record=" << QString::fromUtf8(r.toJson());
    });
    QObject::connect(&client,&ArsClient::responseReceived,&app,
                     [&](const QString& request,const QString& op,const QJsonObject& result){
        if (op=="history") {
            const QString id=requestToId.take(request);
            const auto items=result.value("items").toArray();
            qInfo().noquote() << "[HISTORY] id=" << id << "source="
                              << result.value("source").toString() << "items=" << items.size();
            for (const auto& value:items) {
                const QJsonObject item=value.toObject();
                const auto record=item.value("record").toObject();
                qInfo().noquote() << "  id=" << record.value("id").toString()
                                  << "seq=" << item.value("sequence").toString()
                                  << "receivedAt=" << utc(qint64(item.value("receivedAtMs").toDouble()))
                                  << "eventAt=" << utc(qint64(record.value("timestamp").toDouble()))
                                  << "record=" << QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact));
            }
            if (!id.isEmpty()) {
                if (result.value("hasMore").toBool()) {
                    Page page=pages.value(id);
                    page.after=result.value("nextSequence").toString();
                    page.until=result.value("untilSequence").toString();
                    pages.insert(id,page);
                    qInfo().noquote() << "[HISTORY] More items available: next" << id;
                } else {
                    pages.remove(id);
                    qInfo() << "[HISTORY] End of history";
                }
            }
        } else if (op=="recent") {
            const QString id=requestToId.take(request);
            const auto items=result.value("items").toArray();
            qInfo().noquote() << "[RECENT] id=" << id << "source=" << result.value("source").toString()
                              << "indexed=" << result.value("indexed").toBool() << "count=" << items.size();
            for (const auto& value:items) {
                const auto item=value.toObject();
                qInfo().noquote() << "  id=" << item.value("record").toObject().value("id").toString()
                                  << "seq=" << item.value("sequence").toString()
                                  << "receivedAt=" << utc(qint64(item.value("receivedAtMs").toDouble()));
            }
            if (!id.isEmpty()) {
                auto page=recentPages.value(id);
                page.fromMs=qint64(result.value("fromMs").toDouble());
                page.toMs=qint64(result.value("toMs").toDouble());
                page.offset=qint64(result.value("nextOffset").toDouble());
                page.more=result.value("hasMore").toBool();
                recentPages.insert(id,page);
                if (page.more) qInfo().noquote() << "[RECENT] More: recentnext" << id;
            }
        } else if (op=="search_hot") {
            const auto items=result.value("items").toArray();
            qInfo().noquote() << "[HOT_SEARCH] mode=" << result.value("mode").toString()
                              << "matched=" << items.size() << "scanned=" << result.value("scanned").toInt()
                              << "hotIds=" << result.value("hotIds").toInt();
            for (const auto& value:items) {
                const auto item=value.toObject();
                qInfo().noquote() << "  id=" << item.value("record").toObject().value("id").toString()
                                  << "seq=" << item.value("sequence").toString()
                                  << "receivedAt=" << utc(qint64(item.value("receivedAtMs").toDouble()));
            }
            searchPage.cursor=result.value("nextCursor").toString();
            searchPage.more=result.value("hasMore").toBool();
            if (searchPage.more) qInfo() << "[HOT_SEARCH] More: hotnext";
        } else if (op=="subscribe") {
            qInfo().noquote() << "[RECEIVER] Subscribed to" << result.value("channel").toString();
        } else {
            qInfo().noquote() << "[RESULT]" << op
                              << QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
        }
    });
    QObject::connect(&client,&ArsClient::requestFailed,&app,
                     [&](const QString& request,const QString& op,const QString& reason){
        requestToId.remove(request);
        qWarning().noquote() << "[ERROR]" << op << reason;
    });
    QObject::connect(&client,&ArsClient::connectionError,&app,
                     [&](const QString& reason){ qWarning().noquote() << "[CONNECTION_ERROR]" << reason; });
    QObject::connect(&client,&ArsClient::disconnected,&app,[&]{
        qWarning() << "[RECEIVER] Disconnected; retrying";
    });
    QObject::connect(&client,&ArsClient::ready,&app,[&]{
        qInfo() << "[RECEIVER] Connected. Waiting for live events and commands";
        client.subscribeLive(channel);
        help();
    });

    // Do not use a blocking std::getline loop: that would freeze Qt's TCP event loop.
    QByteArray stdinBuffer;
    QSocketNotifier stdinNotifier(STDIN_FILENO,QSocketNotifier::Read,&app);
    auto onStdinReadable = [&]{
        char chunk[2048];
        const ssize_t size=::read(STDIN_FILENO,chunk,sizeof(chunk));
        if (size<=0) { stdinNotifier.setEnabled(false); return; }
        stdinBuffer.append(chunk,int(size));
        if (stdinBuffer.size()>32768) {stdinBuffer.clear();qWarning()<<"Input too long";return;}
        int newline=0;
        while ((newline=stdinBuffer.indexOf('\n'))>=0) {
            const QString line=QString::fromUtf8(stdinBuffer.left(newline)).trimmed();
            stdinBuffer.remove(0,newline+1);
            const QStringList words=line.split(' ',Qt::SkipEmptyParts);
            if (words.isEmpty()) continue;
            const QString op=words.at(0).toLower();
            if (op=="help") {help();continue;}
            if (op=="quit" || op=="exit") {app.quit();return;}
            if (op=="status") {client.status();continue;}
            if (op=="hotnext") {
                if (!searchPage.more) {qInfo()<<"No next hot page";continue;}
                client.searchHot(searchPage.mode,searchPage.filters,searchPage.idContains,searchPage.cursor);
                continue;
            }
            if (op=="hot") {
                searchPage=SearchPage{};
                if (words.size()>1 && (words.at(1)=="latest" || words.at(1)=="records")) searchPage.mode=words.at(1);
                if (words.size()>2) searchPage.idContains=words.at(2);
                client.searchHot(searchPage.mode,searchPage.filters,searchPage.idContains);
                continue;
            }
            if (op=="filter") {
                if (words.size()!=5 || (words.at(1)!="latest" && words.at(1)!="records")) {
                    qInfo()<<"Usage: filter latest|records speed gte 200";continue;
                }
                searchPage=SearchPage{};searchPage.mode=words.at(1);
                bool numeric=false;
                const double num=words.at(4).toDouble(&numeric);
                searchPage.filters=QJsonArray{QJsonObject{{"field",words.at(2)},
                    {"op",words.at(3)},{"value",numeric?QJsonValue(num):QJsonValue(words.at(4))}}};
                client.searchHot(searchPage.mode,searchPage.filters);
                continue;
            }
            if (words.size()<2) {qInfo()<<"Need ID. Type help";continue;}
            const QString id=words.at(1);
            if (op=="sub") client.subscribeLive(id);
            else if (op=="unsub") client.unsubscribeLive(id);
            else if (op=="latest") client.getLatest(id);
            else if (op=="delete") {pages.remove(id);client.deleteById(id,true);}
            else if (op=="evict") {pages.remove(id);client.deleteById(id,false);}
            else if (op=="hydrate") client.hydrateToCache(id);
            else if (op=="recent" || op=="today") {
                RecentPage page;
                recentPages.insert(id,page);
                QString req;
                if (op=="today") {
                    const QDateTime midnight(QDate::currentDate(),QTime(0,0));
                    req=client.recentHistoryPage(id,midnight.toMSecsSinceEpoch(),QDateTime::currentMSecsSinceEpoch(),0);
                } else {
                    const int seconds=durationSeconds(words.size()>2?words.at(2):"30m");
                    if (seconds<=0 || seconds>30*86400) {qInfo()<<"Use 30m, 1h, 24h or Ns";continue;}
                    req=client.recentHistory(id,seconds);
                }
                requestToId.insert(req,id);
            } else if (op=="recentnext") {
                const auto page=recentPages.value(id);
                if (!page.more) {qInfo()<<"No next recent page";continue;}
                const auto req=client.recentHistoryPage(id,page.fromMs,page.toMs,page.offset,page.limit);
                requestToId.insert(req,id);
            } else if (op=="history" || op=="last5") {
                bool valid=true;
                int limit=100;
                if (op=="history" && words.size()>=3) limit=words.at(2).toInt(&valid);
                if (!valid || limit<1 || limit>1000) {qWarning()<<"limit must be 1..1000";continue;}
                Page page;
                page.limit=limit;
                if (op=="last5") page.fromMs=QDateTime::currentMSecsSinceEpoch()-5*60*1000LL;
                pages.insert(id,page);
                const QString req=client.queryHistory(id,page.fromMs,-1,page.limit,page.timeField);
                requestToId.insert(req,id);
            } else if (op=="next") {
                if (!pages.contains(id) || pages.value(id).after.isEmpty()) {
                    qInfo()<<"No next page for ID"<<id;continue;
                }
                const Page page=pages.value(id);
                const QString req=client.queryHistory(id,page.fromMs,-1,page.limit,page.timeField,page.after,page.until);
                requestToId.insert(req,id);
            } else qInfo()<<"Unknown command. Type help";
        }
    };
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    QObject::connect(&stdinNotifier,QOverload<int>::of(&QSocketNotifier::activated),
                     &app,onStdinReadable);
#else
    QObject::connect(&stdinNotifier,&QSocketNotifier::activated,&app,onStdinReadable);
#endif

    QTimer::singleShot(12000,&app,[&]{
        if (!client.isReady()) {
            qCritical() << "[CONNECTION_TIMEOUT] Could not connect to ARS server";
            app.exit(1);
        }
    });
    client.connectToServer(parser.value("host"),quint16(port),transport);
    return app.exec();
}
