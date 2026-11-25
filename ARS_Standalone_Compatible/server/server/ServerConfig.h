#pragma once
#include <QVariant>
#include <QString>

// All runtime values are compiled into ars_server. No INI, QSettings,
// environment overrides or runtime configuration files are consulted.
// Change ServerConfig.cpp and rebuild ars_server to change a value.
// This is a read-only singleton, shared by the foreground and worker threads.
class ServerConfig final {
public:
    static const ServerConfig& instance();
    QVariant value(const QString& key, const QVariant& fallback = QVariant()) const;
    ServerConfig(const ServerConfig&) = delete;
    ServerConfig& operator=(const ServerConfig&) = delete;
private:
    ServerConfig() = default;
};
