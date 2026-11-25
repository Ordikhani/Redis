#!/usr/bin/env python3
"""One-time PostgreSQL provisioning. Uses psql and standard PG* admin variables.
Never drops a database or changes an existing role's password.
"""
import getpass
import os
import subprocess
import sys

def ident(value):
    if not value or '\x00' in value or len(value.encode()) > 63:
        raise ValueError('Invalid PostgreSQL identifier')
    return '"' + value.replace('"', '""') + '"'

def literal(value):
    if '\x00' in value:
        raise ValueError('NUL is not permitted')
    return "E'" + value.replace('\\', '\\\\').replace("'", "''") + "'"

def sql(command):
    # SQL, including a new role's password, goes through stdin, not process arguments.
    p = subprocess.run(['psql', '-X', '-A', '-t', '-v', 'ON_ERROR_STOP=1'],
                       input=command, text=True, capture_output=True)
    if p.returncode:
        raise RuntimeError('PostgreSQL provisioning failed. Verify admin PG* connection settings and privileges.')
    return p.stdout.strip()

def main():
    os.environ.setdefault('PGDATABASE', 'postgres')
    os.environ.setdefault('PGUSER', 'postgres')
    os.environ.setdefault('PGCONNECT_TIMEOUT', '5')
    database = os.environ.get('ARS_DB_NAME', 'ars')
    user = os.environ.get('ARS_DB_USER', 'ars_app')
    db_ident, user_ident = ident(database), ident(user)
    password = os.environ.get('ARS_DB_PASSWORD') or getpass.getpass('New application database password: ')
    if not password:
        raise ValueError('Application password must not be empty')
    exists = sql('SELECT 1 FROM pg_roles WHERE rolname=' + literal(user) + ';')
    if exists != '1':
        sql('CREATE ROLE ' + user_ident + ' LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE PASSWORD ' + literal(password) + ';')
        print('Application role created.')
    else:
        print('Existing application role preserved; its password has not been changed.')
    owner = sql('SELECT pg_get_userbyid(datdba) FROM pg_database WHERE datname=' + literal(database) + ';')
    if owner:
        if owner != user:
            raise RuntimeError('Database exists with a different owner; no changes made to that database.')
        print('Existing database preserved.')
    else:
        sql('CREATE DATABASE ' + db_ident + ' OWNER ' + user_ident + ';')
        print('Database created. Tables/migrations are applied by ars_server at startup.')

if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, OSError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
