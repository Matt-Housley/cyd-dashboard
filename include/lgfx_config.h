#pragma once

// ─────────────────────────────────────────────────────────────
//  Desktop / SDL2 simulation
//  LovyanGFX auto-detects SDL when LGFX_AUTODETECT is defined
//  and SDL2 headers are on the include path.
// ─────────────────────────────────────────────────────────────
#ifdef LGFX_SDL

#define LGFX_AUTODETECT
#include <LovyanGFX.h>
#include <LGFX_AUTODETECT.hpp>
// LGFX is now the auto-detected SDL class.
// Constructor: LGFX(int width, int height, uint8_t scaling = 1)

// ─────────────────────────────────────────────────────────────
//  Hardware – Cheap Yellow Display (ESP32 + ILI9341 + XPT2046)
// ─────────────────────────────────────────────────────────────
#else  // BOARD_CYD

#include <LovyanGFX.hpp>

#if FALSE
#define CYD28_TouchR_CAL_XMIN   185
#define CYD28_TouchR_CAL_XMAX   3700
#define CYD28_TouchR_CAL_YMIN   280
#define CYD28_TouchR_CAL_YMAX   3850
#else
#define CYD28_TouchR_CAL_XMIN   300
#define CYD28_TouchR_CAL_XMAX   3900
#define CYD28_TouchR_CAL_YMIN   200
#define CYD28_TouchR_CAL_YMAX   3700
#endif




class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ILI9341  _panel_instance;
    lgfx::Bus_SPI        _bus_instance;
    lgfx::Light_PWM      _light_instance;
    lgfx::Touch_XPT2046  _touch_instance;

public:
    LGFX(void)
    {
        {   // SPI bus – display (HSPI)
            auto cfg = _bus_instance.config();
            cfg.spi_host    = HSPI_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 55000000;
            cfg.freq_read   = 20000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = 1;  // DMA disabled — testing if channel 1 was dropping tail-end of SPI rows
            cfg.pin_sclk    = 14;
            cfg.pin_mosi    = 13;
            cfg.pin_miso    = 12;
            cfg.pin_dc      = 2;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {   // ILI9341 panel
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = 15;
            cfg.pin_rst          = -1;
            cfg.pin_busy         = -1;
            cfg.memory_width     = WIDTH;
            cfg.memory_height    = HEIGHT;
            cfg.panel_width      = WIDTH;
            cfg.panel_height     = HEIGHT;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = true;
            cfg.invert           = false;
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = true;
            _panel_instance.config(cfg);
        }

        {   // Backlight PWM
            auto cfg = _light_instance.config();
            cfg.pin_bl      = 21;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        {   // Touch – XPT2046 (VSPI)
            auto cfg = _touch_instance.config();
            cfg.x_min           = CYD28_TouchR_CAL_XMIN;
            cfg.x_max           = CYD28_TouchR_CAL_XMAX;
            cfg.y_min           = CYD28_TouchR_CAL_YMIN;
            cfg.y_max           = CYD28_TouchR_CAL_YMAX;
            cfg.pin_int         = 36;
            cfg.bus_shared      = true;
            // Horizontal Screen - display.setRotation(1);
            //cfg.offset_rotation = 0;      //  0,0 top right
            //cfg.offset_rotation = 1;    //  0,0 top left. X & Y swapped.  
            //cfg.offset_rotation = 2;    //  0,0 bottom left.
            //cfg.offset_rotation = 3;    //  0,0 bottom right. X & Y swapped.
            //cfg.offset_rotation = 4;    //  0,0 bottom right.        
            //cfg.offset_rotation = 5;    //  0,0 top right. X & Y swapped.
            cfg.offset_rotation = 0;
            //cfg.offset_rotation = 7;    //  0,0 bottom left. X & Y swapped.
            cfg.spi_host        = VSPI_HOST;
            cfg.freq            = 2500000;
            cfg.pin_sclk        = 25;
            cfg.pin_mosi        = 32;
            cfg.pin_miso        = 39;
            cfg.pin_cs          = 33;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }

        setPanel(&_panel_instance);
    }
};

#endif  // LGFX_SDL / BOARD_CYD

extern LGFX tft;
