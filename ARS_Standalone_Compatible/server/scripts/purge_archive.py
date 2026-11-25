#!/usr/bin/env python3
"""OFFLINE destructive archive purge. No INI or automatic credential export.

Stop ars_server first. The operator must explicitly specify both the TCP
listener address and every PostgreSQL/Redis target to reduce wrong-DB risk.
Redis sync offsets are reset BEFORE truncating the PostgreSQL archive.
"""
import argparse
import os
import socket
import subprocess
import sys
from admin_connection import add_pg_options, add_redis_options, pg_environment, redis_password

CONFIRM = 'DELETE ALL ARS ARCHIVE'


def cmd(sock, *args):
    payload = f'*{len(args)}\r\n'.encode()
    for a in args:
        chunk = str(a).encode()
        payload += f'${len(chunk)}\r\n'.encode() + chunk + b'\r\n'
    sock.sendall(payload)
    with sock.makefile('rb', buffering=0) as inp:
        kind = inp.read(1)
        line = inp.readline().strip()
    if kind == b'-':
        raise RuntimeError('Redis command failed: ' + line.decode(errors='replace'))
    if kind in (b'+', b':'):
        return line.decode()
    raise RuntimeError('Unexpected Redis response')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--confirm', required=True)
    p.add_argument('--server-host', required=True)
    p.add_argument('--server-port', type=int, required=True)
    add_pg_options(p)
    add_redis_options(p)
    # Explicit targeting is mandatory for destructive tasks, regardless of defaults.
    args = p.parse_args()
    if args.confirm != CONFIRM:
        raise ValueError('Refused: confirmation phrase is incorrect')
    supplied = set(sys.argv[1:])
    for option in ('--pg-host', '--pg-port', '--pg-database', '--pg-user',
                   '--redis-host', '--redis-port', '--redis-db', '--redis-prefix'):
        if option not in supplied:
            raise ValueError('Refused: explicitly provide ' + option)
    if not (0 <= args.redis_db <= 15):
        raise ValueError('Redis DB must be 0..15')
    if not args.redis_prefix or len(args.redis_prefix) > 128:
        raise ValueError('Redis prefix must be explicit and nonempty')
    if not 1 <= args.server_port <= 65535:
        raise ValueError('Invalid server port')
    try:
        sock = socket.create_connection((args.server_host, args.server_port), timeout=1)
    except OSError:
        pass
    else:
        sock.close()
        raise RuntimeError('Refused: ARS server TCP port still accepting connections')
    env = pg_environment(args)
    active = subprocess.run([
        'psql', '-X', '-A', '-t', '-w', '-v', 'ON_ERROR_STOP=1', '-c',
        "SELECT count(*) FROM pg_stat_activity "
        "WHERE datname=current_database() AND application_name='ars_server' AND pid<>pg_backend_pid()"
    ], env=env, check=True, capture_output=True, text=True).stdout.strip()
    if int(active) != 0:
        raise RuntimeError('Refused: another ars_server owns the archive database')
    with socket.create_connection((args.redis_host, args.redis_port), timeout=3) as sock:
        sock.settimeout(3)
        password = redis_password()
        if password:
            cmd(sock, 'AUTH', password)
        cmd(sock, 'SELECT', args.redis_db)
        removed = cmd(sock, 'DEL', args.redis_prefix + ':hotv1:synced')
    print('Reset hot checkpoint offsets:', removed)
    subprocess.run(['psql', '-X', '-w', '-v', 'ON_ERROR_STOP=1', '-c',
                    'TRUNCATE TABLE ars_records'], env=env, check=True)
    print('Archive rows deleted. Hot data preserved; next checkpoint restores archive.')
    print('WARNING: WAL, snapshots, backups and old physical disk blocks are NOT erased.')

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as e:
        print('Purge aborted:', e, file=sys.stderr)
        sys.exit(1)
