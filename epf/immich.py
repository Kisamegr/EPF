"""
Talking to Immich, and deciding which photo comes next.

Every call reads the live configuration, so a URL or album changed on the
settings page takes effect on the next request without a restart.
"""
import os
import random
import threading
import time
from datetime import datetime

import requests

from . import config, state, tracking

# Read once at import, like the original: changing the key means a restart.
# Note the README's docker run example spells it IMMICH-API-KEY, which is not
# what this reads.
apikey = os.getenv('IMMICH_API_KEY')

headers = {
    'Accept': 'application/json',
    'x-api-key': apikey,
}
CONNECT_TIMEOUT = 5
READ_TIMEOUT = 20
MAX_ORIGINAL_BYTES = 40 * 1024 * 1024
MAX_THUMBNAIL_BYTES = 8 * 1024 * 1024
MAX_PAGES = 100
CACHE_TTL_SECONDS = 300
_cache_lock = threading.RLock()
_album_cache = {'key': None, 'album_id': None, 'assets': None, 'expires': 0}

class ImmichError(Exception):
    """ Carries the message and HTTP status the caller should report """

    def __init__(self, message, status=500):
        super().__init__(message)
        self.message = message
        self.status = status

def base_url():
    url = config.immich()['url']
    # Validation is repeated for every outbound request, including after a DNS
    # cache change. Redirects are disabled on every request below.
    config.validate(config.current)
    return url

def album_name():
    return config.immich()['album']

def photo_link(asset_id):
    """ The asset in the configured server's web UI, so the link works on the LAN """
    return f"{base_url().rstrip('/')}/photos/{asset_id}"

def resolve_album_id():
    """ Look up the configured album, raising ImmichError if it cannot be used """
    cache_key = (base_url(), album_name())
    with _cache_lock:
        if _album_cache['key'] == cache_key and _album_cache['album_id'] and _album_cache['expires'] > time.monotonic():
            return _album_cache['album_id']
    try:
        response = requests.get(f"{base_url()}/api/albums", headers=headers, timeout=(CONNECT_TIMEOUT, READ_TIMEOUT), allow_redirects=False)
        response.raise_for_status()
    except requests.RequestException as error:
        raise ImmichError('Failed to fetch albums', 502) from error

    wanted = album_name()
    albumid = next((item['id'] for item in response.json()
                    if item['albumName'] == wanted), None)
    if not albumid:
        raise ImmichError("Album not found", 404)
    with _cache_lock:
        _album_cache.update({'key': cache_key, 'album_id': albumid, 'assets': None,
                             'expires': time.monotonic() + CACHE_TTL_SECONDS})
    return albumid

def list_album_assets(albumid):
    """
    Every asset in the album.

    Immich v3 breaking change: GET /api/albums/{id} no longer returns the
    'assets' property, so this goes through the paginated search endpoint.
    """
    cache_key = (base_url(), album_name())
    with _cache_lock:
        if (_album_cache['key'] == cache_key and _album_cache['album_id'] == albumid and
                _album_cache['assets'] is not None and _album_cache['expires'] > time.monotonic()):
            return list(_album_cache['assets'])
    assets = []
    page = 1
    while page <= MAX_PAGES:
        search_body = {
            "albumIds": [albumid],
            "size": 1000,
            "page": page,
            "withExif": True,
        }
        try:
            response = requests.post(f"{base_url()}/api/search/metadata", headers=headers, json=search_body,
                                     timeout=(CONNECT_TIMEOUT, READ_TIMEOUT), allow_redirects=False)
            response.raise_for_status()
        except requests.RequestException as error:
            raise ImmichError('Failed to fetch album details', 502) from error

        result = response.json().get('assets', {})
        assets.extend(result.get('items', []))

        next_page = result.get('nextPage')
        if not next_page:
            break
        try:
            page = int(next_page)
        except (TypeError, ValueError) as error:
            raise ImmichError('Invalid Immich pagination response', 502) from error

    if page > MAX_PAGES:
        raise ImmichError('Immich pagination limit exceeded', 502)

    if not assets:
        raise ImmichError("No images found in album", 404)
    with _cache_lock:
        _album_cache.update({'key': cache_key, 'album_id': albumid, 'assets': list(assets),
                             'expires': time.monotonic() + CACHE_TTL_SECONDS})
    return assets

