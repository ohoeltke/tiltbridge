#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/gpio.h>
#include <thorlog.h>

#if defined(LCD_SSD1306) || defined(UNIVERSAL_BUILD)
#include <driver/i2c_master.h>
#endif

#include "jsonconfig.h"

// ESP-IDF GPIO compatibility helpers (replacing Arduino's pinMode/digitalWrite)
#ifndef OUTPUT
#define OUTPUT GPIO_MODE_OUTPUT
#endif
#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif

static inline void pinMode(int pin, gpio_mode_t mode) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

static inline void digitalWrite(int pin, int level) {
    gpio_set_level((gpio_num_t)pin, level);
}

// Replace Arduino yield() with FreeRTOS equivalent
static inline void yield() {
    taskYIELD();
}
#include "tilt/tiltScanner.h"
#include "bridge_lcd.h"

#if HAVE_LCD
#include "lovyan_config.h"

#if defined(UNIVERSAL_BUILD)
#include "img/oled_logo.h"        // Small logo (for small displays)
#include "img/tft_logo_swapped.h" // Large logo, byte-swapped for setSwapBytes(false)
#elif defined(LCD_SSD1306) || defined(LCD_TFT_ESPI)
#include "img/oled_logo.h" // Small logo
#elif defined(LCD_TFT)
#include "img/tft_logo.h" // Large logo
#endif
#endif // HAVE_LCD

#if defined(AXP192) || defined(UNIVERSAL_BUILD)
#include "axp192.h"  // ESP-IDF compatible AXP192 driver for M5StickC Plus
AXP192_Driver axp192_driver;
#endif

#if defined(LCD_TFT_M5STICKC) || defined(UNIVERSAL_BUILD)
bridge_lcd::M5Variant bridge_lcd::detect_m5_variant() {
    // Use AXP192's detect method to check for device presence
    if (axp192_driver.detect(21, 22)) {
        Log.notice("Detected M5StickC Plus (AXP192 found)" CR);
        return M5Variant::Plus;
    } else {
        Log.notice("Detected M5StickC Plus2 (no AXP192)" CR);
        return M5Variant::Plus2;
    }
}
#endif

#ifdef UNIVERSAL_BUILD
// Check if a display ID looks valid (not all-zero, all-ones, or repeating byte pattern)
static bool is_valid_display_id(uint32_t id)
{
    if (id == 0 || id == 0xFFFFFFFF || id == 0x00FFFFFF) return false;
    // Reject repeating byte patterns (e.g. 0x1F1F1F1F, 0xA5A5A5A5) — floating pins
    uint8_t b0 = id & 0xFF;
    if (((id >> 8) & 0xFF) == b0 && ((id >> 16) & 0xFF) == b0 && ((id >> 24) & 0xFF) == b0)
        return false;
    return true;
}

// Helper: probe SPI display by reading ID via command 0x04
static uint32_t _do_spi_probe(lgfx::Bus_SPI& bus, int cs)
{
    bus.beginTransaction();
    gpio_set_level((gpio_num_t)cs, 1);
    bus.writeCommand(0, 8);  // NOP
    bus.wait();
    gpio_set_level((gpio_num_t)cs, 0);
    bus.writeCommand(0x04, 8);  // RDDID
    bus.beginRead(1);
    uint32_t res = 0;
    for (int i = 0; i < 4; ++i) {
        res |= (bus.readData(8) & 0xFF) << (i * 8);
    }
    bus.endTransaction();
    gpio_set_level((gpio_num_t)cs, 1);
    return res;
}

