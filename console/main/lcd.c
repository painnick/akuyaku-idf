#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "lcd.h"

static const char *TAG = "lcd";

#define FLUSH_LINES 16

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_flush;
static int s_shown_w = -1;
static int s_shown_h = -1;

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int row = 0; row < h; row += FLUSH_LINES) {
        int n = h - row;
        if (n > FLUSH_LINES) {
            n = FLUSH_LINES;
        }
        for (int i = 0; i < n * w; i++) {
            s_flush[i] = color;
        }
        esp_lcd_panel_draw_bitmap(s_panel, x, y + row, x + w, y + row + n, s_flush);
    }
}

esp_err_t lcd_start(void)
{
    gpio_config_t bl = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight gpio");
    gpio_set_level(LCD_PIN_BL, !CONFIG_CONSOLE_LCD_BL_ON_LEVEL);

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * FLUSH_LINES * (int)sizeof(uint16_t) + 8,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = CONFIG_CONSOLE_LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io), TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
#if CONFIG_CONSOLE_LCD_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel), TAG, "st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
#if CONFIG_CONSOLE_LCD_INVERT
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
#else
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, false), TAG, "invert");
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");

    s_flush = heap_caps_malloc(LCD_H_RES * FLUSH_LINES * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_flush == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gpio_set_level(LCD_PIN_BL, CONFIG_CONSOLE_LCD_BL_ON_LEVEL);
    ESP_LOGI(TAG, "ST7789 %dx%d, SPI %d Hz, pins SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d",
             LCD_H_RES, LCD_V_RES, CONFIG_CONSOLE_LCD_PCLK_HZ,
             LCD_PIN_SCLK, LCD_PIN_MOSI, LCD_PIN_CS, LCD_PIN_DC, LCD_PIN_RST, LCD_PIN_BL);
    return ESP_OK;
}

void lcd_color_bars(void)
{
    static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFE0, 0xFFFF};
    const int band = LCD_V_RES / 5;
    for (int y = 0; y < LCD_V_RES; y += FLUSH_LINES) {
        int n = LCD_V_RES - y;
        if (n > FLUSH_LINES) {
            n = FLUSH_LINES;
        }
        for (int row = 0; row < n; row++) {
            uint16_t color = colors[(y + row) / band];
            uint16_t *dst = s_flush + row * LCD_H_RES;
            for (int x = 0; x < LCD_H_RES; x++) {
                dst[x] = color;
            }
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + n, s_flush);
    }
}

void lcd_draw_rgb565(const uint16_t *src, int src_w, int src_h)
{
    int copy_w = src_w;
    int copy_h = src_h;
    int src_x = 0;
    int dst_x = 0;
    int dst_y = 0;

    if (src_w == LCD_H_RES && src_h == LCD_V_RES) {
        dst_x = 0;
        dst_y = 0;
    } else if (src_w > LCD_H_RES && src_h <= LCD_V_RES) {
        copy_w = LCD_H_RES;
        copy_h = src_h;
        src_x = (src_w - LCD_H_RES) / 2;
        dst_y = (LCD_V_RES - src_h) / 2;
    } else {
        ESP_LOGW(TAG, "unsupported frame %dx%d", src_w, src_h);
        return;
    }

    if (s_shown_w != copy_w || s_shown_h != copy_h) {
        fill_rect(0, 0, LCD_H_RES, LCD_V_RES, 0x0000);
        s_shown_w = copy_w;
        s_shown_h = copy_h;
    }

    for (int row0 = 0; row0 < copy_h; row0 += FLUSH_LINES) {
        int n = copy_h - row0;
        if (n > FLUSH_LINES) {
            n = FLUSH_LINES;
        }
        for (int row = 0; row < n; row++) {
            memcpy(s_flush + row * copy_w,
                   src + (size_t)(row0 + row) * src_w + src_x,
                   copy_w * sizeof(uint16_t));
        }
        esp_lcd_panel_draw_bitmap(s_panel, dst_x, dst_y + row0, dst_x + copy_w, dst_y + row0 + n, s_flush);
    }
}
