# Remote Settings Server — Superseded Decision

This was an implementation plan for a persistent ESP32 settings page reachable
on the LAN or Tailscale. It is intentionally **not** the current design.

## Current design

- The captive portal is used only for initial provisioning.
- There is no station-mode settings web server after provisioning.
- The EE04 enters provisioning when its physical setup button is held during
  boot.
- The buttonless OLED prototype enters provisioning only when it has no saved
  Wi-Fi settings. To change its saved Wi-Fi or server URL, erase flash and
  upload again.
- The portal has a per-device, persisted 12-character Wi-Fi password displayed
  by the device: `epf-` plus eight uppercase letters/digits.

This avoids leaving a management service exposed on the LAN. Remote frames use
the Tailscale-enabled firmware profile for transport to EPF; they still do not
expose an ESP32 settings service.

## If this is revisited

Any persistent settings endpoint must be explicitly approved as a new feature
and must include authentication, CSRF protection for state-changing actions,
secret masking, a narrow network policy, and tests on both the 4 MB OLED board
and the EE04. Do not re-enable the old server merely because its source files
remain in the repository.