static uint32_t probe_spi_display(int sclk, int mosi, int miso, int dc, int cs,
                                   spi_host_device_t host, int rst = -1)
{
    lgfx::Bus_SPI bus;

    // Configure CS as output
    gpio_config_t cs_conf = {
        .pin_bit_mask = (1ULL << cs),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs_conf);
    gpio_set_level((gpio_num_t)cs, 1);

    // Toggle reset pin if provided
    if (rst >= 0) {
        gpio_config_t rst_conf = {
            .pin_bit_mask = (1ULL << rst),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_conf);
        gpio_set_level((gpio_num_t)rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)rst, 1);
        vTaskDelay(pdMS_TO_TICKS(120));  // Wait for display to initialize after reset
    }

    // Probe with 4-wire SPI (read from MISO) — only reliable method
    // 3-wire (bidirectional MOSI) produces false positives on floating pins
    uint32_t res = 0;
    {
        auto cfg = bus.config();
        cfg.spi_host = host; cfg.spi_mode = 0;
        cfg.freq_write = 10000000; cfg.freq_read = 8000000;
        cfg.spi_3wire = (miso < 0);
        cfg.use_lock = true; cfg.dma_channel = SPI_DMA_CH_AUTO;
        cfg.pin_sclk = sclk; cfg.pin_mosi = mosi; cfg.pin_miso = miso; cfg.pin_dc = dc;
        bus.config(cfg);
        bus.init();
        res = _do_spi_probe(bus, cs);
        bus.release();
    }

    // Reset probed pins to input (high-Z) to avoid interfering with other probes
    gpio_config_t reset_conf = {
        .pin_bit_mask = (1ULL << sclk) | (1ULL << mosi) | (1ULL << dc) | (1ULL << cs),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (miso >= 0) reset_conf.pin_bit_mask |= (1ULL << miso);
    if (rst >= 0) reset_conf.pin_bit_mask |= (1ULL << rst);
    gpio_config(&reset_conf);

    return res;
}

