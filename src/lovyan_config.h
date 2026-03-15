#ifndef TILTBRIDGE_LOVYAN_CONFIG_H
#define TILTBRIDGE_LOVYAN_CONFIG_H

#include <LovyanGFX.hpp>
#include <esp_log.h>

#if defined(LCD_SSD1306)
// Configuration class for SSD1306 OLED displays (128x64)
class LGFX_SSD1306 : public lgfx::LGFX_Device
{
    lgfx::Panel_SSD1306 _panel_instance;
    lgfx::Bus_I2C _bus_instance;

public:
    LGFX_SSD1306(void) {}  // Empty constructor - configure() does the work

    void configure(int sda_pin, int scl_pin, uint8_t i2c_addr = 0x3C, int reset_pin = -1) {
        {
            auto cfg = _bus_instance.config();
            cfg.i2c_port = 0;  // Use I2C port 0
            cfg.freq_write = 400000;  // 400kHz I2C
            cfg.freq_read = 400000;
            cfg.pin_sda = sda_pin;
            cfg.pin_scl = scl_pin;
            cfg.i2c_addr = i2c_addr;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = -1;  // SSD1306 doesn't use CS
            cfg.pin_rst = reset_pin;
            cfg.pin_busy = -1;
            cfg.panel_width = 128;
            cfg.panel_height = 64;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = false;
            cfg.invert = false;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel_instance.config(cfg);
        }

        setPanel(&_panel_instance);
    }
};

#elif defined(LCD_TFT) && defined(CYD)
// Universal configuration class for all CYD (Cheap Yellow Display) variants
// Supports: ESP32-2432S024, ESP32-2432S028 (v1/v2/v3), ESP32-2432S032
// Auto-detects display driver (ILI9341, ILI9342, ST7789) and backlight pin at runtime
class LGFX_CYD : public lgfx::LGFX_Device
{
    // All three panel types — only one is used at runtime
    lgfx::Panel_ILI9341 _panel_ili9341;
    lgfx::Panel_ILI9342 _panel_ili9342;
    lgfx::Panel_ST7789  _panel_st7789;
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;
    lgfx::Touch_XPT2046 _touch_instance;

    // Read display register via SPI
    static uint32_t _read_cmd(lgfx::IBus* bus, int32_t pin_cs, uint8_t cmd, uint8_t dummy_bits = 1)
    {
        bus->beginTransaction();
        gpio_set_level((gpio_num_t)pin_cs, 1);
        bus->writeCommand(0, 8);  // NOP to sync
        bus->wait();
        gpio_set_level((gpio_num_t)pin_cs, 0);
        bus->writeCommand(cmd, 8);
        bus->beginRead(dummy_bits);
        uint32_t res = 0;
        for (int i = 0; i < 4; ++i) {
            res |= (bus->readData(8) & 0xFF) << (i * 8);
        }
        bus->endTransaction();
        gpio_set_level((gpio_num_t)pin_cs, 1);
        return res;
    }

public:
    LGFX_CYD(void) {}  // Empty — configure() does the work

    void configure()
    {
        // Step 1: Configure SPI bus (identical across all CYD variants)
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = HSPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 14;
            cfg.pin_mosi = 13;
            cfg.pin_miso = 12;
            cfg.pin_dc = 2;
            _bus_instance.config(cfg);
        }

        // Configure CS pin for ID read
        gpio_config_t cs_conf = {
            .pin_bit_mask = (1ULL << 15),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cs_conf);

        // Step 2: Read display ID to detect panel type
        // Use spi_3wire=false for the probe so MISO (GPIO12) is used for reading
        {
            auto cfg = _bus_instance.config();
            cfg.spi_3wire = false;
            _bus_instance.config(cfg);
        }
        _bus_instance.init();
        uint32_t id04_d1 = _read_cmd(&_bus_instance, 15, 0x04, 1);  // RDDID, 1 dummy bit
        uint32_t id04_d0 = _read_cmd(&_bus_instance, 15, 0x04, 0);  // RDDID, 0 dummy bits
        uint32_t id09    = _read_cmd(&_bus_instance, 15, 0x09, 1);  // RDDST (display status)
        uint32_t idDA    = _read_cmd(&_bus_instance, 15, 0xDA, 0);  // Read ID1
        uint32_t idDB    = _read_cmd(&_bus_instance, 15, 0xDB, 0);  // Read ID2
        uint32_t idDC    = _read_cmd(&_bus_instance, 15, 0xDC, 0);  // Read ID3
        _bus_instance.release();
        // Restore spi_3wire for normal operation
        {
            auto cfg = _bus_instance.config();
            cfg.spi_3wire = true;
            _bus_instance.config(cfg);
        }

