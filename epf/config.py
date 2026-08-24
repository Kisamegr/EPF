"""
Runtime configuration: the defaults, the live values, and the watcher that picks
up edits made to config.yaml outside the web UI.

The live settings are held in one dict that is only ever updated in place. Other
modules import this module and read `immich()` when they need a value; they must
not copy values out at import time, or they would be frozen at whatever the
config was when the process started.
"""
import copy
import ipaddress
import os
import tempfile
import threading
from urllib.parse import urlsplit

import yaml
from watchdog.events import FileSystemEventHandler
from watchdog.observers import Observer

# Written by the settings page and watched for external edits. Containers use
# /config/config.yaml; the override keeps startup behavior testable elsewhere.
CONFIG_PATH = os.getenv('EPF_CONFIG_PATH', '/config/config.yaml')

DEFAULT_CONFIG = {
    'immich': {
        'url': 'http://192.0.2.10',   # Immich server URL ("localhost" is forbidden)
        'album': 'default_album',       # Album name
        'rotation': 270,                # 0/90/180/270
        'enhanced': 1.3,                # From 0.0 .. 1.0
        'contrast': 0.9,                # From 0.0 .. 1.0
        'strength': 0.8,                # From 0.0 .. 1.0
        'display_mode': 'fill',         # fit/fill
        'image_order': 'random',        # random/newest
        'sleep_start_hour': 23,         # Sleep start time 23:00 (11:00 PM)
        'sleep_start_minute': 0,
        'sleep_end_hour': 6,            # Sleep end time 6:00 (6:00 AM)
        'sleep_end_minute': 0,
        'wakeup_interval': 60,          # Minutes
    },
    'notify': {
        'enabled': False,               # Master switch
        'battery_threshold': 20,        # Warn at or below this percentage
        'min_interval_hours': 12,       # Never warn more often than this
        # Which linked services actually receive warnings. On by default, so
        # linking one is enough; untick to leave a linked service out.
        'use_telegram': True,
        'use_line': True,
    },
}

# Deep-copied on purpose: a shallow copy would share the inner dict with
# DEFAULT_CONFIG, so updating the live settings would rewrite the defaults and
# the reset button would restore whatever was last saved.
current = copy.deepcopy(DEFAULT_CONFIG)
_lock = threading.RLock()

# Holds tracking.txt and events.jsonl. No photos are ever written here.
photodir = os.getenv('IMMICH_PHOTO_DEST', '/photos')

def verify_storage(config_path=CONFIG_PATH):
    """Fail startup early when bind mounts are not writable by the service user."""
    for directory in (os.path.dirname(config_path) or '.', photodir):
        os.makedirs(directory, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix='.epf-write-check.', dir=directory)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
            os.unlink(temporary)

def immich():
    """ The live Immich settings. Read it per use, never cache the result. """
    return current['immich']

def notify():
    """ The live notification settings """
    return current['notify']

def sections():
    """ The names of the configuration sections, in a stable order """
    return list(DEFAULT_CONFIG.keys())

def apply(new_config):
    """
    Adopt new settings, in place, so every module that read this dict sees them.
    Unknown keys are ignored and missing ones keep their current value.
    """
    validated = validate(new_config)
    with _lock:
        for section in DEFAULT_CONFIG:
            current[section].clear()
            current[section].update(validated[section])

    settings = current['immich']
    print("Configuration updated: URL = {url}, Album = {album}, angle = {rotation}, "
          "enhance = {enhanced}, contrast = {contrast}, strength = {strength}, "
          "display_mode = {display_mode}, image_order = {image_order}".format(**settings))

def _allowed_origins():
    return {value.strip().rstrip('/') for value in os.environ.get('IMMICH_ALLOWED_ORIGINS', '').split(',') if value.strip()}

