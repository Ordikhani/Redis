#!/usr/bin/env python3
"""ARS central service end-to-end save/publish benchmark (Python 3 stdlib only).

Requires an isolated ARS test instance; records are retained by the server.
Measures TCP request -> server acknowledgement, not subscriber delivery.
"""
import argparse
import asyncio
import json
import os
import socket
import statistics
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone


@dataclass
class Metrics:
    sent: int = 0
    success: int = 0
    failed: int = 0
    errors: list = field(default_factory=list)
    latencies_ms: list = field(default_factory=list)


class Worker:
    def __init__(self, number, cfg, token, metrics, prefix):
        self.number, self.cfg, self.token = number, cfg, token
        self.metrics, self.prefix = metrics, prefix
        self.reader = self.writer = self.task = None
        self.pending = {}
        self.pending_event = asyncio.Event()
        self.closed = False
        self.index = 0

    async def connect(self):
        self.reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(self.cfg.host, self.cfg.port), timeout=8)
        sock = self.writer.get_extra_info('socket')
        if sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        auth = dict(requestId=f'auth-{self.number}', op='auth', args={'token': self.token})
        self.writer.write(json.dumps(auth, separators=(',', ':')).encode() + b'\n')
        await self.writer.drain()
        response = await asyncio.wait_for(self.reader.readline(), timeout=8)
        if not response:
            raise RuntimeError('server disconnected during authentication')
        answer = json.loads(response)
        if not answer.get('ok'):
            raise RuntimeError(f'authentication failed: {answer.get("error", answer)}')
        self.task = asyncio.create_task(self.receive())

    async def receive(self):
        try:
            while True:
                line = await self.reader.readline()
                if not line:
                    break
                message = json.loads(line)
                if 'event' in message:
                    continue
                rid = message.get('requestId', '')
                began = self.pending.pop(rid, None)
                if began is None:
                    continue
                self.metrics.latencies_ms.append((time.perf_counter() - began) * 1000)
                if message.get('ok'):
                    self.metrics.success += 1
                else:
                    self.metrics.failed += 1
                    if len(self.metrics.errors) < 8:
                        self.metrics.errors.append(message.get('error', 'unknown error'))
                self.pending_event.set()
        except (ValueError, ConnectionError, OSError) as e:
            if len(self.metrics.errors) < 8:
                self.metrics.errors.append(f'receiver error: {e}')
        finally:
            self.closed = True
            self.pending_event.set()

    async def send_for(self, duration):
        start = time.perf_counter()
        cutoff = start + duration
        interval = self.cfg.clients / self.cfg.rate if self.cfg.rate else 0
        due = start
        while time.perf_counter() < cutoff and not self.closed:
            if interval:
                now = time.perf_counter()
                if now < due:
                    await asyncio.sleep(due - now)
                due = max(due + interval, time.perf_counter())
            while len(self.pending) >= self.cfg.inflight and not self.closed:
                self.pending_event.clear()
                await self.pending_event.wait()
            if self.closed or time.perf_counter() >= cutoff:
                break
            i = self.index
            self.index += 1
            rec_id = f'{self.prefix}-{(self.number + i*self.cfg.clients) % self.cfg.ids:05d}'
            now_ms = int(time.time() * 1000)
            record = {
                'id': rec_id, 'timestamp': now_ms,
                'latitude': 35.7, 'longitude': 51.4,
                'source': 'ARSBenchmark', 'payload': f'worker={self.number};n={i}'
            }
            args = {'record': record}
            if self.cfg.op == 'publish':
                args['channel'] = self.cfg.channel
            rid = f'b{self.number}-{i}'
            request = {'requestId': rid, 'op': self.cfg.op, 'args': args}
            self.pending[rid] = time.perf_counter()
            self.writer.write(json.dumps(request, separators=(',', ':')).encode() + b'\n')
            self.metrics.sent += 1
            if self.writer.transport.get_write_buffer_size() > 256*1024:
                try:
                    await asyncio.wait_for(self.writer.drain(), timeout=5)
                except asyncio.TimeoutError:
                    break
        end_sending = time.perf_counter()
        while self.pending and not self.closed and time.perf_counter() - end_sending < self.cfg.drain_sec:
            self.pending_event.clear()
            try:
                await asyncio.wait_for(self.pending_event.wait(), timeout=1)
            except asyncio.TimeoutError:
                pass
        return len(self.pending)

    async def close(self):
        if self.writer:
            self.writer.close()
            try:
                await asyncio.wait_for(self.writer.wait_closed(), timeout=2)
            except (asyncio.TimeoutError, ConnectionError, OSError):
                pass
        if self.task:
            try:
                await asyncio.wait_for(self.task, timeout=2)
            except asyncio.TimeoutError:
                self.task.cancel()


