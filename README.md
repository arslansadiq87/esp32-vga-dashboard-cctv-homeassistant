# VGA Dashboard

A retro-style, 640 × 480 Home Assistant dashboard for an ESP32-S3 board with 16 MB flash and 8 MB PSRAM. It renders an LVGL interface to a 16-bit RGB565 VGA output, polls Home Assistant over Wi-Fi, and displays a JPEG snapshot from a Dahua-compatible DVR/camera.

> **Camera behavior:** this firmware requests a JPEG snapshot over HTTP. It does not open or decode an RTSP video stream. The RTSP port and substream settings some DVRs expose are not used by this project.

## What it does

- Drives a 640 × 480 RGB565 display with ESP-IDF's RGB LCD peripheral, two PSRAM framebuffers, and LVGL 8.
- Displays 16 configurable Home Assistant entity cards, room temperature, local time/date, sunrise/sunset, connection state, and camera panel.
- Polls Home Assistant's REST API using a long-lived access token.
- Gets a JPEG from `/cgi-bin/snapshot.cgi?channel=<channel>` over HTTP, handles the camera's HTTP Digest challenge, decodes/scales the image, and refreshes it periodically.
- Keeps the display running if network configuration is missing or a network service is unavailable.

## Hardware

### Required

- ESP32-S3 development board with 16 MB flash and 8 MB octal PSRAM, compatible with the `esp32-s3-devkitc1-n16r8` PlatformIO board definition.
- VGA monitor and an appropriate 16-bit RGB-to-VGA interface/DAC wired to the GPIOs below. The ESP32 GPIO signals are digital RGB data; connect them through the intended interface for your hardware.
- Wi-Fi network reachable by both the ESP32 and Home Assistant, plus a Dahua-compatible DVR/camera if you want the camera panel to show an image.

### GPIO assignment

The code maps the RGB565 data bits to these GPIOs. The bit order is the RGB565 lane order used by the ESP-IDF driver:

| Signal | GPIO |
| --- | --- |
| Blue B0–B4 | 15, 16, 17, 18, 21 |
| Green G0–G5 | 9, 10, 11, 12, 13, 14 |
| Red R0–R4 | 4, 5, 6, 7, 8 |
| HSYNC | 1 |
| VSYNC | 2 |

Confirm the pinout against your specific board and VGA interface before wiring. The project uses 640 × 480 pixels, negative HSYNC/VSYNC, 24 MHz pixel clock, and 57.143 Hz vertical refresh (800 × 525 total timing). These timings and pins are defined in `src/main.cpp`.

### PCB fabrication and assembly files

The `schematics/` folder contains the board fabrication and assembly files:

- [Gerber package](schematics/Gerber_Esp32s3_Vga.zip) - zipped PCB manufacturing layers, board outline, and plated/non-plated drill files. Upload this ZIP to your PCB manufacturer's Gerber review/order tool.
- [Bill of Materials (BOM)](schematics/BOM_Board1_PCB1_2026-10-22.xlsx) - component list for the board.
- [Pick-and-place file](schematics/PickAndPlace_PCB1_2026_10_22.xlsx) - component placement data for assembly.

Check the Gerbers and BOM against the board revision and your manufacturer's requirements before ordering or assembly.

## Repository layout

```text
VGA_Dashboard/
├── include/
│   ├── dashboard_config.h             # Configuration structures and local config include
│   ├── dashboard_local.example.h      # Safe template for Wi-Fi and Home Assistant settings
│   ├── camera_local.example.h         # Safe template for DVR settings
│   ├── dashboard_data.h               # Dashboard data types and polling API
│   ├── dashboard_ui.h                 # LVGL screen API
│   ├── camera_view.h                  # Camera panel API
│   └── lv_conf.h                      # LVGL color, memory, and font options
├── src/
│   ├── main.cpp                       # VGA driver, LVGL setup, main event loop
│   ├── dashboard_data.cpp              # Wi-Fi, time, and Home Assistant REST polling
│   ├── dashboard_ui.cpp                # Layout, colors, labels, and image widget
│   └── camera_view.cpp                 # HTTP snapshot, Digest auth, JPEG decode/scaling
├── scripts/embed_managed_certs.py      # Embeds upstream public CA certificates at build time
├── schematics/
│   ├── Gerber_Esp32s3_Vga.zip             # PCB manufacturing layers and drill data
│   ├── BOM_Board1_PCB1_2026-10-22.xlsx    # Bill of materials
│   └── PickAndPlace_PCB1_2026_10_22.xlsx  # Assembly placement data
├── certs/                              # Public upstream CA certificates; not private keys
├── default_16MB.csv                    # Flash partition layout
├── platformio.ini                      # PlatformIO board, framework, library, and build settings
└── sdkconfig.defaults                  # ESP-IDF defaults needed for this board/build
```