        ESP_LOGW("CYD", "ID04(d1)=0x%08X ID04(d0)=0x%08X ID09=0x%08X",
                 (unsigned)id04_d1, (unsigned)id04_d0, (unsigned)id09);
        ESP_LOGW("CYD", "IDDA=0x%02X IDDB=0x%02X IDDC=0x%02X",
                 (unsigned)(idDA & 0xFF), (unsigned)(idDB & 0xFF), (unsigned)(idDC & 0xFF));

        // Determine panel type from available IDs
        // ST7789: RDDID typically returns 0x85, 0x85, 0x52 or similar
        // ILI9341: RDDID returns 0x00, 0x93, 0x41
        // ILI9342: RDDID returns 0x00, 0x93, 0xE3 (or first byte 0xE3)
        uint8_t id_byte = id04_d1 & 0xFF;
        // Also check with 0 dummy bits in case the display needs that
        uint8_t id_byte_d0 = id04_d0 & 0xFF;
        // And individual ID registers (0xDA/DB/DC work on ST7789 even when 0x04 doesn't)
        uint8_t id1 = idDA & 0xFF;
        uint8_t id2 = idDB & 0xFF;
        uint8_t id3 = idDC & 0xFF;

        // Step 3: Select the correct panel based on ID
        lgfx::Panel_LCD* panel = nullptr;
        bool need_invert = false;
        int backlight_pin = 21;  // Default for original 2.8" CYD

        // If display status (0x09) is non-zero but RDDID is zero, it helps disambiguate
        bool status_nonzero = (id09 & 0xFFFFFF) != 0;
        // ST7789 IDs vary by manufacturer: 0x85, 0x81, or shifted variants
        bool is_st7789 = (id_byte == 0x85 || id_byte_d0 == 0x85 ||
                          id_byte == 0x81 || id_byte_d0 == 0x81 ||
                          id2 == 0x85 || id2 == 0x81 || id3 == 0x52);
        bool is_ili9342 = (id_byte == 0xE3 || id_byte_d0 == 0xE3);
        bool is_ili9341 = (id_byte == 0x93 || id_byte_d0 == 0x93 ||
                           ((id_byte == 0x00) && status_nonzero));

        if (is_st7789) {
            panel = &_panel_st7789;
            need_invert = true;
            backlight_pin = 27;
            ESP_LOGW("CYD", "Detected ST7789 display, backlight=GPIO%d", backlight_pin);
        } else if (is_ili9342) {
            panel = &_panel_ili9342;
            need_invert = false;
            backlight_pin = 21;
            ESP_LOGW("CYD", "Detected ILI9342 display, backlight=GPIO%d", backlight_pin);
        } else if (is_ili9341) {
            // ILI9341 — original 2.8" v1
            panel = &_panel_ili9341;
            need_invert = false;
            backlight_pin = 21;
            ESP_LOGW("CYD", "Detected ILI9341 display, backlight=GPIO%d", backlight_pin);
        } else {
            // Unknown ID — default to ST7789 (most common on newer CYD boards)
            panel = &_panel_st7789;
            need_invert = true;
            backlight_pin = 27;
            ESP_LOGW("CYD", "Unknown display ID (0x%02X), defaulting to ST7789, backlight=GPIO%d", id_byte, backlight_pin);
        }

        panel->setBus(&_bus_instance);

