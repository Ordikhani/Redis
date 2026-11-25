#!/usr/bin/env python3
"""Verify backup manifest and file hashes without touching any database."""
from pathlib import Path
import argparse
import hashlib
import json


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('backup_directory')
    args=p.parse_args()
    folder=Path(args.backup_directory).resolve()
    if (folder/'.incomplete').exists():
        raise SystemExit('FAILED: incomplete backup marker present')
    metadata=json.loads((folder/'manifest.json').read_text(encoding='utf-8'))
    assert metadata['crossStoreAtomic'] is False
    for filename,expected in metadata['filesSha256'].items():
        if Path(filename).name != filename: raise SystemExit('Unsafe manifest filename')
        sha=hashlib.sha256()
        with (folder/filename).open('rb') as file:
            for block in iter(lambda:file.read(1024*1024),b''):sha.update(block)
        if sha.hexdigest()!=expected:raise SystemExit('FAILED sha256: '+filename)
        print('VERIFIED',filename,'sha256',expected)
    print('Backup files verified. IMPORTANT: not a cross-store atomic snapshot and no restore test was performed.')

if __name__=='__main__': main()
