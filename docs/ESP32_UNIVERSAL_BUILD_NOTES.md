# ESP32 Universal Build — Development Notes

## Goal

Create a **single universal ESP32 binary** that auto-detects the connected display
at runtime, supporting all currently supported ESP32 board types:

| Board Type | Display | Interface | Resolution | Bus |
|---|---|---|---|---|
| CYD (all variants) | ILI9341 / ILI9342 / ST7789 | SPI | 240×320 | HSPI |
| Lolin D32 Pro + TFT Shield | ILI9341 | SPI | 240×320 | VSPI |
| M5StickC Plus | ST7789 | SPI | 135×240 | VSPI |
| M5StickC Plus2 | ST7789 | SPI | 135×240 | HSPI |
| TTGO TFT | ST7789 | SPI | 135×240 | VSPI |
| SSD1306 OLED | SSD1306 | I2C | 128×64 | I2C |
| Headless | (none) | — | — | — |

## Architecture

### New Types (bridge_lcd.h)

- **`DisplayType` enum**: `DISP_NONE`, `DISP_SSD1306`, `DISP_CYD`, `DISP_D32_PRO`,
  `DISP_M5STICKC_PLUS`, `DISP_M5STICKC_PLUS2`, `DISP_TTGO_TFT`
- **`DisplayCategory` enum**: `CAT_NONE`, `CAT_SMALL` (128×64 / 135×240),
  `CAT_LARGE` (240×320)
- **`DisplayInfo` struct**: type, category, width, height, has_touch, has_axp192,
  tilts_per_page, hardware_version

### LGFX Display Classes (lovyan_config.h)

Under `#elif defined(UNIVERSAL_BUILD)`, all LGFX classes are defined:

- `LGFX_SSD1306` — configurable I2C pins and address
- `LGFX_CYD` — runtime auto-detection of ILI9341/ILI9342/ST7789 panel + backlight pin
- `LGFX_D32_Pro` — ILI9341 on VSPI, RST=GPIO33
- `LGFX_M5StickC` — configurable for Plus (VSPI) or Plus2 (HSPI) variants
- `LGFX_TFT_ESPI` — ST7789 for TTGO, backlight on GPIO4

### Probe Sequence (bridge_lcd_impl.cpp → `detect_display()`)

The probe sequence is ordered to minimize false positives:

1. **AXP192 on I2C** (SDA=21, SCL=22) — unique I2C power management IC on M5StickC Plus
2. **CYD on HSPI** (SCLK=14, MOSI=13, MISO=12, DC=2, CS=15) — uses `probe_spi_display()` with RDDID command
3. **D32 Pro on VSPI** (SCLK=18, MOSI=23, MISO=19, DC=27, CS=14, RST=33) — creates full LGFX_D32_Pro, calls `init()` + `readCommand(0x04)`, keeps object if display found
4. **TTGO TFT on VSPI** (SCLK=18, MOSI=19, DC=16, CS=5) — same approach as D32 Pro
5. **M5StickC Plus2 on HSPI** (SCLK=13, MOSI=15, DC=14, CS=5) — same approach
6. **SSD1306 OLED on I2C** — tries multiple pin combinations (5/4, 21/22, 4/15, 17/18)
7. **Fallback**: headless mode

### Pin Cleanup After Probing

After each SPI probe, all probed pins are reset to INPUT (high-Z) mode to avoid
interfering with subsequent probes on different pin configurations.

## Implementation Phases

### Phase 1: Build Environment (platformio.ini)
- Added `[env:esp32_universal]` with `-D UNIVERSAL_BUILD=1`
- No board-specific display flags (LCD_TFT, CYD, etc.)
- Builds at ~51% flash usage (~1.67 MB), leaving ~1.6 MB free

### Phase 2: Display Type Infrastructure (bridge_lcd.h)
- Added DisplayType, DisplayCategory enums and DisplayInfo struct
- Extended existing guards for `UNIVERSAL_BUILD`
- Added `detect_display()` method declaration

### Phase 3: LGFX Classes (lovyan_config.h)
- Duplicated all LGFX classes for the universal build section
- CYD class includes runtime panel detection via SPI ID reading
- M5StickC class is configurable for Plus vs Plus2

### Phase 4: Detection & Initialization (bridge_lcd_impl.cpp)
- `probe_spi_display()`: generic SPI probe helper with pin cleanup
- `detect_display()`: 6-step probe sequence
- Universal init path in `init()` via `switch(display_info.type)`
- Extended `reinit()`, `clear()`, `print_line()`, `print_tilt_to_line()`,
  `i2c_device_at_address()` guards for UNIVERSAL_BUILD

