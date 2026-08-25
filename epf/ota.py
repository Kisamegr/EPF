"""
OTA (Over-The-Air) firmware update management for ESP32 devices.

Handles staging uploaded .bin firmware binaries, maintaining metadata,
serving firmware payloads, tracking device installation results, and logging.
"""
import hashlib
import json
import os
import tempfile
from datetime import datetime
from epf import eventlog

def _ota_dir():
    """Directory where staged OTA binary and metadata persist."""
    dir_path = os.getenv('EPF_OTA_DIR')
    if not dir_path:
        from epf import config
        dir_path = os.path.join(os.path.dirname(config.CONFIG_PATH) or '.', 'ota')
    os.makedirs(dir_path, exist_ok=True)
    return dir_path

def _staged_binary_path():
    return os.path.join(_ota_dir(), 'staged.bin')

def _metadata_path():
    return os.path.join(_ota_dir(), 'metadata.json')

def _last_result_path():
    return os.path.join(_ota_dir(), 'last_result.json')

def _atomic_write_bytes(filepath, data):
    directory = os.path.dirname(filepath)
    fd, temporary = tempfile.mkstemp(prefix='.ota-tmp.', dir=directory)
    try:
        with os.fdopen(fd, 'wb') as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, filepath)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)

def _atomic_write_json(filepath, data):
    encoded = json.dumps(data, indent=2).encode('utf-8')
    _atomic_write_bytes(filepath, encoded)

def stage_firmware(file_storage):
    """
    Store an uploaded firmware binary file and save its metadata.
    Returns the metadata dictionary.
    """
    data = file_storage.read()
    if not data:
        raise ValueError('Firmware binary is empty')

    sha256_hash = hashlib.sha256(data).hexdigest()
    size = len(data)
    filename = os.path.basename(file_storage.filename or 'firmware.bin')

    metadata = {
        'filename': filename,
        'size': size,
        'sha256': sha256_hash,
        'uploaded_at': datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    }

    _atomic_write_bytes(_staged_binary_path(), data)
    _atomic_write_json(_metadata_path(), metadata)
    return metadata

def get_staged_info():
    """Return staged firmware metadata dict or None if no valid binary is staged."""
    bin_path = _staged_binary_path()
    meta_path = _metadata_path()
    if not (os.path.exists(bin_path) and os.path.exists(meta_path)):
        return None

    try:
        with open(meta_path, 'r', encoding='utf-8') as f:
            meta = json.load(f)
            # Ensure size matches staged.bin on disk
            meta['size'] = os.path.getsize(bin_path)
            return meta
    except Exception:
        return None

def cancel_staged():
    """Cancel staged firmware update. Returns True if a staged update was cleared."""
    cleared = False
    for path in (_staged_binary_path(), _metadata_path()):
        if os.path.exists(path):
            try:
                os.unlink(path)
                cleared = True
            except OSError:
                pass
    return cleared

def get_binary_filepath():
    """Return path to staged firmware binary file if staged, else None."""
    info = get_staged_info()
    if not info:
        return None
    return _staged_binary_path()

def record_ack(status, error_detail=None, ip=None, mac=None):
    """
    Record OTA installation result from frame device.
    Status should be 'success' or 'failed'.
    If 'success', clears the staged firmware update.
    """
    result = {
        'status': status,
        'error': error_detail or '',
        'ts': datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
        'ip': ip or '',
        'mac': mac or ''
    }

    _atomic_write_json(_last_result_path(), result)

    eventlog.record(
        'ota_update_result',
        status=status,
        error=error_detail or None,
        ip=ip,
        mac=mac
    )

    if status == 'success':
        cancel_staged()

    return result

def get_last_result():
    """Return last OTA execution result dict or None."""
    path = _last_result_path()
    if not os.path.exists(path):
        return None
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except Exception:
        return None
