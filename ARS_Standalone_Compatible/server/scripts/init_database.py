#!/usr/bin/env python3
"""One-time PostgreSQL provisioning: no INI, no external config needed by server.
Run with administrator PG* environment; input the new ars_app password on prompt.
Does not alter the password for an existing role or drop an existing database.
"""
import argparse
import getpass
import os
import subprocess
import sys


def ident(value):
    if not value or '\x00' in value or len(value.encode()) > 63:
        raise ValueError('Invalid PostgreSQL identifier')
    return '"' + value.replace('"','""') + '"'

def literal(value):
    if '\x00' in value:
        raise ValueError('NUL not permitted')
    return "E'" + value.replace('\\','\\\\').replace("'", "''") + "'"

def sql(command):
    p = subprocess.run(['psql', '-X', '-A', '-t', '-v', 'ON_ERROR_STOP=1'],
                       input=command, text=True, capture_output=True)
    if p.returncode:
        raise RuntimeError('PostgreSQL admin provisioning failed. Check PG* credentials and privileges.')
    return p.stdout.strip()

def main():
    os.environ.setdefault('PGDATABASE','postgres')
    os.environ.setdefault('PGUSER','postgres')
    os.environ.setdefault('PGCONNECT_TIMEOUT','5')
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--database', default='ars')
    p.add_argument('--user', default='ars_app')
    a = p.parse_args()
    database, user = a.database, a.user
    db_ident, user_ident = ident(database), ident(user)
    exists = sql('SELECT 1 FROM pg_roles WHERE rolname=' + literal(user) + ';')
    if exists != '1':
        pwd = getpass.getpass('New application role password (same as compiled ServerConfig.cpp): ')
        if not pwd:
            raise ValueError('Application role password must not be empty')
        sql('CREATE ROLE ' + user_ident + ' LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE PASSWORD ' + literal(pwd) + ';')
        print('Application role created.')
    else:
        print('Existing role preserved; its password was NOT changed.')
    owner = sql('SELECT pg_get_userbyid(datdba) FROM pg_database WHERE datname=' + literal(database) + ';')
    if owner:
        if owner != user:
            raise RuntimeError('Database exists under a different owner; no changes made.')
        print('Existing database preserved.')
    else:
        sql('CREATE DATABASE ' + db_ident + ' OWNER ' + user_ident + ';')
        print('Database created; schema migration runs in ars_server.')

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError) as e:
        print(str(e), file=sys.stderr)
        sys.exit(1)
