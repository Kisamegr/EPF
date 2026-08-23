# EPF — Immich e-paper photo frame

EPF is a private photo-frame system built around [Immich](https://immich.app).
The server selects a photo from an Immich album, processes it for the six-color
Spectra e-paper panel, and streams the prepared panel data to an ESP32.

The project has two parts:

- **Server** — a Flask application running in Docker. It talks to Immich,
  selects the next photo, crops and enhances it, converts it to the panel
  palette, and serves the packed result.
- **Firmware** — an ESP32 application that connects to Wi-Fi, downloads the
  prepared result, and either writes it to the e-paper panel or emulates that
  final step on a small OLED.

The project is still a work in progress. The OLED profile is the convenient
development and networking test path; the EE04 profile is the target hardware
path.

## System flow

```text
Immich
   |
   |  API: album metadata and original photo
   v
EPF Flask server
   |  select, crop, enhance, dither, pack
   |  GET /download
   v
ESP32 firmware
   |
   +-- ee04_epaper_tailscale: Wi-Fi -> MicroLink/Tailscale -> private EPF server
   |
   +-- prototype_oled: consume the complete payload and show the photo name
   |
   +-- ee04_epaper / ee04_epaper_tailscale: send the payload to the Spectra 6 panel
   |
   +-- GET /sleep, then deep-sleep on the e-paper profile
```

The OLED profile performs the same `/download` request and consumes the same
prepared payload as the e-paper profile. It only skips the physical e-paper
commands and shows the selected Immich photo name on the OLED instead.

## Server setup

### Requirements

- Docker Desktop on Windows, using the WSL 2 Linux engine, or Docker Engine on
  Linux/NAS
- An Immich server reachable from the EPF server container
- An Immich API key with these read-only permissions:
  - `album.read`
  - `asset.read`
  - `asset.view`
  - `asset.download`

The API-key user must be able to see the album containing the photos.

### Run with Docker Compose

The Compose file is [`docker-compose.yml`](docker-compose.yml). Create a `.env`
file beside it:

```env
IMMICH_API_KEY=your-immich-api-key
TZ=Europe/Stockholm
EPF_PORT=15001
```

Start the published image:

```powershell
docker compose up -d
```

Open the settings page at:

```text
http://localhost:15001
```

In the settings page, configure:

- **Immich Server URL** — for example `http://192.168.1.220:2283`
- **Album Name** — must match the Immich album name exactly, including case

The Immich URL is the address of Immich from the Docker container. The ESP32
does not connect directly to Immich; it connects to the computer or NAS
running EPF.

### Build the server image locally

From the repository root:

```powershell
docker build -t epf:local .
```

To use that image with Compose, change the service in
`docker-compose.yml` to:

```yaml
services:
  epf:
    image: epf:local
```

Then recreate the container:

```powershell
docker compose up -d --force-recreate
```

Alternatively, comment out `image:` and enable `build: .`, then use:

```powershell
docker compose up -d --build
```

After changing server code, rebuild the image before recreating the container.

### Persistent data

Compose mounts these directories beside the repository:

```text
config\   settings, shown-photo history, and system log
photos\   tracking data and history files
```

Keep both mounts when deploying or updating the image. Recreating a container
without them resets the settings and photo history.

### Portainer

For a Portainer deployment, create a Stack from `docker-compose.yml` or from a
Git repository. The Portainer host must be able to pull the selected image.
Use a registry image tag such as `epf:0.1.0`, or build/import the image on the
Portainer host; an image tagged `epf:local` on a Windows PC is not automatically
available on another Docker host.

## ESP32 firmware

The firmware is under [`Arduino`](Arduino), and PlatformIO uses that directory
as its source directory.

### Hardware profiles

| Profile | Board and display | Behavior |
| --- | --- | --- |
| `prototype_oled` | ESP-WROOM-32 + 128x32 SSD1306 OLED | Default local profile; performs the server exchange and shows status/photo name on OLED |
| `prototype_oled_tailscale` | ESP-WROOM-32 + 128x32 SSD1306 OLED | Experimental 4 MB Tailscale test profile |
| `ee04_epaper` | Seeed XIAO ePaper Display Board EE04 + 7.3-inch Spectra 6 | Default local profile; no Tailscale client is compiled or started |
| `ee04_epaper_tailscale` | Seeed XIAO ePaper Display Board EE04 + 7.3-inch Spectra 6 | Parents' profile; joins Tailscale through MicroLink and enables deep sleep |

The default PlatformIO environment is `prototype_oled`.

### OLED wiring

| OLED | ESP32 |
| --- | --- |
| GND | GND |
| VCC | 3V3 |
| SDA | GPIO21 |
| SCL/SCK | GPIO22 |

The OLED profile disables battery measurement and deep sleep so the result
remains visible while testing.

### Configure the server URL

For the OLED prototype, set the EPF server address in
[`Arduino/config.h`](Arduino/config.h):

```cpp
#define SERVER_BASE_URL "http://192.168.1.134:15001"
```

Use the LAN IP address of the Windows computer or NAS running EPF and the
published EPF port. Do not use `localhost`: from the ESP32, `localhost` means
the ESP32 itself.

The firmware also stores the server URL in ESP32 Preferences. A saved value
overrides the value in `config.h`; the serial monitor prints the actual value
used as `nas url: ...`. An empty saved value falls back to `config.h`.

### Choose Tailscale per frame

Tailscale is a compile-time option selected by the PlatformIO environment. This
keeps a local frame completely offline from Tailscale instead of merely having
it ignore a setting at runtime.

The XIAO local profile uses ordinary HTTP to the LAN EPF server. The parents'
profile also uses HTTP at the application layer because the Tailscale tunnel
provides the encryption; the OLED-only local profile retains its existing
optional HTTPS support.

For a local XIAO frame:

```powershell
pio run -e ee04_epaper -t upload
```

For the parents' XIAO frame, use `ee04_epaper_tailscale` and configure the auth key as
described below. The current 4 MB OLED board also has an experimental
Tailscale-enabled environment:

```powershell
pio run -e prototype_oled_tailscale -t upload
```

The normal `prototype_oled` environment remains the local, non-Tailscale test
path.

### Configure Tailscale for the parents' EE04

The EE04 profile uses the third-party [MicroLink ESP32 Tailscale client](https://github.com/CamM2325/microlink).
It registers as a real device in your tailnet and provides a WireGuard-backed
`100.x.x.x` address. Tailscale is not included in the normal OLED build;
`prototype_oled_tailscale` is a separate experimental test environment.

1. In the Tailscale admin console, create a reusable, pre-approved auth key.
   Prefer a tag restricted to this frame and disable machine-key expiry for the
   resulting device if you want unattended operation.
2. Copy [`Arduino/tailscale_secrets.h.example`](Arduino/tailscale_secrets.h.example)
   to `Arduino/tailscale_secrets.h` and put the auth key in that local file.
   The real file is ignored by Git.
3. Install Tailscale on the computer or NAS running EPF and find its tailnet
   IPv4 address with `tailscale ip -4`.
4. Set `SERVER_BASE_URL` to that tailnet address, for example:

   ```cpp
   #define SERVER_BASE_URL "http://100.90.80.70:15001"
   ```

   The Docker port must still be published as `15001:5000`, and the host
   firewall must allow TCP 15001 from the tailnet.
5. Build and upload the `ee04_epaper_tailscale` environment. After boot, the device
   appears in the Tailscale admin dashboard as `epf-frame` and logs its assigned
   `100.x.x.x` address.

The MicroLink component is included under [`components`](components) and is
used only for the EE04 ESP-IDF/Arduino build. The final encrypted path is:

```text
EE04 -> Wi-Fi -> Tailscale/WireGuard -> EPF host -> Immich
```

The frame does not need Immich credentials, and no public photo endpoint is
required.

### Build and upload

From the EPF repository root:

```powershell
pio run -e prototype_oled
pio run -e prototype_oled -t upload
```

Open the serial monitor at 115200 baud. A successful OLED test looks like:

```text
WiFi Connected. Downloading image
nas url: http://192.168.1.134:15001
Image data received
[status] Image received | photo-name.jpg
```

The firmware then requests `/sleep`. The OLED profile does not enter deep
sleep; the e-paper profile does.

For the physical target:

```powershell
pio run -e ee04_epaper_tailscale
pio run -e ee04_epaper_tailscale -t upload
```

The EE04 display wiring is defined by the board profile. Verify the 50-pin
jumper and FPC orientation before connecting the panel.

### Current 4 MB test board

The existing `prototype_oled` board definition is `esp32dev`, which PlatformIO
reports as a 4 MB ESP32. Its normal local firmware builds and runs without
Tailscale. The `prototype_oled_tailscale` environment uses a 4 MB OTA layout so
it can be tried on that board, but the XIAO ESP32-S3 Plus with PSRAM remains the
recommended Tailscale target. The S3 profile has substantially more headroom
for MicroLink, WireGuard, networking buffers, and the e-paper application.

## Troubleshooting

### The OLED says “No server URL”

The ESP32 has Wi-Fi but no usable saved server URL. Confirm
`SERVER_BASE_URL` in `Arduino/config.h`, then upload again. If the serial log
shows an old non-empty URL, the saved ESP32 Preference is overriding the
compile-time value; reconfigure it through the captive portal or erase the
ESP32 flash before uploading.

### The server is reachable from Windows but not from the ESP32

- Use the Windows/NAS LAN IP, not `localhost` or `127.0.0.1`.
- Confirm the ESP32 and EPF host are on the same reachable network.
- Allow inbound TCP port `15001` through the Windows firewall.
- Confirm Docker publishes `15001:5000`.

### The EE04 does not appear in Tailscale

- Confirm `Arduino/tailscale_secrets.h` exists and contains a valid `tskey-auth-...` key.
- Check the serial log for `Tailscale failed`; the most common causes are an
  expired/non-reusable key, missing internet access, or an unsupported board profile.
- Use the `ee04_epaper_tailscale` environment, not `ee04_epaper` or `prototype_oled`.
- The MicroLink client is a third-party Tailscale-compatible implementation;
  the XIAO ESP32-S3 with PSRAM is the intended hardware class.

### Immich is unreachable from the EPF web page

The Immich URL must include the scheme and port, for example:

```text
http://192.168.1.220:2283
```

Do not append `/api`; EPF adds the API paths itself. Check the container logs:

```powershell
docker compose logs --tail=100 epf
```

### The OLED shows “Endpoint OK” instead of a photo name

Rebuild and recreate the EPF Docker container so it includes the
`X-Photo-Name` response header:

```powershell
docker build -t epf:local .
docker compose up -d --force-recreate
```

## License

This project is licensed under the MIT License.
