# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An Immich-backed photo frame split across a Docker server and ESP32 firmware
that talk over HTTP:

- **Server** (`app.py`, `epf/`, `cpy.pyx`, `templates/`, `static/`) — a Flask app, normally run in Docker on a NAS. Pulls photos from an [Immich](https://immich.app) album, crops/enhances/dithers them to the panel's 6-color palette, and serves the result already packed for the display.
- **Firmware** (`Arduino/`) — one sketch with two compile-time hardware profiles. `prototype_oled` runs on an ESP-WROOM-32 with a 128x32 SSD1306 OLED and exercises the real server exchange while emulating the final display step; `ee04_epaper` targets the Seeed XIAO ESP32-S3 EE04 and the 7.3" Spectra 6 panel.
- **CAD** (`CAD/*.STEP`) — enclosure parts, not built by any toolchain here.

PlatformIO is configured in `platformio.ini` with `Arduino/` as `src_dir`; its
default environment is `prototype_oled`. There are no automated tests or
Python linter configuration.

## Server: build and run

`docker compose up -d`, with a `.env` alongside holding `IMMICH_API_KEY`. The
published image maps host port `15001` to Flask port `5000`; a local image can
be built with `docker build -t epf:local .` and selected in Compose. Directly,
`python app.py` serves on `0.0.0.0:5000`, but Windows cannot import the
committed Linux `cpy.so` outside a Linux environment.

`docker-compose.yml` exists because two things are easy to get wrong and both fail silently:

- Two paths are **hardcoded**, not configurable: `/config/config.yaml` (written by the settings page, watched by `watchdog` for external edits, created from `DEFAULT_CONFIG` if missing) and `/photos` (override with `IMMICH_PHOTO_DEST`; holds only `tracking.txt` — no photos are ever written to disk). A plain `docker restart` keeps them, but recreating the container — which is what updating the image requires — discards anything not bind-mounted, so settings revert to `DEFAULT_CONFIG` without any error.
- **`TZ` must be set.** The base image has no timezone, so `datetime.now()` returns UTC. `/sleep` derives both the sleep window and the wake-up schedule from local time, so an unset `TZ` shifts the frame's quiet hours by the whole UTC offset. Zone data is already in the image, so the env var alone is enough — no `tzdata` install and no `/etc/localtime` mount.

`IMMICH_API_KEY` is read once at import into the module-level `headers` dict. Note the README's `docker run` example writes `IMMICH-API-KEY` with hyphens, which the app does not read.

## Server: how the code is laid out

`app.py` holds the Flask app and every route; everything else is in `epf/`:

| module | holds |
| --- | --- |
| `config.py` | `DEFAULT_CONFIG`, the live settings, `config.yaml` read/write, the watchdog observer |
| `state.py` | in-memory runtime state: battery reading, current photo, pre-chosen next photo |
| `eventlog.py` | the JSONL event log and `client_ip()` |
| `tracking.py` | `tracking.txt` |
| `immich.py` | album/asset queries, `select_asset()`, health check, thumbnail fetch |
| `imaging.py` | the pipeline; takes its settings as arguments and reads no globals |
| `battery.py` | voltage → percentage |
| `notify.py` | low-battery push over Telegram or LINE |
| `credentials.py` | the notification tokens, in `/config/credentials.json` |

**The live settings are one dict that is only ever updated in place** (`config.apply()` calls `.update()`; it never rebinds `config.current`). Modules call `config.immich()` at the point of use. Copying a value out at import time — `from epf.config import current; url = current['immich']['url']` — would freeze it at start-up, which is the one way to break this layout. `config.current` is a `deepcopy` of `DEFAULT_CONFIG` for the same reason a shallow copy was wrong: the two would share the inner dict, and saving settings would rewrite the defaults the reset button restores.

The front end is split the same way: `templates/settings.html` is markup only, with `static/css/settings.css`, `static/js/i18n.js` (the translation dictionary) and `static/js/settings.js` (behaviour). Two things stay inline in the template and must: the theme bootstrap has to run before the stylesheet or the wrong theme flashes, and `window.EPF_DEFAULTS` is how the server's defaults reach the static JS. Assets are linked through `static_url()`, which appends the file's mtime so a browser cannot serve a stale one after an update.

## Server: HTTP contract with the firmware

This contract is the thing to be careful about — both sides must change together.

- `GET /download` — the device sends its battery voltage in a `batteryCap` **request header** (millivolts; the OLED profile sends `0`). Response is `text/plain`: ASCII hex bytes as `"XX,XX,..."` terminated by `};`, i.e. C-array source text, not binary. `X-Photo-Url` carries the Immich web URL of the chosen photo (intended for writing an NFC tag; the firmware does not read it yet), and `X-Photo-Name` carries the selected asset's original filename for the OLED emulator.
- `GET /sleep` — returns `{current_time, next_wakeup, sleep_duration}` where `sleep_duration` is **milliseconds**. The firmware divides by 1000 and passes it to `esp_deep_sleep`. Falls back to 24h if absent. `/download` and `/sleep` are two separate requests per wake cycle.
- `GET /setting` (GET renders, POST saves) — the config UI; `/` redirects here. Battery percentage shown here comes from the last `/download` request's header, cached in module globals for one hour, so it reads 0% until the device has checked in.
- `GET /log?limit=N` — the system-log card, newest first (limit clamped to 500). Events are appended as JSONL to `events.jsonl` beside `tracking.txt`, so the mount that keeps the settings keeps the history; `log_event()` never raises and is guarded by a lock, because the threaded dev server means the device and a browser can write at once. The file is trimmed to `LOG_MAX_ENTRIES` once it passes `LOG_TRIM_BYTES`. Events: `checkin` (ip, battery, asset, album, plus `mac`/`rssi` **only if the firmware sends `X-Device-Mac`/`X-Device-Rssi`** — HTTP carries no MAC and the container cannot read the LAN's ARP table), `settings_saved` (with a before/after diff), `config_reloaded`, `tracking_reset`, `notified`, `notify_bound`, `notify_unbound`, `log_cleared`, `error`, `startup`. `/sleep` deliberately writes nothing: it fires every wake-up and its answer is implied by the check-in. `POST /log/clear` empties the file.
- `POST /notify/bind` / `POST /notify/unbind` / `GET /notify/channels` / `POST /notify/test` — linking a notification service. **Nothing is stored until a test message is actually delivered**, so "linked" always means "known to work". Credentials live in `/config/credentials.json` (`credentials.py`), not the environment, so a token can be changed without recreating the container, and **`/notify/channels` reports only whether each channel is linked — never the values**, since the settings page has no authentication. The low-battery warning is triggered from `/download`, the only place a reading arrives, and sent **on a thread** because the frame gives up after 50s. It goes to every channel that is both linked and ticked on the settings page (`use_telegram`/`use_line`, on by default), so linking two services does not force both to be messaged; rate limiting lives in `state.notify['last_sent']`, in memory, so a restart allows one extra message. LINE needs the Messaging API — LINE Notify was discontinued in 2025.
- `GET /status` — fills the header indicators, fetched by the page after load rather than blocking the render. Reports Immich health by calling `GET /api/albums` (one request covers reachability, whether the key is accepted, and whether the configured album still exists), plus how long ago the frame checked in, derived from `last_battery_update`/`last_photo['shown_at']` and judged stale past two `wakeup_interval`s. Returns codes such as `unauthorized` or `album_missing` rather than sentences, so the page renders them in the active language.
- `GET /preview/original` and `GET /preview/next` — the two images in the "photos" card: what the frame is showing, and what it will be given next. Both proxy Immich's `/api/assets/{id}/thumbnail?size=preview` through `proxy_thumbnail`, because the API key never reaches the browser and originals may be HEIC or RAW that browsers cannot display. `/preview/original` 404s until the device has fetched an image at least once; `/preview/next` 404s until something has been chosen.
- `GET /next` (choose only if nothing is remembered) and `POST /next` (always choose again, which is the swap button) — the frame is handed exactly the asset `/next` reported, so the page and the device cannot disagree. Under `random` ordering there was previously no such thing as "next": it did not exist until the device asked.

## Server: image pipeline

`/download` → pick asset → `scale_img_in_memory()` → `convert_to_c_code_in_memory()`. Everything is in-memory `BytesIO`.

1. **Asset selection.** Album assets are fetched via paginated `POST /api/search/metadata` filtered by `albumIds` — *not* `GET /api/albums/{id}`, which stopped returning `assets` in Immich v3. `tracking.txt` records which assets have been shown: line 1 is the album name (changing albums resets the file), remaining lines are asset IDs. `image_order` is `random` (reset when exhausted) or `newest` (reset when a newer photo appears).
2. **Scale + enhance.** `cpy.load_scaled()` rotates and either letterboxes (`fit`) or center-crops (`fill`) to 800x480, then PIL `ImageEnhance` applies `enhanced` (saturation) and `contrast`.
3. **Quantize.** `cpy.convert_image()` does Floyd-Steinberg dithering to six pure-RGB colors, with `strength` scaling the error diffusion. The commented-out PIL `.quantize()` block in `scale_img_in_memory` is the superseded version.
4. **Pack.** `depalette_image()` nearest-matches each pixel against the module-level `palette` (the *measured* panel colors, e.g. yellow is `(255,243,56)`) and applies `indices[indices > 3] += 1` to line up with the panel's color codes in `Arduino/epd7in3e.h`. Then two 4-bit indices are packed per byte.

Three palettes must stay consistent: the pure-RGB one inside `cpy.pyx:convert_image`, the measured one at the top of `app.py`, and the `EPD_7IN3E_*` codes in the firmware header.

## cpy: the Cython module

**`cpy.so` is a prebuilt Linux x86-64 binary committed to the repo, and there is no `setup.py` or build step anywhere — not in the Dockerfile, not in `requirements.txt`.** Editing `cpy.pyx` therefore has no effect until you compile it yourself and replace `cpy.so`; on Windows you cannot load the committed `.so` at all, so `import app` fails locally. Assume any `.pyx` change needs a Linux build (`cython` + `numpy` headers) plus a note to the user that the binary must be regenerated.

`EPD_W`/`EPD_H` are duplicated as module constants in `cpy.pyx`; the target size is not passed in. `scale_img_in_memory`'s `target_width`/`target_height` arguments only affect the (currently disabled) date-overlay positioning.

## Firmware: build and flow

PlatformIO uses `Arduino/` as the source directory. The environments are:

- `prototype_oled` — ESP-WROOM-32, 128x32 SSD1306 OLED; default environment
- `ee04_epaper` — Seeed XIAO ESP32-S3 EE04 and Spectra 6 e-paper target

Build with `pio run -e prototype_oled` or `pio run -e ee04_epaper`; upload with
the same environment plus `-t upload`. Arduino IDE can still compile the
sketch, but must select the matching `EPF_HARDWARE_PROFILE` build flag.

`setup()` runs once per wake and never returns to `loop()`:

1. The e-paper profile checks battery voltage; the OLED profile skips battery
   measurement.
2. Initialize the selected display backend. The OLED profile must initialize
   successfully before networking starts.
3. Connect through `WifiCaptivePortal` using up to five saved SSIDs.
4. `downloadImage()` reads `SERVER_BASE_URL`, performs `GET /download`, and
   streams/parses the ASCII hex panel payload. The request includes a
   `batteryCap` header; OLED sends `0`.
5. With `EPF_USE_EPAPER`, parsed bytes go to `epd.SendData()` after
   `SendCommand(0x10)`, then `TurnOnDisplay()` and `epd.Sleep()` run. With
   `EPF_USE_OLED`, those e-paper calls compile out; the complete payload is
   still consumed and the `X-Photo-Name` response header is shown on the OLED.
6. After a successful download, the firmware requests `/sleep`. The e-paper
   profile uses the returned duration for deep sleep; the OLED profile leaves
   the device awake so the result remains visible.

Server URL state lives in the `data` Preferences namespace. The captive portal
writes `SERVER_BASE_URL` there, and a saved value overrides the
`SERVER_BASE_URL` fallback in `Arduino/config.h`. An empty saved value falls
back to the compile-time value. `downloadImage()` prints the actual URL as
`nas url: ...`, which is the first networking diagnostic to check.

The OLED prototype has no physical configuration button
(`EPF_HAS_CONFIG_BUTTON=0`); the EE04 profile uses GPIO 2 for the setup/wake
button. The OLED profile also disables deep sleep so the status remains on the
screen. The e-paper profile wakes on the timer or GPIO 2 (`ext1`), and
`epd.Sleep()` before hibernation is required for the low-power target.

`WifiCaptive*` files are adapted from [TRMNL firmware](https://github.com/usetrmnl/firmware/tree/main/lib/wificaptive); `epd7in3e.*` and `epdif.*` are Waveshare vendor drivers. Keep profile-specific behavior behind the existing `EPF_USE_OLED`/`EPF_USE_EPAPER` compile-time guards.

## Conventions

Everything committed to this repo is written in **English** — code, comments, identifiers, commit messages, docs — because changes may be submitted upstream as merge requests. Pre-existing Traditional Chinese comments in `cpy.pyx` and `Arduino/button.h` are the original author's; leave them alone, but write new comments in English.
