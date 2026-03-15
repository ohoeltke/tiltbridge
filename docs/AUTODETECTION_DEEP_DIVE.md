# Display Auto-Detection — Deep Dive

## Overview

The universal build (`esp32_universal`) detects the connected display at runtime
during `bridge_lcd::init()`. The detection runs once at boot, before WiFi or BLE
initialization. The result is stored in `display_info` (a `DisplayInfo` struct)
and determines which LGFX display class is instantiated and how the UI is rendered.

Total detection time: ~500–800 ms (depending on how many probes run before a match).

## Detection Flow

```
boot
  │
  ▼
detect_display()
  │
  ├── Probe 1: AXP192 on I2C (SDA=21, SCL=22)
  │   │  Method: i2c_master_probe() at address 0x34
  │   │  Match: → M5StickC Plus
  │   └── Miss: continue
  │
  ├── Probe 2: CYD on HSPI (SCLK=14, MOSI=13, MISO=12, DC=2, CS=15)
  │   │  Method: probe_spi_display() → RDDID (cmd 0x04) via bare Bus_SPI
  │   │  Match: is_valid_display_id(id) → CYD
  │   │  Cleanup: all probed pins reset to INPUT (high-Z)
  │   └── Miss: continue
  │
  ├── Probe 3: D32 Pro on VSPI (SCLK=18, MOSI=23, MISO=19, DC=27, CS=14, RST=33)
  │   │  Method: new LGFX_D32_Pro() → init() → readCommand(0x04)
  │   │  Match: is_valid_display_id(id) → D32 Pro (tft object kept)
  │   │  No cleanup: LGFX object deleted on miss, pins released by destructor
  │   └── Miss: delete object, continue
  │
  ├── Probe 4: TTGO TFT on VSPI (SCLK=18, MOSI=19, DC=16, CS=5)
  │   │  Method: new LGFX_TFT_ESPI() → init() → readCommand(0x04)
  │   │  Match: is_valid_display_id(id) → TTGO TFT (tft object kept)
  │   └── Miss: delete object, continue
  │
  ├── Probe 5: M5StickC Plus2 on HSPI (SCLK=13, MOSI=15, DC=14, CS=5)
  │   │  Method: new LGFX_M5StickC() → configure(true) → init() → readCommand(0x04)
  │   │  Match: is_valid_display_id(id) → M5StickC Plus2 (tft object kept)
  │   └── Miss: delete object, continue
  │
  ├── Probe 6: SSD1306 OLED on I2C
  │   │  Method: i2c_device_at_address(0x3C, ...) on 4 pin combos:
  │   │    (SDA=5,  SCL=4)  — Heltec WiFi Kit 32
  │   │    (SDA=21, SCL=22) — Generic ESP32 boards
  │   │    (SDA=4,  SCL=15) — TTGO LoRa32 (RST on GPIO 16)
  │   │    (SDA=17, SCL=18) — Heltec Wireless Stick variant
  │   │  Match: any ACK at 0x3C → SSD1306
  │   └── Miss: continue
  │
  └── Fallback: Headless (no display)
```

## Probe Methods

### 1. I2C Device Probe (`i2c_device_at_address`)

Used for: AXP192 (address 0x34), SSD1306 (address 0x3C)

Creates a temporary I2C master bus on the given SDA/SCL pins, sends a probe
(address-only transaction) to the target address, and checks for an ACK.
The I2C bus is released after each probe attempt.

**Reliability**: High. I2C ACK/NACK is a definitive signal. If a device ACKs
at the probed address, it is present on those pins. False positives can only
occur if a *different* I2C device at the same address is connected.

### 2. Bare SPI Probe (`probe_spi_display`)

Used for: CYD (Probe 2)

Creates a temporary `lgfx::Bus_SPI`, configures it for the target pins, sends
the RDDID command (0x04) and reads 4 bytes via MISO. Then releases the bus and
resets all probed pins to INPUT mode.

**Key detail**: `spi_3wire` is set to `(miso < 0)`. For CYD, MISO=GPIO12 is
available, so `spi_3wire=false` (4-wire mode) is used. This allows reading the
display's response via the dedicated MISO pin. With `spi_3wire=true`, the read
would use bidirectional MOSI, which produces unreliable results on floating pins.

### 3. Full LGFX Init + Read (`readCommand`)

