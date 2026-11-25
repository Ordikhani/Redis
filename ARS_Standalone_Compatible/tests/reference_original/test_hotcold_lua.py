"""Unit tests for the Redis Lua scripts shared by HotColdStore and HotColdWorker.

The scripts are read verbatim from central/server/HotColdScripts.h and executed on a
throw-away redis-server, so these tests check the exact text the C++ server sends.
Needs only Python 3 and a redis-server binary (or set ARS_TEST_REDIS_SERVER).
No ARS build, Qt, PostgreSQL or project data is touched.

    python3 -m unittest discover -s central/tests -p 'test_hotcold_lua.py' -v
"""
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

HEADER = Path(__file__).resolve().parents[1] / 'server' / 'HotColdScripts.h'
REDIS = os.environ.get('ARS_TEST_REDIS_SERVER') or shutil.which('redis-server')


def load_scripts():
    text = HEADER.read_text(encoding='utf-8')
    found = dict(re.findall(r'inline constexpr const char\* (\w+) = R"LUA\((.*?)\)LUA";', text, re.S))
    missing = {'append', 'evict', 'advanceSynced', 'stageBatch', 'publishRecovery'} - found.keys()
    if missing:
        raise AssertionError(f'scripts missing from {HEADER}: {sorted(missing)}')
    return found


class Resp:
    """Minimal RESP2 client: enough for EVAL and a few inspection commands."""

    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.file = self.sock.makefile('rb')

    def close(self):
        self.file.close()
        self.sock.close()

    def call(self, *args):
        out = [b'*%d\r\n' % len(args)]
        for a in args:
            b = a if isinstance(a, bytes) else str(a).encode()
            out.append(b'$%d\r\n%s\r\n' % (len(b), b))
        self.sock.sendall(b''.join(out))
        return self._read()

    def _read(self):
        line = self.file.readline()
        kind, rest = line[:1], line[1:-2]
        if kind == b'+':
            return rest.decode()
        if kind == b'-':
            raise RuntimeError(rest.decode())
        if kind == b':':
            return int(rest)
        if kind == b'$':
            n = int(rest)
            if n < 0:
                return None
            data = self.file.read(n + 2)[:-2]
            return data.decode()
        if kind == b'*':
            n = int(rest)
            return None if n < 0 else [self._read() for _ in range(n)]
        raise RuntimeError(f'bad reply {line!r}')


