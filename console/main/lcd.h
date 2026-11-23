#pragma once

#include <stdint.h>

#include "esp_err.h"

#define LCD_H_RES 240
#define LCD_V_RES 320

/*
 * GMT024-08-SPI8P on an AI-Thinker ESP32-CAM.
 * The camera and PSRAM (GPIO16/17) stay wired, so the panel uses the
 * SD-card pins. Tie the panel RST pad to 3.3V. GPIO12 is the flash
 * voltage strap and is left unused. GPIO4 also drives the onboard flash LED.
 * Header: GND, VCC 3.3V, SCL, SDA, RST, DC, CS, BL.
 */
#define LCD_PIN_SCLK 14
#define LCD_PIN_MOSI 13
#define LCD_PIN_CS 15
#define LCD_PIN_DC 2
#define LCD_PIN_RST -1
#define LCD_PIN_BL 4

esp_err_t lcd_start(void);
void lcd_color_bars(void);
void lcd_draw_rgb565(const uint16_t *src, int src_w, int src_h);