void bridge_lcd::detect_display()
{
    ESP_LOGW("DETECT", "Starting display auto-detection...");

    // ---- Probe 1: AXP192 on I2C (M5StickC Plus) ----
    if (axp192_driver.detect(21, 22)) {
        ESP_LOGW("DETECT", "Found AXP192 -> M5StickC Plus");
        display_info = {
            .type = DisplayType::DISP_M5STICKC_PLUS,
            .category = DisplayCategory::CAT_SMALL,
            .width = 135, .height = 240,
            .has_touch = false, .has_axp192 = true,
            .tilts_per_page = 5,
            .hardware_version = "M5StickC Plus",
        };
        return;
    }

    // ---- Probe 2: CYD on HSPI (SCLK=14, MOSI=13, MISO=12, DC=2, CS=15) ----
    {
        uint32_t id = probe_spi_display(14, 13, 12, 2, 15, HSPI_HOST);
        ESP_LOGW("DETECT", "CYD probe HSPI: ID=0x%08X", (unsigned)id);
        if (is_valid_display_id(id)) {
            ESP_LOGW("DETECT", "Found display on CYD pins -> CYD");
            display_info = {
                .type = DisplayType::DISP_CYD,
                .category = DisplayCategory::CAT_LARGE,
                .width = 240, .height = 320,
                .has_touch = true, .has_axp192 = false,
                .tilts_per_page = 15,
                .hardware_version = "CYD",
            };
            return;
        }
    }

    // ---- Probe 3: D32 Pro on VSPI (SCLK=18, MOSI=23, MISO=19, DC=27, CS=14, RST=33) ----
    // Init the actual LGFX class and read ID; keep the object if display found.
    {
        auto* d32 = new LGFX_D32_Pro();
        d32->init();
        uint32_t id = d32->getPanel()->readCommand(0x04, 0, 4);
        ESP_LOGW("DETECT", "D32 Pro init+read: ID=0x%08X", (unsigned)id);
        if (is_valid_display_id(id)) {
            ESP_LOGW("DETECT", "Found D32 Pro TFT (ILI9341)");
            tft = d32;  // Keep the initialized display object
            display_info = {
                .type = DisplayType::DISP_D32_PRO,
                .category = DisplayCategory::CAT_LARGE,
                .width = 240, .height = 320,
                .has_touch = false, .has_axp192 = false,
                .tilts_per_page = 15,
                .hardware_version = "D32 Pro TFT",
            };
            return;
        }
        delete d32;
    }

    // ---- Probe 4: TTGO TFT on VSPI (SCLK=18, MOSI=19, DC=16, CS=5) ----
    {
        auto* ttgo = new LGFX_TFT_ESPI();
        ttgo->init();
        uint32_t id = ttgo->getPanel()->readCommand(0x04, 0, 4);
        ESP_LOGW("DETECT", "TTGO init+read: ID=0x%08X", (unsigned)id);
        if (is_valid_display_id(id)) {
            ESP_LOGW("DETECT", "Found TTGO TFT (ST7789)");
            tft = ttgo;  // Keep the initialized display object
            display_info = {
                .type = DisplayType::DISP_TTGO_TFT,
                .category = DisplayCategory::CAT_SMALL,
                .width = 135, .height = 240,
                .has_touch = false, .has_axp192 = false,
                .tilts_per_page = 5,
                .hardware_version = "TTGO TFT",
            };
            return;
        }
        delete ttgo;
    }

    // ---- Probe 5: M5StickC Plus2 on HSPI (SCLK=13, MOSI=15, DC=14, CS=5) ----
    {
        auto* m5 = new LGFX_M5StickC();
        m5->configure(true);  // Plus2 variant
        m5->init();
        uint32_t id = m5->getPanel()->readCommand(0x04, 0, 4);
        ESP_LOGW("DETECT", "M5StickC Plus2 init+read: ID=0x%08X", (unsigned)id);
        if (is_valid_display_id(id)) {
            ESP_LOGW("DETECT", "Found M5StickC Plus2");
            tft = m5;  // Keep the initialized display object
            display_info = {
                .type = DisplayType::DISP_M5STICKC_PLUS2,
                .category = DisplayCategory::CAT_SMALL,
                .width = 135, .height = 240,
                .has_touch = false, .has_axp192 = false,
                .tilts_per_page = 5,
                .hardware_version = "M5StickC Plus2",
            };
            return;
        }
        delete m5;
    }

    // ---- Probe 6: SSD1306 OLED on I2C ----
    if (i2c_device_at_address(0x3C, 5, 4) ||
        i2c_device_at_address(0x3C, 21, 22) ||
        i2c_device_at_address(0x3C, 4, 15) ||
        i2c_device_at_address(0x3C, 17, 18)) {
        ESP_LOGW("DETECT", "Found SSD1306 OLED on I2C");
        display_info = {
            .type = DisplayType::DISP_SSD1306,
            .category = DisplayCategory::CAT_SMALL,
            .width = 128, .height = 64,
            .has_touch = false, .has_axp192 = false,
            .tilts_per_page = 5,
            .hardware_version = "OLED SSD1306",
        };
        return;
    }

    // ---- Nothing found: headless ----
    ESP_LOGW("DETECT", "No display found -> headless mode");
    display_info = {
        .type = DisplayType::DISP_NONE,
        .category = DisplayCategory::CAT_NONE,
        .width = 0, .height = 0,
        .has_touch = false, .has_axp192 = false,
        .tilts_per_page = 1,
        .hardware_version = "Headless",
    };
}
#endif // UNIVERSAL_BUILD


////////////////////////////////////////////////////////////
// Public Methods
////////////////////////////////////////////////////////////


