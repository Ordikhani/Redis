#!/usr/bin/env python3
"""Opt-in ARS live integration test for both transports, on a dedicated lab DB.

ARS_RUN_INTEGRATION=1 python3 tests/udp_tcp_e2e.py
Requires a running ARS server with TCP 7090, UDP 7092 and test-only DB.
Never runs on production intentionally: this test writes/deletes its own unique IDs.
"""
import base64
import json
import os
import socket
import time
import uuid
from collections import deque

HOST = os.getenv('ARS_TEST_HOST', '127.0.0.1')
TCP_PORT = int(os.getenv('ARS_TEST_TCP_PORT', '7090'))
UDP_PORT = int(os.getenv('ARS_TEST_UDP_PORT', '7092'))


def check(condition, description):
    if not condition:
        raise AssertionError(description)
    print('[PASS]', description, flush=True)


class Client:
    def __init__(self, mode):
        self.mode = mode
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM if mode == 'udp' else socket.SOCK_STREAM)
        self.socket.settimeout(0.4)
        self.socket.connect((HOST, UDP_PORT if mode == 'udp' else TCP_PORT))
        self.events = deque()
        self._buffer = b''
        self._parts = {}
        self.request('hello')

    def close(self):
        self.socket.close()

    def _write(self, obj):
        encoded = json.dumps(obj, separators=(',', ':')).encode()
        if self.mode == 'tcp':
            self.socket.sendall(encoded + b'\n')
            return
        if len(encoded) <= 1200:
            self.socket.send(encoded)
            return
        ident = uuid.uuid4().hex
        for i, start in enumerate(range(0, len(encoded), 650)):
            chunk = encoded[start:start+650]
            part = {'udpFrame': 1, 'frameId': ident, 'index': i,
                    'count': (len(encoded)+649)//650,
                    'payload': base64.b64encode(chunk).decode()}
            self.socket.send(json.dumps(part, separators=(',', ':')).encode())

    def _read_one(self):
        if self.mode == 'tcp':
            while b'\n' not in self._buffer:
                self._buffer += self.socket.recv(65536)
            line, self._buffer = self._buffer.split(b'\n', 1)
            return json.loads(line)
        data = self.socket.recv(65507)
        packet = json.loads(data)
        if packet.get('udpFrame') != 1:
            return packet
        key = packet['frameId']
        if key not in self._parts:
            self._parts[key] = [None] * packet['count']
        pieces = self._parts[key]
        if len(pieces) != packet['count']:
            del self._parts[key]
            return None
        pieces[packet['index']] = base64.b64decode(packet['payload'])
        if any(chunk is None for chunk in pieces):
            return None
        del self._parts[key]
        return json.loads(b''.join(pieces))

    def _accept(self, packet):
        if packet is None:
            return None
        if packet.get('event') == 'record':
            self.events.append(packet)
            if self.mode == 'udp' and packet.get('eventId'):
                # Acknowledging live events is part of UDP subscriber contract.
                self._write({'requestId': uuid.uuid4().hex, 'op': 'ack_event',
                             'args': {'eventId': packet['eventId']}})
            return None
        return packet

    def request(self, op, *, timeout=15, request_id=None, **kwargs):
        req = {'requestId': request_id or uuid.uuid4().hex, 'op': op, 'args': kwargs}
        deadline = time.monotonic()+timeout
        resend = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= resend:
                self._write(req)
                resend = time.monotonic()+(0.7 if self.mode == 'udp' else timeout+1)
            try:
                response = self._accept(self._read_one())
            except socket.timeout:
                continue
            if response is None or response.get('requestId') != req['requestId']:
                continue
            if not response.get('ok'):
                raise RuntimeError(f'{op}: {response.get("code")} {response.get("error")}')
            return response['result']
        raise TimeoutError(f'{self.mode}/{op} response timed out')

    def wait_live(self, expected_id, timeout=10):
        deadline = time.monotonic()+timeout
        while time.monotonic()<deadline:
            for item in list(self.events):
                if item['record']['id'] == expected_id:
                    self.events.remove(item)
                    return item
            try:
                self._accept(self._read_one())
            except socket.timeout:
                continue
        raise TimeoutError(f'{self.mode} did not receive live event {expected_id}')


