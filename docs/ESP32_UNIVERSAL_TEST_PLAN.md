# ESP32 Universal Build — Test Plan

## Overview

This test plan covers verification of the `esp32_universal` build target across all
supported ESP32 board types. The universal binary auto-detects the connected display
at runtime and must behave identically to the existing per-board build targets.

## Prerequisites

- PlatformIO with `espressif32@6.13.0` (ESP-IDF 5.5.3)
- Build: `pio run -e esp32_universal`
- Flash: `esptool.py --port <PORT> --baud 115200 --chip esp32 write_flash 0x10000 .pio/build/esp32_universal/firmware.bin`
- Serial monitor at 115200 baud for log verification

---

## Test Matrix

### T1 — Lolin D32 Pro + ILI9341 TFT Shield

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| T1.1 | Flash universal binary, observe serial log | `DETECT: D32 Pro init+read: ID=0x??????` (non-zero, non-0xFFFFFF) | ✅ Pass |
| T1.2 | Display type reported correctly | `Display type: D32 Pro TFT (240x320)` | ✅ Pass |
| T1.3 | Backlight turns on | GPIO 32 HIGH, display illuminated | ✅ Pass |
| T1.4 | TiltBridge logo displayed | Blue logo shown with correct colors for ~2 seconds | ✅ Pass |
| T1.5 | WiFi AP screen | Shows "TiltBridgeAP" SSID and IP 192.168.4.1 | ✅ Pass |
| T1.6 | WiFi configuration via browser | Connect to AP, open 192.168.4.1, configure WiFi | ✅ Pass |
| T1.7 | WiFi connected screen | Shows mDNS URL and IP address, mDNS resolves | ✅ Pass |
| T1.8 | Tilt display | Shows Tilt hydrometer data (temp, gravity, color block) | ✅ Pass |
| T1.9 | Screen rotation (invertTFT) | Display flips 180° when invertTFT toggled in config | ✅ Pass |
| T1.10 | OTA update screen | Shows OTA progress during firmware update | ⏭️ Skipped |
| T1.11 | Reboot cycle | Device reboots cleanly, re-detects display, shows logo | ✅ Pass |
| T1.12 | Compare with per-board build | Behavior matches `env:d32_pro_tft` build | ✅ Pass |
| T1.13 | Rotation hint on AP screen | "NOTE - If this appears upside-down..." text shown on large displays | ✅ Pass |

### T2 — CYD 2.4" (ESP32-2432S024, ILI9341)

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| T2.1 | Flash universal binary, observe serial log | `DETECT: CYD probe HSPI: ID=0x??????` (non-zero) | |
| T2.2 | Display type reported correctly | `Display type: CYD (240x320)` | |
| T2.3 | CYD panel auto-detection | `CYD: Detected ILI9341 display, backlight=GPIO21` | |
| T2.4 | Backlight turns on | PWM backlight via GPIO 21, display illuminated | |
| T2.5 | TiltBridge logo displayed | Blue logo with correct colors | |
| T2.6 | WiFi AP screen | Shows AP name and IP | |
| T2.7 | Touch functionality | Touch input works for screen navigation | |
| T2.8 | Tilt display | Shows Tilt data with color blocks | |
| T2.9 | Rotation hint on AP screen | "NOTE - If this appears upside-down..." text shown | |
| T2.10 | Compare with per-board build | Behavior matches `env:lcd_tft` CYD build | |

### T3 — CYD 2.8" v1/v2/v3 (ESP32-2432S028, ILI9341/ILI9342)

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| T3.1 | Flash universal binary, observe serial log | `DETECT: CYD probe HSPI: ID=0x??????` | |
| T3.2 | CYD panel auto-detection | `Detected ILI9341` or `Detected ILI9342` | |
| T3.3 | Backlight pin correct | GPIO 21 for ILI9341/ILI9342 variants | |
| T3.4 | Display content | Logo, AP screen, Tilt data all render correctly | |
| T3.5 | Touch functionality | XPT2046 touch works | |

### T4 — CYD 3.2" (ESP32-2432S032, ST7789)

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| T4.1 | Flash universal binary, observe serial log | `DETECT: CYD probe HSPI: ID=0x??????` | ✅ Pass (verified on cyd-universal) |
| T4.2 | CYD panel auto-detection | `Detected ST7789 display, backlight=GPIO27` | ✅ Pass (verified on cyd-universal) |
| T4.3 | Backlight pin correct | GPIO 27 (different from 2.4"/2.8" models!) | ✅ Pass |
| T4.4 | Color inversion | ST7789 uses `invert=true`, colors correct | ✅ Pass |
| T4.5 | Display content | Logo, AP screen, Tilt data all render correctly | ✅ Pass |
| T4.6 | Touch functionality | XPT2046 touch works | |
| T4.7 | Rotation hint on AP screen | "NOTE - If this appears upside-down..." text shown | ✅ Pass |

### T5 — M5StickC Plus (AXP192 + ST7789)

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| T5.1 | Flash universal binary, observe serial log | `DETECT: Found AXP192 -> M5StickC Plus` | |
| T5.2 | Display type reported correctly | `Display type: M5StickC Plus (135x240)` | |
| T5.3 | AXP192 power init | LDO2, LDO3, GPIO0 set correctly for display power | |
| T5.4 | Display content | Small XBitmap logo, AP screen with small font | |
| T5.5 | Small display layout | Uses CAT_SMALL layout, 5 tilts per page | |
| T5.6 | Compare with per-board build | Behavior matches `env:m5stickc_plus` build | |

