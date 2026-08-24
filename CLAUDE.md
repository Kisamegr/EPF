# EPF contributor notes

Read [`CHANGELOG.md`](CHANGELOG.md) before changing this project. It records
the completed audit work, deployment choices, and the most recent firmware
provisioning fix.

## Project shape

EPF is an Immich-backed e-paper photo frame with two codebases:

- **Server:** Flask/Gunicorn/Docker in `app.py` and `epf/`. It reads from an
  Immich album, processes images, and serves device deliveries.
- **Firmware:** PlatformIO ESP32 project in `Arduino/`. `prototype_oled` is
  the development board; `ee04_epaper` is the physical target. Each also has
  an optional Tailscale environment.

Important server modules include `epf/security.py` (authentication, CSRF,
rate limits), `epf/config.py` (persisted settings/watcher), and
`epf/deliveries.py` (leased/acknowledged delivery state).

## Non-negotiable security decisions

- Do not remove authentication, CSRF checks, device-token checks, checksum
  verification, delivery acknowledgements, or HTTPS certificate validation to
  make testing easier.
- Normal local firmware requires HTTPS. Only a Tailscale-enabled profile may
  use application-layer HTTP, because Tailscale encrypts that path.
- The server's Immich address must be an allowed literal-IP URL, not a DNS
  hostname. The frame's EPF URL is separate and should normally be the HTTPS
  reverse-proxy address.
- Docker binds to `127.0.0.1` by default. `EPF_BIND_ADDRESS` is an opt-in
  single LAN address for a reverse proxy, never `0.0.0.0`; restrict the host
  firewall to the reverse-proxy machine.
- The active model is **one frame per EPF instance/device token**. Do not add
  multi-device behavior unless explicitly asked.
- The station-mode ESP settings page is deliberately disabled. Keep initial
  setup within the captive portal and preserve physical re-entry behavior.

## Local secrets and configuration

Never read, display, or commit `.env`, `Arduino/device_secrets.h`, or
`Arduino/tailscale_secrets.h`. Templates document their expected shape.

Required server secrets are `EPF_ADMIN_PASSWORD_HASH`, `EPF_DEVICE_TOKEN`, and
`EPF_SESSION_SECRET`. The running container also needs `IMMICH_API_KEY`,
`IMMICH_ALLOWED_ORIGINS`, and `IMMICH_ALLOWED_IPS`. The persisted configuration
is `config/config.yaml` when using Compose; do not assume the example config is
the active one.

The device secret file must provide `EPF_DEVICE_TOKEN` and, for ordinary HTTPS
profiles, `EPF_HAS_SERVER_CA_CERT` plus `EpfServerCaCert`. Use the exact macro
form shown in `Arduino/device_secrets.h.example`.

## Delivery protocol

The device requests a protected delivery, stages the binary payload in SPIFFS,
checks the advertised byte count and SHA-256 header, updates the display, and
then sends its delivery ID to `POST /ack`. A delivery is leased for 24 hours;
the administrator can release it from the portal if a frame is replaced.

Any server or firmware change to `/download`, `/ack`, headers, payload size, or
authentication must change both sides and add/update tests.

## Firmware behavior

- `prototype_oled`: buttonless. On a fresh/erased device it opens the captive
  portal; after settings are saved, erase flash to provision it again.
- `ee04_epaper`: hold the physical setup button during boot to open the portal.
- The provisioning SSID is `EPF-Setup`. Its password is exactly 12 characters:
  lowercase `epf-` followed by eight uppercase letters/digits. It is persisted
  under the NVS key `prov_pass` (key names may not exceed 15 characters).
- The server URL saved by provisioning overrides `SERVER_BASE_URL` in
  `Arduino/config.h`. Check the serial line `nas url:` when diagnosing a
  device.

From PowerShell, PlatformIO may not be on `PATH`; use:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e prototype_oled
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e prototype_oled -t upload
```

Build all affected environments when firmware/platform settings change. Recent
verified builds: `prototype_oled`, `ee04_epaper`, and
`ee04_epaper_tailscale`.

## Verification and editing

- Use `apply_patch` for source/doc edits and preserve unrelated uncommitted
  work. Do not reset, clean, or overwrite user files.
- The bundled local Python runtime may lack Flask/PyYAML/Werkzeug; use CI or a
  normal project environment for the Python test suite rather than weakening
  tests. Syntax checks still work locally.
- Docker is operated by the user in this workspace; do not assume a local
  Docker CLI is available.
- `cpy.so` is a committed Linux binary. Editing `cpy.pyx` requires rebuilding
  that binary on Linux; Windows cannot import it directly.
- Keep code, comments, docs, and commit messages in English.

## Documentation maintenance

When behavior or deployment guidance changes, update `README.md`, this file,
and `CHANGELOG.md` in the same change. `remote-settings-server-plan.md` is an
archived, superseded design decision—not a backlog item.