PlatformIO creates `.pio/` build output and `managed_components/` dependency sources locally. They are regenerated as needed and excluded from version control.

## Configure credentials and entities

Do this before building a device with live integrations. The `*_local.h` files are private local configuration; the `*.example.h` files are safe templates.

### 1. Wi-Fi and Home Assistant

Copy the example header in the project directory:

**PowerShell**

```powershell
Copy-Item include/dashboard_local.example.h include/dashboard_local.h
```

**macOS/Linux**

```sh
cp include/dashboard_local.example.h include/dashboard_local.h
```

Edit `include/dashboard_local.h`. Its `DashboardConfig` fields are positional and must stay in this order (the field definition is in `include/dashboard_config.h`):

1. `wifiSsid` — Wi-Fi network name.
2. `wifiPassword` — Wi-Fi password.
3. `haUrl` — Home Assistant base URL, for example `http://homeassistant.local:8123` or `http://192.168.1.20:8123`. Do not append `/api` or a trailing API endpoint.
4. `haToken` — Home Assistant long-lived access token. In Home Assistant, create one from your user profile's security section. Treat it like a password.
5. `haCaCert` — PEM certificate authority text when using HTTPS. The example uses `nullptr` for local HTTP. HTTPS requests require a CA certificate; do not disable certificate verification.
6. `timeZone` — POSIX TZ string used by ESP-IDF's `configTzTime()`, for example `UTC0`. It is not an IANA name such as `Europe/London`.
7. `sunEntity` — entity ID whose attributes contain `next_rising` and `next_setting`; Home Assistant's default `sun.sun` is typical.
8. `cards` — exactly 16 entries, each `{ "DISPLAY LABEL", "domain.entity_id", "UNIT" }`.
9. `roomTemperature` — entity ID for the room temperature footer. Use an empty string if you do not want to configure it.
10. `pollSeconds` — requested interval in seconds. The firmware clamps this to 5–300 seconds; each entity is fetched separately, so a full dashboard refresh takes longer than one HTTP request.

The example entries (`YOUR_*`) are placeholders. Replace every placeholder you use. The dashboard can still render with no HA configuration, but it will show unavailable values and will not start the polling task.

### 2. Home Assistant entity IDs and units

Use entity IDs exactly as shown under **Developer Tools → States** in Home Assistant, such as `sensor.living_room_temperature`. An entity ID must contain a dot and may contain letters, numbers, underscores, and dots. The 16 cards can show numeric states or text states. Labels are short titles shown on the cards.

The card unit is a display/unit hint. Numeric handling in `src/dashboard_data.cpp` includes:

- `W`: accepts `W` and converts `kW` to watts.
- `%`: accepts percentages in the 0–100 range.
- `ppm`: accepts non-negative parts-per-million values.
- `C` or `°C`: accepts Celsius; an entity reported in Fahrenheit is converted to Celsius for this type.
- Other units (such as `V`) display the numeric state with the configured suffix, without an additional physical conversion.
- An empty unit displays a plain number or text state.

For the separate `roomTemperature` field, the code formats the value as Celsius and converts Fahrenheit values reported by Home Assistant. Unknown, unavailable, non-numeric, and implausible values are shown as unavailable. Check the entity's reported `unit_of_measurement` if a card remains blank.

