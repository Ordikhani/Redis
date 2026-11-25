#include "Startup.h"
#include "ServerConfig.h"
#include "Redis/ArsRedisClient.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QTcpServer>
#include <QUdpSocket>
#include <QProcess>
#include <QDebug>

namespace {
void report(bool ok, const QString& name, const QString& message) {
    qInfo().noquote() << (ok ? "[OK]" : "[FAIL]") << name << message;
}
bool localHost(const QString& host) {
    return host == "localhost" || QHostAddress(host).isLoopback();
}
bool validPort(const ServerConfig& settings, const QString& key, int fallback) {
    bool ok = false;
    const int port = settings.value(key, fallback).toInt(&ok);
    if (!ok || port < 1 || port > 65535) {
        report(false, key, "Port must be between 1 and 65535");
        return false;
    }
    return true;
}
}

bool Startup::check() {
    const ServerConfig& s = ServerConfig::instance();
    report(true, "Config", "Compiled into ars_server (ServerConfig.cpp); no INI file");
    bool ok = true;
    report(true, "Client API", "Credential-free TCP/UDP (hello handshake)");
    const QString pgPassword = s.value("postgres/password").toString();
    const bool pgPasswordOk = !pgPassword.isEmpty() && pgPassword != "CHANGE_ME_BEFORE_CONNECTING_TO_POSTGRES";
    report(pgPasswordOk, "PostgreSQL password", pgPasswordOk ? "Compiled-in credential present (value hidden)" : "Edit ARS_PG_PASSWORD in ServerConfig.cpp and rebuild ars_server");
    ok = pgPasswordOk && ok;

    const bool pgPort = validPort(s, "postgres/port", 5432);
    if (!QSqlDatabase::isDriverAvailable("QPSQL")) {
        report(false, "PostgreSQL", "QPSQL driver missing. Install the PostgreSQL plugin for your Qt kit (Install QPSQL plugin for the chosen Qt 6.7 kit).");
        ok = false;
    } else if (pgPort) {
        const QString name = "ars_startup_check";
        {
            auto db = QSqlDatabase::addDatabase("QPSQL", name);
            db.setHostName(s.value("postgres/host", "127.0.0.1").toString());
            db.setPort(s.value("postgres/port", 5432).toInt());
            db.setDatabaseName(s.value("postgres/database", "ars").toString());
            db.setUserName(s.value("postgres/user", "ars_app").toString());
            db.setPassword(pgPassword);
            db.setConnectOptions("connect_timeout=3");
            const bool connected = db.open();
            const QString target = QString("%1:%2 database=%3 user=%4")
                .arg(db.hostName()).arg(db.port()).arg(db.databaseName(), db.userName());
            if (!connected) {
                QString error = db.lastError().text();
                if (!db.password().isEmpty()) error.replace(db.password(), "[hidden]");
                report(false, "PostgreSQL", target + " : " + error +
                    " Check service, database name, password and QPSQL. Provision a missing database with init_database.py.");
                ok = false;
            } else {
                QSqlQuery q(db);
                const bool healthy = q.exec("SELECT 1");
                report(healthy, "PostgreSQL", target + " (login and query)");
                ok = healthy && ok;
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    ok = pgPort && ok;

    if (s.value("redis/enabled", true).toBool()) {
        const bool redisPort = validPort(s, "redis/port", 6379);
        const QString host = s.value("redis/host", "127.0.0.1").toString();
        const int port = s.value("redis/port", 6379).toInt();
        ArsRedisClient redis;
        bool ready = redisPort && redis.connectToHost(host, quint16(port),
            s.value("redis/password").toString(), s.value("redis/db", 0).toInt());
        if (ready) {
            const auto ping = redis.sendCommand({"PING"}, 1500);
            ready = ping.type == ArsRedisReply::String && ping.str == "PONG";
        }
        if (ready) {
            auto config = [&](const QByteArray& key) {
                auto r = redis.sendCommand({"CONFIG", "GET", key}, 1500);
                return r.type == ArsRedisReply::Array && r.elements.size() == 2 ? r.elements[1].str : QByteArray();
            };
            const bool durable = config("appendonly") == "yes" &&
                config("appendfsync") == "always" && config("maxmemory-policy") == "noeviction";
            if (!durable) {
                qWarning().noquote() << "[WARN] Redis: use appendonly yes, appendfsync always, maxmemory-policy noeviction for strict durability.";
                if (s.value("redis/requireDurability", false).toBool()) ready = false;
            }
        }
        report(ready, "Redis", QString("%1:%2").arg(host).arg(port) +
            (ready ? " PING/authentication OK" : " unavailable or durability check failed. Check service, port, password, DB number and CONFIG permission."));
        if (!ready && s.value("redis/required", true).toBool()) ok = false;
        if (!ready && !s.value("redis/required", true).toBool())
            qWarning() << "[WARN] PostgreSQL-only fallback explicitly allowed by redis/required=false";
    } else {
        qInfo() << "[INFO] Redis disabled explicitly; PostgreSQL-only mode";
    }
    const bool arsPort = validPort(s, "server/port", 7090);
    const QHostAddress address(s.value("server/host", "127.0.0.1").toString());
    QTcpServer probe;
    const bool bindOk = arsPort && !address.isNull() &&
        probe.listen(address, quint16(s.value("server/port", 7090).toInt()));
    report(bindOk, "ARS listen", address.toString() + ":" + s.value("server/port", 7090).toString() +
        (bindOk ? " available" : " : " + probe.errorString() + " (check address, port conflict or an already running ARS)"));
    probe.close();
    bool udpOk = true;
    if (s.value("udp/enabled", false).toBool()) {
        const bool udpPortOk = validPort(s, "udp/port", 7092);
        bool maxOk = false;
        const int maxBytes = s.value("udp/maxDatagramBytes", 1200).toInt(&maxOk);
        const QHostAddress udpHost(s.value("udp/host", s.value("server/host", "127.0.0.1")).toString());
        QUdpSocket udpProbe;
        udpOk = udpPortOk && maxOk && maxBytes >= 512 && maxBytes <= 4096 && !udpHost.isNull() &&
            udpProbe.bind(udpHost, quint16(s.value("udp/port", 7092).toInt()));
        report(udpOk, "ARS UDP listen", udpHost.toString() + ":" + s.value("udp/port", 7092).toString() +
            (udpOk ? " available" : " : port occupied, invalid address or maxDatagramBytes out of range"));
        udpProbe.close();
    }
    return bindOk && udpOk && ok;
}

bool Startup::startLocalServices() {
#ifdef Q_OS_LINUX
    const ServerConfig& s = ServerConfig::instance();
    bool ok = true;
    for (const auto& service : {QString("postgres"), QString("redis")}) {
        if (service == "redis" && !s.value("redis/enabled", true).toBool()) continue;
        const auto host = s.value(service + "/host", "127.0.0.1").toString();
        const int defaultPort = service == "redis" ? 6379 : 5432;
        const int port = s.value(service + "/port", defaultPort).toInt();
        if (!localHost(host) || port != defaultPort) {
            report(false, service, "Auto-start supports only local default-port system services. Use run_lab.sh start for the Docker lab.");
            ok = false; continue;
        }
        QTcpSocket socket;
        socket.connectToHost(host, quint16(port));
        if (socket.waitForConnected(500)) continue;
        QProcess process;
        const QString unit = service == "redis" ? "redis-server" : "postgresql";
        process.start("systemctl", {"--no-ask-password", "start", unit});
        if (!process.waitForStarted(2000) || !process.waitForFinished(15000)) {
            process.kill(); process.waitForFinished(1000);
            report(false, unit, "Could not start service. Check installation and service-management permissions.");
            ok = false; continue;
        }
        const bool started = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        report(started, unit, started ? "Start requested" : QString::fromUtf8(process.readAllStandardError()));
        ok = started && ok;
    }
    return ok;
#else
    report(false, "Services", "Auto-start is available for Linux systemd services; start PostgreSQL/Redis using your service manager.");
    return false;
#endif
}
