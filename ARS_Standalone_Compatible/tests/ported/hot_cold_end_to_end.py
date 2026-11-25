#!/usr/bin/env python3
"""Automated black-box lifecycle test of a RUNNING, dedicated ARS test server.

No Redis/SQL driver needed; uses only Python standard library and ARS TCP protocol.
No token is required. Optionally set ARS_TEST_HOST/ARS_TEST_PORT. The server config
must use an isolated PostgreSQL DB and Redis key prefix.
"""
import json
import os
import socket
import time
import uuid

HOST = os.environ.get('ARS_TEST_HOST', '127.0.0.1')
PORT = int(os.environ.get('ARS_TEST_PORT', '7091'))
CHANNEL = 'ars:test:live'


def now_ms():
    return int(time.time() * 1000)


def check(value, description):
    if not value:
        raise AssertionError(description)
    print('[PASS]', description, flush=True)


class Client:
    def __init__(self):
        self.socket = socket.create_connection((HOST, PORT), 10)
        self.socket.settimeout(12)
        self.reader = self.socket.makefile('rb')
        self.events = []
        self.call('hello')

    def send(self, op, **args):
        """Send without waiting; pair with receive(request_id)."""
        request_id = uuid.uuid4().hex
        self.socket.sendall((json.dumps({
            'requestId': request_id, 'op': op, 'args': args
        }) + '\n').encode('utf-8'))
        return request_id

    def call(self, op, **args):
        return self.receive(self.send(op, **args), op)

    def receive(self, request_id, op='request'):
        while True:
            line = self.reader.readline()
            if not line:
                raise ConnectionError('Server disconnected during ' + op)
            message = json.loads(line)
            if message.get('event') == 'record':
                self.events.append(message)
                continue
            if message.get('requestId') != request_id:
                raise AssertionError('Unexpected request ID: ' + str(message))
            if not message.get('ok'):
                raise RuntimeError(f'{op} failed: {message.get("error")}')
            return message['result']

    def close(self):
        self.reader.close()
        self.socket.close()


def record(id_, number, *, old=False):
    return {
        'id': id_, 'timestamp': now_ms() - (300_000 if old else 0),
        'source': 'AutomatedLifecycleTest',
        'latitude': 35.71 + number * 0.00001,
        'features': {'counter': number, 'phase': 'old-event' if old else 'normal'}
    }


def wait_recovery(client, pending, timeout=60):
    job_id = pending.get('recoveryJobId') or pending.get('jobId')
    if not job_id:
        raise AssertionError('Asynchronous recovery response did not include jobId')
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = client.call('recovery_status', jobId=job_id)
        if state['status'] == 'completed':
            return state
        if state['status'] == 'failed':
            raise AssertionError('Recovery failed: ' + str(state.get('error')))
        time.sleep(0.05)
    raise AssertionError('Recovery job timed out: ' + job_id)


def full_history(reader, id_):
    data = []
    cursor = ''
    until = ''
    first_source = ''
    while True:
        result = reader.call('history', id=id_, limit=100,
                             timeField='receivedAt', afterSequence=cursor,
                             untilSequence=until)
        first_source = first_source or result['source']
        data.extend(result['items'])
        if not result['hasMore']:
            break
        cursor = result['nextSequence']
        until = result['untilSequence']
    return first_source, data


