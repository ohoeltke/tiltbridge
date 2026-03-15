# CLAUDE.md — TiltBridge Project Guide

## Project Overview

TiltBridge is an ESP32-based bridge that receives Bluetooth Low Energy (BLE)
broadcasts from Tilt Hydrometer devices and forwards the data (temperature,
gravity) to various logging services via WiFi. It supports multiple ESP32
board types with different displays.

Repository: https://github.com/thorrak/tiltbridge (upstream)
Fork: https://github.com/ohoeltke/tiltbridge

## Build System

- **PlatformIO** with ESP-IDF framework (NOT Arduino)
- **Platform**: `espressif32@6.13.0` (ESP-IDF 5.5.3)
- **PIO binary**: `~/.platformio/penv/bin/pio`
- Build a target: `pio run -e <environment>`
- Flash: use `esptool.py` directly via PIO's Python env for reliability:
  ```
  ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy@1.40501.0/esptool.py \
    --port /dev/cu.usbserial-XX --baud 115200 --chip esp32 write_flash \
    0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 ota_data_initial.bin \
    0x10000 firmware.bin 0x330000 littlefs.bin
  ```
- Don't forget to flash the LittleFS partition (0x330000) — it contains the web UI files
- Build filesystem: `pio run -e <env> -t buildfs`

## Key Build Environments

| Environment | Board | Display | Notes |
|---|---|---|---|
| `esp32_universal` | esp32dev | Auto-detect all | **Universal binary** — recommended |
| `lcd_ssd1306` | heltec_wifi_kit_32 | SSD1306 OLED | Per-board (legacy) |
| `lcd_tft` | lolin_d32_pro | ILI9341 TFT | Per-board (legacy) |
| `tft_espi` | esp32dev | ST7789 TTGO | Per-board (legacy) |
| `m5stickc_plus` | m5stick-c | ST7789 M5 | Per-board (legacy) |
| `s3_tdisplay` | lilygo-t-display-s3 | ST7735 | ESP32-S3, separate chip |

## Project Structure

```
src/
  bridge_lcd.h          — Display types, enums, class declaration
  bridge_lcd.cpp        — Non-display-specific LCD methods (in bridge_lcd_impl.cpp too)
  bridge_lcd_impl.cpp   — Display init, detection, rendering (main LCD logic)
  lovyan_config.h       — LovyanGFX display class definitions (LGFX_*)
  main.cpp              — App entry, main loop
  tiltBridge.h          — Global config, version
  tiltScanner.*         — BLE Tilt scanning
  http_server.*         — Web UI HTTP server
  wifi_mgr.*            — WiFi management (ESP-IDF native)
  axp192.*              — AXP192 PMIC driver (M5StickC Plus)
  img/
    tft_logo.h          — 288x240 RGB565 logo for TFT displays
    tft_logo_swapped.h  — Byte-swapped version for universal build
    oled_logo.h         — XBitmap logo for OLED/small displays
docs/                   — Development documentation (english)
```

## Display Auto-Detection (Universal Build)

The `esp32_universal` environment auto-detects displays at boot via a 6-step
probe sequence. See `docs/AUTODETECTION_DEEP_DIVE.md` for full details.

Probe order: AXP192 (I2C) → CYD (bare SPI) → D32 Pro (full LGFX init) →
TTGO (full init) → M5StickC Plus2 (full init) → SSD1306 (I2C) → Headless

Key types in `bridge_lcd.h`:
- `DisplayType` enum — identifies the specific board/display
- `DisplayCategory` enum — CAT_NONE, CAT_SMALL, CAT_LARGE (determines UI layout)
- `DisplayInfo` struct — runtime display properties

## Important Gotchas

1. **spi_3wire must be false for SPI ID reading** — With `spi_3wire=true`, MISO
   is not used and all reads return 0x000000.

2. **pushImage + setSwapBytes(true) crashes on large images** in universal build.
   Use pre-swapped image data with `setSwapBytes(false)` instead.

3. **D32 Pro needs full LGFX init() for probe** — bare SPI probe returns 0xFFFFFFFF
   because ILI9341 needs hardware reset (RST pin toggle) before responding to RDDID.

4. **D32 Pro backlight is external** — GPIO 32, not managed by LovyanGFX. Must be
   set manually: `pinMode(32, OUTPUT); digitalWrite(32, HIGH);`

5. **is_valid_display_id()** rejects 0x00000000, 0xFFFFFFFF, 0x00FFFFFF, and
   repeating byte patterns. Without this, floating MISO pins cause false positives.

6. **print_line() needs display category check** in universal build — SSD1306
   uses different text layout than TFT displays.

7. **Serial port**: PlatformIO may auto-detect wrong ports (e.g., Bluetooth
   speakers). Always specify `--upload-port` explicitly.

## Code Conventions

- Language: C++ (ESP-IDF, not Arduino — but some Arduino-compat shims exist)
- Comments and docs in English
- Compile-time display selection via `#ifdef LCD_TFT`, `#ifdef LCD_SSD1306`, etc.
- Universal build adds `#elif defined(UNIVERSAL_BUILD)` sections
- LovyanGFX for all display rendering (replaces TFT_eSPI from earlier versions)
- Logging: `ESP_LOGW("TAG", ...)` for warnings, `Log.info(...)` for app-level

## Branches

- `master` — stable release branch
- `next` — development branch (upstream)
- `cyd-universal` — CYD-only runtime detection (merged)
- `esp32-universal` — Full universal build (current work)

## Testing

Tested hardware (2026-03-15):
- Lolin D32 Pro + ILI9341 TFT Shield
- CYD 3.2" ESP32-2432S032 (ST7789)
- Heltec WiFi Kit 32 (SSD1306 OLED)

See `docs/ESP32_UNIVERSAL_TEST_PLAN.md` for full test matrix.