inline void bridge_lcd::init_power() {
#ifdef LCD_TFT_M5STICKC
    m5_variant = detect_m5_variant();

    if (m5_variant == M5Variant::Plus) {
        // M5StickC Plus: Initialize AXP192 for power/backlight
        AXP192_InitDef initDef = {
            .EXTEN  = true,
            .BACKUP = true,
            .DCDC1  = 3300,
            .DCDC2  = 0,
            .DCDC3  = 0,
            .LDO2   = 3000,
            .LDO3   = 3000,
            .GPIO0  = 2800,
            .GPIO1  = -1,
            .GPIO2  = -1,
            .GPIO3  = -1,
            .GPIO4  = -1,
        };
        axp192_driver.begin(21, 22, initDef);
    } else {
        // M5StickC Plus2: Set HOLD pin (GPIO4) HIGH to maintain power
        // Without this, the device may shut down on battery - see M5Stack docs
        // n.b - I ended up commenting this out as there was a weird 'clicking" noise when turning 
        //       off the device via the button when running on battery. I think the fix is to capture 
        //       the button press (GPIO 35?) and then set GPIO4 to low, but am fine with just disabling
        //       battery operation for now (which is what happens when these are commented out)
        //pinMode(4, OUTPUT);
        //digitalWrite(4, HIGH);

        // Turn on backlight via GPIO27
        pinMode(27, OUTPUT);
        digitalWrite(27, HIGH);
    }
#elif defined(PIN_POWER_ON)
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
#endif
}