Home Assistant endpoints used by this firmware are `GET /api/states/<entity_id>` with `Authorization: Bearer <token>`. The client needs access to each configured entity. The sun card reads `attributes.next_rising` and `attributes.next_setting` from the configured sun entity.

### 3. DVR/camera details

Copy the camera template:

**PowerShell**

```powershell
Copy-Item include/camera_local.example.h include/camera_local.h
```

**macOS/Linux**

```sh
cp include/camera_local.example.h include/camera_local.h
```

Set these values in `include/camera_local.h`:

- `DVR_HOST`: DVR/camera hostname or IP address only (no `http://`, path, or port).
- `DVR_HTTP_PORT`: HTTP port serving the snapshot endpoint (template default `80`).
- `DVR_USERNAME`, `DVR_PASSWORD`: a camera account allowed to view snapshots. Use a dedicated least-privilege account if supported.
- `DVR_CHANNEL`: camera channel number passed to the snapshot endpoint.
- `DVR_SNAPSHOT_INTERVAL_MS`: successful refresh interval in milliseconds (template default `1200`). On failure, the firmware retries after 3 seconds.

The request URL is `http://<DVR_HOST>:<DVR_HTTP_PORT>/cgi-bin/snapshot.cgi?channel=<DVR_CHANNEL>`. The client first requests without credentials, then responds to a `401` Digest challenge using MD5 Digest authentication. JPEG files above 512 KiB are rejected. The image is scaled into a 200 × 136 pixel panel. The device and DVR must be reachable on the same routed network; the camera connection reuses the dashboard Wi-Fi connection.

This is a periodic still-image snapshot, not continuous video. `DVR_RTSP_PORT` and `DVR_SUBSTREAM` are not needed by this firmware; RTSP URLs and RTSP-only camera services are not supported by the current `camera_view.cpp`. The snapshot endpoint and Digest authentication behavior are DVR/firmware dependent. If your camera has a different snapshot URL or authentication scheme, update `fetchSnapshot()` / `request()` in `src/camera_view.cpp`.

### Credential safety

- Never commit `include/dashboard_local.h` or `include/camera_local.h`. `.gitignore` excludes them by filename.
- The example headers contain placeholders only. Make private copies locally and edit those.
- Do not put real credentials, private keys, personal camera URLs, or tokens in source, screenshots, issue reports, or commits. A token in source is compiled into firmware, so anyone with the firmware image may be able to recover it.
- If a credential was ever committed or published, deleting the file in a later commit does not remove it from Git history. Revoke/rotate the affected token/password and clean the repository history before making it public.
- Only the contents of this `VGA_Dashboard` project folder are intended for the repository. Build commands below honor `.gitignore`; a manual website file upload may not. Check the upload contents before publishing.

## Build and upload with VS Code + PlatformIO

