# E-paper ESP32 Frame

- **This project is currently a Work in Progress (WIP)!**

This project leverages **Immich** as a service for organizing albums and photos. Photos intended for display are grouped into specific albums, and a FLASK server hosted on a NAS or cloud server handles image cropping and editing before sending them to the ESP32. Since the ESP32 remains in deep sleep most of the time, and all image processing is handled by the server, the EPD updates photos very quickly, typically within 15 seconds. This significantly reduces power consumption.

## Features

- **Captive portal**: By long-pressing setup button on the ESP32 when boot up, the device enters setup mode, allowing the Wi-Fi setup page to store up to five SSIDs. This enhances mobility and makes it easier to switch between networks.
Mostly modifieded from TRMNL WiFiCaptive[https://github.com/usetrmnl/firmware/tree/main/lib/wificaptive]
- **Fully Automated Photo Management**: Manage photos through Immich without additional manual processes; photos will automatically sync to the frame.
- **Ultra-low Power Consumption**: As all image processing and quantization are handled by the server, the device only consumes ~16µA during deep sleep, with photo updates completed within 30 seconds.
- **Customizable Display**: Configure photo orientation, basic color adjustments, album name, and more through the server webpage.
- **Multi-language settings page**: The settings page is available in English, Traditional Chinese, Simplified Chinese and Japanese, picked automatically from your browser's language and switchable from the header.
- **Cython impelementation**: Use Cython to significantly accelerate photo processing, achieving up to a 5x speed boost.
- **HTTPS supported**: ESP32 now can connect to secured server.
- **Sleep time impelementation**: ESP32 will enter deep sleep during the specified sleep period.
- **One button**: When the ESP32 is in deep sleep, a short press of the setting button will wake it up and restart the process, while a long press(~5s) of the setting button during boot will enter setting mode.

## Table of Contents

- [Components](#components)
- [Installation](#installation)
- [License](#license)

## Components

- ESP-WROOM-32 development board for the prototype
- Small 128x32 SSD1306 I2C OLED
- Seeed XIAO ePaper Display Board EE04 (ESP32-S3) for the final build
- Seeed 7.3-inch Spectra 6 e-paper display
- Picture frame: A standard picture frame that accommodates the e-paper frame.
- Li-Po battery with PH2.0 header
- EE04's built-in user buttons for wake and setup

## Installation

### Clone the Repository

```bash
$ git clone https://github.com/jwchen119/epf.git
```

### Manually Build Docker Image

```bash
$ git clone https://github.com/jwchen119/epf.git
$ docker build -t jwchen119/epf .
```

### Download Precompiled Docker Image

If you prefer not to build the image yourself, you can download the precompiled image from [DockerHub](https://hub.docker.com/r/jwchen119/epf):

```bash
docker pull jwchen119/epf
```

### Run with Docker Compose (recommended)

Create a `.env` file next to `docker-compose.yml` holding your Immich API key, then bring it up:

```bash
$ echo "IMMICH_API_KEY=<replace-your-immich-api-key>" > .env
$ docker compose up -d
```

`./config` and `./photos` are bind-mounted, so settings saved from the web page and the shown-photo history survive `docker compose pull` and a container rebuild. `TZ` is set too, because the sleep window and wake-up schedule are evaluated against local time and the image would otherwise run in UTC. Both `EPF_PORT` (default `15001`) and `TZ` (default `Asia/Taipei`) can be overridden in the same `.env` file.

### Run the Container Manually

If you would rather not use compose, mount both directories yourself. Anything written outside them lives only in the container and is lost as soon as the container is recreated - which is what updating to a new image does:

```bash
$ docker run --name epf \
    -e IMMICH_API_KEY='<replace-your-immich-api-key>' \
    -e TZ=Asia/Taipei \
    -v "$(pwd)/config:/config" \
    -v "$(pwd)/photos:/photos" \
    -d -p <replace-port>:5000 jwchen119/epf
```

### Configure `config.yaml` (no longer needed, configure the settings directly from webpage)
<details>
Below is an example of a configured `config.yaml` file:

```yaml
immich:
  # Album name, must match the album name created in Immich
  album: testAlbme
  # Photo rotation angle, accepts only (0, 90, 180, 270)
  rotation: 270
  # Immich server URL
  url: http://192.168.100.36:2283
  # Color(Saturation) enhancement level using PIL's ImageEnhance.Color (1.0 = original level)
  enhanced: 1.5
  # Contrast level using PIL's ImageEnhance.Contrast (1.0 = original level)
  contrast: 1.2
```
</details>

### Hardware profiles

The Arduino firmware now uses a compile-time hardware profile so the same EPF
logic can be exercised on the temporary OLED hardware and later moved to the
EE04 e-paper board.

| Profile | Board/display | Selection |
| --- | --- | --- |
| `prototype_oled` | ESP-WROOM-32 + 128x32 SSD1306 OLED | default PlatformIO environment |
| `ee04_epaper` | Seeed XIAO ePaper Display Board EE04 + 7.3-inch Spectra 6 | future target |

For the OLED prototype, connect GND to GND, VCC to 3V3, SDA to GPIO21 and
SCL/SCK to GPIO22. The OLED shows boot, Wi-Fi, fetch and completion status;
it deliberately does not decode or display images. Deep sleep and battery
measurement are disabled in this profile so the result remains visible while
testing.

The EE04 profile uses the board's routed display signals: BUSY GPIO4, DC
GPIO10, RESET GPIO38, CS GPIO44, SCK GPIO7, MOSI GPIO9 and panel power enable
GPIO43. Set the EE04 jumper to 50-pin before connecting the Spectra 6 panel,
and verify the FPC orientation before powering it.

### PlatformIO builds

From the EPF repository:

```text
pio run -e prototype_oled
pio run -e ee04_epaper
```

The Arduino IDE path remains available and uses the EE04 profile by default.
When using the OLED prototype in Arduino IDE, add
`-DEPF_HARDWARE_PROFILE=EPF_PROFILE_PROTOTYPE_OLED` to the board build flags.

## License

This project is licensed under the MIT License.