void bridge_lcd::init() {
    init_power();

#ifdef LCD_SSD1306
    // For the OLED displays, we need to initialize the I2C bus -- but first, we need to figure out what pins to use
    int sda_pin = -1, scl_pin = -1;
    int reset_pin = -1;

#ifdef I2C_SDA_PIN
    // The user is explicitly supplying the SDA and SCL pins
#ifndef I2C_SCL_PIN
#error "If you define I2C_SDA_PIN, you must also define I2C_SCL_PIN"
#endif
    // If the user explicitly supplies an SDA/SCL pin, we'll use that
    sda_pin = I2C_SDA_PIN;
    scl_pin = I2C_SCL_PIN;

#ifdef I2C_RESET_PIN
    reset_pin = I2C_RESET_PIN;
    pinMode(I2C_RESET_PIN, OUTPUT);
    // Apparently for the Heltec boards you have to do this twice. Go figure.
    digitalWrite(I2C_RESET_PIN, LOW); // Set I2C_RESET_PIN low to reset OLED
    vTaskDelay(pdMS_TO_TICKS(200));
    digitalWrite(I2C_RESET_PIN, HIGH); // While OLED is running, must set I2C_RESET_PIN in high
    vTaskDelay(pdMS_TO_TICKS(200));
    digitalWrite(I2C_RESET_PIN, LOW); // Set I2C_RESET_PIN low to reset OLED
    vTaskDelay(pdMS_TO_TICKS(200));
    digitalWrite(I2C_RESET_PIN, HIGH); // While OLED is running, must set I2C_RESET_PIN in high
#endif

#else

    // We're currently supporting three sets of hardware - The ESP32 "OLED"
    // board, TTGO Boards, and the sleeve (which I think nobody uses)
    if (i2c_device_at_address(0x3c, 5, 4)) {
        // This is the ESP32 "OLED" board
        sda_pin = 5;
        scl_pin = 4;
    } else if (i2c_device_at_address(0x3c, 21, 22)) {
        // This is the "sleeve": address, SDA, SCK
        sda_pin = 21;
        scl_pin = 22;
    } else {
        // For the "TTGO" style OLED shields, you have to power a pin to run the backlight.
        pinMode(16, OUTPUT);
        digitalWrite(16, LOW); // Set GPIO16 low to reset OLED
        vTaskDelay(pdMS_TO_TICKS(50));
        digitalWrite(16, HIGH); // While OLED is running, must set GPIO16 in high
        if (i2c_device_at_address(0x3c, 4, 15)) {
            sda_pin = 4;
            scl_pin = 15;
        } else {
            digitalWrite(16, LOW);                    // We weren't able to find the TTGO board, so reset the pin

            pinMode(21, OUTPUT);
            digitalWrite(21, LOW); // Set GPIO21 low to reset OLED
            vTaskDelay(pdMS_TO_TICKS(50));
            digitalWrite(21, HIGH); // While OLED is running, must set GPIO21 in high
            if (i2c_device_at_address(0x3c, 17, 18)) {
                sda_pin = 17;
                scl_pin = 18;
            } else {
                digitalWrite(21, LOW);                    // We weren't able to find the TTGO board, so reset the pin
                // ... and just default to the "sleeve" configuration
                sda_pin = 21;
                scl_pin = 22;
            }
        }
    }
#endif

    // Create and configure the LovyanGFX SSD1306 display
    auto ssd1306_tft = new LGFX_SSD1306();
    ssd1306_tft->configure(sda_pin, scl_pin, 0x3C, reset_pin);
    tft = ssd1306_tft;

    tft->init();
    if(!config.invertTFT) {
        // Due to historical reasons, the "non-inverted" orientation is technically the one that has had
        // flipScreenVertically() called.
        tft->setRotation(2);
    } else {
        // config.invertTFT is set. Toggle the semaphore.
        tft->setRotation(0);
    }


#elif defined(LCD_TFT_ESPI) || defined(LCD_TFT)
    // Initialize appropriate LovyanGFX configuration based on hardware
#if defined(LCD_TFT) && defined(CYD)
    {
        auto cyd_tft = new LGFX_CYD();
        cyd_tft->configure();
        tft = cyd_tft;
    }
#elif defined(LCD_TFT)
    tft = new LGFX_D32_Pro();
#elif defined(LCD_TFT_M5STICKC)
    {
        auto m5_tft = new LGFX_M5StickC();
        m5_tft->configure(m5_variant == M5Variant::Plus2);
        tft = m5_tft;
    }
#elif defined(ESP32S3)
    tft = new LGFX_S3_TDisplay();
    // tft = new LGFX();
#else
    tft = new LGFX_TFT_ESPI();
#endif
    
    tft->init();
    tft->setSwapBytes(true);
    reinit();

    tft->setFont(&FreeSans9pt7b);

#ifdef TFT_BACKLIGHT
    pinMode(TFT_BACKLIGHT, OUTPUT);
    digitalWrite(TFT_BACKLIGHT, HIGH);
#endif // TFT_BACKLIGHT

#elif defined(UNIVERSAL_BUILD)
    // ---- Universal build: detect display at runtime ----
    detect_display();
    ESP_LOGW("DETECT", "Display type: %s (%dx%d)", display_info.hardware_version,
             display_info.width, display_info.height);

    switch (display_info.type) {
    case DisplayType::DISP_CYD: {
        auto cyd_tft = new LGFX_CYD();
        cyd_tft->configure();
        tft = cyd_tft;
        tft->init();
        tft->setSwapBytes(true);
        reinit();
        tft->setFont(&FreeSans9pt7b);
        break;
    }
    case DisplayType::DISP_D32_PRO: {
        if (!tft) {
            tft = new LGFX_D32_Pro();
        }
        tft->init();
        // D32 Pro TFT shield backlight on GPIO 32
        pinMode(32, OUTPUT);
        digitalWrite(32, HIGH);
        tft->setSwapBytes(true);
        reinit();
        tft->setFont(&FreeSans9pt7b);
        break;
    }
    case DisplayType::DISP_M5STICKC_PLUS: {
        // Init AXP192 power
        AXP192_InitDef initDef = {
            .EXTEN = true, .BACKUP = true,
            .DCDC1 = 3300, .DCDC2 = 0, .DCDC3 = 0,
            .LDO2 = 3000, .LDO3 = 3000, .GPIO0 = 2800,
            .GPIO1 = -1, .GPIO2 = -1, .GPIO3 = -1, .GPIO4 = -1,
        };
        axp192_driver.begin(21, 22, initDef);
        auto m5_tft = new LGFX_M5StickC();
        m5_tft->configure(false);  // Plus (not Plus2)
        tft = m5_tft;
        tft->init();
        tft->setSwapBytes(true);
        reinit();
        tft->setFont(&FreeSans9pt7b);
        break;
    }
    case DisplayType::DISP_M5STICKC_PLUS2: {
        pinMode(27, OUTPUT);
        digitalWrite(27, HIGH);
        if (!tft) {
            auto m5_tft = new LGFX_M5StickC();
            m5_tft->configure(true);  // Plus2
            tft = m5_tft;
            tft->init();
        }
        tft->setSwapBytes(true);
        reinit();
        tft->setFont(&FreeSans9pt7b);
        break;
    }
    case DisplayType::DISP_TTGO_TFT: {
        if (!tft) {
            tft = new LGFX_TFT_ESPI();
            tft->init();
        }
        tft->setSwapBytes(true);
        reinit();
        tft->setFont(&FreeSans9pt7b);
        break;
    }
    case DisplayType::DISP_SSD1306: {
        // Probe again to find the right I2C pins
        int sda_pin = 21, scl_pin = 22;  // Default
        if (i2c_device_at_address(0x3C, 5, 4)) { sda_pin = 5; scl_pin = 4; }
        else if (i2c_device_at_address(0x3C, 21, 22)) { sda_pin = 21; scl_pin = 22; }
        else if (i2c_device_at_address(0x3C, 4, 15)) {
            pinMode(16, OUTPUT); digitalWrite(16, HIGH);
            sda_pin = 4; scl_pin = 15;
        }
        else if (i2c_device_at_address(0x3C, 17, 18)) {
            pinMode(21, OUTPUT); digitalWrite(21, HIGH);
            sda_pin = 17; scl_pin = 18;
        }
        auto ssd1306_tft = new LGFX_SSD1306();
        ssd1306_tft->configure(sda_pin, scl_pin);
        tft = ssd1306_tft;
        tft->init();
        if (!config.invertTFT) tft->setRotation(2);
        else tft->setRotation(0);
        break;
    }
    case DisplayType::DISP_NONE:
    default:
        ESP_LOGW("DETECT", "Running in headless mode");
        tft = nullptr;
        break;
    }

#endif // LCD_TFT_ESPI / UNIVERSAL_BUILD
}

