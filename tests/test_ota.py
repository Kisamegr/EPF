import hashlib
import io
import os
import tempfile
import unittest
from werkzeug.security import generate_password_hash

# Setup required test environment before imports
os.environ['EPF_SESSION_SECRET'] = 'test-secret-key-1234567890'
os.environ['EPF_ADMIN_PASSWORD_HASH'] = generate_password_hash('admin123')
os.environ['EPF_DEVICE_TOKEN'] = 'device-secret-token-abcdef'
os.environ['IMMICH_ALLOWED_ORIGINS'] = 'http://192.0.2.10'
os.environ['IMMICH_ALLOWED_IPS'] = '192.0.2.10'

from app import app
from epf import ota, security

class OtaUpdateWebTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        os.environ['EPF_OTA_DIR'] = self.temp_dir.name
        self.client = app.test_client()
        self.device_token = os.environ['EPF_DEVICE_TOKEN']

    def tearDown(self):
        self.temp_dir.cleanup()

    def _login_admin(self):
        with self.client.session_transaction() as sess:
            sess['admin'] = True
            sess['_csrf'] = 'test-csrf-token-xyz'

    def test_ota_module_unit(self):
        # Unit test ota staging and persistence
        dummy_content = b'ESP32_FIRMWARE_BINARY_V1.0'
        file_obj = io.BytesIO(dummy_content)
        file_obj.filename = 'test_firmware.bin'

        meta = ota.stage_firmware(file_obj)
        expected_hash = hashlib.sha256(dummy_content).hexdigest()
        self.assertEqual(meta['filename'], 'test_firmware.bin')
        self.assertEqual(meta['size'], len(dummy_content))
        self.assertEqual(meta['sha256'], expected_hash)

        info = ota.get_staged_info()
        self.assertIsNotNone(info)
        self.assertEqual(info['sha256'], expected_hash)

        # Cancel staging
        cancelled = ota.cancel_staged()
        self.assertTrue(cancelled)
        self.assertIsNone(ota.get_staged_info())

    def test_ota_admin_endpoints_auth(self):
        # Unauthenticated calls to /ota/status & /ota/upload should fail
        res = self.client.get('/ota/status')
        self.assertEqual(res.status_code, 401)
        self.assertEqual(res.get_json()['error'], 'authentication_required')

        res = self.client.post('/ota/upload')
        self.assertEqual(res.status_code, 401)

    def test_ota_upload_and_cancel_flow(self):
        self._login_admin()
        csrf_headers = {'X-CSRF-Token': 'test-csrf-token-xyz'}

        # Upload non-.bin file -> reject
        data = {'firmware': (io.BytesIO(b'hello'), 'test.txt')}
        res = self.client.post('/ota/upload', headers=csrf_headers, data=data, content_type='multipart/form-data')
        self.assertEqual(res.status_code, 400)

        # Upload valid .bin file -> stage
        bin_data = b'\x00\x01\x02\x03\x04\x05' * 100
        data = {'firmware': (io.BytesIO(bin_data), 'firmware_v2.bin')}
        res = self.client.post('/ota/upload', headers=csrf_headers, data=data, content_type='multipart/form-data')
        self.assertEqual(res.status_code, 200)
        staged = res.get_json()['staged']
        self.assertEqual(staged['filename'], 'firmware_v2.bin')

        # Check status
        res = self.client.get('/ota/status')
        self.assertEqual(res.status_code, 200)
        self.assertIsNotNone(res.get_json()['staged'])

        # Cancel staged
        res = self.client.post('/ota/cancel', headers=csrf_headers)
        self.assertEqual(res.status_code, 200)
        self.assertTrue(res.get_json()['cancelled'])

    def test_ota_device_endpoints(self):
        # Unauthenticated device call -> 401
        res = self.client.get('/ota/check')
        self.assertEqual(res.status_code, 401)

        auth_headers = {'Authorization': f'Bearer {self.device_token}'}

        # Check when nothing staged
        res = self.client.get('/ota/check', headers=auth_headers)
        self.assertEqual(res.status_code, 200)
        self.assertFalse(res.get_json()['available'])

        # Stage a binary via ota module
        dummy_content = b'DEVICE_OTA_PAYLOAD_TEST_123'
        file_obj = io.BytesIO(dummy_content)
        file_obj.filename = 'device_firmware.bin'
        meta = ota.stage_firmware(file_obj)

        # Check available
        res = self.client.get('/ota/check', headers=auth_headers)
        self.assertEqual(res.status_code, 200)
        self.assertTrue(res.get_json()['available'])

        # Download binary stream
        res = self.client.get('/ota/binary', headers=auth_headers)
        self.assertEqual(res.status_code, 200)
        self.assertEqual(res.data, dummy_content)
        self.assertEqual(res.headers.get('X-EPF-OTA-SHA256'), meta['sha256'])
        self.assertEqual(res.headers.get('Content-Length'), str(len(dummy_content)))

        # Device ACK failure
        res = self.client.post('/ota/ack', headers=auth_headers, json={'status': 'failed', 'error': 'Flash write timeout'})
        self.assertEqual(res.status_code, 200)
        self.assertTrue(res.get_json()['acknowledged'])
        last_res = ota.get_last_result()
        self.assertEqual(last_res['status'], 'failed')
        self.assertEqual(last_res['error'], 'Flash write timeout')
        # Staged firmware remains because update failed
        self.assertIsNotNone(ota.get_staged_info())

        # Device ACK success
        res = self.client.post('/ota/ack', headers=auth_headers, json={'status': 'success'})
        self.assertEqual(res.status_code, 200)
        last_res = ota.get_last_result()
        self.assertEqual(last_res['status'], 'success')
        # Staged firmware should be cleared after success ACK
        self.assertIsNone(ota.get_staged_info())

if __name__ == '__main__':
    unittest.main()
