"""Offline checks of lab isolation and the copied independent Qt SDK."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
LAB = ROOT / 'central' / 'lab'
EXTERNAL = ROOT / 'examples' / 'independent_qt_app'

class LabSetupTest(unittest.TestCase):
    def test_database_ports_loopback_only(self):
        compose = (LAB / 'compose.yaml').read_text()
        self.assertIn('127.0.0.1:15432:5432',compose)
        self.assertIn('127.0.0.1:16379:6379',compose)
        self.assertIn('ars_lab_postgres',compose)
        self.assertIn('ars_lab_redis',compose)

    def test_redis_durable_lab_settings(self):
        compose = (LAB / 'compose.yaml').read_text()
        for term in ('appendonly','appendfsync','always','noeviction'):
            self.assertIn(term,compose)
        ini=(LAB / 'server.lab.ini').read_text()
        for value in ('port=7093','port=15432','port=16379','database=ars_test',
                      'requireDurability=true','inactiveAfterSec=8','keyPrefix=ars_lab_2026'):
            self.assertIn(value,ini)

    def test_independent_example_includes_exact_sdk_copy(self):
        for original, copied in (
            ('central/client/ArsClient.cpp','vendor/ars/client/ArsClient.cpp'),
            ('central/client/ArsClient.h','vendor/ars/client/ArsClient.h'),
            ('src/Core/ArsDataRecord.cpp','vendor/ars/model/ArsDataRecord.cpp'),
            ('include/ARS/ArsDataRecord.h','vendor/ars/include/ARS/ArsDataRecord.h'),
        ):
            self.assertEqual((ROOT/original).read_bytes(),(EXTERNAL/copied).read_bytes())

    def test_backup_checker_rejects_tampering(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp)
            data=b'test archive bytes'
            (d/'archive.pgcustom').write_bytes(data)
            (d/'manifest.json').write_text(json.dumps({
                'crossStoreAtomic':False,
                'filesSha256':{'archive.pgcustom':hashlib.sha256(data).hexdigest()}
            }))
            command=[sys.executable,str(LAB/'verify_backup.py'),str(d)]
            ok=subprocess.run(command,capture_output=True)
            self.assertEqual(ok.returncode,0,ok.stderr)
            (d/'archive.pgcustom').write_bytes(data+b'corrupt')
            corrupted=subprocess.run(command,capture_output=True)
            self.assertNotEqual(corrupted.returncode,0)

    def test_backup_checker_rejects_incomplete(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp)
            (d/'.incomplete').write_text('incomplete')
            result=subprocess.run([sys.executable,str(LAB/'verify_backup.py'),str(d)],capture_output=True)
            self.assertNotEqual(result.returncode,0)

if __name__=='__main__':unittest.main()
