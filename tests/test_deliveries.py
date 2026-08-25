import os
import tempfile
import unittest
from unittest.mock import patch

os.environ.setdefault('IMMICH_ALLOWED_ORIGINS', 'https://192.0.2.10')
os.environ.setdefault('IMMICH_ALLOWED_IPS', '192.0.2.10')

from epf import deliveries


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.original_db_path = deliveries.DB_PATH
        deliveries.DB_PATH = os.path.join(self.directory.name, 'epf.sqlite3')

    def tearDown(self):
        deliveries.DB_PATH = self.original_db_path
        try:
            self.directory.cleanup()
        except Exception:
            pass

    def test_retry_returns_the_same_active_delivery(self):
        first = deliveries.create('asset-one', 'album', b'payload')
        retry = deliveries.create('asset-two', 'album', b'other')
        self.assertEqual(retry, first)

    def test_expired_delivery_does_not_block_a_new_one(self):
        with patch('epf.deliveries.time.time', return_value=100):
            first = deliveries.create('asset-one', 'album', b'payload', lease_seconds=10)
        with patch('epf.deliveries.time.time', return_value=111):
            self.assertIsNone(deliveries.active())
            second = deliveries.create('asset-two', 'album', b'other')
        self.assertNotEqual(second[0], first[0])

    def test_acknowledgement_and_cancellation_claim_only_once(self):
        delivery = deliveries.create('asset-one', 'album', b'payload')
        self.assertEqual(deliveries.acknowledge(delivery[0]), ('asset-one', 'album'))
        self.assertIsNone(deliveries.acknowledge(delivery[0]))
        replacement = deliveries.create('asset-two', 'album', b'other')
        self.assertEqual(deliveries.cancel_active(), replacement[0])
        self.assertIsNone(deliveries.active())
