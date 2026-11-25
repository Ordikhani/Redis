#!/usr/bin/env python3
"""Black-box tests against a RUNNING test server. Uses unique IDs and cleans them.
No client token. ARS_TEST_HOST/ARS_TEST_PORT optional. Never run on production.
"""
import json
import os
import socket
import time
import uuid

HOST = os.environ.get('ARS_TEST_HOST', '127.0.0.1')
PORT = int(os.environ.get('ARS_TEST_PORT', '7090'))

class Peer:
    def __init__(self):
        self.socket = socket.create_connection((HOST, PORT), timeout=15)
        self.file = self.socket.makefile('rb')
        self.events = []
        self.hello = self.call('hello')
    def receive(self):
        raw = self.file.readline()
        if not raw:
            raise EOFError('Connection closed')
        return json.loads(raw)
    def call(self, op, **args):
        request_id = uuid.uuid4().hex
        data = json.dumps(dict(requestId=request_id, op=op, args=args)).encode() + b'\n'
        # Deliberately fragment TCP frames to exercise framing.
        self.socket.sendall(data[:7])
        self.socket.sendall(data[7:])
        while True:
            reply = self.receive()
            if 'event' in reply:
                self.events.append(reply)
                continue
            assert reply['requestId'] == request_id, reply
            return reply
    def ok(self, op, **args):
        reply = self.call(op, **args)
        assert reply['ok'], reply
        return reply['result']
    def close(self):
        self.file.close()
        self.socket.close()

count = 0
def check(condition, description):
    global count
    assert condition, description
    count += 1
    print('PASS', description)

