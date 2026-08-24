"""
Which photos have already been shown.

tracking.txt holds the album name on the first line and one asset id per line
after it, so changing albums resets the history on its own.
"""
import os
import tempfile
import threading

from . import config, eventlog

tracking_file = os.path.join(config.photodir, 'tracking.txt')
_lock = threading.RLock()

if not os.path.exists(tracking_file):
    open(tracking_file, 'w').close()

def _album():
    return config.immich()['album']

def shown_ids():
    """ The asset ids already shown from the current album """
    album = _album()
    try:
      with _lock:
        if not os.path.exists(tracking_file):
            open(tracking_file, 'w').close()

        os.chmod(tracking_file, 0o640)

        with open(tracking_file, 'r+') as handle:
            lines = handle.readlines()

            # Empty, or a different album: start over
            if not lines or lines[0].strip() != album:
                handle.seek(0)
                handle.truncate()
                handle.write(f"{album}\n")
                return set()

            return set(line.strip() for line in lines[1:] if line.strip())
    except Exception as error:
        print(f"Error reading tracking file: {error}")
        return set()

def mark_shown(asset_id):
    """ Record that an asset has been sent to the frame """
    album = _album()
    try:
      with _lock:
        lines = []
        if os.path.exists(tracking_file):
            with open(tracking_file, 'r', encoding='utf-8') as handle:
                lines = handle.readlines()
        if not lines or lines[0].strip() != album:
            lines = [f"{album}\n"]
        if asset_id in {line.strip() for line in lines[1:]}:
            return True
        lines.append(f"{asset_id}\n")
        fd, temporary = tempfile.mkstemp(prefix='.tracking.', dir=config.photodir)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as handle:
                handle.writelines(lines)
                handle.flush()
                os.fsync(handle.fileno())
            os.chmod(temporary, 0o640)
            os.replace(temporary, tracking_file)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
        return True
    except PermissionError:
        print(f"Permission denied when writing to {tracking_file}")
    except IOError as error:
        print(f"IO Error when writing to tracking file: {error}")
    except Exception as error:
        print(f"Unexpected error writing to tracking file: {error}")
    return False

def reset(reason=None):
    """ Forget the history, so the album starts again """
    eventlog.record('tracking_reset', reason=reason)
    try:
        with _lock:
            fd, temporary = tempfile.mkstemp(prefix='.tracking.', dir=config.photodir)
            with os.fdopen(fd, 'w') as handle:
                handle.flush()
                os.fsync(handle.fileno())
            os.chmod(temporary, 0o640)
            os.replace(temporary, tracking_file)
    except Exception as error:
        print(f"Error resetting tracking file: {error}")
