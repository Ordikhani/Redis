"""Black-box tests for the maintenance worker (HotColdWorker) and its fixes.

Each test starts its OWN ars_server from ARS_SERVER_BINARY with a copy of ARS_TEST_CONFIG
(free port, unique Redis key prefix, short lifecycle timers). Stop any other ARS that
uses the same PostgreSQL database first: only one writer may own a database.

    export ARS_SERVER_BINARY="$(pwd)/build/central/ars_server"
    export ARS_TEST_CONFIG="$(pwd)/central/lab/server.lab.ini"
    Set ARS_SERVER_BINARY and ARS_TEST_CONFIG to an isolated test server.ini
    python3 -m unittest discover -s central/tests -p 'test_worker.py' -v

The PostgreSQL-reconnect tests also need `psql` on PATH (they terminate the worker's
backend, application_name='ars_maintenance_worker', or the client thread's backend,
application_name='ars_server'); they are skipped otherwise.
Only isolated lab services must be used: records are written under random IDs.
"""
import configparser
import json
import os
from pathlib import Path
import random
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import uuid

BINARY = os.environ.get('ARS_SERVER_BINARY')
CONFIG = os.environ.get('ARS_TEST_CONFIG')


def now_ms():
    return int(time.time() * 1000)


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), 10)
        self.sock.settimeout(20)
        self.reader = self.sock.makefile('rb')
        self.request('hello', expect_ok=True)

    def close(self):
        self.reader.close()
        self.sock.close()

    def request(self, op, expect_ok=None, **args):
        rid = uuid.uuid4().hex
        self.sock.sendall((json.dumps({'requestId': rid, 'op': op, 'args': args}) + '\n').encode())
        while True:
            line = self.reader.readline()
            if not line:
                raise ConnectionError('server closed the connection during ' + op)
            msg = json.loads(line)
            if msg.get('event'):
                continue
            if msg.get('requestId') != rid:
                raise AssertionError('unexpected reply: %r' % msg)
            if expect_ok is True and not msg.get('ok'):
                raise AssertionError('%s failed: %s' % (op, msg.get('error')))
            return msg

    def call(self, op, **args):
        return self.request(op, expect_ok=True, **args)['result']

    def save(self, id_, n):
        return self.call('save', record={'id': id_, 'timestamp': now_ms(), 'source': 'WorkerTest',
                                         'features': {'n': n}})

    def full_history(self, id_):
        items, after, until, sources = [], '', '', set()
        while True:
            page = self.call('history', id=id_, limit=1000, afterSequence=after, untilSequence=until)
            sources.add(page['source'])
            items.extend(page['items'])
            if not page['hasMore']:
                return items, sources
            after, until = page['nextSequence'], page['untilSequence']


@unittest.skipUnless(BINARY and CONFIG,
                     'set ARS_SERVER_BINARY and ARS_TEST_CONFIG')
class WorkerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.proc = None
        self.client = None

    def tearDown(self):
        if self.client:
            self.client.close()
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.tmp.cleanup()

    def start(self, **overrides):
        cfg = configparser.ConfigParser()
        cfg.optionxform = str
        cfg.read(CONFIG)
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            port = s.getsockname()[1]
        cfg['server']['port'] = str(port)
        cfg['redis']['keyPrefix'] = 'ars_worker_test_' + uuid.uuid4().hex[:12]
        for section in ('lifecycle', 'memory'):
            if not cfg.has_section(section):
                cfg.add_section(section)
        for dotted, value in overrides.items():
            section, name = dotted.split('__')
            cfg[section][name] = str(value)
        self.config = cfg
        path = Path(self.tmp.name) / 'server.ini'
        with path.open('w') as out:
            cfg.write(out)
        self.log = open(Path(self.tmp.name) / 'server.log', 'w+')
        self.proc = subprocess.Popen([BINARY, '--config', str(path)], stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 40
        while time.time() < deadline:
            if self.proc.poll() is not None:
                self.fail('ars_server exited early:\n' + self.server_log())
            try:
                self.client = Client(port)
                return self.client
            except OSError:
                time.sleep(0.2)
        self.fail('ars_server did not become ready:\n' + self.server_log())

    def server_log(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read()[-4000:]

    def wait_for(self, predicate, timeout, what):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = predicate()
            if last:
                return last
            time.sleep(0.25)
        self.fail('timed out waiting for %s (last=%r)\n%s' % (what, last, self.server_log()))

    # ------------------------------------------------------------------------
    def test_status_reports_worker_and_postgres(self):
        c = self.start()
        status = c.call('status')
        self.assertTrue(status['maintenanceWorker'], status)
        self.assertTrue(status['postgres'], status)
        self.assertFalse(status['postgresReconnecting'], status)

    def wait_cold(self, c, id_, timeout=20):
        self.wait_for(lambda: c.call('latest', id=id_).get('source') == 'postgres', timeout, 'ID to become cold')

    def test_capacity_rejection_is_reported_with_code(self):
        c = self.start(memory__maxActiveIds=1, lifecycle__inactiveAfterSec=3,
                       lifecycle__scanEveryMs=250, lifecycle__checkpointEverySec=1)
        cold, hot, extra = ('cap-' + uuid.uuid4().hex for _ in range(3))
        c.save(cold, 1)
        self.wait_cold(c, cold)
        c.save(hot, 1)  # now the single hot slot is taken (for ~3 s)
        reply = c.request('save', record={'id': extra, 'timestamp': now_ms()})
        self.assertFalse(reply['ok'])
        self.assertEqual(reply.get('code'), 'HOT_CAPACITY_REACHED', reply)
        reply = c.request('hydrate', id=cold)
        self.assertEqual(reply.get('code'), 'HOT_CAPACITY_REACHED', reply)
        c.save(hot, 2)  # an ID that is already hot is never refused
        failure = c.request('history', id='')  # unrelated failures keep the generic shape
        self.assertNotIn('code', failure)

    def test_cold_write_is_acknowledged_only_after_recovery(self):
        c = self.start(lifecycle__inactiveAfterSec=2, lifecycle__scanEveryMs=250,
                       lifecycle__checkpointEverySec=1)
        id_ = 'held-' + uuid.uuid4().hex
        for n in range(3):
            c.save(id_, n)
        self.wait_cold(c, id_)
        result = c.call('publish', channel='worker-test', record={'id': id_, 'timestamp': now_ms()})
        self.assertTrue(result['stored'], result)
        self.assertEqual(result['storage'], 'redis', result)
        self.assertTrue(result['restoredFromPostgres'], result)
        self.assertEqual(result['restoredHistoryCount'], 3, result)
        self.assertNotIn('recoveryPending', result)
        items, sources = c.full_history(id_)
        self.assertEqual((len(items), sources), (4, {'redis'}))

    def test_history_stays_complete_across_evictions(self):
        """Saves and reads are timed around the inactivity cutoff so the worker evicts the
        ID while the server is reading it or about to append to it. Before the fixes this
        could produce a one-row hot history or an empty read; now every read is complete and
        every write to the just-evicted ID is held until its history is restored."""
        c = self.start(lifecycle__inactiveAfterSec=1, lifecycle__scanEveryMs=200,
                       lifecycle__checkpointEverySec=1, lifecycle__workerIntervalMs=10)
        id_ = 'race-' + uuid.uuid4().hex
        rng = random.Random(7)
        saved = 0
        sources = set()
        for _ in range(25):
            c.save(id_, saved)
            saved += 1
            time.sleep(rng.uniform(0.95, 1.35))
            items, used = c.full_history(id_)
            sources |= used
            self.assertEqual(len(items), saved, 'history lost rows after %d saves (sources=%s)' % (saved, used))
            sequences = [int(i['sequence']) for i in items]
            self.assertEqual(sequences, sorted(set(sequences)), 'duplicate or unordered rows')
        self.assertIn('postgres', sources, 'the test never observed an eviction; lengthen the sleeps')

    def kill_backends(self, application_name):
        pg = self.config['postgres']
        env = dict(os.environ, PGHOST=pg.get('host', '127.0.0.1'), PGPORT=pg.get('port', '5432'),
                   PGDATABASE=pg.get('database', 'ars'), PGUSER=pg.get('user', 'ars_app'),
                   PGPASSWORD=self.config.get('postgres', 'password', fallback=''))
        out = subprocess.run(['psql', '-Atc', "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity "
                                             "WHERE application_name='%s' AND pid<>pg_backend_pid()" % application_name],
                             env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertGreaterEqual(int(out.stdout.strip() or 0), 1, 'no %s backend found' % application_name)

    @unittest.skipUnless(shutil.which('psql'), 'psql not on PATH')
    def test_client_thread_reconnects_postgres_and_relocks(self):
        c = self.start()
        killed_at = now_ms()
        self.kill_backends('ars_server')
        self.wait_for(lambda: (lambda s: s['postgres'] and s['postgresReconnects'] >= 1)(c.call('status')),
                      30, 'client-thread PostgreSQL reconnect')
        result = c.save('pgmain-' + uuid.uuid4().hex, 0)  # a new ID needs the client-thread SQL connection
        self.assertTrue(result['stored'], result)
        self.assertGreater(result['receivedAtMs'], killed_at)
        self.assertIn('[POSTGRES_RECONNECTED]', self.server_log())

    @unittest.skipUnless(shutil.which('psql'), 'psql not on PATH')
    def test_worker_recovers_after_postgres_connection_is_killed(self):
        c = self.start(lifecycle__checkpointEverySec=1, lifecycle__inactiveAfterSec=3,
                       lifecycle__scanEveryMs=250)
        self.wait_for(lambda: c.call('status')['maintenanceWorker'], 15, 'worker connections')
        killed_at = now_ms()
        self.kill_backends('ars_maintenance_worker')

        id_ = 'pgkill-' + uuid.uuid4().hex
        for n in range(5):
            c.save(id_, n)

        def recovered():
            s = c.call('status')
            return s['maintenanceWorker'] and s['lastCheckpointSucceeded'] and s['lastCheckpointAtMs'] > killed_at
        self.wait_for(recovered, 30, 'checkpoint after PostgreSQL reconnect')

        def archived():
            items, sources = c.full_history(id_)
            return sources == {'postgres'} and len(items) == 5
        self.wait_for(archived, 30, 'eviction to PostgreSQL with all 5 rows')
        self.assertIn('[WORKER_POSTGRES_RECONNECTED]', self.server_log())


if __name__ == '__main__':
    unittest.main()