void bridge_lcd::reinit() {
#if defined(LCD_TFT) || defined(LCD_TFT_ESPI) || defined(UNIVERSAL_BUILD)
    clear();
#if defined(UNIVERSAL_BUILD)
    if (display_info.type == DisplayType::DISP_SSD1306) {
        tft->setRotation(config.invertTFT ? 0 : 2);
    } else {
        tft->setRotation(config.invertTFT ? 1 : 3);
    }
#else
    if (config.invertTFT) {
        tft->setRotation(1);
    } else {
        tft->setRotation(3);
    }
#endif
#elif defined (LCD_SSD1306)
    // We can only flip the screen, not determine the current orientation
    if(config.invertTFT) {
        tft->setRotation(0);
    } else {
        tft->setRotation(2);
    }
#endif
}


void bridge_lcd::checkTouch()
{
#ifdef TOUCH_CS

    uint16_t x = 0, y = 0; // Touch coordinates (not used here)
    bool touched = tft->getTouch(&x, &y);

    if (touched && ! touchLatch && ! setWiFiPushed) {
        // New touch, not currently waiting to process a touch elsewhere
        touchLatch = true;
    } else if (touched && touchLatch) {
        // Same touch, do nothing
    } else if (! touched && touchLatch) {
        // Clear touchlatch, trigger a tap
        touchLatch = false;
        setWiFiPushed = true;
    } else {
        // On this day in history, nothing happened
        touchLatch = false;
    }
#endif
}


void bridge_lcd::print_line(const char *left_text, uint8_t line)
{
#if defined(LCD_TFT_ESPI)
    print_line("", left_text, "", line);
#else
    print_line(left_text, "", "", line);
#endif
}

void bridge_lcd::print_line(const char *left_text, const char *right_text, uint8_t line) {
#if defined(LCD_TFT_ESPI)
    print_line("", left_text, right_text, line);
#else
    print_line(left_text, "", right_text, line);
#endif
}

void bridge_lcd::print_line(const char *left_text, const char *middle_text, const char *right_text, uint8_t line) {
    print_line(left_text, middle_text, right_text, line, false);
}

