#!/usr/bin/env python3
"""Backup PostgreSQL and optional Redis. Manual admin CLI; no runtime config files.
For consistent restore points, quiesce all ARS writers before taking backups.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from datetime import datetime, timezone
from admin_connection import add_pg_options, add_redis_options, pg_environment, redis_password


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as inp:
        for part in iter(lambda: inp.read(1024 * 1024), b''):
            h.update(part)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', help='New output directory; overwrite forbidden')
    p.add_argument('--postgres-only', action='store_true')
    add_pg_options(p)
    add_redis_options(p)
    a = p.parse_args()
    env = pg_environment(a)
    directory = Path(a.directory).expanduser().resolve()
    directory.mkdir(mode=0o700, exist_ok=False)
    marker = directory / '.incomplete'
    marker.write_text('Backup is incomplete; never restore this directory.\n')
    pgfile = directory / 'archive.pgcustom'
    subprocess.run(['pg_dump', '-w', '--format=custom', '--no-owner', '--no-acl',
                    '-f', str(pgfile)], env=env, check=True)
    files = {'archive.pgcustom': digest(pgfile)}
    if not a.postgres_only:
        env = env.copy()
        pwd = redis_password()
        if pwd:
            env['REDISCLI_AUTH'] = pwd
        else:
            env.pop('REDISCLI_AUTH', None)
        rdb = directory / 'redis.rdb'
        subprocess.run(['redis-cli', '-h', a.redis_host, '-p', str(a.redis_port),
                        '--rdb', str(rdb)], env=env, check=True)
        files['redis.rdb'] = digest(rdb)
    manifest = {'createdAtUtc': datetime.now(timezone.utc).isoformat(),
                'postgresDatabase': env['PGDATABASE'],
                'includesRedis': not a.postgres_only,
                'crossStoreAtomic': False, 'filesSha256': files}
    (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    marker.unlink()
    print('Backup saved:', directory)
    print('WARNING: PostgreSQL and Redis backups are NOT an atomic snapshot.')

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        print('Backup failed:', e, file=sys.stderr)
        sys.exit(1)
