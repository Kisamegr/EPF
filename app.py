#-*- coding:utf8 -*-
"""
The Flask app and its HTTP routes.

Everything else lives in the epf package: see epf/__init__.py for the map. The
contract with the firmware is /download and /sleep; the rest serves the settings
page. Run with `python app.py`, or `docker compose up -d`.
"""
import io
import hashlib
import json
import os
import threading
import time
import atexit
from datetime import datetime, timedelta

from flask import (Flask, jsonify, make_response, redirect, render_template, request, send_file,
                   session, url_for)
from epf import (battery, config, credentials, eventlog, imaging, immich, notify,
                 ota, state, tracking)
from epf import deliveries, security

app = Flask(__name__)
security.configure(app)
security.protect_routes(app)
_observer = None
_initialized = False

# ---------------------------------------------------------------- template glue

@app.context_processor
def inject_current_year():
    """ Expose the current year to every template, so the footer never goes stale """
    return {'current_year': datetime.now().year, 'csrf_token': security.csrf_token()}

@app.context_processor
def inject_static_url():
    """
    static_url('css/settings.css') with the file's modification time appended, so
    a browser cannot keep serving a stale stylesheet or script after an update.
    """
    def static_url(filename):
        try:
            version = int(os.path.getmtime(os.path.join(app.static_folder, filename)))
        except OSError:
            version = 0
        return url_for('static', filename=filename, v=version)

    return {'static_url': static_url}

@app.context_processor
def inject_current_photo():
    """ Expose the photo the frame is showing, or None before the first check-in """
    if not state.last_photo['asset_id']:
        return {'photo': None}

    shown_at = state.last_photo['shown_at']
    return {'photo': {
        'asset_id': state.last_photo['asset_id'],
        'taken_at': state.last_photo['taken_at'] or '',
        # The configured server rather than my.immich.app, so the link opens the
        # web UI on the LAN instead of needing an internet round trip
        'link': immich.photo_link(state.last_photo['asset_id']),
        'shown_at': shown_at.strftime('%Y-%m-%d %H:%M') if shown_at else '',
    }}

def _no_store(response):
    """ Status, previews and the log change constantly: never cache them """
    response.headers['Cache-Control'] = 'no-store'
    return response

def _flat_defaults():
    """
    Every default in one flat dict.

    The page's reset button looks fields up by form-field name, which is unique
    across the sections, so it does not need the nesting.
    """
    flat = {}
    for values in config.DEFAULT_CONFIG.values():
        flat.update(values)
    return flat

def _fresh_battery():
    """ The last reported voltage, or 0 once it is more than an hour old """
    if time.time() - state.battery['updated'] < 3600:
        return state.battery['voltage']
    return 0

# --------------------------------------------------------------- settings page

@app.route('/login', methods=['GET', 'POST'])
def login():
    """The sole public administrator entrypoint; credentials never live in config.yaml."""
    if request.method == 'POST':
        client = request.remote_addr or 'unknown'
        if not security.configured():
            return render_template('login.html', error='Server credentials are not configured.'), 503
        if not security.login_allowed(client):
            eventlog.record('login_rate_limited', ip=eventlog.client_ip())
            return render_template('login.html', error='Invalid credentials.'), 429
        if security.verify_admin_password(request.form.get('password', '')):
            session.clear()
            session['admin'] = True
            security.csrf_token()
            security.clear_login_failures(client)
            return redirect(security.local_redirect_target(request.args.get('next'), url_for('settings')))
        security.record_login_failure(client)
        eventlog.record('login_failed', ip=eventlog.client_ip())
        return render_template('login.html', error='Invalid credentials.'), 401
    return render_template('login.html', error=None)

@app.route('/healthz')
def healthz():
    return jsonify({'ok': bool(security.configured())}), 200 if security.configured() else 503