void bridge_lcd::print_line(const char *left_text, const char *middle_text, const char *right_text, uint8_t line, bool add_gutter) {
#ifdef LCD_SSD1306
    int16_t starting_pixel_row = 0;

    starting_pixel_row = (SSD_LINE_CLEARANCE + SSD1306_FONT_HEIGHT) * (line - 1) + SSD_LINE_CLEARANCE;

    // The coordinates define the left starting point of the text
    tft->setTextDatum(textdatum_t::top_left);
    tft->drawString(left_text, 0, starting_pixel_row);

    tft->setTextDatum(textdatum_t::top_left);
    tft->drawString(middle_text, 48, starting_pixel_row);

    tft->setTextDatum(textdatum_t::top_right);
    tft->drawString(right_text, 128, starting_pixel_row);
#elif defined(UNIVERSAL_BUILD)
    int16_t starting_pixel_row = 0;
    if (display_info.type == DisplayType::DISP_SSD1306) {
        // SSD1306 OLED: 128x64, small text layout
        starting_pixel_row = (SSD_LINE_CLEARANCE + SSD1306_FONT_HEIGHT) * (line - 1) + SSD_LINE_CLEARANCE;
        tft->setTextDatum(textdatum_t::top_left);
        tft->drawString(left_text, 0, starting_pixel_row);
        tft->setTextDatum(textdatum_t::top_left);
        tft->drawString(middle_text, 48, starting_pixel_row);
        tft->setTextDatum(textdatum_t::top_right);
        tft->drawString(right_text, 128, starting_pixel_row);
    } else {
        // TFT displays: 240x320 or 135x240
        starting_pixel_row = (tft->fontHeight()) * (line - 1) + 2;
        if(add_gutter)
            tft->drawString(left_text, 25, starting_pixel_row);
        else
            tft->drawString(left_text, 1, starting_pixel_row);
        yield();
        tft->drawString(middle_text, 134, starting_pixel_row);
        yield();
        if(add_gutter)
            tft->drawString(right_text, 300 - tft->textWidth(right_text), starting_pixel_row);
        else
            tft->drawString(right_text, tft->width() - 1 - tft->textWidth(right_text), starting_pixel_row);
    }
#elif defined(LCD_TFT)
    int16_t starting_pixel_row = 0;
    starting_pixel_row = (tft->fontHeight()) * (line - 1) + 2;

    if(add_gutter)  // We need space to the left to be able to display the Tilt color block
        tft->drawString(left_text, 25, starting_pixel_row);
    else
        tft->drawString(left_text, 1, starting_pixel_row);

    yield();
    tft->drawString(middle_text, 134, starting_pixel_row);
    yield();
    if(add_gutter)
        tft->drawString(right_text, 300 - tft->textWidth(right_text), starting_pixel_row);
    else
        tft->drawString(right_text, 319 - tft->textWidth(right_text), starting_pixel_row);
#elif defined(LCD_TFT_ESPI)
    // ignore left text as we color the text by the tilt
    int16_t starting_pixel_row = 0;

    starting_pixel_row = (TFT_ESPI_LINE_CLEARANCE + TFT_ESPI_FONT_SIZE) * (line - 1) + TFT_ESPI_LINE_CLEARANCE;

    // LovyanGFX::drawString(const char *string, int32_t poX, int32_t poY)
    // TODO - Replace middle_text with left_text (and skip all middle text instead)
    tft->drawString(middle_text, 0, starting_pixel_row);
    tft->drawString(right_text, tft->width() / 2, starting_pixel_row);
#endif
}


void bridge_lcd::clear() {
#if defined(LCD_SSD1306) || defined(LCD_TFT) || defined(LCD_TFT_ESPI) || defined(UNIVERSAL_BUILD)
    tft->fillScreen(0x0000);  // Black
#endif
    yield();
}


////////////////////////////////////////////////////////////
// Private Methods
////////////////////////////////////////////////////////////

