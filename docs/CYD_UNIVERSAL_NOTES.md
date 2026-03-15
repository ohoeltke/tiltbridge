# CYD Universal Display Auto-Detection — Development Notes

## Goal

A single binary (`env:cyd`) should support all CYD (Cheap Yellow Display) variants:
- ESP32-2432S024 (2.4")
- ESP32-2432S028 v1/v2/v3 (2.8")
- ESP32-2432S032 (3.2")

The displays differ in their display driver (ILI9341, ILI9342, ST7789) and partly in the backlight pin (GPIO 21 vs GPIO 27). The touch controller (XPT2046) and SPI pin assignment are identical across all variants.

## Known CYD Variants

| Board | Display | Driver | Backlight | Touch |
|-------|---------|--------|-----------|-------|
| ESP32-2432S028 v1 | 2.8" | ILI9341 | GPIO 21 | XPT2046 (R) |
| ESP32-2432S028 v2 | 2.8" | ILI9342 | GPIO 21 | XPT2046 (R) |
| ESP32-2432S028 v3 | 2.8" | ST7789 | GPIO 27 | XPT2046 (R) |
| ESP32-2432S032 | 3.2" | ST7789 | GPIO 27 | XPT2046 (R) |
| ESP32-2432S024 | 2.4" | ILI9341 or ST7789 | GPIO 27 | XPT2046 (R) |

Sources: ESP32-Marauder CYD project, various community documentation.

## Modified Files (based on `origin/next`)

### 1. `platformio.ini`
- **Platform upgrade**: `espressif32` (unversioned, resolved to 6.3.1 / ESP-IDF 5.0.2) → `espressif32@6.13.0` (ESP-IDF 5.5.3)
- Reason: The codebase uses APIs only available from ESP-IDF 5.2+ (`httpd_req_async_handler_begin`, `driver/i2c_master.h`, `littlefs` partition type)
- The official project uses ESP-IDF 5.5.1 for releases

### 2. `src/lovyan_config.h`
- The existing `LGFX_CYD` class was replaced with a universal version
- Contains all three panel types as members (`Panel_ILI9341`, `Panel_ILI9342`, `Panel_ST7789`)
- New `configure()` method with runtime detection instead of constructor initialization
- `_read_cmd()` helper function for reading display registers via SPI

### 3. `src/bridge_lcd_impl.cpp`
- CYD initialization changed: `new LGFX_CYD()` → `new LGFX_CYD(); cyd_tft->configure();`
- Same approach as `LGFX_M5StickC` (which also uses `configure()`)

## Approaches to Display Detection

### Approach 1: Compile-time `#ifdef` (first attempt, works)
- Separate `env:cyd32` with its own build flag `-D CYD32=1`
- Separate `LGFX_CYD32` class with `Panel_ST7789` and `pin_bl = 27`
- **Advantage**: Simple, reliable
- **Disadvantage**: Requires a separate binary per CYD variant
- **Status**: Works, was merged into the `next` branch (see `cyd32-separate-target` branch in fork)

### Approach 2: Runtime Display ID via SPI Command 0x04 (current approach)

#### Attempt 2a: Reading ID with `spi_3wire = true` (failed)
- All registers returned `0x00000000`
- **Cause**: In 3-wire SPI mode, MOSI is used bidirectionally, but the ESP32 SPI driver does not support half-duplex reads on the MOSI line. The MISO pin (GPIO 12) is ignored in 3-wire mode.
- **Lesson learned**: `spi_3wire = true` completely prevents SPI reads

#### Attempt 2b: Reading ID with `spi_3wire = false` for the probe (works)
- Temporarily set `spi_3wire = false`, initialize bus, read IDs, release bus, restore `spi_3wire = true`
- **Result on ESP32-2432S032 (3.2" ST7789)**:
  ```
  ID04(d1)=0xD9818181  ID04(d0)=0xFFD9C040  ID09=0x00610000
  IDDA=0x81  IDDB=0x81  IDDC=0xB3
  ```
- The ST7789 returns `0x81` as its ID (not the standard value `0x85` from the documentation)
- **Lesson learned**: Display IDs vary by manufacturer/batch

#### Attempt 2c: Fallback strategy for unknown IDs
- Since IDs vary by manufacturer, a multi-stage detection was implemented:
  1. Read multiple SPI commands (0x04, 0x09, 0xDA, 0xDB, 0xDC)
  2. With different dummy bit values (0 and 1)
  3. Match known IDs (ST7789: 0x85/0x81, ILI9342: 0xE3, ILI9341: 0x93)
  4. For unknown IDs: default to ST7789 with GPIO 27 (most common on newer boards)

### Discarded Approaches

- **LovyanGFX Auto-Detect**: LovyanGFX has a built-in auto-detection system in `LGFX_AutoDetect_ESP32_all.hpp`, but it does not know the CYD boards. It uses the same `_read_panel_id()` method that we replicated.
- **Backlight pin as detection criterion**: GPIO 21 vs 27 cannot be reliably tested, as a floating GPIO pin does not produce a definitive signal.

## Critical Bug: Display Stays Dark

On the first attempt with the 3.2" CYD, the display remained dark. Cause: The **backlight pin** is GPIO 27 (not GPIO 21 as on the 2.8"). Found by researching the [ESP32-Marauder CYD project](https://github.com/Fr4nkFletcher/ESP32-Marauder-Cheap-Yellow-Display).

## Current Status

- ✅ Build compiles successfully (`env:cyd`)
- ✅ Flash usage: 50.2% (only +2.5 KB compared to single-display version)
- ✅ **ESP32-2432S032 (3.2" ST7789)**: Display works, detected via ID `0x81`
- ✅ Touch was not explicitly tested, but the code is identical to the working `env:cyd` version

## Pending Tests

### Boards that still need testing:
- [ ] **ESP32-2432S028 v1 (2.8" ILI9341)** — Is the ID correctly detected as ILI9341? Backlight GPIO 21?
- [ ] **ESP32-2432S028 v2 (2.8" ILI9342)** — Is `0xE3` read?
- [ ] **ESP32-2432S028 v3 (2.8" ST7789)** — Is `0x85` or `0x81` read? Backlight GPIO 27?
- [ ] **ESP32-2432S024 (2.4")** — What IDs does this board return?

### Test Procedure:
1. Flash binary: `pio run -e cyd -t upload --upload-port /dev/cu.usbserial-XX`
2. Open serial monitor (115200 baud)
3. Press the reset button
4. Look for `W (xxx) CYD:` lines — these show the read IDs and the detected display type
5. Check if the display lights up and content is displayed correctly

### What to do with unknown IDs:
- Note the serial log lines `ID04(d1)=...`, `IDDA=...` etc.
- If the display is not recognized: add the new ID values to the `is_st7789`, `is_ili9342`, or `is_ili9341` conditions in `lovyan_config.h`

## Debug Logging

Currently `ESP_LOGW("CYD", ...)` logs are active at WARNING level. They output:
- All read register values (ID04, ID09, IDDA, IDDB, IDDC)
- The detected display type and backlight pin

The logs should be downgraded to `ESP_LOGI` (Info) or removed after all testing is complete.

## Dependencies

- **PlatformIO**: `espressif32@6.13.0` (ESP-IDF 5.5.3)
- **LovyanGFX**: Git commit `2e0dc974a9b6521bf155afb53a9a92a623a60803`
- **Python**: `intelhex` module may need to be installed: `~/.platformio/penv/bin/pip install intelhex`
