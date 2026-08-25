import copy
import os
import tempfile
import unittest

os.environ['IMMICH_ALLOWED_ORIGINS'] = 'http://192.0.2.10,https://192.0.2.10'
os.environ['IMMICH_ALLOWED_IPS'] = '192.0.2.10'

from epf import config


class ConfigValidationTests(unittest.TestCase):
    def test_rejects_unapproved_immich_origin(self):
        candidate = copy.deepcopy(config.DEFAULT_CONFIG)
        candidate['immich']['url'] = 'https://192.0.2.99'
        with self.assertRaises(ValueError):
            config.validate(candidate)

    def test_rejects_hostname_to_eliminate_dns_rebinding(self):
        candidate = copy.deepcopy(config.DEFAULT_CONFIG)
        candidate['immich']['url'] = 'https://immich.example'
        with self.assertRaises(ValueError):
            config.validate(candidate)

    def test_atomic_write_round_trip(self):
        candidate = copy.deepcopy(config.DEFAULT_CONFIG)
        candidate['immich']['url'] = 'https://192.0.2.10'
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'config.yaml')
            config.write_file(candidate, path)
            self.assertEqual(config.read_file(path), candidate)
