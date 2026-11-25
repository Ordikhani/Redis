#!/usr/bin/env python3
"""PostgreSQL archive backup without INI. Credentials are prompted or PGPASSWORD."""
import argparse
import os
import subprocess
import sys
from pathlib import Path
from admin_connection import add_pg_options, pg_environment

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('destination', help='New file, never overwritten')
    add_pg_options(p)
    a = p.parse_args()
    env = pg_environment(a)
    dest = Path(a.destination)
    fd = os.open(dest, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, 'wb') as f:
            subprocess.run(['pg_dump', '-w', '--format=custom', '--no-owner', '--no-acl'],
                           env=env, stdout=f, check=True)
    except BaseException:
        dest.unlink(missing_ok=True)
        raise
    print('Backup created:', dest)

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as e:
        print(str(e), file=sys.stderr)
        sys.exit(1)
