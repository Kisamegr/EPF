# Changelog

This file records the current implementation state and deployment decisions so
future contributors and coding agents do not need to rediscover them from the
audit notes or serial logs.

## Unreleased

### Security and server

- Implemented the findings in `EPF_AUDIT.md` and
  `EPF_UNCOMMITTED_CHANGES_VERIFICATION.md`.
- The server now requires an administrator password hash, a device bearer
  token, and a session secret from `.env`. Admin forms use CSRF protection,
  and login attempts are rate limited.
- Immich is constrained to explicitly allowed literal-IP origins/addresses;
  use the direct Immich container address, not its reverse-proxy hostname.
- Docker listens only on loopback by default. `EPF_BIND_ADDRESS` may expose one
  explicit LAN address for a reverse proxy; restrict that host port in the
  firewall to the proxy host.
- Fixed Gunicorn startup outside an application context and startup validation
  of the persisted configuration.
- Implemented binary deliveries with a SHA-256 checksum, delivery ID, staged
  SPIFFS write, acknowledgement, 24-hour lease, and an administrator action to
  release a pending delivery.

### Firmware

- Normal firmware profiles require HTTPS and validate the configured server CA
  certificate. HTTP is accepted only by a Tailscale-enabled profile, where
  Tailscale provides the encrypted transport.
- Added secure provisioning access: a device token is required for API calls;
  the normal station-mode settings server is intentionally disabled after
  setup. The EE04 re-enters setup with its physical button; the buttonless OLED
  prototype requires erasing flash to change saved Wi-Fi/server settings.
- Enlarged SPIFFS and added capacity, payload-length, and checksum validation
  before an e-paper update.
- Added and verified PlatformIO profiles for OLED/EE04, local/Tailscale builds.
- Fixed provisioning password persistence: ESP32 NVS key names are limited to
  15 characters. The previous overlong key caused the password displayed on
  the OLED and the access point password to differ. It now uses `prov_pass` and
  caches one value for the full boot session.
- Provisioning password format is now exactly 12 characters: `epf-` followed
  by eight uppercase letters/digits. The prefix is lowercase and passwords are
  case-sensitive.

### Current deployment decisions

- This deployment is one frame per EPF instance/device token. Multi-device
  support is deliberately deferred.
- EPF-to-Immich uses a direct local address (for example
  `http://192.0.2.220:2283`).
- Frame-to-EPF uses an HTTPS reverse-proxy address (for example
  `https://frame.example.net`). The CA certificate for that endpoint
  must be compiled into `Arduino/device_secrets.h` and is not committed.

### Validation performed

- Python syntax checks and JavaScript checks passed.
- `prototype_oled`, `ee04_epaper`, and `ee04_epaper_tailscale` PlatformIO
  builds passed. The local bundled Python runtime lacks several server test
  dependencies, so the Python test suite needs the normal project dependency
  environment or CI to run.
