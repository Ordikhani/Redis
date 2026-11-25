"""CLI connection helpers for manually invoked ADMIN scripts (no INI files).

The server credentials are compiled into C++ and are NOT exported here.
For maintenance, an operator must supply the matching database password
through a protected environment (PGPASSWORD) or an interactive prompt.
"""
import getpass
import os


def add_pg_options(parser):
    parser.add_argument('--pg-host', default='127.0.0.1')
    parser.add_argument('--pg-port', type=int, default=5432)
    parser.add_argument('--pg-database', default='ars')
    parser.add_argument('--pg-user', default='ars_app')


def pg_environment(args):
    env = os.environ.copy()
    env.update(PGHOST=args.pg_host, PGPORT=str(args.pg_port),
               PGDATABASE=args.pg_database, PGUSER=args.pg_user,
               PGCONNECT_TIMEOUT='5')
    if not env.get('PGPASSWORD'):
        env['PGPASSWORD'] = getpass.getpass('PostgreSQL password for administrative action: ')
    if not env['PGPASSWORD']:
        raise ValueError('PostgreSQL password required')
    return env


def add_redis_options(parser):
    parser.add_argument('--redis-host', default='127.0.0.1')
    parser.add_argument('--redis-port', type=int, default=6379)
    parser.add_argument('--redis-db', type=int, default=0)
    parser.add_argument('--redis-prefix', default='arscentral')


def redis_password():
    return os.environ.get('ARS_REDIS_PASSWORD', '')