@app.route('/setting', methods=['GET', 'POST'])
def settings():
    voltage = _fresh_battery()
    percentage = battery.percentage(voltage) if voltage > 0 else 0

    if voltage > 0:
        print(f"Battery: {voltage:.0f}mV ({percentage:.1f}%)")
    else:
        print("No battery information available")

    def render(error=None):
        return render_template('settings.html',
                               config=config.current,
                               defaults=_flat_defaults(),
                               battery_voltage=voltage,
                               battery_percentage=percentage,
                               error=error,
                               csrf_token=security.csrf_token())

    if request.method != 'POST':
        return render()

    # Field name -> how to read it. Anything absent is treated as text.
    number = {'rotation': int, 'enhanced': float, 'contrast': float, 'strength': float,
              'sleep_start_hour': int, 'sleep_start_minute': int,
              'sleep_end_hour': int, 'sleep_end_minute': int, 'wakeup_interval': int,
              'battery_threshold': int, 'min_interval_hours': int}
    boolean = {'enabled'}
    # Tick boxes: an unticked box is simply absent from the form, so its absence
    # has to mean False rather than "unchanged"
    checkbox = {'use_telegram', 'use_line'}

    submitted = {}
    for section in config.sections():
        live = config.current[section]
        submitted[section] = {}
        for key, previous in live.items():
            if key in checkbox:
                submitted[section][key] = key in request.form
                continue
            if key in boolean:
                # A select rather than a checkbox, because an unchecked checkbox
                # is simply absent from the form and would look like "unchanged"
                submitted[section][key] = request.form.get(key, str(previous)) == 'true'
                continue
            raw = request.form.get(key, previous)
            try:
                submitted[section][key] = number[key](raw) if key in number else raw
            except (TypeError, ValueError):
                return render(error=f"'{key}' is not a valid number")

    try:
        config.write_file(submitted)
    except (ValueError, OSError) as error:
        return render(error=f"Configuration was not saved: {error}")

    # Record only the fields that actually moved, so the log stays useful
    changes = {}
    for section, values in submitted.items():
        for key, value in values.items():
            if config.current[section].get(key) != value:
                changes[key] = [config.current[section].get(key), value]

    config.apply(submitted)
    eventlog.record('settings_saved', ip=eventlog.client_ip(), changes=changes or None)

    return redirect(url_for('settings'))

@app.route('/')
def index():
    return redirect(url_for('settings'))

# ------------------------------------------------------------------- page data