        // Step 4: Configure panel (shared settings)
        {
            auto cfg = panel->config();
            cfg.pin_cs = 15;
            cfg.pin_rst = -1;
            cfg.pin_busy = -1;
            cfg.panel_width = 240;
            cfg.panel_height = 320;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = true;
            cfg.invert = need_invert;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            panel->config(cfg);
        }

        // Step 5: Configure backlight
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = backlight_pin;
            cfg.invert = false;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            panel->setLight(&_light_instance);
        }

        // Step 6: Configure touch (XPT2046, identical across all CYD R variants)
        {
            auto cfg = _touch_instance.config();
            cfg.x_min = 0;
            cfg.x_max = 239;
            cfg.y_min = 0;
            cfg.y_max = 319;
            cfg.pin_int = 36;
            cfg.bus_shared = false;
            cfg.offset_rotation = 0;
            cfg.spi_host = VSPI_HOST;
            cfg.freq = 1000000;
            cfg.pin_sclk = 25;
            cfg.pin_mosi = 32;
            cfg.pin_miso = 39;
            cfg.pin_cs = 33;
            _touch_instance.config(cfg);
            panel->setTouch(&_touch_instance);
        }

        setPanel(panel);
    }
};

#elif defined(LCD_TFT)
// Configuration class for D32 Pro TFT (ILI9341)
class LGFX_D32_Pro : public lgfx::LGFX_Device
{
    lgfx::Panel_ILI9341 _panel_instance;
    lgfx::Bus_SPI _bus_instance;

public:
    LGFX_D32_Pro(void)
    {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = VSPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 18;
            cfg.pin_mosi = 23;
            cfg.pin_miso = 19;
            cfg.pin_dc = 27;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = 14;
            cfg.pin_rst = 33;
            cfg.pin_busy = -1;
            cfg.panel_width = 240;
            cfg.panel_height = 320;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = true;
            cfg.invert = false;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = true;
            _panel_instance.config(cfg);
        }

        setPanel(&_panel_instance);
    }
};

#elif defined(LCD_TFT_M5STICKC)
// Configuration class for M5StickC Plus and Plus2
// Plus: AXP192 power management, DC=23, RST=18, backlight via AXP192
// Plus2: No AXP192, DC=14, RST=12, backlight via GPIO27 PWM
class LGFX_M5StickC : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 _panel_instance;
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;

public:
    LGFX_M5StickC(void) {}  // Empty constructor - configure() does the work

    void configure(bool isPlus2) {
        {
            auto cfg = _bus_instance.config();
            // Plus2 uses HSPI, Plus uses VSPI
            cfg.spi_host = isPlus2 ? HSPI_HOST : VSPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = isPlus2 ? 15000000 : 16000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 13;
            cfg.pin_mosi = 15;
            cfg.pin_miso = -1;
            cfg.pin_dc = isPlus2 ? 14 : 23;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = 5;
            cfg.pin_rst = isPlus2 ? 12 : 18;
            cfg.pin_busy = -1;
            cfg.panel_width = 135;
            cfg.panel_height = 240;
            cfg.offset_x = 52;
            cfg.offset_y = 40;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = true;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = true;
            _panel_instance.config(cfg);
        }

        if (isPlus2) {
            auto lcfg = _light_instance.config();
            lcfg.pin_bl = 27;
            lcfg.invert = false;
            lcfg.freq = 256;  // M5GFX uses 256 Hz for Plus2
            lcfg.pwm_channel = 7;
            _light_instance.config(lcfg);
            _panel_instance.setLight(&_light_instance);
        }
        // Plus: No Light binding - AXP192 handles backlight

        setPanel(&_panel_instance);
    }
};

#elif defined(UNIVERSAL_BUILD)
// ---- Universal build: all ESP32 display classes available ----