Used for: D32 Pro (Probe 3), TTGO (Probe 4), M5StickC Plus2 (Probe 5)

Creates the full LGFX display object, calls `init()` (which sends the complete
display initialization sequence including hardware reset via RST pin if
configured), then reads the display ID via `readCommand(0x04, 0, 4)`.

**Why full init?** Some displays (notably the ILI9341 on D32 Pro) do not respond
to RDDID until after a hardware reset. The bare SPI probe returned `0xFFFFFFFF`
because the ILI9341 was in an undefined state. Only after the full LovyanGFX
`init()` sequence (which toggles RST=GPIO33) does the display respond to
read commands.

**Trade-off**: Full init is slower (~200 ms per probe) and sends SPI commands to
whatever is connected to those pins. On boards where those pins are connected to
other peripherals (e.g., LoRa module, SD card), these commands are harmless
(wrong CS/DC) but waste time. The LGFX object is deleted on miss, freeing
its memory.

## ID Validation (`is_valid_display_id`)

Every SPI probe result goes through this validation:

```cpp
static bool is_valid_display_id(uint32_t id)
{
    if (id == 0 || id == 0xFFFFFFFF || id == 0x00FFFFFF) return false;
    // Reject repeating byte patterns (e.g. 0x1F1F1F1F, 0xA5A5A5A5)
    uint8_t b0 = id & 0xFF;
    if (((id >> 8) & 0xFF) == b0 &&
        ((id >> 16) & 0xFF) == b0 &&
        ((id >> 24) & 0xFF) == b0)
        return false;
    return true;
}
```

**Rejected patterns**:
| Pattern | Cause |
|---------|-------|
| `0x00000000` | No device, MISO pulled/floating low |
| `0xFFFFFFFF` | No device, MISO pulled/floating high |
| `0x00FFFFFF` | Common "empty" response (3-byte 0xFF with leading zero) |
| `0xXXXXXXXX` (all bytes equal) | Floating MISO producing repeating noise pattern |

## Probe Order Rationale

The probes are ordered to minimize false positives and side effects:

1. **AXP192 first** — The AXP192 is a unique I2C power management IC only found on
   the M5StickC Plus. It has a distinctive I2C address (0x34) that no other
   TiltBridge-supported device uses. Detection is fast (~1 ms) and has zero
   side effects on other boards.

2. **CYD second** — Uses bare SPI probe (lightweight, with pin cleanup). The CYD
   pin configuration (HSPI with SCLK=14, MOSI=13, MISO=12) is unique and doesn't
   overlap with other boards. Fast probe (~10 ms).

3. **D32 Pro third** — Uses full LGFX init because the ILI9341 requires a hardware
   reset. VSPI pins (18/23/19) overlap with TTGO TFT pins, but the additional
   pins (DC=27, CS=14, RST=33) are unique. Must come before TTGO because both
   share VSPI but with different pin assignments — running D32 Pro first won't
   interfere because its CS/DC are different from TTGO's.

4. **TTGO TFT fourth** — Also on VSPI but with different DC (16) and CS (5).
   After D32 Pro's LGFX object is deleted, the VSPI peripheral is released
   and available for TTGO's probe.

5. **M5StickC Plus2 fifth** — On HSPI (same host as CYD), but CYD has already
   been probed and its pins cleaned up. The Plus2 uses different pins
   (SCLK=13, MOSI=15, CS=5) that don't conflict.

6. **SSD1306 OLED last** — I2C probe is non-destructive and fast. Placed last
   because SPI displays are more common in TiltBridge deployments and should
   be detected first for faster boot on those boards.

## False Positive Risks

### Risk 1: Non-display SPI devices on matching pins (MEDIUM)

**Scenario**: A custom board has a non-display SPI device (e.g., LoRa module,
SD card reader) wired to the same SPI pins as one of the probed displays.

**Example**: The Heltec WiFi LoRa 32 has a LoRa module (SX1276) on VSPI. If
its pin mapping overlapped with the TTGO probe (SCLK=18, MOSI=19, CS=5), the
LoRa module might respond to RDDID with a non-zero value that passes
`is_valid_display_id()`.

**Current mitigation**: The `is_valid_display_id()` function rejects common
floating-pin patterns. However, a real SPI device could return a valid-looking
but meaningless ID.

**Observed**: On the Heltec OLED board, the TTGO probe returned `0xFFFFFFFF`
(rejected) and M5StickC Plus2 probe returned `0x1F1F1F1F` (rejected by
repeating-byte check). No false positives occurred after applying these filters.

