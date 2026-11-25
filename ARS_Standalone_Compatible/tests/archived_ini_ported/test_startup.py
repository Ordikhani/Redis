"""Black-box startup regression tests. Set ARS_SERVER_BINARY and ARS_TEST_CONFIG.
The config must point to isolated, running PostgreSQL/Redis services.
"""
import configparser
import os
from pathlib import Path
import socket
import shutil
import subprocess
import tempfile
import unittest

BINARY = os.environ.get('ARS_SERVER_BINARY')
CONFIG = os.environ.get('ARS_TEST_CONFIG')

@unittest.skipUnless(BINARY and CONFIG, 'set ARS_SERVER_BINARY and ARS_TEST_CONFIG for startup integration tests')
class StartupTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.directory = Path(self.tmp.name)
        self.config = configparser.ConfigParser()
        self.config.optionxform = str
        self.config.read(CONFIG)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.config['server']['port'] = str(self.port)
        self.path = self.directory / 'server.ini'
        self.write_config()
        self.env = os.environ.copy()
        self.env.pop('ARS_CONFIG', None)

    def tearDown(self):
        self.tmp.cleanup()

    def write_config(self):
        with self.path.open('w') as out:
            self.config.write(out)

    def run_server(self, *args, env=None):
        return subprocess.run([BINARY, *args], cwd=self.directory, env=env or self.env,
                              capture_output=True, text=True, timeout=30)

    def test_check_success_from_foreign_directory(self):
        r = self.run_server('--config', str(self.path), '--check')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('[CHECK PASSED]', r.stderr)
        with socket.socket() as sock:
            self.assertNotEqual(sock.connect_ex(('127.0.0.1', self.port)), 0)

    def test_config_beside_relocated_executable(self):
        binary = self.directory / 'ars_server'
        shutil.copy2(BINARY, binary)
        foreign = self.directory / 'unrelated-working-directory'
        foreign.mkdir()
        r = subprocess.run([str(binary), '--check'], cwd=foreign, env=self.env,
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(str(self.path), r.stderr)

    def test_environment_config_and_legacy_argument(self):
        env = dict(self.env, ARS_CONFIG=str(self.path))
        r = self.run_server('--check', env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_server(str(self.path), '--check')
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_explicit_missing_path_never_falls_back(self):
        r = self.run_server('--config', str(self.directory/'missing.ini'), '--check')
        self.assertEqual(r.returncode, 2)
        self.assertIn('[FAIL]', r.stderr)

    def test_reports_all_failed_dependencies(self):
        self.config['postgres']['port'] = '1'
        self.config['redis']['port'] = '1'
        self.write_config()
        env = dict(self.env)
        r = self.run_server('--config', str(self.path), '--check', env=env)
        self.assertEqual(r.returncode, 2)
        for name in ('PostgreSQL', 'Redis', '[NOT READY]'):
            self.assertIn(name, r.stderr)

    def test_wrong_database_and_occupied_port(self):
        self.config['postgres']['database'] = 'ars_missing_startup_test'
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', self.port))
            sock.listen()
            self.write_config()
            r = self.run_server('--config', str(self.path), '--check')
        self.assertEqual(r.returncode, 2)
        self.assertIn('[FAIL] PostgreSQL', r.stderr)
        self.assertIn('[FAIL] ARS listen', r.stderr)

    def test_postgres_password_comes_from_ini_and_is_not_logged(self):
        password = self.config['postgres'].get('password', '')
        self.assertTrue(password, 'isolated test server.ini must contain password')
        self.write_config()
        r = self.run_server('--config', str(self.path), '--check')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn(password, r.stdout + r.stderr)

    def test_check_refuses_mutating_flag(self):
        r = self.run_server('--check', '--start-local-services')
        self.assertEqual(r.returncode, 2)
        self.assertIn('read-only', r.stderr)

    def test_invalid_port_and_retries(self):
        self.config['redis']['port'] = '70000'
        self.write_config()
        r = self.run_server('--config', str(self.path), '--check')
        self.assertEqual(r.returncode, 2)
        self.assertIn('65535', r.stderr)
        r = self.run_server('--retries', '0')
        self.assertEqual(r.returncode, 2)

if __name__ == '__main__':
    unittest.main()