// SSD1306 OLED (128x64, I2C)
class LGFX_SSD1306 : public lgfx::LGFX_Device
{
    lgfx::Panel_SSD1306 _panel_instance;
    lgfx::Bus_I2C _bus_instance;

public:
    LGFX_SSD1306(void) {}
    void configure(int sda_pin, int scl_pin, uint8_t i2c_addr = 0x3C, int reset_pin = -1) {
        {
            auto cfg = _bus_instance.config();
            cfg.i2c_port = 0;
            cfg.freq_write = 400000;
            cfg.freq_read = 400000;
            cfg.pin_sda = sda_pin;
            cfg.pin_scl = scl_pin;
            cfg.i2c_addr = i2c_addr;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = -1;
            cfg.pin_rst = reset_pin;
            cfg.pin_busy = -1;
            cfg.panel_width = 128;
            cfg.panel_height = 64;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = false;
            cfg.invert = false;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel_instance.config(cfg);
        }
        setPanel(&_panel_instance);
    }
};

// CYD universal (240x320, HSPI, auto-detects ILI9341/ILI9342/ST7789)
// (LGFX_CYD class is defined above in the LCD_TFT && CYD section)
// For universal build, we duplicate it here:
class LGFX_CYD : public lgfx::LGFX_Device
{
    lgfx::Panel_ILI9341 _panel_ili9341;
    lgfx::Panel_ILI9342 _panel_ili9342;
    lgfx::Panel_ST7789  _panel_st7789;
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;
    lgfx::Touch_XPT2046 _touch_instance;

    static uint32_t _read_cmd(lgfx::IBus* bus, int32_t pin_cs, uint8_t cmd, uint8_t dummy_bits = 1)
    {
        bus->beginTransaction();
        gpio_set_level((gpio_num_t)pin_cs, 1);
        bus->writeCommand(0, 8);
        bus->wait();
        gpio_set_level((gpio_num_t)pin_cs, 0);
        bus->writeCommand(cmd, 8);
        bus->beginRead(dummy_bits);
        uint32_t res = 0;
        for (int i = 0; i < 4; ++i) {
            res |= (bus->readData(8) & 0xFF) << (i * 8);
        }
        bus->endTransaction();
        gpio_set_level((gpio_num_t)pin_cs, 1);
        return res;
    }

public:
    LGFX_CYD(void) {}
    void configure()
    {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = HSPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 14;
            cfg.pin_mosi = 13;
            cfg.pin_miso = 12;
            cfg.pin_dc = 2;
            _bus_instance.config(cfg);
        }
        gpio_config_t cs_conf = {
            .pin_bit_mask = (1ULL << 15),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cs_conf);
        {
            auto cfg = _bus_instance.config();
            cfg.spi_3wire = false;
            _bus_instance.config(cfg);
        }
        _bus_instance.init();
        uint32_t id04_d1 = _read_cmd(&_bus_instance, 15, 0x04, 1);
        uint32_t id04_d0 = _read_cmd(&_bus_instance, 15, 0x04, 0);
        uint32_t id09    = _read_cmd(&_bus_instance, 15, 0x09, 1);
        uint32_t idDA    = _read_cmd(&_bus_instance, 15, 0xDA, 0);
        uint32_t idDB    = _read_cmd(&_bus_instance, 15, 0xDB, 0);
        uint32_t idDC    = _read_cmd(&_bus_instance, 15, 0xDC, 0);
        _bus_instance.release();
        {
            auto cfg = _bus_instance.config();
            cfg.spi_3wire = true;
            _bus_instance.config(cfg);
        }
        ESP_LOGW("CYD", "ID04(d1)=0x%08X ID04(d0)=0x%08X ID09=0x%08X",
                 (unsigned)id04_d1, (unsigned)id04_d0, (unsigned)id09);
        ESP_LOGW("CYD", "IDDA=0x%02X IDDB=0x%02X IDDC=0x%02X",
                 (unsigned)(idDA & 0xFF), (unsigned)(idDB & 0xFF), (unsigned)(idDC & 0xFF));
        uint8_t id_byte = id04_d1 & 0xFF;
        uint8_t id_byte_d0 = id04_d0 & 0xFF;
        uint8_t id2 = idDB & 0xFF;
        uint8_t id3 = idDC & 0xFF;
        lgfx::Panel_LCD* panel = nullptr;
        bool need_invert = false;
        int backlight_pin = 21;
        bool status_nonzero = (id09 & 0xFFFFFF) != 0;
        bool is_st7789 = (id_byte == 0x85 || id_byte_d0 == 0x85 ||
                          id_byte == 0x81 || id_byte_d0 == 0x81 ||
                          id2 == 0x85 || id2 == 0x81 || id3 == 0x52);
        bool is_ili9342 = (id_byte == 0xE3 || id_byte_d0 == 0xE3);
        bool is_ili9341 = (id_byte == 0x93 || id_byte_d0 == 0x93 ||
                           ((id_byte == 0x00) && status_nonzero));
        if (is_st7789) {
            panel = &_panel_st7789; need_invert = true; backlight_pin = 27;
            ESP_LOGW("CYD", "Detected ST7789 display, backlight=GPIO%d", backlight_pin);
        } else if (is_ili9342) {
            panel = &_panel_ili9342; need_invert = false; backlight_pin = 21;
            ESP_LOGW("CYD", "Detected ILI9342 display, backlight=GPIO%d", backlight_pin);
        } else if (is_ili9341) {
            panel = &_panel_ili9341; need_invert = false; backlight_pin = 21;
            ESP_LOGW("CYD", "Detected ILI9341 display, backlight=GPIO%d", backlight_pin);
        } else {
            panel = &_panel_st7789; need_invert = true; backlight_pin = 27;
            ESP_LOGW("CYD", "Unknown display ID (0x%02X), defaulting to ST7789, backlight=GPIO%d", id_byte, backlight_pin);
        }
        panel->setBus(&_bus_instance);
        {
            auto cfg = panel->config();
            cfg.pin_cs = 15; cfg.pin_rst = -1; cfg.pin_busy = -1;
            cfg.panel_width = 240; cfg.panel_height = 320;
            cfg.offset_x = 0; cfg.offset_y = 0; cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8; cfg.dummy_read_bits = 1;
            cfg.readable = true; cfg.invert = need_invert;
            cfg.rgb_order = false; cfg.dlen_16bit = false; cfg.bus_shared = false;
            panel->config(cfg);
        }
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = backlight_pin; cfg.invert = false;
            cfg.freq = 44100; cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            panel->setLight(&_light_instance);
        }
        {
            auto cfg = _touch_instance.config();
            cfg.x_min = 0; cfg.x_max = 239; cfg.y_min = 0; cfg.y_max = 319;
            cfg.pin_int = 36; cfg.bus_shared = false; cfg.offset_rotation = 0;
            cfg.spi_host = VSPI_HOST; cfg.freq = 1000000;
            cfg.pin_sclk = 25; cfg.pin_mosi = 32; cfg.pin_miso = 39; cfg.pin_cs = 33;
            _touch_instance.config(cfg);
            panel->setTouch(&_touch_instance);
        }
        setPanel(panel);
    }
};

