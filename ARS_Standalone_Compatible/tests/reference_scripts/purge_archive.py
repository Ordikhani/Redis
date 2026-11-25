#!/usr/bin/env python3
"""Offline ADMIN operation: delete PostgreSQL archived rows, preserve RAM data.

STOP ars_server first. Reset the Redis synced offset BEFORE deleting PostgreSQL
rows; otherwise the old checkpoint counters could cause permanent data loss.
The next checkpoint will archive the still-hot records again.
"""
import argparse
import configparser
import os
import socket
import subprocess
import sys
from pathlib import Path

CONFIRM = 'DELETE ALL ARS ARCHIVE'


def cmd(sock, *args):
    packet = f'*{len(args)}\r\n'.encode()
    for arg in args:
        value = str(arg).encode()
        packet += f'${len(value)}\r\n'.encode() + value + b'\r\n'
    sock.sendall(packet)
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
    p.add_argument('--config', required=True, help='Same server.ini used by ars_server')
    p.add_argument('--confirm', required=True, help='Exact phrase: ' + CONFIRM)
    args = p.parse_args()
    if args.confirm != CONFIRM:
        raise ValueError('Refused: confirmation phrase does not match')
    conf = configparser.ConfigParser()
    if not conf.read(args.config):
        raise ValueError('Cannot read INI file')
    if not conf.getboolean('redis', 'enabled', fallback=True):
        raise RuntimeError('Refused: Redis is disabled; cannot safely reset hot checkpoint offsets')

    host = conf.get('server', 'host', fallback='127.0.0.1')
    port = conf.getint('server', 'port', fallback=7090)
    # Refuse if central server still accepts connections (including localhost only).
    try:
        sock = socket.create_connection((host, port), timeout=1)
    except OSError:
        pass
    else:
        sock.close()
        raise RuntimeError('Refused: ars_server appears to be RUNNING; stop it first')

    env = os.environ.copy()
    # The explicit --config is authoritative; unrelated PG* environment values
    # must never redirect this destructive command to a different database.
    env['PGHOST'] = conf.get('postgres', 'host', fallback='127.0.0.1')
    env['PGPORT'] = conf.get('postgres', 'port', fallback='5432')
    env['PGDATABASE'] = conf.get('postgres', 'database', fallback='ars')
    env['PGUSER'] = conf.get('postgres', 'user', fallback='ars_app')
    for section, variable in (('postgres', 'ARS_DB_PASSWORD'), ('redis', 'ARS_REDIS_PASSWORD')):
        if variable not in env and conf.has_option(section, 'passwordFile'):
            secret = Path(args.config).resolve().parent / conf.get(section, 'passwordFile')
            env[variable] = secret.read_text().rstrip('\r\n')
    env['PGPASSWORD'] = env.get('ARS_DB_PASSWORD', env.get('PGPASSWORD', ''))
    env.setdefault('PGCONNECT_TIMEOUT', '5')
    # Also reject another ARS instance connected to the same PostgreSQL DB.
    active = subprocess.run(
        ['psql', '-X', '-A', '-t', '-w', '-v', 'ON_ERROR_STOP=1', '-c',
         "SELECT count(*) FROM pg_stat_activity "
         "WHERE datname=current_database() AND application_name='ars_server' AND pid<>pg_backend_pid()"],
        env=env, capture_output=True, text=True, check=True).stdout.strip()
    if int(active) != 0:
        raise RuntimeError('Refused: another ars_server is connected to PostgreSQL')

    redis_host = conf.get('redis', 'host', fallback='127.0.0.1')
    redis_port = conf.getint('redis', 'port', fallback=6379)
    prefix = conf.get('redis', 'keyPrefix', fallback='arscentral')
    db = conf.getint('redis', 'db', fallback=0)
    if not (0 <= db <= 15):
        raise ValueError('Redis db is out of range')
    with socket.create_connection((redis_host, redis_port), timeout=3) as sock:
        sock.settimeout(3)
        password = env.get('ARS_REDIS_PASSWORD', '')
        if password:
            cmd(sock, 'AUTH', password)
        cmd(sock, 'SELECT', db)
        # Destructive operation is deliberately limited to the one ARS namespace key.
        removed = cmd(sock, 'DEL', prefix + ':hotv1:synced')
    print('Reset hot checkpoint offsets:', removed)
    # Never use RESTART IDENTITY. Redis still holds history with global sequences.
    subprocess.run(['psql', '-X', '-w', '-v', 'ON_ERROR_STOP=1', '-c',
                    'TRUNCATE TABLE ars_records'], env=env, check=True)
    print('PostgreSQL ARS archive rows deleted. Hot records remain in Redis.')
    print('NEXT CHECKPOINT WILL REPOPULATE THE ARCHIVE FROM HOT RECORDS.')
    print('This does not securely erase backups, WAL, AOF, or old disk blocks.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print('Purge aborted:', error, file=sys.stderr)
        sys.exit(1)
