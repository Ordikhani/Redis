#!/usr/bin/env python3
"""Consistent PostgreSQL custom-format backup. Does not overwrite a destination."""
import os
import subprocess
import sys
from pathlib import Path

def main():
    if len(sys.argv) != 2:
        raise ValueError('Usage: backup_database.py /path/backup.dump')
    target = Path(sys.argv[1])
    env = os.environ.copy()
    env.setdefault('PGHOST', '127.0.0.1')
    env.setdefault('PGPORT', '5432')
    env.setdefault('PGDATABASE', env.get('ARS_DB_NAME', 'ars'))
    env.setdefault('PGUSER', env.get('ARS_DB_USER', 'ars_app'))
    env.setdefault('PGPASSWORD', env.get('ARS_DB_PASSWORD', ''))
    env.setdefault('PGCONNECT_TIMEOUT', '5')
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, 'wb') as output:
            subprocess.run(['pg_dump', '-w', '--format=custom', '--no-owner', '--no-acl'],
                           env=env, stdout=output, check=True)
    except BaseException:
        target.unlink(missing_ok=True)
        raise
    print('Backup created:', target)

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
