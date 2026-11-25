"""Unit/smoke tests for safe disk tools; no PostgreSQL or Redis required."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

SCRIPTS = Path(__file__).resolve().parents[2] / 'server' / 'scripts'


class ManagementToolsTest(unittest.TestCase):
    def test_backup_output_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp) / 'bin'
            bin_dir.mkdir()
            pg_dump = bin_dir / 'pg_dump'
            redis = bin_dir / 'redis-cli'
            pg_dump.write_text('#!/usr/bin/env python3\n'
                               'import pathlib,sys\n'
                               'pathlib.Path(sys.argv[sys.argv.index("-f")+1]).write_bytes(b"PG BACKUP")\n')
            redis.write_text('#!/usr/bin/env python3\n'
                             'import pathlib,sys,os\n'
                             'assert "REDISCLI_AUTH" not in os.environ, "Blank auth must not be sent"\n'
                             'pathlib.Path(sys.argv[sys.argv.index("--rdb")+1]).write_bytes(b"REDIS BACKUP")\n')
            pg_dump.chmod(0o755)
            redis.chmod(0o755)
            env = os.environ.copy()
            env['PATH'] = str(bin_dir) + os.pathsep + env.get('PATH', '')
            env.pop('ARS_REDIS_PASSWORD', None)
            env['REDISCLI_AUTH'] = ''
            output = Path(tmp) / 'out'
            cmd = [sys.executable, str(SCRIPTS / 'backup_all.py'), str(output)]
            subprocess.run(cmd, check=True, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            meta = json.loads((output / 'manifest.json').read_text())
            self.assertEqual(set(meta['filesSha256']), {'archive.pgcustom', 'redis.rdb'})
            self.assertFalse(meta['crossStoreAtomic'])
            self.assertFalse((output / '.incomplete').exists())
            duplicated = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertNotEqual(duplicated.returncode, 0, 'must not overwrite an existing backup')

    def test_purge_rejects_running_server(self):
        with tempfile.TemporaryDirectory() as tmp, socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            cfg = Path(tmp) / 'server.ini'
            cfg.write_text('[server]\nhost=127.0.0.1\nport=' + str(listener.getsockname()[1]) +
                           '\n[redis]\nenabled=true\n')
            proc = subprocess.run([sys.executable, str(SCRIPTS / 'purge_archive.py'),
                                   '--config', str(cfg), '--confirm', 'DELETE ALL ARS ARCHIVE'],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn(b'RUNNING', proc.stderr)

    def test_purge_resets_redis_offset_before_sql(self):
        with tempfile.TemporaryDirectory() as tmp, socket.socket() as server:
            server.bind(('127.0.0.1', 0))
            server.listen()
            port = server.getsockname()[1]
            events = []
            def mock_redis():
                connection, _ = server.accept()
                with connection:
                    connection.settimeout(5)
                    stream = connection.makefile('rb')
                    for _ in range(2):
                        count = int(stream.readline().strip()[1:])
                        args = []
                        for __ in range(count):
                            length = int(stream.readline().strip()[1:])
                            args.append(stream.read(length).decode())
                            stream.read(2)
                        events.append(args)
                        connection.sendall(b'+OK\r\n' if args[0] == 'SELECT' else b':1\r\n')
            thread = threading.Thread(target=mock_redis, daemon=True)
            thread.start()
            fake_bin = Path(tmp) / 'bin'
            fake_bin.mkdir()
            # First psql invocation returns zero active ARS processes; second
            # is the destructive TRUNCATE statement.
            pg = fake_bin / 'psql'
            pg.write_text('#!/usr/bin/env python3\n'
                          'import os,sys,pathlib\n'
                          'assert os.environ["PGDATABASE"] == "ars_test", "Config must override unrelated PGDATABASE"\n'
                          'text=" ".join(sys.argv)\n'
                          'if "pg_stat_activity" in text: print("0")\n'
                          'else: pathlib.Path(os.environ["TEST_SQL_LOG"]).write_text(text)\n')
            pg.chmod(0o755)
            cfg = Path(tmp) / 'server.ini'
            cfg.write_text('[server]\nhost=127.0.0.1\nport=1\n'
                           '[postgres]\ndatabase=ars_test\n'
                           '[redis]\nenabled=true\nhost=127.0.0.1\nport=' + str(port) +
                           '\ndb=0\nkeyPrefix=ars_test_prefix\n')
            env = os.environ.copy()
            env['PATH'] = str(fake_bin) + os.pathsep + env.get('PATH', '')
            env['TEST_SQL_LOG'] = str(Path(tmp) / 'sql.log')
            env['PGDATABASE'] = 'unrelated_database_must_not_be_used'
            proc = subprocess.run([sys.executable, str(SCRIPTS / 'purge_archive.py'),
                                   '--config', str(cfg), '--confirm', 'DELETE ALL ARS ARCHIVE'],
                                  env=env, capture_output=True, text=True)
            thread.join(timeout=5)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(events, [['SELECT', '0'], ['DEL', 'ars_test_prefix:hotv1:synced']])
            self.assertIn('TRUNCATE TABLE ars_records', (Path(tmp) / 'sql.log').read_text())

    def test_purge_requires_exact_confirmation(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = Path(tmp) / 'server.ini'
            cfg.write_text('[redis]\nenabled=true\n')
            proc = subprocess.run([sys.executable, str(SCRIPTS / 'purge_archive.py'),
                                   '--config', str(cfg), '--confirm', 'YES'],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn(b'confirmation', proc.stderr)


if __name__ == '__main__':
    unittest.main()