@app.route('/status')
def status():
    """
    Live health for the header: can we reach Immich, and is the frame checking in.

    Returns machine-readable codes rather than sentences, so the page can render
    them in whichever language it is showing.
    """
    # The device is silent between wake-ups, so "connected" can only mean
    # "checked in recently enough", measured against its own wake-up interval.
    last_seen = (datetime.fromtimestamp(state.battery['updated'])
                 if state.battery['updated'] else None)
    shown_at = state.last_photo['shown_at']
    if shown_at and (last_seen is None or shown_at > last_seen):
        last_seen = shown_at

    if last_seen is None:
        frame = {'state': 'unknown', 'code': 'never', 'minutes_ago': None}
    else:
        minutes_ago = max(0, int((datetime.now() - last_seen).total_seconds() // 60))
        interval = int(config.immich()['wakeup_interval'])
        frame = {
            'state': 'ok' if minutes_ago <= interval * 2 else 'warn',
            'code': 'seen',
            'minutes_ago': minutes_ago,
            'last_seen': last_seen.strftime('%Y-%m-%d %H:%M'),
        }

    voltage = _fresh_battery()
    return _no_store(jsonify({
        'immich': immich.check_health(),
        'frame': frame,
        'battery': {
            'voltage': voltage,
            'percentage': battery.percentage(voltage) if voltage > 0 else None,
        },
    }))

@app.route('/log')
def read_log():
    """ Recent events, newest first """
    try:
        limit = min(max(int(request.args.get('limit', 50)), 1), 500)
    except ValueError:
        limit = 50
    return _no_store(jsonify({'entries': eventlog.recent(limit)}))

@app.route('/notify/bind', methods=['POST'])
def bind_notification():
    """
    Store credentials for a channel, but only once a test message has arrived.

    Nothing is written unless the send succeeds, so "bound" always means "known
    to work" rather than "something was typed in".
    """
    channel = request.form.get('channel')
    if channel not in credentials.FIELDS:
        return _no_store(jsonify({"error": "unknown_channel", "detail": channel})), 400

    values = {field: (request.form.get(field) or '').strip()
              for field in credentials.FIELDS[channel]}
    missing = [field for field, value in values.items() if not value]
    if missing:
        return _no_store(jsonify({"error": "not_configured",
                                  "detail": ', '.join(missing)})), 400

    try:
        notify.send("E-paper frame: notifications are set up", channel, values)
    except notify.NotifyError as error:
        eventlog.record('error', where='notify', message=error.code,
                        detail=error.detail, channel=channel)
        return _no_store(jsonify({"error": error.code, "detail": error.detail})), 502

    credentials.save_verified(channel, values)
    eventlog.record('notify_bound', channel=channel, ip=eventlog.client_ip())
    return _no_store(jsonify({'bound': True, 'channel': channel,
                              'channels': credentials.summary()}))

@app.route('/notify/unbind', methods=['POST'])
def unbind_notification():
    """ Forget a channel's credentials """
    channel = request.form.get('channel')
    if channel not in credentials.FIELDS:
        return _no_store(jsonify({"error": "unknown_channel", "detail": channel})), 400

    credentials.forget(channel)
    eventlog.record('notify_unbound', channel=channel, ip=eventlog.client_ip())
    return _no_store(jsonify({'channels': credentials.summary()}))

@app.route('/notify/channels')
def notification_channels():
    """ Whether each channel is bound. Never the credentials themselves. """
    return _no_store(jsonify({'channels': credentials.summary(),
                              'fields': {c: list(f) for c, f in credentials.FIELDS.items()}}))

@app.route('/notify/test', methods=['POST'])
def test_notification():
    """
    Send where a real warning would go, so the test proves the actual path.

    That means linked and ticked, not merely linked: testing a service the page
    has unticked would be reporting on something that will never be used.
    """
    channels = notify.send_channels()
    if not channels:
        return _no_store(jsonify({"error": "not_configured", "detail": "no channel bound"})), 400

    failures = {}
    for channel in channels:
        try:
            notify.send("E-paper frame: test notification", channel)
            eventlog.record('notified', channel=channel, reason='test')
        except notify.NotifyError as error:
            failures[channel] = error.detail or error.code
            eventlog.record('error', where='notify', message=error.code,
                            detail=error.detail, channel=channel)

    if failures:
        return _no_store(jsonify({"error": "rejected", "detail": failures})), 502
    return _no_store(jsonify({'sent': True, 'channels': channels}))

@app.route('/log/clear', methods=['POST'])
def clear_log():
    """ Empty the event log, then record that it happened """
    try:
        eventlog.clear()
    except Exception as error:
        return _no_store(jsonify({"error": str(error)})), 500

    eventlog.record('log_cleared', ip=eventlog.client_ip())
    return _no_store(jsonify({'cleared': True}))

def _serve_thumbnail(asset_id):
    try:
        content, content_type = immich.fetch_thumbnail(asset_id)
    except immich.ImmichError as error:
        return _no_store(jsonify({"error": error.message})), error.status
    return _no_store(send_file(io.BytesIO(content), mimetype=content_type))

@app.route('/preview/original')
def preview_original():
    """ The photo the frame is showing now """
    if not state.last_photo['asset_id']:
        return jsonify({"error": "No photo has been sent to the frame yet"}), 404
    return _serve_thumbnail(state.last_photo['asset_id'])

@app.route('/preview/next')
def preview_next():
    """ The photo the frame will be given on its next wake-up """
    if not state.next_photo['asset']:
        return jsonify({"error": "No photo has been chosen yet"}), 404
    return _serve_thumbnail(state.next_photo['asset']['id'])

@app.route('/next', methods=['GET', 'POST'])
def upcoming_photo():
    """
    What the frame will show next. GET chooses one only if none is remembered;
    POST always picks again, which is what the "swap" button uses.
    """
    album = config.immich()['album']
    try:
        if request.method == 'POST' or not state.next_photo['asset'] \
                or state.next_photo['album'] != album:
            immich.refresh_next_photo()
    except immich.ImmichError as error:
        eventlog.record('error', where='next', message=error.message, ip=eventlog.client_ip())
        return _no_store(jsonify({"error": error.message})), error.status

    asset = state.next_photo['asset']
    if not asset:
        return _no_store(jsonify({"error": "No photo could be chosen"})), 404

    if request.method == 'POST':
        eventlog.record('photo_swapped', ip=eventlog.client_ip(),
                        asset_id=asset['id'], album=album)

    return _no_store(jsonify({
        'asset_id': asset['id'],
        'link': immich.photo_link(asset['id']),
        'taken_at': immich.taken_at_text(asset),
    }))

# -------------------------------------------------------------------- OTA update

@app.route('/ota/status', methods=['GET'])
def ota_status():
    """Return staged firmware details and last update execution result."""
    return _no_store(jsonify({
        'staged': ota.get_staged_info(),
        'last_result': ota.get_last_result()
    }))

@app.route('/ota/upload', methods=['POST'])
def ota_upload():
    """Upload a new .bin firmware binary file to stage an OTA update."""
    uploaded_file = request.files.get('firmware') or request.files.get('file')
    if not uploaded_file or not uploaded_file.filename:
        return _no_store(jsonify({'error': 'no_file_uploaded'})), 400

    if not uploaded_file.filename.lower().endswith('.bin'):
        return _no_store(jsonify({'error': 'invalid_file_type', 'detail': 'File must be a .bin binary'})), 400

    try:
        meta = ota.stage_firmware(uploaded_file)
        eventlog.record('ota_staged', filename=meta['filename'], sha256=meta['sha256'],
                        size=meta['size'], ip=eventlog.client_ip())
        return _no_store(jsonify({'staged': meta}))
    except Exception as error:
        eventlog.record('error', where='ota_upload', message=str(error), ip=eventlog.client_ip())
        return _no_store(jsonify({'error': 'upload_failed', 'detail': str(error)})), 500

@app.route('/ota/cancel', methods=['POST'])
def ota_cancel():
    """Cancel staged firmware update."""
    cancelled = ota.cancel_staged()
    if cancelled:
        eventlog.record('ota_cancelled', ip=eventlog.client_ip())
    return _no_store(jsonify({'cancelled': cancelled}))

@app.route('/ota/check', methods=['GET'])
def ota_check():
    """Device endpoint to check if an OTA firmware update is staged."""
    staged = ota.get_staged_info()
    # Keep the device probe deliberately small.  The frame only needs this
    # boolean; metadata belongs to /ota/status and making it part of the probe
    # needlessly increases the amount of JSON the firmware must parse.
    return jsonify({'available': staged is not None})

@app.route('/ota/binary', methods=['GET'])
def ota_binary():
    """Device endpoint to stream the staged raw firmware binary."""
    filepath = ota.get_binary_filepath()
    info = ota.get_staged_info()
    if not filepath or not info:
        return jsonify({'error': 'no_staged_firmware'}), 404

    response = make_response(send_file(filepath, mimetype='application/octet-stream',
                                       as_attachment=True, download_name=info['filename']))
    response.headers['Content-Length'] = str(info['size'])
    response.headers['X-EPF-OTA-SHA256'] = info['sha256']
    return response

@app.route('/ota/ack', methods=['POST'])
def ota_ack():
    """Device endpoint to report the outcome of an OTA firmware update."""
    data = request.get_json(silent=True) or request.form or {}
    status_val = data.get('status') or request.headers.get('X-EPF-OTA-Status', 'failed')
    error_val = data.get('error') or request.headers.get('X-EPF-OTA-Error', '')
    mac = request.headers.get('X-Device-Mac', '')

    if status_val not in {'success', 'failed'}:
        status_val = 'failed'

    res = ota.record_ack(status_val, error_detail=error_val, ip=eventlog.client_ip(), mac=mac)
    return jsonify({'acknowledged': True, 'result': res})

# ------------------------------------------------- the contract with the frame

@app.route('/download', methods=['GET'])
def process_and_download():
    """
    The panel image, as C-array source text: "XX,XX,..." terminated by "};".

    The device sends its battery voltage in the batteryCap request header, in
    millivolts, and reads the X-Photo-Url response header for the NFC tag.
    """
    reported_mv = 0
    try:
        reported_mv = float(request.headers.get('batteryCap', '0'))
        if reported_mv > 0:
            state.battery.update({'voltage': reported_mv, 'updated': time.time()})
    except (TypeError, ValueError):
        reported_mv = 0

    album = config.immich()['album']
    if not config.immich()['url'] or not album:
        message = "Immich URL or Album not configured"
        eventlog.record('error', where='download', message=message, ip=eventlog.client_ip())
        return jsonify({"error": message}), 500

    try:
        # An unacknowledged delivery is immutable: every retry returns identical
        # bytes and never advances photo history.
        active = deliveries.active()
        if active:
            delivery_id, asset_id, delivery_album, payload = active
            if delivery_album != album:
                return jsonify(error='delivery_pending'), 409
            selected, albumid = {'id': asset_id}, None
        else:
            with state.lock:
                if state.next_photo['asset'] and state.next_photo['album'] == album:
                    selected = state.next_photo['asset']
                    albumid = state.next_photo['album_id']
                else:
                    albumid = immich.resolve_album_id()
                    selected = immich.select_asset(immich.list_album_assets(albumid))
                asset_id = selected['id']
                settings_now = config.immich()
                settings_hash = hashlib.sha256(json.dumps({key: settings_now[key] for key in ('rotation', 'display_mode', 'enhanced', 'contrast', 'strength')}, sort_keys=True).encode()).hexdigest()
                payload = deliveries.cached_payload(asset_id, settings_hash)
                if payload is None:
                    image = imaging.open_asset(io.BytesIO(immich.fetch_original(asset_id)), selected.get('originalPath'))
                    processed = imaging.scale_img_in_memory(image, rotation=settings_now['rotation'], display_mode=settings_now['display_mode'], enhanced=settings_now['enhanced'], contrast=settings_now['contrast'], strength=settings_now['strength'])
                    payload = imaging.pack_binary_for_panel(processed)
                    deliveries.cache_payload(asset_id, settings_hash, payload)
                delivery_id, asset_id, _, payload = deliveries.create(asset_id, album, payload)

        response = make_response(payload)
        response.mimetype = 'application/octet-stream'
        response.headers['Content-Length'] = str(len(payload))
        response.headers['X-EPF-Protocol'] = '2'
        response.headers['X-Delivery-Id'] = delivery_id
        response.headers['X-Asset-Id'] = asset_id
        response.headers['X-Payload-SHA256'] = hashlib.sha256(payload).hexdigest()
        response.headers['X-Sleep-Seconds'] = str(_sleep_duration_seconds())
        response.headers['X-EPF-OTA-Available'] = '1' if ota.get_staged_info() else '0'
        # The OLED emulator uses this header to show the original photo name
        # while still consuming the same prepared panel payload as the frame.
        photo_name = selected.get('originalFileName') or f"image_{asset_id}"
        photo_name = str(photo_name).replace('\r', ' ').replace('\n', ' ')
        response.headers['X-Photo-Name'] = photo_name.encode(
            'ascii', 'replace'
        ).decode('ascii')
        # Deep link for writing an NFC tag; the firmware does not read it yet
        if albumid:
            response.headers['X-Photo-Url'] = f"https://my.immich.app/albums/{albumid}/photos/{asset_id}"

        # MAC and signal strength are only here if the firmware sends them: HTTP
        # carries no MAC and the container cannot read the LAN's ARP table.
        eventlog.record('checkin', ip=eventlog.client_ip(), asset_id=asset_id, album=album,
                        battery_mv=int(reported_mv) if reported_mv else None,
                        battery_pct=battery.percentage(reported_mv) if reported_mv else None,
                        mac=request.headers.get('X-Device-Mac'),
                        rssi=request.headers.get('X-Device-Rssi'),
                        agent=request.headers.get('User-Agent'))

        if reported_mv:
            # Sent on a thread: the frame gives up after 50 seconds and must not
            # wait on Telegram or LINE
            notify.check_battery(battery.percentage(reported_mv), reported_mv)

        return response

    except immich.ImmichError as error:
        eventlog.record('error', where='download', message=error.message, ip=eventlog.client_ip())
        return jsonify({"error": error.message}), error.status
    except Exception as error:
        eventlog.record('error', where='download', message=type(error).__name__, ip=eventlog.client_ip())
        return jsonify({"error": 'download_failed'}), 500

@app.route('/ack', methods=['POST'])
def acknowledge_delivery():
    delivery_id = request.headers.get('X-Delivery-Id', '')
    active = deliveries.active()
    if not delivery_id or not active or active[0] != delivery_id:
        return jsonify(error='unknown_delivery'), 409
    _, asset_id, _album, _payload = active
    # Tracking is idempotent. Write it before the acknowledgement so a storage
    # failure keeps the valid delivery retryable rather than losing history.
    if not tracking.mark_shown(asset_id):
        eventlog.record('error', where='ack', message='tracking_write_failed', ip=eventlog.client_ip())
        return jsonify(error='tracking_write_failed'), 503
    acknowledged = deliveries.acknowledge(delivery_id)
    if not acknowledged:
        return jsonify(error='unknown_delivery'), 409
    with state.lock:
        state.clear_next_photo()
        state.last_photo.update({'asset_id': asset_id, 'shown_at': datetime.now(), 'taken_at': ''})
    eventlog.record('delivery_acknowledged', delivery_id=delivery_id, ip=eventlog.client_ip())
    return jsonify(acknowledged=True)

@app.route('/delivery/cancel', methods=['POST'])
def cancel_delivery():
    """Administrator recovery action for a lost or replaced frame."""
    delivery_id = deliveries.cancel_active()
    if delivery_id is None:
        return jsonify(cancelled=False, error='no_active_delivery'), 404
    eventlog.record('delivery_cancelled', delivery_id=delivery_id, ip=eventlog.client_ip())
    return jsonify(cancelled=True, delivery_id=delivery_id)

@app.route('/delivery', methods=['GET'])
def delivery_status():
    active = deliveries.active()
    if active is None:
        return jsonify(active=False)
    delivery_id, asset_id, album, _payload = active
    return jsonify(active=True, delivery_id=delivery_id, asset_id=asset_id, album=album)

def _sleep_data():
    """Sleep metadata shared by the authenticated compatibility endpoint and /download."""
    now = datetime.now()
    settings_now = config.immich()
    interval = int(settings_now['wakeup_interval'])
    def next_interval(base_time, intervals=1):
        total_minutes = base_time.hour * 60 + base_time.minute
        upcoming = interval * ((total_minutes // interval) + intervals)
        candidate = base_time.replace(hour=(upcoming % 1440) // 60, minute=upcoming % 60, second=0, microsecond=0)
        return candidate + timedelta(days=1) if candidate < base_time else candidate
    next_wakeup = next_interval(now)
    sleep_start = now.replace(hour=settings_now['sleep_start_hour'], minute=settings_now['sleep_start_minute'], second=0, microsecond=0)
    sleep_end = now.replace(hour=settings_now['sleep_end_hour'], minute=settings_now['sleep_end_minute'], second=0, microsecond=0)
    if sleep_end < sleep_start:
        if now >= sleep_start: sleep_end += timedelta(days=1)
        elif now < sleep_end: sleep_start -= timedelta(days=1)
    if sleep_start <= next_wakeup < sleep_end:
        next_wakeup = sleep_end
    sleep_ms = int((next_wakeup - now).total_seconds() * 1000)
    if sleep_ms < 600000:
        next_wakeup = next_interval(now, 2)
        if sleep_start <= next_wakeup < sleep_end: next_wakeup = sleep_end
        sleep_ms = int((next_wakeup - now).total_seconds() * 1000)
    return {'current_time': now.strftime('%Y-%m-%d %H:%M:%S'), 'next_wakeup': next_wakeup.strftime('%Y-%m-%d %H:%M:%S'), 'sleep_duration': sleep_ms}

def _sleep_duration_seconds():
    return max(1, _sleep_data()['sleep_duration'] // 1000)

@app.route('/sleep', methods=['GET'])
def get_sleep_duration():
    """
    How long the frame should sleep, in milliseconds. Derived from local time, so
    TZ has to be set or the quiet hours land at the wrong time of day.
    """
    return jsonify(_sleep_data())

# -------------------------------------------------------------------- start-up

def initialize_application():
    """Initialize exactly once per worker; Gunicorn imports this module directly."""
    global _observer, _initialized
    if _initialized:
        return
    if not security.configured(app):
        raise RuntimeError('EPF_SESSION_SECRET, EPF_ADMIN_PASSWORD_HASH, and EPF_DEVICE_TOKEN are required')
    config.ensure_file(config.CONFIG_PATH)
    config.verify_storage(config.CONFIG_PATH)
    loaded = config.read_file(config.CONFIG_PATH)
    if loaded is None:
        raise RuntimeError('No valid configuration file is available')
    config.apply(loaded)
    _observer = config.start_watcher(config.apply, config.CONFIG_PATH)
    eventlog.record('startup')
    _initialized = True

def _stop_observer():
    if _observer:
        _observer.stop()
        _observer.join(timeout=5)

atexit.register(_stop_observer)

def main():
    if os.environ.get('EPF_ALLOW_DEV_SERVER') != '1':
        raise RuntimeError('Direct Flask serving is disabled; run the Gunicorn command from Dockerfile.')
    app.run(host='0.0.0.0', port=5000, use_reloader=False)

initialize_application()

if __name__ == '__main__':
    main()