def main():
    if HOST not in ('127.0.0.1', 'localhost'):
        raise SystemExit('For safety this test accepts localhost only')
    if os.getenv('ARS_RUN_INTEGRATION') != '1':
        raise SystemExit('Set ARS_RUN_INTEGRATION=1 only on a dedicated test DB')
    clients = [Client('tcp'),Client('udp')]
    prefix = 'E2E_'+uuid.uuid4().hex
    created=[]
    try:
        # Both mode-specific clients can exercise the same request family.
        for idx, client in enumerate(clients):
            rid = prefix+('_TCP' if idx == 0 else '_UDP')
            created.append(rid)
            record = {'id':rid, 'timestamp':int(time.time()*1000), 'speed':265,
                      'features': {'category':'dual','counter':1},
                      'payload': 'fragment-'+'X'*5000}
            status=client.request('status')
            check(status['postgres'] is True, f'{client.mode} status')
            save=client.request('save', record=record)
            check(bool(save.get('sequence')),f'{client.mode} save')
            latest=client.request('latest', id=rid)
            check(latest['found'] and latest['record']['id']==rid and
                  latest['record']['payload']==record['payload'],
                  f'{client.mode} latest with fragmented payload')
            # Repeated ID yields more than one sequence; history remains paged.
            save2=client.request('save',record=dict(record, timestamp=record['timestamp']+1))
            check(save2['sequence']!=save['sequence'],f'{client.mode} duplicate record ID history')
            hist=client.request('history', id=rid,limit=10,timeField='receivedAt')
            check(len(hist['items']) >= 2, f'{client.mode} history')
            page1=client.request('history',id=rid,limit=1,timeField='receivedAt')
            check(page1['hasMore'],f'{client.mode} page 1')
            page2=client.request('history',id=rid,limit=1,timeField='receivedAt',
                afterSequence=page1['nextSequence'],untilSequence=page1['untilSequence'])
            check(page2['items'][0]['sequence']!=page1['items'][0]['sequence'],f'{client.mode} page 2')
            rec=client.request('recent', id=rid,lookbackSec=1800,limit=10)
            check(len(rec['items']) >= 1,f'{client.mode} recent')
            hot=client.request('search_hot',mode='records',idContains=rid,limit=10,scanBudget=50)
            check(bool(hot['items']),f'{client.mode} search_hot')
            filtered=client.request('search_hot', mode='latest', idContains=rid,
                filters=[{'field':'features.category','op':'eq','value':'dual'},
                         {'field':'speed','op':'gte','value':200}],limit=10,scanBudget=50)
            check(bool(filtered['items']),f'{client.mode} hot filtered search')
            if client.mode=='udp':
                stable=uuid.uuid4().hex
                first=client.request('save',request_id=stable,record=record)
                duplicate=client.request('save',request_id=stable,record=record)
                check(first['sequence']==duplicate['sequence'],'udp identical requestId is deduplicated')
            sub=client.request('subscribe',channel='e2e:dual')
            check(sub['subscribed'],f'{client.mode} subscribe')
            client.request('unsubscribe',channel='e2e:dual')
            evict=client.request('delete',id=rid,removeFromDisk=False)
            check(evict.get('archivePreserved') is True,f'{client.mode} evict, preserve SQL')
            hydrated=client.request('hydrate',id=rid)
            check('status' in hydrated,f'{client.mode} hydrate')
            if hydrated['status']=='queued':
                stop=time.monotonic()+30
                while time.monotonic()<stop:
                    result=client.request('recovery_status',jobId=hydrated['jobId'])
                    if result['status']=='completed':break
                    if result['status']=='failed':raise AssertionError('recovery failed '+str(result))
                    time.sleep(0.15)
                else: raise TimeoutError('recovery_status never completed')
            check(client.request('latest',id=rid)['found'],f'{client.mode} recovered latest')
            deleted=client.request('delete',id=rid,removeFromDisk=True)
            check('deleted' in deleted,f'{client.mode} delete')
        channel='e2e:'+prefix
        for receiver in clients:
            receiver.request('subscribe',channel=channel)
        for publisher in clients:
            rid=prefix+'_LIVE_'+publisher.mode
            created.append(rid)
            rec={'id':rid,'timestamp':int(time.time()*1000),'features':{'source':publisher.mode}}
            published=publisher.request('publish',channel=channel,record=rec)
            check(bool(published.get('sequence')),publisher.mode+' publish')
            for receiver in clients:
                event=receiver.wait_live(rid)
                check(event['record']['id']==rid,f'{publisher.mode} to {receiver.mode} live')
            publisher.request('delete',id=rid,removeFromDisk=True)
        for client in clients:
            client.request('unsubscribe',channel=channel)
        print('ALL TCP/UDP INTEGRATION CHECKS PASSED',flush=True)
    finally:
        for rid in created:
            try: clients[0].request('delete',id=rid,removeFromDisk=True,timeout=3)
            except Exception: pass
        for client in clients:
            try: client.close()
            except OSError: pass


if __name__=='__main__': main()