def fetch_original(asset_id):
    """ The asset's original bytes, for the image pipeline """
    try:
        response = requests.get(f"{base_url()}/api/assets/{asset_id}/original", headers=headers, stream=True,
                                timeout=(CONNECT_TIMEOUT, READ_TIMEOUT), allow_redirects=False)
        response.raise_for_status()
        declared = int(response.headers.get('Content-Length', '0') or 0)
        if declared > MAX_ORIGINAL_BYTES:
            raise ImmichError('Source image exceeds size limit', 413)
        chunks, total = [], 0
        for chunk in response.iter_content(64 * 1024):
            total += len(chunk)
            if total > MAX_ORIGINAL_BYTES:
                raise ImmichError('Source image exceeds size limit', 413)
            chunks.append(chunk)
        return b''.join(chunks)
    except requests.RequestException as error:
        raise ImmichError('Failed to download image', 502) from error

def fetch_thumbnail(asset_id):
    """
    (bytes, content type) for a browser-friendly rendering of the asset.

    A thumbnail rather than the original because originals may be HEIC or RAW,
    which browsers cannot display, and it has to come through the server at all
    because the API key never reaches the browser.
    """
    try:
        response = requests.get(f"{base_url()}/api/assets/{asset_id}/thumbnail", headers=headers,
                                params={'size': 'preview'}, timeout=(CONNECT_TIMEOUT, READ_TIMEOUT), allow_redirects=False)
    except requests.RequestException as error:
        raise ImmichError(f"Could not reach Immich: {error}", 502)

    if response.status_code != 200:
        raise ImmichError(f"Immich returned {response.status_code}", 502)

    if len(response.content) > MAX_THUMBNAIL_BYTES:
        raise ImmichError('Thumbnail exceeds size limit', 413)
    return response.content, response.headers.get('Content-Type', 'image/jpeg')

def check_health():
    """
    One request that settles reachability, credentials and whether the configured
    album exists. Returns the shape /status reports, using codes rather than
    sentences so the page can phrase them in whichever language it is showing.
    """
    if not base_url() or not album_name():
        return {'state': 'error', 'code': 'not_configured'}

    try:
        response = requests.get(f"{base_url()}/api/albums", headers=headers, timeout=(CONNECT_TIMEOUT, 6), allow_redirects=False)
    except (requests.RequestException, ImmichError):
        return {'state': 'error', 'code': 'unreachable'}

    if response.status_code in (401, 403):
        return {'state': 'error', 'code': 'unauthorized'}
    if response.status_code != 200:
        return {'state': 'error', 'code': 'server_error', 'http': response.status_code}

    wanted = album_name()
    names = [album.get('albumName') for album in response.json()]
    if wanted in names:
        return {'state': 'ok', 'code': 'connected', 'album': wanted}
    return {'state': 'warn', 'code': 'album_missing', 'album': wanted}

def _taken_at(asset):
    return asset.get('exifInfo', {}).get('dateTimeOriginal', '1970-01-01T00:00:00')

def taken_at_text(asset):
    """
    When the photo was taken, for display. Empty when Immich has no EXIF date.

    Already fetched: list_album_assets asks withExif, and 'newest' ordering sorts
    on this very field.
    """
    raw = (asset or {}).get('exifInfo', {}).get('dateTimeOriginal')
    if not raw:
        return ''
    # 2019-08-14T10:23:45.000+08:00 -> 2019-08-14 10:23
    text = str(raw)
    return text[:16].replace('T', ' ') if len(text) >= 16 else text

def select_asset(assets):
    """
    Choose which asset to show next, honouring image_order and the history in
    tracking.txt. The history is reset when the album runs out, or when a newer
    photo turns up while ordering by date.
    """
    order = config.immich()['image_order']
    shown = tracking.shown_ids()

    if order == 'newest':
        latest_id = max(assets, key=_taken_at)['id']
        if not shown or latest_id not in shown:
            tracking.reset(reason='newer_photo')
            remaining = sorted(assets, key=_taken_at, reverse=True)
        else:
            remaining = sorted([a for a in assets if a['id'] not in shown],
                               key=_taken_at, reverse=True)
        if not remaining:
            # Everything has been shown and nothing is newer: begin again rather
            # than indexing an empty list, which used to 500 the device
            tracking.reset(reason='album_exhausted')
            remaining = sorted(assets, key=_taken_at, reverse=True)
        return remaining[0]

    remaining = [a for a in assets if a['id'] not in shown]
    if not remaining:
        tracking.reset(reason='album_exhausted')
        remaining = assets
    return random.choice(remaining)

def refresh_next_photo():
    """ Choose and remember the photo the frame will get next. Raises ImmichError. """
    with state.lock:
        albumid = resolve_album_id()
        asset = select_asset(list_album_assets(albumid))
        state.next_photo.update({'asset': asset, 'album': album_name(),
                                 'album_id': albumid, 'chosen_at': datetime.now()})
    return asset