@unittest.skipUnless(REDIS, 'redis-server binary not found')
class HotColdLuaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scripts = load_scripts()
        cls.tmp = tempfile.TemporaryDirectory()
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            cls.port = s.getsockname()[1]
        cls.proc = subprocess.Popen(
            [REDIS, '--port', str(cls.port), '--bind', '127.0.0.1', '--save', '',
             '--appendonly', 'no', '--dir', cls.tmp.name],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 10
        while True:
            try:
                cls.r = Resp(cls.port)
                cls.r.call('PING')
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        cls.r.close()
        cls.proc.terminate()
        cls.proc.wait(10)
        cls.tmp.cleanup()

    def setUp(self):
        self.r.call('FLUSHALL')
        self.next_seq = 1000
        self.id = 'TARGET-101'

    # Key layout mirrors HotColdStore::key(): prefix:hotv1:kind[:base64url(id)].
    def k(self, kind, per_id=True):
        return f'test:hotv1:{kind}:{self.id}' if per_id else f'test:hotv1:{kind}'

    def append(self, timestamp=None, expect_active=False, received=None, cap=0):
        self.next_seq += 1
        received = received or int(time.time() * 1000)
        item = {'record': {'id': self.id, 'timestamp': timestamp or received},
                'sequence': str(self.next_seq), 'receivedAtMs': received}
        reply = self.r.call('EVAL', self.scripts['append'], 4,
                            self.k('history'), self.k('latest'), self.k('lastSeen', False), self.k('recvIndex'),
                            json.dumps(item, separators=(',', ':')), item['record']['timestamp'], received,
                            self.id, '1' if expect_active else '0', cap)
        return reply, item

    def advance(self, old, new, first_seq, last_seq):
        return self.r.call('EVAL', self.scripts['advanceSynced'], 2, self.k('synced', False), self.k('history'),
                           self.id, old, new, first_seq, last_seq)

    def evict(self, cutoff):
        return self.r.call('EVAL', self.scripts['evict'], 5, self.k('history'), self.k('latest'),
                           self.k('lastSeen', False), self.k('synced', False), self.k('recvIndex'),
                           self.id, cutoff)

    def history_len(self):
        return self.r.call('LLEN', self.k('history'))

    def synced(self):
        value = self.r.call('HGET', self.k('synced', False), self.id)
        return int(value) if value is not None else 0

    def seq_at(self, index):
        return json.loads(self.r.call('LINDEX', self.k('history'), index))['sequence']

    # ---- append -------------------------------------------------------------
    def test_append_builds_history_indexes_and_latest_by_event_time(self):
        self.assertEqual(self.append(timestamp=2000)[0], 1)
        self.assertEqual(self.append(timestamp=1000)[0], 2)  # older event arrives later
        self.assertEqual(self.history_len(), 2)
        self.assertEqual(self.r.call('ZCARD', self.k('recvIndex')), 2)
        latest = json.loads(self.r.call('GET', self.k('latest')))
        self.assertEqual(latest['record']['timestamp'], 2000)
        self.assertIsNotNone(self.r.call('ZSCORE', self.k('lastSeen', False), self.id))

    def test_append_expecting_active_succeeds_while_hot(self):
        self.append()
        self.assertEqual(self.append(expect_active=True)[0], 2)

    def test_append_expecting_active_after_eviction_writes_nothing(self):
        """The save/evict race: the server saw the ID as hot, the worker evicted it."""
        _, first = self.append(received=1000)
        self.assertEqual(self.advance(0, 1, first['sequence'], first['sequence']), 1)
        self.assertEqual(self.evict(cutoff=5000), 1)
        reply, _ = self.append(expect_active=True)
        self.assertEqual(reply, -1)
        self.assertEqual(self.r.call('EXISTS', self.k('history'), self.k('latest'), self.k('recvIndex')), 0)
        self.assertIsNone(self.r.call('ZSCORE', self.k('lastSeen', False), self.id))

    def test_old_behaviour_would_start_a_one_row_history(self):
        """Documents what the race did before the fix (append without the guard)."""
        _, first = self.append(received=1000)
        self.advance(0, 1, first['sequence'], first['sequence'])
        self.evict(cutoff=5000)
        self.assertEqual(self.append(expect_active=False)[0], 1)  # unguarded path
        self.assertEqual(self.history_len(), 1)

    def test_append_enforces_max_active_ids_for_new_ids_only(self):
        self.assertEqual(self.append(cap=1)[0], 1)
        self.assertEqual(self.append(expect_active=True, cap=1)[0], 2)  # already hot: never refused
        self.id = 'TARGET-202'
        self.assertEqual(self.append(cap=1)[0], -2)
        self.assertEqual(self.r.call('EXISTS', self.k('history'), self.k('latest')), 0)
        self.assertEqual(self.r.call('ZCARD', self.k('lastSeen', False)), 1)

    # ---- recovery: stageBatch + publishRecovery ---------------------------------
    def stage(self, rows, first_index):
        args = ['EVAL', self.scripts['stageBatch'], 2, self.k('staging'), self.k('staging_index'),
                3600, len(rows), first_index]
        for row in rows:
            args += [json.dumps(row, separators=(',', ':')), row['receivedAtMs']]
        return self.r.call(*args)

    def publish(self, count, best, cap=0):
        return self.r.call('EVAL', self.scripts['publishRecovery'], 7, self.k('staging'), self.k('history'),
                           self.k('latest'), self.k('synced', False), self.k('lastSeen', False),
                           self.k('staging_index'), self.k('recvIndex'),
                           count, json.dumps(best, separators=(',', ':')), self.id, int(time.time() * 1000), cap)

    def archived_rows(self, n):
        return [{'record': {'id': self.id, 'timestamp': 1000 + i}, 'sequence': str(500 + i),
                 'receivedAtMs': 2000 + i} for i in range(n)]

    def test_recovery_stages_in_batches_and_publishes_atomically(self):
        rows = self.archived_rows(5)
        self.assertEqual(self.stage(rows[:3], 0), 3)
        self.assertEqual(self.stage(rows[3:], 3), 2)
        self.assertGreater(self.r.call('TTL', self.k('staging')), 0)  # crash leaves no permanent garbage
        self.assertEqual(self.publish(5, rows[-1]), 1)
        self.assertEqual(self.history_len(), 5)
        self.assertEqual(self.r.call('TTL', self.k('history')), -1)  # active history never expires
        self.assertEqual(self.synced(), 5)  # restored rows are already archived
        self.assertEqual(self.r.call('ZRANGE', self.k('recvIndex'), 0, -1),
                         ['%016d' % i for i in range(5)])
        self.assertEqual(self.r.call('EXISTS', self.k('staging'), self.k('staging_index')), 0)
        # The restored ID accepts a guarded append, and a later archive pass can evict it again.
        self.assertEqual(self.append(expect_active=True)[0], 6)

    def test_recovery_publish_refuses_when_already_active_or_full(self):
        rows = self.archived_rows(2)
        self.stage(rows, 0)
        self.r.call('ZADD', self.k('lastSeen', False), 1, 'SOMEONE-ELSE')
        self.assertEqual(self.publish(2, rows[-1], cap=1), -1)
        self.assertEqual(self.r.call('EXISTS', self.k('history')), 0)
        self.r.call('ZADD', self.k('lastSeen', False), 1, self.id)
        self.assertEqual(self.publish(2, rows[-1]), 0)
        self.assertEqual(self.r.call('EXISTS', self.k('history')), 0)

    # ---- advanceSynced -------------------------------------------------------
    def fill(self, count):
        return [self.append()[1] for _ in range(count)]

    def test_advance_moves_offset_for_matching_batch(self):
        rows = self.fill(5)
        self.assertEqual(self.advance(0, 3, rows[0]['sequence'], rows[2]['sequence']), 1)
        self.assertEqual(self.synced(), 3)
        self.assertEqual(self.advance(3, 5, rows[3]['sequence'], rows[4]['sequence']), 1)
        self.assertEqual(self.synced(), 5)

    def test_advance_refuses_stale_offset(self):
        rows = self.fill(4)
        self.advance(0, 2, rows[0]['sequence'], rows[1]['sequence'])
        self.assertEqual(self.advance(0, 2, rows[0]['sequence'], rows[1]['sequence']), -1)
        self.assertEqual(self.synced(), 2)

    def test_advance_refuses_rows_beyond_history(self):
        rows = self.fill(2)
        self.assertEqual(self.advance(0, 3, rows[0]['sequence'], 'x'), -2)
        self.assertEqual(self.synced(), 0)

    def test_desynchronised_reply_cannot_mark_unarchived_rows_as_archived(self):
        """A delayed LRANGE reply hands the worker rows [0,2) while it asked for [2,4).
        PostgreSQL received [0,2) again; the offset must not jump to 4, and the
        unarchived rows [2,4) must survive the evict script."""
        rows = self.fill(6)
        self.assertEqual(self.advance(0, 2, rows[0]['sequence'], rows[1]['sequence']), 1)
        stale_first, stale_last = rows[0]['sequence'], rows[1]['sequence']
        self.assertEqual(self.advance(2, 4, stale_first, stale_last), -3)
        self.assertEqual(self.synced(), 2)
        self.assertEqual(self.evict(cutoff=int(time.time() * 1000) + 60_000), 0)
        self.assertEqual(self.history_len(), 6)

    # ---- evict ----------------------------------------------------------------
    def test_evict_requires_full_archive_and_idleness(self):
        rows = self.fill(3)
        future = int(time.time() * 1000) + 60_000
        self.assertEqual(self.evict(cutoff=future), 0)  # nothing archived yet
        self.advance(0, 3, rows[0]['sequence'], rows[2]['sequence'])
        self.assertEqual(self.evict(cutoff=0), 0)  # archived, but active after the cutoff
        self.assertEqual(self.evict(cutoff=future), 1)
        self.assertEqual(self.r.call('EXISTS', self.k('history'), self.k('latest'), self.k('recvIndex')), 0)
        self.assertIsNone(self.r.call('HGET', self.k('synced', False), self.id))

    def test_new_row_after_archive_blocks_eviction(self):
        rows = self.fill(2)
        self.advance(0, 2, rows[0]['sequence'], rows[1]['sequence'])
        self.append()
        self.assertEqual(self.evict(cutoff=int(time.time() * 1000) + 60_000), 0)
        self.assertEqual(self.history_len(), 3)


if __name__ == '__main__':
    unittest.main()