**Potential improvement**: Add a secondary ID verification — after RDDID (0x04),
also read RDDST (0x09) or individual ID registers (0xDA/DB/DC). A real display
would return consistent, known values across multiple registers. A non-display
device would return random or mismatched values.

### Risk 2: I2C address collision for SSD1306 (LOW)

**Scenario**: A non-display I2C device at address 0x3C is connected (e.g., a
temperature sensor, EEPROM, or another OLED display type).

**Mitigation**: Address 0x3C is the standard SSD1306/SH1106 OLED address. Few
non-display devices use it. If a different device responds, the SSD1306 LGFX
init would fail silently (no crash, but no display output).

**Potential improvement**: After I2C ACK, send the SSD1306 display-on command
and verify a response. Or read the SSD1306 chip ID register.

### Risk 3: AXP192 on non-M5StickC boards (VERY LOW)

**Scenario**: A custom board uses an AXP192 PMIC at address 0x34 on SDA=21/SCL=22
but has a different display.

**Mitigation**: Unlikely in practice. The AXP192 is rare outside M5Stack products.

### Risk 4: CYD probe on boards with HSPI peripherals on matching pins (LOW)

**Scenario**: A board uses GPIO 14 (SCLK) and GPIO 13 (MOSI) for a non-display
HSPI device, and GPIO 12 (MISO) returns valid-looking data.

**Mitigation**: The CYD uses a bare SPI probe with `spi_3wire=false`. For a false
positive, the non-display device would need to:
1. Have CS on GPIO 15 (pulled low during probe)
2. Respond to the RDDID command (0x04) with a valid-looking 4-byte ID

This is extremely unlikely for non-display devices.

## False Negative Risks

### Risk 1: D32 Pro without hardware reset (FIXED)

**Problem**: The initial bare SPI probe (without RST toggle) returned `0xFFFFFFFF`
because the ILI9341 was in an undefined state.

**Fix**: Changed D32 Pro probe to use full LGFX init (which toggles RST=GPIO33).
Now returns valid ID.

### Risk 2: CYD with spi_3wire=true (FIXED)

**Problem**: With `spi_3wire=true`, MISO is not used for reading, so the RDDID
command always returned `0x00000000`.

**Fix**: Set `spi_3wire=false` during the CYD probe so GPIO 12 (MISO) is used
for reading the display response.

### Risk 3: Unknown I2C pin combination for SSD1306 (LOW)

**Scenario**: An SSD1306 OLED board uses SDA/SCL pins not in the probe list
(currently: 5/4, 21/22, 4/15, 17/18).

**Impact**: The OLED would not be detected → fallback to headless mode.

**Fix**: Add the new pin combination to the probe list. This is a code change,
not a detection logic issue.

### Risk 4: Display that returns only zeros (LOW)

**Scenario**: A display chip that doesn't implement RDDID (command 0x04) and
returns all zeros.

**Impact**: `is_valid_display_id(0x00000000)` returns false → display not detected.

**Mitigation**: All known display chips in TiltBridge-supported boards (ILI9341,
ILI9342, ST7789, SSD1306) support RDDID or I2C probe. Unknown displays would
need custom probe logic.

## Crash / Side-Effect Risks

### Risk 1: pushImage with setSwapBytes(true) on large images (CONFIRMED)

**Problem**: Calling `tft->pushImage()` with `setSwapBytes(true)` on the large
TFT logo (288x240, ~138 KB pixel data) causes an ESP32 crash/reset in the
universal build. The exact cause is unclear — likely a DMA-related memory issue
or watchdog timeout during the byte-swap operation.

**Symptoms**: Display goes black, backlight turns off, no serial output (hard reset).

**Workaround**: Pre-swap the bytes in the logo header file at compile time
(`tft_logo_swapped.h`), then push with `setSwapBytes(false)`.

**Note**: This issue does NOT occur in per-board builds (e.g., `env:lcd_tft`).
It is specific to the universal build, possibly due to different memory layout
or DMA channel configuration when multiple LGFX classes are compiled in.

### Risk 2: SPI commands sent to non-display peripherals during probe (LOW)

**Problem**: Probes 3–5 call `init()` on full LGFX objects, which sends the
complete display initialization command sequence over SPI. If those pins are
connected to a non-display SPI device (e.g., LoRa module, SD card), the device
receives unexpected commands.

