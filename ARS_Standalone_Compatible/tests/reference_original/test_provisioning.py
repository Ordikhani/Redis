"""Offline tests: ensure provisioning never silently destroys existing databases."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('provision', Path(__file__).parents[1]/'scripts/init_database.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class ProvisioningTests(unittest.TestCase):
    def test_quoting(self):
        self.assertEqual(m.ident('a"b'), '"a""b"')
        self.assertEqual(m.literal("a'b"), "E'a''b'")
        self.assertEqual(m.literal('a\\b'), "E'a\\\\b'")
        with self.assertRaises(ValueError): m.ident('a\x00b')
    @patch.dict('os.environ', {'ARS_DB_PASSWORD':'test', 'ARS_DB_NAME':'ars', 'ARS_DB_USER':'ars_app'})
    def test_existing_preserved(self):
        with patch.object(m, 'sql', side_effect=['1','ars_app']) as sql:
            m.main()
            self.assertEqual(sql.call_count, 2)
            self.assertTrue(all(c.args[0].startswith('SELECT') for c in sql.call_args_list))
    @patch.dict('os.environ', {'ARS_DB_PASSWORD':'test', 'ARS_DB_NAME':'ars', 'ARS_DB_USER':'ars_app'})
    def test_wrong_owner_rejected(self):
        with patch.object(m, 'sql', side_effect=['1','someone_else']) as sql:
            with self.assertRaises(RuntimeError): m.main()
            self.assertEqual(sql.call_count, 2)
    @patch.dict('os.environ', {'ARS_DB_PASSWORD':'test', 'ARS_DB_NAME':'ars', 'ARS_DB_USER':'ars_app'})
    def test_fresh_creation(self):
        with patch.object(m, 'sql', side_effect=['','','','']) as sql:
            m.main()
            commands = [c.args[0] for c in sql.call_args_list]
            self.assertTrue(commands[1].startswith('CREATE ROLE'))
            self.assertTrue(commands[3].startswith('CREATE DATABASE'))
            self.assertFalse(any('DROP ' in c for c in commands))

if __name__ == '__main__': unittest.main()
