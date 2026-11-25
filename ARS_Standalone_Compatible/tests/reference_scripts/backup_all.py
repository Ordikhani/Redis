#!/usr/bin/env python3
"""Back up PostgreSQL archive and (optional) entire Redis instance to disk.

ARS uses both stores. Their backups are *not* an atomic cross-store snapshot:
quiesce writers for a strict restore point and test restoration separately.
"""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from datetime import datetime, timezone


def digest(file: Path) -> str:
    hasher = hashlib.sha256()
    with file.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            hasher.update(chunk)
    return hasher.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', help='NEW output directory (refuses to overwrite)')
    parser.add_argument('--config', help='Read database endpoints from the same INI as ars_server')
    parser.add_argument('--postgres-only', action='store_true',
                        help='Only archive PostgreSQL; hot unsynced data will not be included')
    args = parser.parse_args()
    env = os.environ.copy()
    if args.config:
        config = configparser.ConfigParser()
        if not config.read(args.config):
            raise ValueError('Cannot read config: ' + args.config)
        for section, key, variable, fallback in (
            ('postgres', 'host', 'PGHOST', '127.0.0.1'),
            ('postgres', 'port', 'PGPORT', '5432'),
            ('postgres', 'database', 'PGDATABASE', 'ars'),
            ('postgres', 'user', 'PGUSER', 'ars_app'),
            ('redis', 'host', 'ARS_REDIS_HOST', '127.0.0.1'),
            ('redis', 'port', 'ARS_REDIS_PORT', '6379'),
        ):
            env[variable] = config.get(section, key, fallback=fallback)
        for section, variable in (('postgres', 'ARS_DB_PASSWORD'), ('redis', 'ARS_REDIS_PASSWORD')):
            if variable not in env and config.has_option(section, 'passwordFile'):
                secret = Path(args.config).resolve().parent / config.get(section, 'passwordFile')
                env[variable] = secret.read_text().rstrip('\r\n')
        env['PGPASSWORD'] = env.get('ARS_DB_PASSWORD', env.get('PGPASSWORD', ''))
    directory = Path(args.directory).expanduser().resolve()
    directory.mkdir(mode=0o700, parents=False, exist_ok=False)
    marker = directory / '.incomplete'
    marker.write_text('Backup in progress; do not restore from here yet.\n')
    env.setdefault('PGHOST', '127.0.0.1')
    env.setdefault('PGPORT', '5432')
    env.setdefault('PGDATABASE', env.get('ARS_DB_NAME', 'ars'))
    env.setdefault('PGUSER', env.get('ARS_DB_USER', 'ars_app'))
    env.setdefault('PGPASSWORD', env.get('ARS_DB_PASSWORD', ''))
    env.setdefault('PGCONNECT_TIMEOUT', '5')
    pg_file = directory / 'archive.pgcustom'
    subprocess.run(['pg_dump', '-w', '--format=custom', '--no-owner', '--no-acl',
                    '-f', str(pg_file)], env=env, check=True)
    files = {'archive.pgcustom': digest(pg_file)}
    if not args.postgres_only:
        # --rdb exports every logical database in this Redis instance, not just
        # the ARS namespace. Use a dedicated Redis instance if this matters.
        # Even an empty REDISCLI_AUTH makes redis-cli send AUTH. A Redis server
        # without a password correctly rejects that command.
        if env.get('ARS_REDIS_PASSWORD'):
            env['REDISCLI_AUTH'] = env['ARS_REDIS_PASSWORD']
        else:
            env.pop('REDISCLI_AUTH', None)
        redis_file = directory / 'redis.rdb'
        subprocess.run(['redis-cli', '-h', env.get('ARS_REDIS_HOST', '127.0.0.1'),
                        '-p', env.get('ARS_REDIS_PORT', '6379'), '--rdb', str(redis_file)],
                       env=env, check=True)
        files['redis.rdb'] = digest(redis_file)
    manifest = {
        'createdAtUtc': datetime.now(timezone.utc).isoformat(),
        'postgresDatabase': env['PGDATABASE'],
        'includesRedis': not args.postgres_only,
        'crossStoreAtomic': False,
        'filesSha256': files,
    }
    (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    marker.unlink()
    print('Backup saved to:', directory)
    print('IMPORTANT: PostgreSQL + Redis backups are not one atomic point-in-time snapshot.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print('Backup failed:', exc, file=sys.stderr)
        print('An .incomplete marker may remain; do not restore from an incomplete backup.', file=sys.stderr)
        sys.exit(1)
