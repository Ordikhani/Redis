"""Structural checks for compiled-in configuration. NOT a Qt/network E2E test."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'server'

class CompiledConfigContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = (SERVER/'server/ServerConfig.cpp').read_text()
        cls.main = (SERVER/'server/main.cpp').read_text()
        cls.startup = (SERVER/'server/Startup.cpp').read_text()
        cls.server = (SERVER/'server/Server.cpp').read_text()
        cls.store = (SERVER/'server/HotColdStore.cpp').read_text()
        cls.worker = (SERVER/'server/HotColdWorker.cpp').read_text()
        cls.pg = (SERVER/'server/PostgresStore.cpp').read_text()
        cls.cmake = (SERVER/'CMakeLists.txt').read_text()

    def test_zero_ini_files(self):
        self.assertEqual([], list(ROOT.rglob('*.ini')))

    def test_not_using_runtime_settings(self):
        for src in (self.main, self.startup, self.server, self.store, self.worker, self.pg):
            self.assertNotIn('QSettings', src)
        self.assertNotIn('parser.addOption({{"c", "config"}', self.main)
        self.assertNotIn('findConfig(', self.startup)
        self.assertNotIn('StageRuntimeConfig', self.cmake)
        self.assertNotIn('install(FILES', self.cmake)

    def test_single_config_access(self):
        for src in (self.startup, self.server, self.store, self.worker):
            self.assertIn('ServerConfig::instance()', src)
        self.assertIn('const ServerConfig& s', self.pg)
        self.assertIn('ServerConfig.cpp', self.cmake)

    def test_password_database_redis_defaults(self):
        for key in ('postgres/password','postgres/host','postgres/port',
                    'redis/password','redis/port','redis/keyPrefix',
                    'server/port','udp/port', 'lifecycle/inactiveAfterSec',
                    'recovery/ackTimeoutMs'):
            self.assertIn('"'+key+'"', self.config)
        self.assertIn('ARS_PG_PASSWORD', self.config)
        self.assertIn('ARS_LAB_PROFILE', self.config)

    def test_reconnect_reads_same_values(self):
        self.assertIn('m_postgres.reconnect(settings,true)', self.store)
        self.assertIn('ServerConfig::instance()', self.worker)
        self.assertIn('postgres.open(settings, false)', self.worker)

    def test_listeners_and_commands_retained(self):
        for op in ('hello','status','subscribe','publish','save','search_hot',
                   'recent','latest','history','delete','hydrate','recovery_status'):
            self.assertIn('"'+op+'"',self.server)
        self.assertIn('m_udpListener.bind(', self.server)
        self.assertIn('m_listener.listen(', self.server)

    def test_release_doesnt_install_config(self):
        self.assertIn('install(TARGETS ars_server RUNTIME DESTINATION bin)',self.cmake)
        self.assertNotIn('add_custom_command(TARGET ars_server POST_BUILD',self.cmake)
        self.assertNotIn('server.ini', self.cmake)

if __name__=='__main__': unittest.main(verbosity=2)
