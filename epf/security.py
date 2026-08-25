"""Authentication, CSRF protection, device credentials, and simple rate limits."""
import hmac
import os
import secrets
import threading
import time
from collections import defaultdict, deque
from functools import wraps
from urllib.parse import urlsplit

from flask import current_app, jsonify, redirect, request, session, url_for
from werkzeug.security import check_password_hash

SAFE_METHODS = {'GET', 'HEAD', 'OPTIONS'}
DEVICE_PATHS = {'/download', '/ack', '/sleep', '/ota/check', '/ota/binary', '/ota/ack'}
PUBLIC_PATHS = {'/login', '/healthz'}
_lock = threading.Lock()
_requests = defaultdict(deque)
_login_failures = defaultdict(deque)

def configure(app):
    app.secret_key = os.environ.get('EPF_SESSION_SECRET') or secrets.token_urlsafe(48)
    app.config['EPF_ADMIN_PASSWORD_HASH'] = os.environ.get('EPF_ADMIN_PASSWORD_HASH', '')
    app.config['EPF_DEVICE_TOKEN'] = os.environ.get('EPF_DEVICE_TOKEN', '')
    app.config['SESSION_COOKIE_HTTPONLY'] = True
    app.config['SESSION_COOKIE_SAMESITE'] = 'Strict'
    app.config['SESSION_COOKIE_SECURE'] = os.environ.get('EPF_COOKIE_SECURE', 'true').lower() == 'true'

def configured(app=None):
    """Return whether required secrets are present, including during app import."""
    settings = (app or current_app).config
    return bool(settings['EPF_ADMIN_PASSWORD_HASH'] and settings['EPF_DEVICE_TOKEN'] and os.environ.get('EPF_SESSION_SECRET'))

def csrf_token():
    token = session.get('_csrf')
    if not token:
        token = secrets.token_urlsafe(32)
        session['_csrf'] = token
    return token

def _accepts_json():
    return request.path.startswith(('/notify/', '/log/', '/next', '/status', '/preview/', '/ota/'))

def _fail_auth(status=401):
    if _accepts_json() or request.method != 'GET':
        return jsonify(error='authentication_required'), status
    return redirect(url_for('login', next=request.full_path))

def _rate_limit(identity, limit, window=60):
    now = time.monotonic()
    key = (identity, request.path)
    with _lock:
        queue = _requests[key]
        while queue and queue[0] <= now - window:
            queue.popleft()
        if len(queue) >= limit:
            return False
        queue.append(now)
    return True

def device_required(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        token = request.headers.get('Authorization', '').removeprefix('Bearer ').strip()
        expected = current_app.config['EPF_DEVICE_TOKEN']
        if not expected or not token or not hmac.compare_digest(token, expected):
            return jsonify(error='device_authentication_required'), 401
        if not _rate_limit('device:' + token[:12], 12):
            return jsonify(error='rate_limited'), 429
        return view(*args, **kwargs)
    return wrapped

def admin_required(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        if not configured() or not session.get('admin'):
            return _fail_auth()
        if not _rate_limit('admin:' + (request.remote_addr or 'unknown'), 120):
            return jsonify(error='rate_limited'), 429
        if request.method not in SAFE_METHODS:
            supplied = request.headers.get('X-CSRF-Token') or request.form.get('csrf_token')
            if not supplied or not hmac.compare_digest(supplied, session.get('_csrf', '')):
                return jsonify(error='csrf_failed'), 400
        return view(*args, **kwargs)
    return wrapped

def protect_routes(app):
    @app.before_request
    def require_boundary():
        if request.path in PUBLIC_PATHS or request.path.startswith('/static/'):
            return None
        endpoint = current_app.view_functions.get(request.endpoint)
        if endpoint is None:
            return None
        if getattr(endpoint, '_epf_protected', False):
            return endpoint()
        wrapper = device_required(endpoint) if request.path in DEVICE_PATHS else admin_required(endpoint)
        wrapper._epf_protected = True
        current_app.view_functions[request.endpoint] = wrapper
        return wrapper()

def verify_admin_password(password):
    password_hash = current_app.config['EPF_ADMIN_PASSWORD_HASH']
    return bool(password_hash and check_password_hash(password_hash, password))

def login_allowed(ip, maximum=5, window=900):
    now = time.monotonic()
    with _lock:
        failures = _login_failures[ip]
        while failures and failures[0] <= now - window:
            failures.popleft()
        return len(failures) < maximum

def record_login_failure(ip):
    with _lock:
        _login_failures[ip].append(time.monotonic())

def clear_login_failures(ip):
    with _lock:
        _login_failures.pop(ip, None)

def local_redirect_target(value, default):
    parsed = urlsplit(value or '')
    if parsed.scheme or parsed.netloc or not parsed.path.startswith('/') or parsed.path.startswith('//'):
        return default
    return value