def main():
    prefix = 'E2E_' + uuid.uuid4().hex[:12]
    hot, cold, old, removable, silent, bulk = (prefix + suffix for suffix in
                                              ('_HOT', '_COLD', '_OLD', '_DELETE', '_SILENT', '_BULK'))
    writer = Client()
    reader = Client()
    ids = (hot, cold, old, removable, silent, bulk)
    print('[TEST] IDs:', ', '.join(ids), flush=True)
    try:
        status = writer.call('status')
        idle = status['inactiveAfterSec']
        period = status['checkpointEverySec']
        check(status['hotColdEnabled'] and status['redisCache'] and status['postgres'],
              'Redis hot store and PostgreSQL are available')
        check(1 <= idle <= 15 and 1 <= period <= 10,
              'Use dedicated test server: inactiveAfterSec<=15 and checkpointEverySec<=10')
        reader.call('subscribe', channel=CHANNEL)
        first = writer.call('publish', channel=CHANNEL, record=record(cold, 1))
        second = writer.call('publish', channel=CHANNEL, record=record(cold, 2))
        check(first['storage'] == 'redis' and second['storage'] == 'redis',
              'Repeated cold ID starts in Redis')
        check(first['sequence'] != second['sequence'],
              'Repeated ID gets two unique history sequences')
        # Search every active ID (latest only) and all historical rows currently in RAM.
        hot_search = reader.call('search_hot', mode='latest', idContains=cold,
                                 filters=[{'field': 'source', 'op': 'eq', 'value': 'AutomatedLifecycleTest'}],
                                 limit=20, scanBudget=120)
        check(any(x['record']['id'] == cold for x in hot_search['items']),
              'RAM search returns matching hot latest record with a field filter')
        all_rows = []
        cursor = '0:0'
        for _ in range(200):
            page = reader.call('search_hot', mode='records', idContains=cold,
                               cursor=cursor, limit=1, scanBudget=20)
            all_rows.extend(x for x in page['items'] if x['record']['id'] == cold)
            if not page['hasMore']:
                break
            cursor = page['nextCursor']
        else:
            raise AssertionError('RAM history search cursor did not terminate')
        check(len(all_rows) == 2, 'RAM search across all records supports pagination')
        recent = reader.call('recent', id=cold, lookbackSec=1800, limit=1)
        check(recent['source'] == 'redis' and recent['indexed'] and
              len(recent['items']) == 1 and recent['hasMore'],
              'Recent hot history uses the Redis received-time index')
        recent_next = reader.call('recent', id=cold, fromMs=recent['fromMs'],
                                  toMs=recent['toMs'], offset=recent['nextOffset'], limit=1)
        check(len(recent_next['items']) == 1 and not recent_next['hasMore'],
              'Recent history pagination keeps its time window fixed')
        empty = reader.call('recent', id=cold, fromMs=first['receivedAtMs'] + 3600_000,
                            toMs=first['receivedAtMs'] + 7200_000, limit=50)
        check(len(empty['items']) == 0, 'Recent history excludes records outside requested time window')
        # Reader receives live messages and retains the event data.
        reader.call('status')  # drains asynchronous events into reader.events
        check(len([e for e in reader.events if e['record']['id'] == cold]) >= 2,
              'Independent receiver gets live events')
        old_ack = writer.call('publish', channel=CHANNEL, record=record(old, 1, old=True))
        old_history = reader.call('history', id=old, limit=10,
                                  timeField='receivedAt', fromMs=old_ack['receivedAtMs'] - 2000)
        check(len(old_history['items']) == 1 and
              old_history['items'][0]['record']['timestamp'] < old_ack['receivedAtMs'] - 240_000,
              'Server receivedAtMs is independent of delayed event timestamp')
        old_event_history = reader.call('history', id=old, limit=10,
                                        timeField='timestamp', fromMs=old_ack['receivedAtMs'] - 2000)
        check(len(old_event_history['items']) == 0,
              'Different timestamp vs receivedAt history filters')
        writer.call('publish', channel=CHANNEL, record=record(removable, 1))
        saved = writer.call('save', record=record(silent, 1))
        reader.call('status')
        silent_source, silent_items = full_history(reader, silent)
        check(saved['stored'] and len(silent_items) == 1 and silent_source == 'redis',
              'save creates history even without live publishing')
        check(not any(e['record']['id'] == silent for e in reader.events),
              'save does not produce a live notification')
        # Cross the 100-record checkpoint batch boundary and history pagination boundary.
        for n in range(205):
            writer.call('save', record=record(bulk, n))
        bulk_source, before_archive = full_history(reader, bulk)
        check(bulk_source == 'redis' and len(before_archive) == 205,
              '205 records for one ID stay in Redis across 100-record history pages')
        # Keep HOT alive throughout the checkpoint window. COLD remains silent.
        started = time.monotonic()
        idx = 0
        while time.monotonic() - started < max(idle + 3, period + 4):
            idx += 1
            writer.call('publish', channel=CHANNEL, record=record(hot, idx))
            time.sleep(min(0.7, idle / 3))
        active_source, active_records = full_history(reader, hot)
        check(active_source == 'redis' and len(active_records) >= 2,
              'Frequently updated ID remains in Redis with full history')
        status = writer.call('status')
        check(status['lastCheckpointSucceeded'] and status['lastCheckpointAtMs'] > 0,
              'Periodic checkpoint completed without evicting active IDs')
        # Cold ID should be archived automatically after >=idle seconds.
        deadline = time.monotonic() + max(12, 2 * idle)
        while True:
            source, archived = full_history(reader, cold)
            if source == 'postgres':
                break
            if time.monotonic() > deadline:
                raise AssertionError('COLD ID did not become PostgreSQL-only after inactivity')
            writer.call('publish', channel=CHANNEL, record=record(hot, idx + 1))
            time.sleep(0.4)
        check(len(archived) == 2, 'COLD history fully archived and no longer active in Redis')
        cold_window = reader.call('recent', id=cold, lookbackSec=1800, limit=50)
        check(cold_window['source'] == 'postgres' and len(cold_window['items']) == 2,
              'Inactive ID recent history reads directly from PostgreSQL, without restoring old RAM data')
        check(archived[0]['sequence'] == first['sequence'] and
              archived[1]['sequence'] == second['sequence'],
              'Archive preserved original sequence and ordering')
        check(archived[0]['receivedAtMs'] == first['receivedAtMs'],
              'Archive preserved original server receive time')
        bulk_source, bulk_archived = full_history(reader, bulk)
        check(bulk_source == 'postgres' and len(bulk_archived) == 205,
              'Large inactive ID archives all 205 records across checkpoint batches')
        bulk_new_record = record(bulk, 205)
        # The server holds a write to a cold ID until the worker has restored its history,
        # then stores it and only then acknowledges it (ok:true always means stored).
        bulk_request = writer.send('save', record=bulk_new_record)
        # A different hot ID must remain writable while the large ID is being restored.
        other = Client()
        try:
            started = time.monotonic()
            other.call('save', record=record(hot, idx + 2))
            unrelated_latency = time.monotonic() - started
        finally:
            other.close()
        check(unrelated_latency < 1.5,
              'Unrelated hot write remains responsive during background recovery')
        bulk_reopened = writer.receive(bulk_request, 'save')
        bulk_source, bulk_restored = full_history(reader, bulk)
        check(bulk_reopened['stored'] and bulk_reopened['restoredFromPostgres']
              and bulk_reopened['restoredHistoryCount'] == 205
              and bulk_source == 'redis' and len(bulk_restored) == 206,
              'Write to a large cold ID is acknowledged only after its 205-row history is restored')
        print('[TEST] COLD remained silent; now reactivating it', flush=True)
        comeback_record = record(cold, 3)
        comeback = writer.call('publish', channel=CHANNEL, record=comeback_record)
        check(comeback['storage'] == 'redis' and comeback['stored']
              and comeback['restoredHistoryCount'] == 2,
              'Reactivation publish is acknowledged only after complete COLD history recovery')
        source, restored = full_history(reader, cold)
        check(source == 'redis' and len(restored) == 3,
              'Reactivated ID has old and new entries in Redis')
        restored_window = reader.call('recent', id=cold, lookbackSec=1800, limit=50)
        check(restored_window['source'] == 'redis' and restored_window['indexed'] and
              len(restored_window['items']) == 3,
              'Reactivated ID has a rebuilt received-time index for the complete restored history')
        check(restored[0]['sequence'] == first['sequence'] and
              restored[2]['sequence'] == comeback['sequence'],
              'Reactivation keeps earlier historical sequence values')
        # Test explicit cache eviction, restore and full deletion.
        evicted = reader.call('delete', id=cold, removeFromDisk=False)
        check(evicted['archivePreserved'], 'Cache-only eviction keeps database archive')
        source, archived_again = full_history(reader, cold)
        check(source == 'postgres' and len(archived_again) == 3,
              'Cache eviction checkpointed all pending changes')
        hydrate_pending = reader.call('hydrate', id=cold)
        if hydrate_pending.get('status') == 'queued':
            wait_recovery(reader, hydrate_pending)
        source, hydrated = full_history(reader, cold)
        check(source == 'redis' and len(hydrated) == 3,
              'Explicit hydrate returns complete SQL history to Redis')
        reader.call('delete', id=cold, removeFromDisk=True)
        check(not reader.call('latest', id=cold)['found'] and
              not reader.call('history', id=cold)['items'],
              'Permanent delete removes ID from Redis and PostgreSQL')
        print('\nALL END-TO-END SCENARIOS PASSED', flush=True)
    finally:
        # Do not clean unrelated IDs; test only its own random prefix.
        for id_ in ids:
            try:
                writer.call('delete', id=id_, removeFromDisk=True)
            except Exception as ex:
                print('[CLEANUP WARNING]', id_, str(ex), flush=True)
        writer.close()
        reader.close()


if __name__ == '__main__':
    main()
