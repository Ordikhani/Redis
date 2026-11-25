#include "ServerConfig.h"
#include <QHash>
#include <QVariant>

// === EDIT VALUES HERE, THEN REBUILD ars_server ===
// Build an isolated lab variant with -DARS_LAB_PROFILE=ON; do not use it
// against production data. The lab profile matches tests/reference_lab/compose.yaml.
#ifdef ARS_LAB_PROFILE
#define ARS_PG_PORT 15432
#define ARS_DB_NAME "ars_test"
#define ARS_PG_PASSWORD "lab-only-change-me-2026"
#define ARS_REDIS_PORT 16379
#define ARS_TCP_PORT 7093
#define ARS_UDP_PORT 7094
#define ARS_REDIS_PREFIX "ars_lab_2026"
#define ARS_IDLE_SEC 8
#define ARS_CHECKPOINT_SEC 3
#define ARS_SCAN_MS 250
#define ARS_REQUIRE_DURABILITY true
#else
#define ARS_PG_PORT 5432
#define ARS_DB_NAME "ars"
// SET THE ROLE PASSWORD HERE FOR YOUR OWN ENVIRONMENT.
#define ARS_PG_PASSWORD "CHANGE_ME_BEFORE_CONNECTING_TO_POSTGRES"
#define ARS_REDIS_PORT 6379
#define ARS_TCP_PORT 7090
#define ARS_UDP_PORT 7092
#define ARS_REDIS_PREFIX "arscentral"
#define ARS_IDLE_SEC 60
#define ARS_CHECKPOINT_SEC 300
#define ARS_SCAN_MS 1000
#define ARS_REQUIRE_DURABILITY false
#endif
// IMPORTANT: Compiling a password into a binary HIDES it from casual file
// browsing, but DOES NOT protect it against reverse engineering or strings.
// Do not commit real credentials to public repositories or distribute binaries
// with production database credentials to untrusted users.
namespace {
const QHash<QString, QVariant>& defaults() {
    static const QHash<QString, QVariant> values {
        // Client-facing TCP listener
        {QStringLiteral("server/host"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("server/port"), ARS_TCP_PORT},
        {QStringLiteral("server/maxClients"), 64},
        // Client-facing UDP listener (both protocols are always available)
        {QStringLiteral("udp/enabled"), true},
        {QStringLiteral("udp/host"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("udp/port"), ARS_UDP_PORT},
        {QStringLiteral("udp/maxDatagramBytes"), 1200},
        // PostgreSQL archive. Set password to the REAL ars_app role password.
        {QStringLiteral("postgres/host"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("postgres/port"), ARS_PG_PORT},
        {QStringLiteral("postgres/database"), QStringLiteral(ARS_DB_NAME)},
        {QStringLiteral("postgres/user"), QStringLiteral("ars_app")},
        {QStringLiteral("postgres/password"), QStringLiteral(ARS_PG_PASSWORD)},
        // Redis hot store
        {QStringLiteral("redis/enabled"), true},
        {QStringLiteral("redis/required"), true},
        {QStringLiteral("redis/host"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("redis/port"), ARS_REDIS_PORT},
        {QStringLiteral("redis/db"), 0},
        {QStringLiteral("redis/password"), QStringLiteral("")},
        {QStringLiteral("redis/keyPrefix"), QStringLiteral(ARS_REDIS_PREFIX)},
        {QStringLiteral("redis/requireDurability"), ARS_REQUIRE_DURABILITY},
        // Lifecycle and bounded maintenance queues
        {QStringLiteral("lifecycle/inactiveAfterSec"), ARS_IDLE_SEC},
        {QStringLiteral("lifecycle/checkpointEverySec"), ARS_CHECKPOINT_SEC},
        {QStringLiteral("lifecycle/scanEveryMs"), ARS_SCAN_MS},
        {QStringLiteral("lifecycle/batchSize"), 100},
        {QStringLiteral("lifecycle/maxBatchesPerSweep"), 4},
        {QStringLiteral("lifecycle/workerIntervalMs"), 100},
        {QStringLiteral("memory/maxActiveIds"), 0},
        {QStringLiteral("recovery/maxQueuedJobs"), 64},
        {QStringLiteral("recovery/ackTimeoutMs"), 10000},
    };
    return values;
}
} // namespace

const ServerConfig& ServerConfig::instance() {
    static const ServerConfig config;
    return config;
}

QVariant ServerConfig::value(const QString& key, const QVariant& fallback) const {
    const auto& values = defaults();
    const auto it = values.constFind(key);
    return it == values.cend() ? fallback : it.value();
}