### Phase 5: Logo Display Fix
- `pushImage()` with `setSwapBytes(true)` crashes on large images in universal build (DMA issue)
- Solution: pre-swap bytes in logo header file at build time (`tft_logo_swapped.h`)
- Push with `setSwapBytes(false)` using pre-swapped data → no crash, correct colors
- Small displays use XBitmap logo (`oled_logo.h`) which has no byte-order issues

## Testing

### Tested Hardware

| Board | Display | Status | Notes |
|---|---|---|---|
| CYD 3.2" ESP32-2432S032 | ST7789 | ✅ Verified | On branch `cyd-universal` (predecessor) |
| Lolin D32 Pro + TFT Shield | ILI9341 | ✅ Verified | Logo, AP screen, backlight all working |

### Pending Hardware Tests

| Board | Display | Status |
|---|---|---|
| CYD 2.4" / 2.8" | ILI9341 / ILI9342 | ⏳ Hardware arrives next week |
| M5StickC Plus | ST7789 | ⏳ No hardware available |
| M5StickC Plus2 | ST7789 | ⏳ No hardware available |
| TTGO TFT | ST7789 | ⏳ No hardware available |
| SSD1306 OLED | SSD1306 | ⏳ No hardware available |

## Issues Encountered & Solutions

### 1. SPI Display ID Reading Returns 0x000000

**Problem**: `probe_spi_display()` with `spi_3wire = true` returned all zeros.
MISO was not being used for reading.

**Solution**: Set `spi_3wire = false` during probe phase so MISO pin is used for
reading display ID registers. Restore `spi_3wire = true` after probing for normal
operation.

### 2. D32 Pro Probe Returns 0xFFFFFFFF

**Problem**: Generic SPI probe without hardware reset (RST pin toggle) fails because
the ILI9341 needs a reset before it responds to RDDID (command 0x04).

**Solution**: Instead of using the generic `probe_spi_display()` for D32 Pro, create
the full `LGFX_D32_Pro` object and call `init()` which sends the proper initialization
sequence including hardware reset via GPIO 33. Then use `readCommand(0x04)` to read
the display ID. Keep the initialized object (`tft = d32`) to avoid double-initialization.

### 3. D32 Pro Display Goes Black After `init()`

**Problem**: After successful probe, calling `tft->init()` a second time in the switch
case caused a hardware reset that turned off the backlight and left the display in an
unknown state.

**Solution**: Keep the LGFX object from the probe phase (already initialized). In the
switch case, check `if (!tft)` and only create + init a new object if needed. The probe
already did `init()`, so we skip it and go directly to `reinit()`.

### 4. D32 Pro Backlight on GPIO 32

**Problem**: After display detection, the backlight stayed off because GPIO 32 was never
configured as output + HIGH. The LGFX_D32_Pro class does not include a `Light_PWM`
configuration (backlight is controlled externally).

**Solution**: Explicitly set `pinMode(32, OUTPUT); digitalWrite(32, HIGH);` in the
D32 Pro init case.

### 5. `pushImage()` Crash with `setSwapBytes(true)`

**Problem**: Pushing the large TFT logo (288×240, ~138 KB pixel data) with
`setSwapBytes(true)` caused an ESP32 crash/reset. The display went completely black
with no serial output after the push.

**Diagnosis**: Likely a DMA-related issue where LovyanGFX tries to byte-swap the large
image buffer during DMA transfer, exceeding available memory or causing a watchdog timeout.

**Attempted solutions**:
- Row-by-row push with manual byte swap in a 288-pixel buffer → also crashed
- Full push with `setSwapBytes(false)` → worked but colors wrong (R/B channels swapped)

**Final solution**: Pre-swap the bytes in the logo pixel data at compile time:
1. Python script generates `tft_logo_swapped.h` from `tft_logo.h` with byte-swapped pixel values
2. Universal build includes `tft_logo_swapped.h` instead of `tft_logo.h`
3. `pushImage()` called with `setSwapBytes(false)` → no DMA issue, correct colors

### 6. Serial Port Issues

**Problem**: PlatformIO auto-detected Bluetooth speaker (`/dev/cu.soundcoreQ20i`)
instead of USB serial. Also, port busy when serial monitor was open.

**Solution**: Always specify `--upload-port /dev/cu.usbserial-10` explicitly. Close
serial monitor before uploading. Used `esptool.py` directly via PIO's Python environment
when `pio run -t upload` failed.

## Build Size

```
Environment       Flash Used    Flash Free    Percentage
esp32_universal   1,674,832     ~1.55 MB      ~51%
```

The build includes all display drivers and both logo images. The original per-board
builds range from 35-45% flash usage, so the universal build adds ~6-16% overhead
for the additional display drivers.

## Branch History

- `cyd32-separate-target` — Initial CYD 3.2" support as separate build target
- `cyd-universal` — CYD-only runtime auto-detection (ILI9341/ILI9342/ST7789)
- `esp32-universal` — Full universal build with all ESP32 display types (this branch)