def main():
    writer, reader = Peer(), Peer()
    record_id = 'test_' + uuid.uuid4().hex
    channel = 'test:' + uuid.uuid4().hex
    try:
        check(writer.hello['ok'] and reader.hello['ok'], 'Two independent token-free clients')
        check(writer.ok('status')['postgres'], 'PostgreSQL healthy')
        check(not reader.ok('latest', id=record_id)['found'], 'Unknown ID has found=false')
        reader.ok('subscribe', channel=channel)
        now = int(time.time()*1000)
        record = dict(id=record_id, timestamp=now, features={'version': 1}, payload='AQID')
        first = writer.ok('publish', channel=channel, record=record)
        event = reader.receive()
        check(event['event'] == 'record' and event['record'] == record, 'Live message after committed write')
        check(reader.ok('latest', id=record_id)['record'] == record, 'Other client sees shared latest')
        second = writer.ok('save', record={**record, 'features': {'version': 2}})
        check(first['sequence'] != second['sequence'], 'Same ID/millisecond records retain unique sequence')
        check(reader.ok('latest', id=record_id)['record']['features']['version'] == 2, 'Tie broken by sequence; cache invalidated')
        writer.ok('save', record={**record, 'timestamp': now-600000, 'features': {'version': 0}})
        check(reader.ok('latest', id=record_id)['record']['features']['version'] == 2, 'Late old record does not regress latest')
        page = reader.ok('history', id=record_id, limit=1, timeField='receivedAt', fromMs=now-1000)
        total = list(page['items'])
        while page['hasMore']:
            page = reader.ok('history', id=record_id, limit=1, timeField='receivedAt', fromMs=now-1000,
                             afterSequence=page['nextSequence'], untilSequence=page['untilSequence'])
            total.extend(page['items'])
        check(len(total) == 3 and len({x['sequence'] for x in total}) == 3, 'Pagination preserves same-time records')
        by_event = reader.ok('history', id=record_id, fromMs=now-1000, toMs=now+1000)
        check(len(by_event['items']) == 2, 'Event-time filtering differs from server receive time')
        check(not reader.call('history', id=record_id, limit=-1)['ok'], 'Invalid history limit rejected')
        check(not writer.call('save', record={'timestamp': now})['ok'], 'Missing ID rejected')
        check(not reader.call('history', id=record_id, timeField='SQL injection')['ok'], 'Unknown time selector rejected')
        reader.ok('unsubscribe', channel=channel)
        # TCP ordering: if incorrectly subscribed, event will precede the status reply.
        writer.ok('publish', channel=channel, record=record)
        reader.ok('status')
        check(not reader.events, 'Unsubscribe stops delivery')
        reader.ok('delete', id=record_id, removeFromDisk=False)
        check(reader.ok('latest', id=record_id)['found'], 'Cache-only delete preserves archive and fallback')
        check(reader.ok('hydrate', id=record_id)['found'], 'Explicit hydration returns archived latest')
        reader.ok('delete', id=record_id, removeFromDisk=True)
        check(not reader.ok('latest', id=record_id)['found'], 'Archive delete invalidates cached latest')
        check(not reader.ok('history', id=record_id)['items'], 'Archive history deleted')
        check(not reader.call('not-an-operation')['ok'], 'Unknown operation rejected')
        if os.environ.get('ARS_RUN_LIFECYCLE') == '1':
            # Run only with a dedicated test config: inactiveAfterSec=2 and scanEveryMs=200.
            status = writer.ok('status')
            check(status['hotColdEnabled'] and status['inactiveAfterSec'] <= 2,
                  'Lifecycle test server has Redis hot mode and <=2s inactivity')
            cold_id = 'lifecycle_' + uuid.uuid4().hex
            try:
                history = []
                item_count = int(os.environ.get('ARS_HISTORY_TEST_COUNT', '25'))
                if item_count < 1 or item_count > 1000:
                    raise ValueError('ARS_HISTORY_TEST_COUNT must be 1..1000')
                for n in range(item_count):
                    item = dict(id=cold_id, timestamp=now + n, features={'index': n})
                    stored = writer.ok('save', record=item)
                    check(stored['storage'] == 'redis', 'Hot record accepted into Redis') if n == 0 else None
                    history.append((item, stored['sequence'], stored['receivedAtMs']))
                active_page = reader.ok('history', id=cold_id, limit=100)
                check(active_page['source'] == 'redis',
                      'Active ID history is served by Redis')
                time.sleep(2.3)
                deadline = time.time() + 20
                while True:
                    cold_page = reader.ok('history', id=cold_id, limit=100)
                    if cold_page['source'] == 'postgres':
                        break
                    if time.time() > deadline:
                        raise AssertionError('Inactive ID was not archived within test deadline')
                    time.sleep(0.3)
                check(cold_page['source'] == 'postgres',
                      'Inactive ID archived in PostgreSQL and evicted from Redis')
                def get_all(id):
                    p = reader.ok('history', id=id, limit=100)
                    found = list(p['items'])
                    while p['hasMore']:
                        p = reader.ok('history', id=id, limit=100, afterSequence=p['nextSequence'],
                                      untilSequence=p['untilSequence'])
                        found.extend(p['items'])
                    return found
                cold_items = get_all(cold_id)
                check(len(cold_items) == item_count, 'All records archived, without loss')
                for actual, expected in zip(cold_items, history):
                    assert actual['sequence'] == expected[1]
                    assert actual['receivedAtMs'] == expected[2]
                check(True, 'Archive keeps original server receive time and sequence')
                resumed = writer.ok('save', record=dict(id=cold_id, timestamp=now + 100, features={'index':25}))
                check(resumed['storage'] == 'redis', 'Reactivated ID stored in Redis')
                resumed_page = reader.ok('history', id=cold_id, limit=100)
                check(resumed_page['source'] == 'redis', 'Reactivated history served from Redis')
                resumed_items = get_all(cold_id)
                check(len(resumed_items) == item_count + 1,
                      'Reactivation restores the COMPLETE history before appending new data')
            finally:
                writer.ok('delete', id=cold_id, removeFromDisk=True)
    finally:
        try:
            writer.ok('delete', id=record_id, removeFromDisk=True)
        finally:
            writer.close()
            reader.close()
    print(f'{count} checks passed')

if __name__ == '__main__':
    main()
