# Universal ESP32 Binary — Runtime Display Auto-Detection

## Goal

Replace the six separate ESP32 build environments with a **single binary** that auto-detects the connected display hardware at boot time.

ESP32-S3 environments remain separate (different chip).

## Probe Sequence (as implemented)

```
1. I2C: AXP192 (0x34) on SDA=21/SCL=22       → M5StickC Plus
2. SPI: HSPI bare probe (SCLK=14, MOSI=13,
        MISO=12, DC=2, CS=15)                  → CYD (sub-detect ILI9341/9342/ST7789)
3. SPI: VSPI full init (SCLK=18, MOSI=23,
        MISO=19, DC=27, CS=14, RST=33)         → D32 Pro
4. SPI: VSPI full init (SCLK=18, MOSI=19,
        DC=16, CS=5)                            → TTGO TFT
5. SPI: HSPI full init (SCLK=13, MOSI=15,
        DC=14, CS=5)                            → M5StickC Plus2
6. I2C: SSD1306 (0x3C) on multiple pin combos  → OLED
7. Nothing found                                → Headless
```

Note: The probe order changed from the original plan. CYD was moved to #2
(lightweight bare SPI probe, unique HSPI pins), and M5StickC Plus2 to #5
(requires full LGFX init). See [AUTODETECTION_DEEP_DIVE.md](AUTODETECTION_DEEP_DIVE.md)
for the rationale and risk analysis.

## Display Categories

| Category | Resolution     | Logo                  | Boards              |
|----------|---------------|-----------------------|----------------------|
| NONE     | —             | —                     | Headless             |
| SMALL    | 128×64/135×240| oled_logo (546 B)     | SSD1306, TTGO, M5    |
| LARGE    | 240×320       | tft_logo (~135 KB)    | CYD, D32 Pro         |

## New Types

```cpp
enum class DisplayType { NONE, SSD1306, CYD, D32_PRO, M5STICKC_PLUS, M5STICKC_PLUS2, TTGO_TFT };
enum class DisplayCategory { NONE, SMALL, LARGE };

struct DisplayInfo {
    DisplayType type;
    DisplayCategory category;
    int width;
    int height;
    bool has_touch;
    bool has_axp192;
    const char* hardware_version;
};
```

## Implementation Phases

### Phase 1 — Foundation (no behavior change)
1. Add `DisplayType`/`DisplayCategory`/`DisplayInfo` to `bridge_lcd.h`
2. Remove `#ifdef` guards from `tft_logo.h` and `oled_logo.h`
3. Merge all LGFX classes in `lovyan_config.h` — remove `#ifdef` guards, make all classes available unconditionally
4. Always compile `axp192.cpp/.h` (remove `#if defined(AXP192)` guard)
5. Create `[env:esp32_universal]` in `platformio.ini`
6. Verify build compiles

### Phase 2 — Detection
7. Implement `detect_display()` probe sequence
8. Test on each board type with serial logging

### Phase 3 — Runtime Branching
9. Refactor `bridge_lcd::init()` to use `detect_display()` result
10. Refactor `init_power()` to use runtime switch
11. Refactor `print_line()` — `#ifdef` → category checks
12. Refactor `display_logo_internal()` — `#ifdef` → category checks
13. Refactor `checkTouch()` — `#ifdef TOUCH_CS` → runtime check
14. Refactor `bridge_lcd.cpp` — remove `#if HAVE_LCD` no-op pattern
15. Refactor `watchButtons.cpp` — `#ifndef LCD_TFT` → runtime check

### Phase 4 — Cleanup
16. Test on all board types
17. Deprecate/remove old per-board environments

## Flash Impact

| Item                  | Added   |
|-----------------------|---------|
| tft_logo data         | +135 KB |
| Panel drivers (all)   | +15 KB  |
| AXP192 driver         | +5 KB   |
| Probe code            | +3 KB   |
| Extra UI paths        | +2 KB   |
| **Total**             | **~160 KB** |

From 50% → ~55% utilization. Still ~1.4 MB free.

## Implementation Status

All phases are implemented and tested on 3 boards:
- ✅ D32 Pro + ILI9341 TFT
- ✅ CYD 3.2" ST7789
- ✅ Heltec WiFi Kit 32 SSD1306 OLED

Per-board build environments are kept for backward compatibility.
The universal build coexists alongside them.

## Risk Assessment (updated with test results)

| Board           | Risk   | Reason                                          | Status |
|-----------------|--------|--------------------------------------------------|--------|
| CYD             | LOW    | Bare HSPI probe, unique pins, tested             | ✅ Verified |
| M5StickC Plus   | LOW    | AXP192 I2C is unique identifier                  | ⏳ Untested |
| SSD1306         | LOW    | I2C probe well-established, tested               | ✅ Verified |
| D32 Pro         | LOW    | Full init probe with RST, tested                 | ✅ Verified |
| TTGO            | MEDIUM | Same VSPI bus as D32, different pins              | ⏳ Untested |
| M5StickC Plus2  | MEDIUM | No AXP192, full init probe needed                | ⏳ Untested |

## Files to Modify

- `src/bridge_lcd.h` — Add DisplayType/DisplayInfo, remove compile-time constants
- `src/bridge_lcd.cpp` — Remove `#if HAVE_LCD` no-op pattern, runtime checks
- `src/bridge_lcd_impl.cpp` — Main refactor: init, layout, power, touch
- `src/lovyan_config.h` — Merge LGFX classes, remove `#ifdef` guards
- `src/axp192.h/.cpp` — Remove `#if defined(AXP192)` guard
- `src/img/tft_logo.h` — Remove `#ifdef` guard
- `src/img/oled_logo.h` — Remove `#ifdef` guard (if present)
- `platformio.ini` — Add `[env:esp32_universal]`
- `src/watchButtons.cpp` — Runtime check for touch vs buttons