**Impact**: Most SPI devices ignore commands that don't match their protocol.
The CS/DC pin combination used by the probe is unique to each display type,
so the non-display device's actual CS pin is not asserted. The commands go to
a "wrong" CS pin and are ignored.

**Observed**: On the Heltec WiFi LoRa 32, probes 3–5 sent SPI commands to pins
shared with the LoRa module. No adverse effects were observed — the LoRa module
was not affected because its CS pin was not driven.

### Risk 3: I2C bus conflict during probe (OBSERVED, NON-FATAL)

**Problem**: The AXP192 I2C probe (Probe 1) uses SDA=21/SCL=22. On the Heltec
board, these GPIOs are used for the OLED reset circuit, producing warnings:

```
W (945) i2c.common: GPIO 21 is not usable, maybe conflict with others
E (951) i2c.master: I2C hardware timeout detected
```

**Impact**: Non-fatal. The I2C probe times out and returns false (no AXP192
found). Detection continues to the next probe. The I2C master bus is released
after the timeout.

**Note**: These warnings appear in the serial log but do not affect functionality.
The SSD1306 OLED is later detected successfully on different I2C pins.

### Risk 4: Memory leak on probe failure (MITIGATED)

**Problem**: Probes 3–5 allocate LGFX objects on the heap (`new LGFX_D32_Pro()`,
etc.). If a probe fails, the object is deleted (`delete d32`). If a probe
succeeds, the object is kept (`tft = d32`).

**Risk**: If `init()` throws an exception or causes a crash before `readCommand`
completes, the allocated object would leak.

**Mitigation**: LovyanGFX `init()` does not throw exceptions. If it fails
internally, it returns false and the object remains in a safe state for deletion.
No leaks have been observed in testing.

### Risk 5: GPIO pin conflicts between probes (MITIGATED)

**Problem**: Multiple probes use overlapping GPIO pins:
- D32 Pro (Probe 3): SCLK=18, MOSI=23, CS=14
- TTGO (Probe 4): SCLK=18, MOSI=19, CS=5
- Both use VSPI_HOST

**Mitigation**: The full LGFX `init()` + `delete` cycle properly acquires and
releases the SPI peripheral. After `delete`, the VSPI host is available for the
next probe. The CYD bare probe additionally resets all used pins to INPUT mode.

## Detection Timing (measured on ESP32 rev 3.1)

| Probe | Time | Notes |
|-------|------|-------|
| AXP192 I2C | ~2 ms | Fast I2C address probe |
| CYD bare SPI | ~8 ms | Lightweight bus init + read + cleanup |
| D32 Pro full init | ~320 ms | Full LGFX init including RST toggle (120 ms wait) |
| TTGO full init | ~210 ms | Full LGFX init (no RST toggle) |
| M5StickC Plus2 full init | ~210 ms | Full LGFX init (with RST toggle) |
| SSD1306 I2C (4 attempts) | ~20 ms | Four I2C probes, ~5 ms each |
| **Total (worst case)** | **~770 ms** | All probes run (no display → headless) |
| **Typical (CYD)** | **~10 ms** | AXP192 miss + CYD hit |
| **Typical (D32 Pro)** | **~330 ms** | AXP192 miss + CYD miss + D32 Pro hit |
| **Typical (SSD1306)** | **~770 ms** | All SPI probes miss, I2C hits |

## Possible Future Improvements

1. **Secondary ID verification**: After RDDID (0x04), also check RDDST (0x09)
   and individual ID registers (0xDA/DB/DC) to reduce false positive risk.

2. **NVS caching**: Store the detected display type in NVS (non-volatile storage).
   On subsequent boots, skip the probe sequence and use the cached result. Add a
   "re-detect" option in the web interface for when hardware changes.

3. **Probe timeout**: Add a per-probe timeout to prevent hangs if an SPI device
   doesn't release the bus.

4. **Avoid full init for probes**: Refactor D32 Pro/TTGO/M5 probes to use bare
   SPI bus with explicit RST toggle (like the CYD probe), avoiding the overhead
   and side effects of full LGFX initialization. This would require extracting
   the RST pin information from the LGFX class configuration.

5. **User-selectable display type**: Add a setting in the web interface to
   manually override the auto-detected display type, bypassing the probe
   sequence entirely.
