# Third-party ESP-IDF components

This directory contains the MicroLink Tailscale-compatible ESP32 client and
its `wireguard_lwip` dependency, copied from:

<https://github.com/CamM2325/microlink>

MicroLink is used only by the `ee04_epaper` PlatformIO environment. The
project uses its standard lwIP/WireGuard network interface, so the existing
HTTP client can reach the EPF server through its Tailscale `100.x.x.x` address.