1. Install [Visual Studio Code](https://code.visualstudio.com/).
2. In VS Code's Extensions view, install **PlatformIO IDE**.
3. Open the `VGA_Dashboard` folder itself (the folder containing `platformio.ini`), not its parent workspace.
4. Create and edit `include/dashboard_local.h` and `include/camera_local.h` from the examples above. These headers are ignored by Git.
5. Connect the ESP32-S3 over USB and select the correct serial port if PlatformIO does not detect it.
6. Use the PlatformIO toolbar or the PlatformIO project tasks to **Build** the `esp32-s3-n16r8` environment.
7. Use **Upload** to flash it. Use **Monitor** to view serial output at 115200 baud.

First build downloads the configured Espressif platform and libraries, so it needs internet access and can take a while. The platform release is pinned in `platformio.ini`; dependencies are declared there. Use a USB cable that supports data. If upload cannot connect, check the selected port, close other serial monitors, and try the board's bootloader procedure for your board.

### PlatformIO CLI alternative

Install PlatformIO Core (or use the PlatformIO terminal bundled with the VS Code extension), open a terminal in this project folder, and run:

```sh
pio run
pio device list
pio run --target upload --upload-port COM5
pio device monitor --baud 115200 --port COM5
```

Replace `COM5` with the actual port (for example `/dev/ttyUSB0` on Linux or `/dev/cu.usbmodem...` on macOS). You can omit explicit ports if PlatformIO detects the right device. To build only, run `pio run`. To clear generated build files, run `pio run --target clean`.

The environment is named `esp32-s3-n16r8` in `platformio.ini`. The target board needs 16 MB flash, octal PSRAM, and the matching RGB pin wiring. A different ESP32-S3 board may need changes to board settings, PSRAM settings, partition layout, and GPIO assignments.

## GitHub preparation

This folder is not initialized as a Git repository by the project files. From a terminal opened in `VGA_Dashboard`, initialize and inspect it before the first push:

```sh
git init
git add .
git status --short
```

Review the staged file list and confirm that neither local configuration header appears. Then commit and connect your GitHub remote using the repository URL from GitHub:

```sh
git commit -m "Prepare VGA Dashboard for GitHub"
git branch -M main
git remote add origin https://github.com/<your-account>/<your-repository>.git
git push -u origin main
```

The public CA certificates under `certs/` are trust certificates used by build dependencies; they are not your Home Assistant/DVR credentials or private keys. Keep those files unless you also change the certificate-embedding build script.

## Troubleshooting

| Symptom | Checks |
| --- | --- |
| No image / monitor reports unsupported timing | Confirm the VGA interface, sync wiring and polarity, board pin mapping, 640 × 480 support, and monitor compatibility with the configured 57.143 Hz timing. |
| Serial says PSRAM is missing or VGA init fails | Confirm the board variant has octal PSRAM and the selected PlatformIO environment matches it. |
| `HA configuration missing` | Confirm `include/dashboard_local.h` exists and replace `YOUR_*` placeholders. Check the URL, SSID, and token are non-empty. |
| Wi-Fi remains pending | Check SSID/password, 2.4 GHz availability on the board, signal strength, and whether the network permits the ESP32 to reach Home Assistant. |
| Home Assistant reports HTTP errors / cards unavailable | Check the base URL and port, token validity/permissions, entity IDs, entity state, and units. For HTTPS, set a valid PEM CA certificate in `haCaCert`. |
| Sunrise/sunset unavailable | Confirm `sunEntity` exists, exposes `next_rising`/`next_setting`, and the POSIX timezone is valid. |
| Camera says `DVR NO RESPONSE` | Check DVR host, HTTP port, snapshot path support, network reachability, and that HTTP snapshots are enabled. |
| Camera says `DVR AUTH ERROR` or `HTTP ERROR` | Check username/password, channel, and that the DVR supports the implemented HTTP Digest challenge and snapshot endpoint. |
| `JPEG TOO LARGE`, `BAD JPEG FRAME`, or decode error | Check that the endpoint returns a complete JPEG under 512 KiB; use a lower-resolution snapshot if the DVR supports it. |
| Build cannot find a local header | Copy both example headers to their matching `_local.h` names, even if you only want to test the display without live services. |
| Build fails after dependency changes | Confirm internet access, retry `pio run`, and inspect the first compiler error. `.pio/` and `managed_components/` are generated dependency/build directories and can be removed to force regeneration. |

## Main code locations

- VGA timing, GPIOs, framebuffer allocation, LVGL driver, and loop: `src/main.cpp`.
- Home Assistant configuration structure: `include/dashboard_config.h`.
- Home Assistant API requests, conversions, timezone and polling interval: `src/dashboard_data.cpp`.
- Cards, layout, colors, status text, and image widget: `src/dashboard_ui.cpp`.
- Snapshot URL, authentication, timeout, JPEG size cap, scaling, and retry behavior: `src/camera_view.cpp`.
- LVGL memory and fonts: `include/lv_conf.h`.
- Flash partition layout: `default_16MB.csv`.
- Board, framework, library versions, compiler flags, and build script: `platformio.ini`.

## License

No top-level license file is currently provided. Add a `LICENSE` file before publishing if you want others to have explicit permission to use, modify, or redistribute this project. The licenses of bundled/generated third-party components remain applicable to those components.
