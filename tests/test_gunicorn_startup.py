import copy
import os
import sys
import tempfile
import types
import unittest
from werkzeug.security import generate_password_hash

os.environ['IMMICH_ALLOWED_ORIGINS'] = 'http://192.0.2.10,https://192.0.2.10'
os.environ['IMMICH_ALLOWED_IPS'] = '192.0.2.10'
os.environ.setdefault('EPF_SESSION_SECRET', 'test-session-secret')
os.environ.setdefault('EPF_DEVICE_TOKEN', 'x' * 48)
os.environ.setdefault('EPF_ADMIN_PASSWORD_HASH', generate_password_hash('test-password'))

from epf import config


class GunicornStartupTests(unittest.TestCase):
    def test_imported_application_loads_persisted_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            old_path, old_photodir = config.CONFIG_PATH, config.photodir
            config.CONFIG_PATH = os.path.join(directory, 'config.yaml')
            config.photodir = os.path.join(directory, 'photos')
            candidate = copy.deepcopy(config.DEFAULT_CONFIG)
            candidate['immich']['url'] = 'https://192.0.2.10'
            candidate['immich']['album'] = 'persisted-album'
            config.write_file(candidate, config.CONFIG_PATH)

            # The image-processing extension is irrelevant to startup. A small
            # stand-in makes this test exercise the same `app:app` import path
            # without depending on a platform-specific compiled extension.
            extension = types.ModuleType('cpy')
            extension.convert_image = lambda *args: None
            extension.load_scaled = lambda *args: None
            previous_extension = sys.modules.get('cpy')
            sys.modules['cpy'] = extension
            try:
                import importlib
                if 'app' in sys.modules:
                    import app
                    app._initialized = False
                    importlib.reload(app)
                else:
                    import app
                self.assertEqual(app.config.immich()['album'], 'persisted-album')
                app._stop_observer()
            finally:
                if previous_extension is None:
                    sys.modules.pop('cpy', None)
                else:
                    sys.modules['cpy'] = previous_extension
                config.CONFIG_PATH, config.photodir = old_path, old_photodir