void bridge_lcd::print_tilt_to_line(tiltHydrometer *tilt, uint8_t line) {
    char gravity[11], temp[9], temp_str[6];
    tilt->cal_smooth_gravity_str(gravity, 11);
    tilt->converted_temp(temp_str, 6, false);
    snprintf(temp, sizeof(temp), "%s %s", temp_str, tilt->is_celsius() ? "C" : "F");

#if defined(LCD_TFT_ESPI)
    tft->setTextColor(tilt_text_colors[tilt->m_color]);
#endif

    // Print line with gutter for the color block for TFT screens
    print_line(tilt_color_names[tilt->m_color], temp, gravity, line, true);

#if defined(LCD_TFT) || defined(UNIVERSAL_BUILD)
    uint16_t fHeight = tft->fontHeight();
    if (tilt_text_colors[tilt->m_color] == 0xFFFF) { // White outline, black square
        tft->fillRect( // White square
            0,
            fHeight * (line - 1) + 2,
            15,
            fHeight - 8,
            0xFFFF);  // White in RGB565
        tft->fillRect( // Black square
            1,
            fHeight * (line - 1) + 3,
            13,
            fHeight - 10,
            0x0000);  // Black in RGB565
    } else {
        // All else
        tft->fillRect(
            0,
            fHeight * (line - 1) + 2,
            15,
            fHeight - 8,
            tilt_text_colors[tilt->m_color]);
    }
#elif defined(LCD_TFT_ESPI)
    tft->setTextColor(0xFFFF);  // White in RGB565
#endif
}

bool bridge_lcd::i2c_device_at_address(uint8_t address, int sda_pin, int scl_pin) {
#if defined(LCD_SSD1306) || defined(UNIVERSAL_BUILD)
    // LCD autodetection using the new ESP-IDF 5.x I2C master driver API
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)sda_pin,
        .scl_io_num = (gpio_num_t)scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    i2c_master_bus_handle_t bus_handle;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK) {
        Log.error("Failed to create I2C bus on pin %d/%d\r\n", sda_pin, scl_pin);
        return false;
    }

    err = i2c_master_probe(bus_handle, address, pdMS_TO_TICKS(100));
    i2c_del_master_bus(bus_handle);

    if (err == ESP_OK)
        return true;
#endif
    return false;
}

void bridge_lcd::display() {
#ifdef LCD_SSD1306
    // LovyanGFX auto-flushes, no explicit display() call needed
#endif
}


void bridge_lcd::display_logo_internal() {
#ifdef LCD_SSD1306
    tft->drawXBitmap(
        (128 - oled_logo_width) / 2,
        (64 - oled_logo_height) / 2,
        oled_logo_bits,
        oled_logo_width,
        oled_logo_height,
        0xFFFF);  // White in monochrome
    display();
#elif defined(LCD_TFT)
    tft->pushImage(
        (320 - 288) / 2, 0,
        gimp_image.width,
        gimp_image.height,
        gimp_image.pixel_data);
#elif defined(UNIVERSAL_BUILD)
    if (display_info.category == DisplayCategory::CAT_LARGE) {
        // pushImage with setSwapBytes(true) crashes on universal build (DMA issue),
        // so we use pre-swapped image data with setSwapBytes(false).
        tft->setSwapBytes(false);
        tft->pushImage(
            (tft->width() - gimp_image_swapped.width) / 2, 0,
            gimp_image_swapped.width,
            gimp_image_swapped.height,
            (const uint16_t*)gimp_image_swapped.pixel_data);
        tft->setSwapBytes(true);
    } else {
        // Small displays: use XBitmap logo
        tft->drawXBitmap(
            (tft->width() - oled_logo_width) / 2,
            (tft->height() - oled_logo_height) / 2,
            oled_logo_bits,
            oled_logo_width,
            oled_logo_height,
            0xFFFF);
    }
#elif defined(LCD_TFT_ESPI)
    tft->drawXBitmap(
        (tft->width() - oled_logo_width) / 2,
        (tft->height() - oled_logo_height) / 2,
        oled_logo_bits,
        oled_logo_width,
        oled_logo_height,
        0xFFFF);  // White in RGB565
    display();
#endif
}