def validate(candidate):
    """Return a complete typed config, rejecting unknown keys and unsafe origins."""
    if not isinstance(candidate, dict) or set(candidate) - set(DEFAULT_CONFIG):
        raise ValueError('unknown configuration section')
    result = copy.deepcopy(DEFAULT_CONFIG)
    for section, defaults in DEFAULT_CONFIG.items():
        values = candidate.get(section, {})
        if not isinstance(values, dict) or set(values) - set(defaults):
            raise ValueError(f'unknown or invalid {section} setting')
        result[section].update(values)
    settings = result['immich']
    url = settings['url'].rstrip('/')
    parsed = urlsplit(url)
    if parsed.scheme not in {'https', 'http'} or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError('Immich URL must be a plain HTTP(S) origin')
    if url not in _allowed_origins():
        raise ValueError('Immich origin is not in IMMICH_ALLOWED_ORIGINS')
    try:
        host_ip = ipaddress.ip_address(parsed.hostname)
    except ValueError as error:
        raise ValueError('Immich origin must use an explicit allowlisted IP address') from error
    allowed_ips = {item.strip() for item in os.environ.get('IMMICH_ALLOWED_IPS', '').split(',') if item.strip()}
    if str(host_ip) not in allowed_ips:
        raise ValueError('Immich IP is not in IMMICH_ALLOWED_IPS')
    settings['url'] = url
    if not isinstance(settings['album'], str) or not settings['album'].strip() or len(settings['album']) > 255:
        raise ValueError('album is invalid')
    if settings['rotation'] not in {0, 90, 180, 270}:
        raise ValueError('rotation is invalid')
    if settings['display_mode'] not in {'fit', 'fill'} or settings['image_order'] not in {'random', 'newest'}:
        raise ValueError('display mode or image order is invalid')
    for key in ('enhanced', 'contrast', 'strength'):
        if not isinstance(settings[key], (int, float)) or not 0 <= settings[key] <= 3:
            raise ValueError(f'{key} must be between 0 and 3')
    for key in ('sleep_start_hour', 'sleep_end_hour'):
        if not isinstance(settings[key], int) or not 0 <= settings[key] <= 23:
            raise ValueError(f'{key} must be an hour')
    for key in ('sleep_start_minute', 'sleep_end_minute'):
        if not isinstance(settings[key], int) or not 0 <= settings[key] <= 59:
            raise ValueError(f'{key} must be a minute')
    if not isinstance(settings['wakeup_interval'], int) or not 10 <= settings['wakeup_interval'] <= 10080:
        raise ValueError('wakeup interval must be 10 to 10080 minutes')
    notice = result['notify']
    if not all(isinstance(notice[key], bool) for key in ('enabled', 'use_telegram', 'use_line')):
        raise ValueError('notification switches must be booleans')
    if not isinstance(notice['battery_threshold'], int) or not 0 <= notice['battery_threshold'] <= 100:
        raise ValueError('battery threshold must be 0 to 100')
    if not isinstance(notice['min_interval_hours'], int) or not 1 <= notice['min_interval_hours'] <= 168:
        raise ValueError('notification interval must be 1 to 168 hours')
    return result

def read_file(path=CONFIG_PATH, fallback=None):
    """
    Load config.yaml, falling back to the defaults if it cannot be read.

    Sections added after a file was written are filled in from the defaults, so
    an older config.yaml does not have to be edited by hand.
    """
    loaded = copy.deepcopy(DEFAULT_CONFIG)
    try:
        with open(path, 'r') as handle:
            stored = yaml.safe_load(handle) or {}
    except Exception as error:
        print(f"Error reading config file: {error}")
        return fallback

    try:
        return validate(stored)
    except ValueError as error:
        print(f"Invalid config file, retaining the previous configuration: {error}")
        return fallback

def write_file(new_config, path=CONFIG_PATH):
    """ Persist settings. Raises, so the caller can report the failure. """
    validated = validate(new_config)
    directory = os.path.dirname(path) or '.'
    fd, temporary = tempfile.mkstemp(prefix='.config.', dir=directory)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as handle:
            yaml.safe_dump(validated, handle, sort_keys=True)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)

def ensure_file(path=CONFIG_PATH):
    """ Create the directory and a default config.yaml when they are missing """
    directory = os.path.dirname(path)
    if directory and not os.path.exists(directory):
        try:
            os.makedirs(directory)
            print(f"Created config directory: {directory}")
        except Exception as error:
            print(f"Error creating config directory: {error}")

    if not os.path.exists(path):
        try:
            initial = copy.deepcopy(DEFAULT_CONFIG)
            origins = _allowed_origins()
            if origins:
                initial['immich']['url'] = sorted(origins)[0]
            write_file(initial, path)
            print(f"Created default configuration file: {path}")
        except Exception as error:
            print(f"Error creating config file: {error}")

class ConfigFileHandler(FileSystemEventHandler):
    """ Reloads the configuration when config.yaml changes on disk """

    def __init__(self, config_path, on_change):
        self.config_path = config_path
        self.on_change = on_change
        ensure_file(config_path)

    def on_modified(self, event):
        if os.path.abspath(event.src_path) == os.path.abspath(self.config_path):
            print("File modification detected, reloading configuration...")
            loaded = read_file(self.config_path)
            if loaded is not None:
                self.on_change(loaded)

def start_watcher(on_change, config_path=CONFIG_PATH):
    """ Watch the config directory and hand new settings to on_change """
    handler = ConfigFileHandler(config_path, on_change)
    observer = Observer()
    observer.schedule(handler, path=os.path.dirname(config_path), recursive=False)
    observer.start()
    return observer