def percentile(sorted_values, value):
    if not sorted_values:
        return None
    pos = (len(sorted_values) - 1) * value
    lo = int(pos)
    hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (pos - lo)


async def benchmark(cfg):
    token = os.environ.get('ARS_API_TOKEN', '')
    if len(token.encode()) < 32:
        raise RuntimeError('Set ARS_API_TOKEN with at least 32 bytes (matching the test ARS server)')
    m = Metrics()
    prefix = 'BENCH-' + datetime.now(timezone.utc).strftime('%Y%m%d%H%M%S')
    workers = [Worker(k, cfg, token, m, prefix) for k in range(cfg.clients)]
    try:
        await asyncio.gather(*(w.connect() for w in workers))
        began = time.perf_counter()
        pending_counts = await asyncio.gather(*(w.send_for(cfg.duration) for w in workers))
        elapsed = time.perf_counter() - began
    finally:
        await asyncio.gather(*(w.close() for w in workers))
    samples = sorted(m.latencies_ms)
    print(f'ARS Benchmark | op={cfg.op} | clients={cfg.clients} | IDs={cfg.ids} | '
          f'target_rate={cfg.rate or "unlimited"}/s')
    print(f'Elapsed (including drain): {elapsed:.2f}s | sent={m.sent} | ACK_OK={m.success} '
          f'| ACK_FAILED={m.failed} | no_ACK={sum(pending_counts)}')
    print(f'ACK rate: {m.success / elapsed:.1f}/s | success fraction: {m.success / m.sent * 100 if m.sent else 0:.1f}%')
    if samples:
        print('ACK latency: mean={:.2f}ms p50={:.2f}ms p95={:.2f}ms p99={:.2f}ms max={:.2f}ms'
              .format(statistics.mean(samples), percentile(samples, .5), percentile(samples, .95),
                      percentile(samples, .99), samples[-1]))
    if m.errors:
        print('Representative errors:', '; '.join(m.errors))
    print(f'Data stored under ID prefix {prefix} (use an isolated test database).')
    if m.failed or sum(pending_counts) or m.success == 0:
        raise SystemExit(1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--port', type=int, default=7091)
    p.add_argument('--duration', type=float, default=30)
    p.add_argument('--rate', type=float, default=500, help='total target requests/sec, 0 = unlimited')
    p.add_argument('--clients', type=int, default=4)
    p.add_argument('--ids', type=int, default=100, help='number of ID variants, repeated across requests')
    p.add_argument('--inflight', type=int, default=64, help='maximum unacknowledged requests per connection')
    p.add_argument('--drain-sec', type=float, default=15)
    p.add_argument('--op', choices=('save', 'publish'), default='publish')
    p.add_argument('--channel', default='ars:test:live')
    cfg = p.parse_args()
    if not (1 <= cfg.clients <= 64 and 1 <= cfg.ids <= 100000 and 1 <= cfg.inflight <= 128 and
            0 <= cfg.rate <= 100000 and 1 <= cfg.duration <= 600 and 1 <= cfg.drain_sec <= 60 and
            1 <= cfg.port <= 65535):
        p.error('Invalid options: clients 1..64, ids 1..100000, inflight 1..128, rate 0..100000, duration 1..600')
    try:
        asyncio.run(benchmark(cfg))
    except (RuntimeError, OSError, asyncio.TimeoutError) as e:
        p.exit(2, f'Benchmark could not start: {e}\n')


if __name__ == '__main__':
    main()
