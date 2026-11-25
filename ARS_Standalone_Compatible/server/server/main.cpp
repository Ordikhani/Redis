#include <QCoreApplication>
#include <QDebug>
#include <QCommandLineParser>
#include <QThread>
#include <exception>
#include "Server.h"
#include "Startup.h"
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ars_server");
    QCommandLineParser parser;
    parser.setApplicationDescription("ARS Hot Data Manager: all settings compiled into ServerConfig.cpp.");
    parser.addHelpOption();
    parser.addOption({"check", "Check compiled-in settings, database connections and listen ports; then exit."});
    parser.addOption({"start-local-services", "Start missing LOCAL default-port systemd PostgreSQL/Redis services (requires OS permission)."});
    parser.addOption({"retries", "Number of startup check attempts, 1..10.", "count", "1"});
    parser.process(app);
    bool countOk = false;
    const int attempts = parser.value("retries").toInt(&countOk);
    if (!parser.positionalArguments().isEmpty() || !countOk || attempts < 1 || attempts > 10 ||
        (parser.isSet("check") && parser.isSet("start-local-services"))) {
        qCritical() << "Invalid arguments. No --config or INI is supported. See --help.";
        return 2;
    }
    try {
        if (parser.isSet("start-local-services")) Startup::startLocalServices();
        bool ready = false;
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            qInfo() << "[STARTUP] Check" << attempt << "of" << attempts;
            if ((ready = Startup::check())) break;
            if (attempt < attempts) QThread::msleep(1000);
        }
        if (!ready) {
            qCritical() << "[NOT READY] ARS did not start. Fix the [FAIL] items above; a client cannot connect until [READY] appears.";
            return 2;
        }
        if (parser.isSet("check")) {
            qInfo() << "[CHECK PASSED] Dependencies and listen port are ready. Start ars_server without --check to serve clients.";
            return 0;
        }
        Server server;
        if (!server.start()) return 1;
        return app.exec();
    } catch (const std::exception& error) {
        qCritical() << "[FAIL] Unexpected startup error:" << error.what();
    } catch (...) {
        qCritical() << "[FAIL] Unexpected startup error";
    }
    return 1;
}