### T6 — M5StickC Plus2 (GPIO power + ST7789)

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| T6.1 | Flash universal binary, observe serial log | `DETECT: M5StickC Plus2 init+read: ID=0x??????` | |
| T6.2 | Display type reported correctly | `Display type: M5StickC Plus2 (135x240)` | |
| T6.3 | Backlight via GPIO 27 | Display illuminated | |
| T6.4 | Display content | Small XBitmap logo, AP screen | |
| T6.5 | No AXP192 false positive | AXP192 probe returns false (no AXP192 on Plus2) | |
| T6.6 | Compare with per-board build | Behavior matches `env:m5stickc_plus2` build | |

### T7 — TTGO TFT (ST7789, 135x240)

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| T7.1 | Flash universal binary, observe serial log | `DETECT: TTGO init+read: ID=0x??????` | |
| T7.2 | Display type reported correctly | `Display type: TTGO TFT (135x240)` | |
| T7.3 | Backlight via GPIO 4 | Display illuminated | |
| T7.4 | Display content | Small XBitmap logo, AP screen | |
| T7.5 | Small display layout | Uses CAT_SMALL layout, 5 tilts per page | |
| T7.6 | Compare with per-board build | Behavior matches `env:tft_espi` build | |

### T8 — SSD1306 OLED (128x64, I2C)

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| T8.1 | Flash universal binary, observe serial log | `DETECT: Found SSD1306 OLED on I2C` | ✅ Pass |
| T8.2 | Display type reported correctly | `Display type: OLED SSD1306 (128x64)` | ✅ Pass |
| T8.3 | I2C pin detection | Correct SDA/SCL pins detected for the board variant | ✅ Pass |
| T8.4 | Display content | XBitmap logo, text screens | ✅ Pass |
| T8.5 | Screen rotation | invertTFT config flips display | ✅ Pass |
| T8.6 | Tilt display | Tilt data in small OLED format | ✅ Pass |
| T8.7 | Compare with per-board build | Behavior matches `env:lcd_ssd1306` build | ✅ Pass |

### T9 — Headless (no display connected)

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| T9.1 | Flash universal binary, observe serial log | `DETECT: No display found -> headless mode` | |
| T9.2 | Display type reported correctly | `Display type: Headless (0x0)` | |
| T9.3 | No crash | Device boots without crash despite no display | |
| T9.4 | WiFi functional | WiFi AP starts, web interface accessible | |
| T9.5 | Tilt scanning | BLE scanning and Tilt detection works | |
| T9.6 | Probe sequence timing | All probes complete within ~500ms | |

---

## Cross-Cutting Tests

### TX1 — Probe Sequence Isolation

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| TX1.1 | CYD probe doesn't interfere with D32 Pro | D32 Pro still detected after CYD probe (different SPI hosts) | ✅ Pass |
| TX1.2 | Pin cleanup after failed probes | All probed pins reset to INPUT/high-Z | ✅ Pass |
| TX1.3 | SPI host released after probe | No SPI bus conflicts between probes | ✅ Pass |

### TX2 — Memory & Flash

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| TX2.1 | Flash usage | < 55% (< 1.8 MB of 3.3 MB app partition) | ✅ Pass (51%) |
| TX2.2 | Heap after init | Sufficient free heap for WiFi + BLE + HTTP | |
| TX2.3 | No memory leaks | Deleted LGFX objects from failed probes freed | |

### TX3 — Stability

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| TX3.1 | 24h runtime | No crashes, watchdog resets, or display glitches | |
| TX3.2 | Multiple reboots | Display detected consistently across 10+ reboots | |
| TX3.3 | Power cycle | Cold boot detects display correctly | ✅ Pass |

### TX4 — Regression

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| TX4.1 | Per-board builds still compile | All existing env targets build without errors | |
| TX4.2 | Per-board builds unchanged | No behavioral changes in existing builds | |

---

## Known Limitations

1. **ESP32 vs ESP32-S3**: The universal binary only covers ESP32 boards. ESP32-S3
   boards (e.g., S3 TDisplay) require a separate binary due to different chip architecture.

2. **pushImage DMA issue**: Large image pushes with `setSwapBytes(true)` crash in the
   universal build. Workaround: pre-swapped image data with `setSwapBytes(false)`.

3. **CYD probe without MISO**: The CYD SPI probe requires `spi_3wire=false` (4-wire SPI)
   so that MISO (GPIO 12) is used for reading display ID registers.

4. **D32 Pro needs full init for probe**: A simple SPI bus probe returns 0xFFFFFFFF because
   the ILI9341 needs a hardware reset (RST pin toggle) before responding to RDDID. The
   probe creates a full LGFX object and calls `init()`.

---

## Test Execution Log

| Date | Tester | Hardware | Tests Run | Results | Notes |
|------|--------|----------|-----------|---------|-------|
| 2026-03-15 | oho | D32 Pro + ILI9341 | T1.1–T1.13, TX1, TX2.1, TX3.3 | All pass (T1.10 skipped) | Full test, including Tilt with 2 hydrometers |
| 2026-03-15 | oho | CYD 3.2" ST7789 | T4.1–T4.5, T4.7 | All pass | Logo, AP screen, rotation hint verified |
| 2026-03-15 | oho | Heltec SSD1306 OLED | T8.1–T8.7 | All pass | Fixed false positive detection (TTGO/M5), fixed OLED print_line layout |
