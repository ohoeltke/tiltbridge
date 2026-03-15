#ifndef TILTBRIDGE_BRIDGE_LCD_H
#define TILTBRIDGE_BRIDGE_LCD_H

#include "tilt/tiltHydrometer.h"

#define LOGO_TIME 2     // Time (in seconds) to display the logo
#define TILT_TIME 10    // Time (in seconds) to display the Tilt screen

// ---- Runtime display detection types ----
enum class DisplayType {
    DISP_NONE,          // Headless (no display)
    DISP_SSD1306,       // 128x64 I2C OLED
    DISP_CYD,           // 240x320 CYD (ILI9341/ILI9342/ST7789 auto-detected)
    DISP_D32_PRO,       // 240x320 ILI9341 on VSPI (Lolin D32 Pro)
    DISP_M5STICKC_PLUS, // 135x240 ST7789 (AXP192 power)
    DISP_M5STICKC_PLUS2,// 135x240 ST7789 (GPIO power)
    DISP_TTGO_TFT,      // 135x240 ST7789
};

enum class DisplayCategory {
    CAT_NONE,           // No display
    CAT_SMALL,          // 128x64 or 135x240 — uses oled_logo, small text
    CAT_LARGE,          // 240x320 — uses tft_logo, large text
};

struct DisplayInfo {
    DisplayType type;
    DisplayCategory category;
    int width;
    int height;
    bool has_touch;
    bool has_axp192;
    int tilts_per_page;
    const char* hardware_version;
};

// ---- Legacy compile-time defines (kept for backward compatibility) ----
// These are still used by per-board environments. The universal build
// sets them based on runtime detection results.

#ifdef LCD_SSD1306
#include <LovyanGFX.hpp>
#include "lovyan_config.h"
#define SSD1306_FONT_HEIGHT     10
#define SSD_LINE_CLEARANCE      2
#define TILTS_PER_PAGE          5 // The actual number is one fewer than this - the first row is used for headers
#define HAVE_LCD                1

#elif defined(LCD_TFT) || defined(LCD_TFT_ESPI)

// For the LCD_TFT displays, we're connecting via SPI
// LovyanGFX handles SPI internally, no separate SPI include needed
#include <LovyanGFX.hpp>
#include "lovyan_config.h"

#define FF_NORMAL               &FreeSans9pt7b

#define HAVE_LCD                1

#if defined(LCD_TFT)
// Big TFTs
#define TILTS_PER_PAGE          15 // The actual number is one fewer than this - the first row is used for headers
#define TILT_FONT_SIZE          2
#define FF_BIG                  &FreeSans12pt7b
#define MIN_PRESSURE            2000
#else
// Smaller TFTs
#define TILTS_PER_PAGE          5 // The actual number is one fewer than this - the first row is used for headers
#define FF_BIG                  FF_NORMAL
#define TFT_ESPI_FONT_SIZE      20
#define TFT_ESPI_LINE_CLEARANCE 4
#define TFT_ESPI_FONT_HEIGHT    2
#endif

#elif defined(UNIVERSAL_BUILD)
// Universal build — all display types available, selected at runtime
#include <LovyanGFX.hpp>
#include "lovyan_config.h"
#define HAVE_LCD                1
#define SSD1306_FONT_HEIGHT     10
#define SSD_LINE_CLEARANCE      2
#define FF_NORMAL               &FreeSans9pt7b
#define FF_BIG                  &FreeSans12pt7b
#define TILT_FONT_SIZE          2
#define MIN_PRESSURE            2000
#define TFT_ESPI_FONT_SIZE      20
#define TFT_ESPI_LINE_CLEARANCE 4
#define TFT_ESPI_FONT_HEIGHT    2
#define TILTS_PER_PAGE          15 // Default for universal build; overridden at runtime later

#endif // LCD_SSD1306


#define SCREEN_TILT             0
#define SCREEN_LOGO             1
#define SCREEN_MAX              2

class bridge_lcd {
public:
    // All public functions are in bridge_lcd.cpp unless otherwise noted
    bridge_lcd();

    void init();                                    // In impl
    void reinit();                                  // In impl
    void clear();                                   // In impl
    void checkTouch();                              // In impl  

    void display_logo(bool fromReset = false);
    void display_wifi_connect_screen(const char *ap_name, const char *ap_pass);
    void display_wifi_success_screen(const char *mdns_url, const char *ip_address_url);
    void display_wifi_reset_screen();
    void display_ota_update_screen();
    void display_wifi_connecting_screen(const char *ssid);
    void display_wifi_disconnected_screen();
    void display_wifi_reconnect_failed();

    void check_screen();

    bool displaying_ota_update_screen = false;

private:
    // All private functions are in bridge_lcd_impl.cpp unless otherwise noted
    void print_line(const char *left_text, uint8_t line);
    void print_line(const char *left_text, const char *right_text, uint8_t line);
    void print_line(const char *left_text, const char *middle_text, const char *right_text, uint8_t line);
    void print_line(const char *left_text, const char *middle_text, const char *right_text, uint8_t line, bool add_gutter);

    void display_logo_internal();
    inline void init_power();

    void print_tilt_to_line(tiltHydrometer *tilt, uint8_t line);
    bool i2c_device_at_address(uint8_t address, int sda_pin, int scl_pin);

#if defined(LCD_TFT_M5STICKC) || defined(UNIVERSAL_BUILD)
    enum class M5Variant { Plus, Plus2 };
    M5Variant detect_m5_variant();
    M5Variant m5_variant;
#endif

#ifdef UNIVERSAL_BUILD
    DisplayInfo display_info;
    void detect_display();
#endif

    uint8_t display_next();                             // Not in impl
    void display_tilt_screen(uint8_t screen_number);    // Not in impl
    void display();

#if defined(HAVE_LCD)
    lgfx::LGFX_Device *tft;
#endif

    bool displaying_wifi_dc_screen = false;
    uint8_t tilt_pages_in_run;  // Number of pages in the current loop through the active tilts (# active tilts / 3)
    uint8_t tilt_on_page;       // The page number currently being displayed
    uint8_t on_screen;
    unsigned long next_screen_at;

    bool touchLatch = false;    // Ensure we only trigger a touch once
};

void screenFlip();

extern bridge_lcd lcd;
extern bool setWiFiPushed;

#endif // TILTBRIDGE_BRIDGE_LCD_H