// D32 Pro TFT (240x320, ILI9341, VSPI)
class LGFX_D32_Pro : public lgfx::LGFX_Device
{
    lgfx::Panel_ILI9341 _panel_instance;
    lgfx::Bus_SPI _bus_instance;
public:
    LGFX_D32_Pro(void)
    {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = VSPI_HOST; cfg.spi_mode = 0;
            cfg.freq_write = 40000000; cfg.freq_read = 16000000;
            cfg.spi_3wire = true; cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 18; cfg.pin_mosi = 23; cfg.pin_miso = 19; cfg.pin_dc = 27;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = 14; cfg.pin_rst = 33; cfg.pin_busy = -1;
            cfg.panel_width = 240; cfg.panel_height = 320;
            cfg.offset_x = 0; cfg.offset_y = 0; cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8; cfg.dummy_read_bits = 1;
            cfg.readable = true; cfg.invert = false;
            cfg.rgb_order = false; cfg.dlen_16bit = false; cfg.bus_shared = true;
            _panel_instance.config(cfg);
        }
        setPanel(&_panel_instance);
    }
};

// M5StickC Plus/Plus2 (135x240, ST7789, SPI)
class LGFX_M5StickC : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 _panel_instance;
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;
public:
    LGFX_M5StickC(void) {}
    void configure(bool isPlus2) {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = isPlus2 ? HSPI_HOST : VSPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = isPlus2 ? 15000000 : 16000000;
            cfg.spi_3wire = true; cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 13; cfg.pin_mosi = 15; cfg.pin_miso = -1;
            cfg.pin_dc = isPlus2 ? 14 : 23;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = 5; cfg.pin_rst = isPlus2 ? 12 : 18; cfg.pin_busy = -1;
            cfg.panel_width = 135; cfg.panel_height = 240;
            cfg.offset_x = 52; cfg.offset_y = 40; cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8; cfg.dummy_read_bits = 1;
            cfg.readable = true; cfg.invert = true;
            cfg.rgb_order = false; cfg.dlen_16bit = false; cfg.bus_shared = true;
            _panel_instance.config(cfg);
        }
        if (isPlus2) {
            auto lcfg = _light_instance.config();
            lcfg.pin_bl = 27; lcfg.invert = false;
            lcfg.freq = 256; lcfg.pwm_channel = 7;
            _light_instance.config(lcfg);
            _panel_instance.setLight(&_light_instance);
        }
        setPanel(&_panel_instance);
    }
};

