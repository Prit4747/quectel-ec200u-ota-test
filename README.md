# Quectel EC200U-CN OTA Test — ESP32-S3 Cellular Hub (UART PPPoS)

ESP32-S3 + Quectel EC200U-CN (Vanix TracX-1b carrier) modem hub — full/delta
OTA over cellular PPP, migrated from an earlier SIMCom A7672S USB-Host
design to a UART PPPoS design using Espressif's `esp_modem` component.

## Stack

| Layer | Component |
|---|---|
| Data link | UART PPPoS via `espressif/esp_modem` (native C++ API) |
| OTA | `esp_https_ota` (full `.bin`) + `espressif/esp_delta_ota` (delta `.patch`) |
| Local control | Wi-Fi AP (`IOT-HUB-AP`) + HTTP server + SSE event stream |

## Wiring summary

| Quectel module | ESP32-S3 |
|---|---|
| Tx | RX GPIO (default GPIO18) |
| Rx | TX GPIO (default GPIO17) |
| GND | GND |

Power comes from the module's own **+V_BAT/-V_BAT** input (dedicated
≥2A, 3.7–4.0V supply or LiPo battery, jumper on **BAT**) — independent of
the ESP32-S3 entirely. See the parent project's `docs/WIRING.md` for the
full walkthrough (jumper location, PWRKEY/RST, LED diagnostics, SIM,
antennas).

## Build

```sh
idf.py set-target esp32s3
idf.py menuconfig   # Modem / Quectel EC200U-CN UART Configuration ->
                     #   UART pins/baud, flow control, SIM APN
idf.py build
idf.py -p <PORT> flash monitor
```

## Web UI

1. Join `IOT-HUB-AP` (open, no password).
2. Open `http://192.168.4.1`.
3. Once `4G` shows `online` with an IP, paste an OTA `.bin`/`.patch` URL
   (see Releases on this repo for an example) into the OTA field and
   click **Download OTA**.

## Delta OTA patches

Use `tools/make_patch.py` to build a `.patch` from two `.bin`s (same
tool/format as the SIMCom project this was migrated from).
