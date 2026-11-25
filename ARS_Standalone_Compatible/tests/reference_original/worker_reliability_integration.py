#!/usr/bin/env python3
"""Black-box Worker regression test for a dedicated isolated ARS test instance.

Required: ARS_API_TOKEN, ARS_TEST_PORT, ARS_TEST_HOST (optional).
The server INI must set memory/maxActiveIds=1, lifecycle/inactiveAfterSec=2,
scanEveryMs <= 1000, and use a disposable Redis DB/key prefix and PostgreSQL DB.
Optional fault injection (only on a disposable test environment):
  ARS_TEST_REDIS_CLI=/usr/bin/redis-cli enables CLIENT PAUSE timeout/reconnect test.
  ARS_TEST_PG_CTL=/usr/bin/pg_ctl and ARS_TEST_PGDATA=/path/to/test/cluster enable
  PostgreSQL stop/start reconnect test. Set ARS_ALLOW_DB_RESTART_TEST=1 explicitly.
"""
import json
import os
import socket
import subprocess
import time
import uuid

HOST = os.environ.get('ARS_TEST_HOST', '127.0.0.1')
PORT = int(os.environ.get('ARS_TEST_PORT', '7091'))
TOKEN = os.environ['ARS_API_TOKEN']


def check(condition, message):
    if not condition:
        raise AssertionError(message)
    print('[PASS]', message, flush=True)


class Client:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=10)
        self.sock.settimeout(15)
        self.reader = self.sock.makefile('rb')
        self.call('auth', token=TOKEN)

    def call(self, op, **args):
        rid = uuid.uuid4().hex
        payload = {'requestId': rid, 'op': op, 'args': args}
        self.sock.sendall((json.dumps(payload) + '\n').encode())
        while True:
            line = self.reader.readline()
            if not line:
                raise ConnectionError('server closed during ' + op)
            msg = json.loads(line)
            if msg.get('requestId') == rid:
                return msg

    def ok(self, op, **args):
        result = self.call(op, **args)
        check(result.get('ok'), f'{op} succeeded: {result}')
        return result['result']

    def close(self):
        self.reader.close()
        self.sock.close()


def wait_until_cold(client, id_, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        latest = client.ok('latest', id=id_)
        if latest.get('found') and latest.get('source') == 'postgres':
            return
        time.sleep(0.2)
    raise AssertionError('ID did not become cold: ' + id_)


def wait_recovery(client, response, timeout=45):
    result = response.get('result', {})
    job = result.get('recoveryJobId') or result.get('jobId')
    check(bool(job), 'cold write/hydrate returns recovery job ID')
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = client.ok('recovery_status', jobId=job)
        if state['status'] == 'completed':
            return state
        if state['status'] == 'failed':
            raise AssertionError('Recovery failed: ' + str(state))
        time.sleep(0.1)
    raise AssertionError('Recovery job timeout: ' + job)


def read_all(client, id_):
    items, after, until = [], '', ''
    while True:
        page = client.ok('history', id=id_, limit=100,
                         timeField='receivedAt', afterSequence=after,
                         untilSequence=until)
        items.extend(page['items'])
        if not page['hasMore']:
            break
        after, until = page['nextSequence'], page['untilSequence']
    return items


def main():
    client = Client()
    try:
        status = client.ok('status')
        check(status['maintenanceWorker'], 'Worker starts healthy with Redis and PostgreSQL')

        cold_id = 'worker_cold_' + uuid.uuid4().hex
        client.ok('save', record={'id': cold_id, 'timestamp': int(time.time()*1000), 'n': 0})
        wait_until_cold(client, cold_id)

        blocker = 'worker_blocker_' + uuid.uuid4().hex
        client.ok('save', record={'id': blocker, 'timestamp': int(time.time()*1000), 'n': 0})
        hydrate = client.call('hydrate', id=cold_id)
        check(not hydrate.get('ok') and hydrate.get('code') == 'HOT_CAPACITY_REACHED',
              'explicit hydrate returns structured HOT_CAPACITY_REACHED')
        rejected = client.call('save', record={'id': 'worker_overflow_' + uuid.uuid4().hex,
                                               'timestamp': int(time.time()*1000), 'n': 1})
        check(not rejected.get('ok') and rejected.get('code') == 'HOT_CAPACITY_REACHED',
              'new hot ID returns structured capacity error')
        client.ok('delete', id=blocker, removeFromDisk=False)

        # Repeated inactive/active transitions exercise the check-then-EVAL race.
        race_id = 'worker_race_' + uuid.uuid4().hex
        expected = 0
        for n in range(5):
            if n:
                wait_until_cold(client, race_id)
            response = client.call('save', record={'id': race_id,
                'timestamp': int(time.time()*1000), 'n': n})
            if not response.get('ok'):
                raise AssertionError('save failed: ' + str(response))
            result = response['result']
            if result.get('recoveryPending'):
                wait_recovery(client, response)
                response = client.call('save', record={'id': race_id,
                    'timestamp': int(time.time()*1000), 'n': n})
                check(response.get('ok') and response['result'].get('stored'),
                      'write is accepted only after recovery completes')
            else:
                check(result.get('stored'), 'new hot write stored')
            expected += 1
            if n < 4:
                time.sleep(2.3)
        history = read_all(client, race_id)
        check(len(history) == expected, 'race/recovery cycles preserve every accepted record')

        redis_cli = os.environ.get('ARS_TEST_REDIS_CLI')
        if redis_cli:
            host = os.environ.get('ARS_REDIS_HOST', '127.0.0.1')
            port = os.environ.get('ARS_REDIS_PORT', '6379')
            args = [redis_cli, '-h', host, '-p', port]
            password = os.environ.get('ARS_REDIS_PASSWORD')
            if password:
                args += ['-a', password]
            args += ['CLIENT', 'PAUSE', '3500']
            subprocess.run(args, check=True, capture_output=True, text=True)
            time.sleep(6)
            recovered = client.ok('status')
            check(recovered['maintenanceWorker'],
                  'Worker discards timed-out RESP socket and reconnects cleanly')
        else:
            print('[SKIP] Redis CLIENT PAUSE fault injection (ARS_TEST_REDIS_CLI unset)')

        pg_ctl, pgdata = os.environ.get('ARS_TEST_PG_CTL'), os.environ.get('ARS_TEST_PGDATA')
        if pg_ctl and pgdata and os.environ.get('ARS_ALLOW_DB_RESTART_TEST') == '1':
            subprocess.run([pg_ctl, '-D', pgdata, '-m', 'fast', '-w', 'stop'], check=True)
            time.sleep(3)
            subprocess.run([pg_ctl, '-D', pgdata, '-w', 'start'], check=True)
            deadline = time.monotonic() + 45
            ready = False
            while time.monotonic() < deadline:
                try:
                    state = client.call('status').get('result', {})
                    # Both connections must be back: the worker's AND the client thread's
                    # (the latter also re-acquires the single-writer lock).
                    ready = bool(state.get('maintenanceWorker') and state.get('postgres'))
                except Exception:
                    ready = False
                if ready:
                    break
                time.sleep(1)
            check(ready, 'Worker and client-thread PostgreSQL connections both reconnect')
            fresh = 'worker_after_pg_restart_' + uuid.uuid4().hex
            stored = client.ok('save', record={'id': fresh, 'timestamp': int(time.time()*1000), 'n': 0})
            check(stored.get('stored'), 'New-ID write (needs the client-thread SQL connection) works after restart')
        else:
            print('[SKIP] PostgreSQL stop/start fault injection (explicit opt-in not supplied)')
    finally:
        client.close()


if __name__ == '__main__':
    main()
