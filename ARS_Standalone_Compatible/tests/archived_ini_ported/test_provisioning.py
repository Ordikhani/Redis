"""Offline tests: ensure provisioning never silently destroys existing databases."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
import tempfile

spec = importlib.util.spec_from_file_location('provision', Path(__file__).resolve().parents[2]/'server/scripts/init_database.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class ProvisioningTests(unittest.TestCase):
    def run_with_ini(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'server.ini'
            path.write_text('[postgres]\ndatabase=ars\nuser=ars_app\npassword=test\n')
            with patch('sys.argv', ['init_database.py','--config',str(path)]):
                m.main()
    def test_quoting(self):
        self.assertEqual(m.ident('a"b'), '"a""b"')
        self.assertEqual(m.literal("a'b"), "E'a''b'")
        self.assertEqual(m.literal('a\\b'), "E'a\\\\b'")
        with self.assertRaises(ValueError): m.ident('a\x00b')
    def test_existing_preserved(self):
        with patch.object(m, 'sql', side_effect=['1','ars_app']) as sql:
            self.run_with_ini()
            self.assertEqual(sql.call_count, 2)
            self.assertTrue(all(c.args[0].startswith('SELECT') for c in sql.call_args_list))
    def test_wrong_owner_rejected(self):
        with patch.object(m, 'sql', side_effect=['1','someone_else']) as sql:
            with self.assertRaises(RuntimeError): self.run_with_ini()
            self.assertEqual(sql.call_count, 2)
    def test_fresh_creation(self):
        with patch.object(m, 'sql', side_effect=['','','','']) as sql:
            self.run_with_ini()
            commands = [c.args[0] for c in sql.call_args_list]
            self.assertTrue(commands[1].startswith('CREATE ROLE'))
            self.assertTrue(commands[3].startswith('CREATE DATABASE'))
            self.assertFalse(any('DROP ' in c for c in commands))

if __name__ == '__main__': unittest.main()
