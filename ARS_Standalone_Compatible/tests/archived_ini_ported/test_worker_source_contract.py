#!/usr/bin/env python3
"""Fast source-contract regressions for critical Worker safety invariants."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WorkerSourceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.worker = (ROOT / 'server/server/HotColdWorker.cpp').read_text()
        cls.store = (ROOT / 'server/server/HotColdStore.cpp').read_text()
        cls.redis = (ROOT / 'server/src/Redis/ArsRedisClient.cpp').read_text()
        cls.pg = (ROOT / 'server/server/PostgresStore.cpp').read_text()
        cls.server = (ROOT / 'server/server/Server.cpp').read_text()
        # Lua scripts live in one shared header used by both threads; their behaviour is
        # executed on a real Redis by test_hotcold_lua.py.
        scripts = (ROOT / 'server/server/HotColdScripts.h').read_text()
        cls.store += scripts
        cls.worker += scripts

    def test_redis_timeout_discards_socket_and_late_replies(self):
        self.assertIn('redis.disconnectFromHost();', self.worker)
        self.assertIn('m_socket.abort();', self.redis)
        self.assertIn('late response bytes', self.redis)
        self.assertIn('requestFailedDetailed', (ROOT / 'producer/vendor/ars/client/ArsClient.h').read_text())

    def test_append_rechecks_active_marker_inside_atomic_lua(self):
        self.assertIn("ARGV[5]=='1' and not active", self.store)
        self.assertIn("redis.call('ZCARD',KEYS[3])>=cap", self.store)
        self.assertIn('QByteArray::number(m_maxActiveIds)', self.store)
        self.assertIn('HotColdScripts::append', self.store)
        self.assertIn('HOT_ID_EVICTED_DURING_APPEND', self.store)

    def test_worker_reconnects_postgres_with_backoff_and_combined_health(self):
        self.assertIn('postgres.reconnect(settings,false)', self.worker)
        self.assertIn('postgres.healthy()', self.worker)
        self.assertIn('redis.isConnected() && postgresReady', self.worker)
        self.assertIn('postgresBackoffMs=qMin(30000,postgresBackoffMs*2)', self.worker)

    def test_capacity_error_is_structured_and_hydrate_is_guarded(self):
        self.assertGreaterEqual(self.store.count('HOT_CAPACITY_REACHED'), 1)
        hydrate = self.store[self.store.index('bool HotColdStore::hydrate'):]
        self.assertIn('m_maxActiveIds>0', hydrate)
        self.assertIn('failWithCode(m_store.error(),code)', self.server)

    def test_recovery_publication_rechecks_capacity_atomically(self):
        self.assertIn("redis.call('ZCARD',KEYS[5])>=cap", self.worker)
        self.assertIn('capacityRejected', self.worker)


    def test_cold_writes_are_held_until_stored(self):
        self.assertIn('processDeferred', self.server)
        self.assertIn('"RECOVERY_PENDING"', self.server)
        self.assertNotIn('result.value("stored").toBool()) {\n            QJsonObject event', self.server)

    def test_client_thread_reconnects_postgres_with_writer_lock(self):
        self.assertIn('m_postgres.reconnect(settings,true)', self.store)
        self.assertIn('m_store.maintainPostgres()', self.server)

    def test_synced_offset_is_guarded_in_both_threads(self):
        self.assertIn('HotColdScripts::advanceSynced', self.worker)
        self.assertIn('HotColdScripts::advanceSynced', self.store)


if __name__ == '__main__':
    unittest.main(verbosity=2)
