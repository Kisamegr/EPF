"""Transactional, retry-safe device deliveries stored beside the photo history."""
import os
import sqlite3
import threading
import time
import uuid
from . import config

DB_PATH = os.path.join(config.photodir, 'epf.sqlite3')
_lock = threading.RLock()
DEFAULT_LEASE_SECONDS = 24 * 60 * 60

def _connect():
    conn = sqlite3.connect(DB_PATH, timeout=10, isolation_level=None)
    conn.execute('PRAGMA journal_mode=WAL')
    conn.execute('PRAGMA busy_timeout=10000')
    conn.execute('''CREATE TABLE IF NOT EXISTS deliveries (id TEXT PRIMARY KEY, asset_id TEXT NOT NULL, album TEXT NOT NULL, payload BLOB NOT NULL, created REAL NOT NULL, expires REAL NOT NULL, acknowledged REAL)''')
    columns = {row[1] for row in conn.execute('PRAGMA table_info(deliveries)')}
    if 'expires' not in columns:
        conn.execute('ALTER TABLE deliveries ADD COLUMN expires REAL')
        conn.execute('UPDATE deliveries SET expires = created + ? WHERE expires IS NULL',
                     (DEFAULT_LEASE_SECONDS,))
    conn.execute('CREATE TABLE IF NOT EXISTS notification_state (name TEXT PRIMARY KEY, value REAL NOT NULL)')
    conn.execute('''CREATE TABLE IF NOT EXISTS panel_cache (
        asset_id TEXT NOT NULL, settings_hash TEXT NOT NULL, payload BLOB NOT NULL,
        created REAL NOT NULL, PRIMARY KEY(asset_id, settings_hash))''')
    return conn

def active():
    now = time.time()
    with _lock, _connect() as conn:
        return conn.execute('SELECT id, asset_id, album, payload FROM deliveries WHERE acknowledged IS NULL AND expires > ? ORDER BY created DESC LIMIT 1', (now,)).fetchone()

def create(asset_id, album, payload, lease_seconds=DEFAULT_LEASE_SECONDS):
    delivery_id = uuid.uuid4().hex
    now = time.time()
    with _lock, _connect() as conn:
        conn.execute('BEGIN IMMEDIATE')
        cutoff = now - 30 * 24 * 60 * 60
        conn.execute('DELETE FROM deliveries WHERE (acknowledged IS NOT NULL AND acknowledged < ?) OR (acknowledged IS NULL AND expires < ?)',
                     (cutoff, cutoff))
        existing = conn.execute('SELECT id, asset_id, album, payload FROM deliveries WHERE acknowledged IS NULL AND expires > ? ORDER BY created DESC LIMIT 1', (now,)).fetchone()
        if existing:
            conn.execute('COMMIT')
            return existing
        conn.execute('INSERT INTO deliveries (id, asset_id, album, payload, created, expires) VALUES (?, ?, ?, ?, ?, ?)',
                     (delivery_id, asset_id, album, payload, now, now + max(1, int(lease_seconds))))
        conn.execute('COMMIT')
    return (delivery_id, asset_id, album, payload)

def acknowledge(delivery_id):
    with _lock, _connect() as conn:
        now = time.time()
        conn.execute('BEGIN IMMEDIATE')
        row = conn.execute('SELECT asset_id, album FROM deliveries WHERE id=? AND acknowledged IS NULL AND expires > ?',
                           (delivery_id, now)).fetchone()
        if row is None:
            conn.execute('ROLLBACK')
            return None
        result = conn.execute('UPDATE deliveries SET acknowledged=? WHERE id=? AND acknowledged IS NULL AND expires > ?',
                              (now, delivery_id, now))
        if result.rowcount != 1:
            conn.execute('ROLLBACK')
            return None
        conn.execute('COMMIT')
        return row

def cancel_active():
    """Cancel the current lease so a replacement frame can fetch a new photo."""
    with _lock, _connect() as conn:
        conn.execute('BEGIN IMMEDIATE')
        row = conn.execute('SELECT id FROM deliveries WHERE acknowledged IS NULL AND expires > ? ORDER BY created DESC LIMIT 1',
                           (time.time(),)).fetchone()
        if row is None:
            conn.execute('ROLLBACK')
            return None
        conn.execute('UPDATE deliveries SET expires=? WHERE id=?', (time.time(), row[0]))
        conn.execute('COMMIT')
        return row[0]

def prune(retention_seconds=30 * 24 * 60 * 60):
    """Discard old completed and expired delivery payloads from the persistent volume."""
    cutoff = time.time() - retention_seconds
    with _lock, _connect() as conn:
        conn.execute('DELETE FROM deliveries WHERE (acknowledged IS NOT NULL AND acknowledged < ?) OR (acknowledged IS NULL AND expires < ?)',
                     (cutoff, cutoff))

def claim_notification(name, minimum_interval):
    """Atomically reserve a persistent notification rate-limit slot."""
    now = time.time()
    with _lock, _connect() as conn:
        conn.execute('BEGIN IMMEDIATE')
        row = conn.execute('SELECT value FROM notification_state WHERE name=?', (name,)).fetchone()
        if row and now - row[0] < minimum_interval:
            conn.execute('COMMIT')
            return False
        conn.execute('INSERT INTO notification_state(name, value) VALUES (?, ?) ON CONFLICT(name) DO UPDATE SET value=excluded.value', (name, now))
        conn.execute('COMMIT')
        return True

def cached_payload(asset_id, settings_hash):
    with _lock, _connect() as conn:
        row = conn.execute('SELECT payload FROM panel_cache WHERE asset_id=? AND settings_hash=?',
                           (asset_id, settings_hash)).fetchone()
        return row[0] if row else None

def cache_payload(asset_id, settings_hash, payload):
    with _lock, _connect() as conn:
        conn.execute('INSERT OR REPLACE INTO panel_cache(asset_id, settings_hash, payload, created) VALUES (?, ?, ?, ?)',
                     (asset_id, settings_hash, payload, time.time()))
        # Retain only the 50 most-recent panel images on the persistent volume.
        conn.execute('DELETE FROM panel_cache WHERE rowid NOT IN (SELECT rowid FROM panel_cache ORDER BY created DESC LIMIT 50)')