// TTGO TFT (135x240, ST7789, VSPI)
class LGFX_TFT_ESPI : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 _panel_instance;
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;
public:
    LGFX_TFT_ESPI(void)
    {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = VSPI_HOST; cfg.spi_mode = 0;
            cfg.freq_write = 40000000; cfg.freq_read = 16000000;
            cfg.spi_3wire = true; cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 18; cfg.pin_mosi = 19; cfg.pin_miso = -1; cfg.pin_dc = 16;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = 5; cfg.pin_rst = 23; cfg.pin_busy = -1;
            cfg.panel_width = 135; cfg.panel_height = 240;
            cfg.offset_x = 52; cfg.offset_y = 40; cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8; cfg.dummy_read_bits = 1;
            cfg.readable = true; cfg.invert = true;
            cfg.rgb_order = false; cfg.dlen_16bit = false; cfg.bus_shared = true;
            _panel_instance.config(cfg);
        }
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = 4; cfg.invert = false;
            cfg.freq = 44100; cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }
        setPanel(&_panel_instance);
    }
};

#elif defined(ESP32S3)
// Configuration class for ESP32-S3 TDisplay
class LGFX_S3_TDisplay : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 _panel_instance;
    lgfx::Bus_Parallel8 _bus_instance;
    lgfx::Light_PWM _light_instance;

public:
    LGFX_S3_TDisplay(void)
    {
        {
            auto cfg = _bus_instance.config();
            cfg.freq_write = 20000000;
            cfg.pin_wr = 8;
            cfg.pin_rd = 9;
            cfg.pin_rs = 7;
            cfg.pin_d0 = 39;
            cfg.pin_d1 = 40;
            cfg.pin_d2 = 41;
            cfg.pin_d3 = 42;
            cfg.pin_d4 = 45;
            cfg.pin_d5 = 46;
            cfg.pin_d6 = 47;
            cfg.pin_d7 = 48;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = 6;
            cfg.pin_rst = 5;
            cfg.pin_busy = -1;
            cfg.panel_width = 170;
            cfg.panel_height = 320;
            cfg.offset_x = 35;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = true;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = true;
            _panel_instance.config(cfg);
        }

        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = 38;
            cfg.invert = false;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        setPanel(&_panel_instance);
    }
};

#else
// Configuration class for TFT_ESPI displays (ST7789)
class LGFX_TFT_ESPI : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 _panel_instance;
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;

public:
    LGFX_TFT_ESPI(void)
    {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = VSPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 18;
            cfg.pin_mosi = 19;
            cfg.pin_miso = -1;
            cfg.pin_dc = 16;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = 5;
            cfg.pin_rst = 23;
            cfg.pin_busy = -1;
            cfg.panel_width = 135;
            cfg.panel_height = 240;
            cfg.offset_x = 52;
            cfg.offset_y = 40;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = true;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = true;
            _panel_instance.config(cfg);
        }

        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = 4;
            cfg.invert = false;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        setPanel(&_panel_instance);
    }
};
#endif

#endif // TILTBRIDGE_LOVYAN_CONFIG_H