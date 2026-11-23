#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

#include "lcd.h"
#include "video_link.h"

static const char *TAG = "console";

typedef struct {
    uint8_t data[VIDEO_LINK_MAX_JPEG];
    uint32_t len;
    uint16_t frame_id;
} jpeg_slot_t;

static jpeg_slot_t *s_assembly;
static jpeg_slot_t *s_ready;
static TaskHandle_t s_decode_task;
static portMUX_TYPE s_ready_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_decode_busy;
static volatile uint32_t s_busy_drops;
static volatile uint32_t s_gap_drops;

static jpeg_rotate_t decoder_rotate(void)
{
#if CONFIG_CONSOLE_JPEG_ROTATE_90
    return JPEG_ROTATE_90D;
#elif CONFIG_CONSOLE_JPEG_ROTATE_180
    return JPEG_ROTATE_180D;
#elif CONFIG_CONSOLE_JPEG_ROTATE_270
    return JPEG_ROTATE_270D;
#else
    return JPEG_ROTATE_0D;
#endif
}

static void output_span(int src_w, int src_h, int *out_w, int *out_h)
{
#if CONFIG_CONSOLE_JPEG_ROTATE_90 || CONFIG_CONSOLE_JPEG_ROTATE_270
    *out_w = src_h;
    *out_h = src_w;
#else
    *out_w = src_w;
    *out_h = src_h;
#endif
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = data;
        ESP_LOGI(TAG, "station " MACSTR " joined", MAC2STR(event->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGW(TAG, "station " MACSTR " left, reason %u", MAC2STR(event->mac), event->reason);
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();

    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(ap));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &ip_info));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(ap));

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t wifi_cfg = {
        .ap = {
            .ssid = VIDEO_LINK_SSID,
            .ssid_len = sizeof(VIDEO_LINK_SSID) - 1,
            .channel = VIDEO_LINK_CHANNEL,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 1,
        },
    };
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    ESP_LOGI(TAG, "SoftAP %s ch %d, video UDP %d", VIDEO_LINK_SSID, VIDEO_LINK_CHANNEL, VIDEO_LINK_PORT);
}

static void publish_frame(void)
{
    bool dropped = false;
    taskENTER_CRITICAL(&s_ready_mux);
    if (s_decode_busy) {
        dropped = true;
    } else {
        s_decode_busy = true;
    }
    taskEXIT_CRITICAL(&s_ready_mux);
    if (dropped) {
        s_busy_drops++;
        return;
    }

    memcpy(s_ready, s_assembly, sizeof(jpeg_slot_t));
    xTaskNotifyGive(s_decode_task);
}

static void receive_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(VIDEO_LINK_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    uint8_t packet[sizeof(video_link_hdr_t) + VIDEO_LINK_PAYLOAD];
    uint16_t current_id = 0;
    uint8_t expected = 0;
    uint8_t got = 0;
    uint64_t mask = 0;
    uint16_t last_len = 0;

    while (true) {
        int n = recvfrom(sock, packet, sizeof(packet), 0, NULL, NULL);
        if (n < (int)sizeof(video_link_hdr_t)) {
            continue;
        }

        video_link_hdr_t hdr;
        memcpy(&hdr, packet, sizeof(hdr));
        if (hdr.magic != VIDEO_LINK_MAGIC || hdr.frag_cnt == 0 || hdr.frag_cnt > VIDEO_LINK_MAX_FRAGS) {
            continue;
        }
        if (hdr.frag_idx >= hdr.frag_cnt || hdr.payload_len == 0 || hdr.payload_len > VIDEO_LINK_PAYLOAD) {
            continue;
        }
        if (n != (int)(sizeof(hdr) + hdr.payload_len)) {
            continue;
        }
        if (hdr.frag_idx + 1 < hdr.frag_cnt && hdr.payload_len != VIDEO_LINK_PAYLOAD) {
            continue;
        }

        if (expected == 0 || hdr.frame_id != current_id || hdr.frag_cnt != expected) {
            if (hdr.frag_idx != 0) {
                s_gap_drops++;
                continue;
            }
            current_id = hdr.frame_id;
            expected = hdr.frag_cnt;
            got = 0;
            mask = 0;
            last_len = 0;
        }

        uint64_t bit = 1ULL << hdr.frag_idx;
        if (mask & bit) {
            continue;
        }
        mask |= bit;
        got++;
        memcpy(s_assembly->data + (size_t)hdr.frag_idx * VIDEO_LINK_PAYLOAD,
               packet + sizeof(hdr), hdr.payload_len);
        if (hdr.frag_idx + 1 == hdr.frag_cnt) {
            last_len = hdr.payload_len;
        }
        if (got != expected || last_len == 0) {
            continue;
        }

        s_assembly->len = (uint32_t)(expected - 1) * VIDEO_LINK_PAYLOAD + last_len;
        s_assembly->frame_id = current_id;
        publish_frame();
        expected = 0;
    }
}

static bool decode_and_show(jpeg_dec_handle_t decoder, uint8_t *pixels, int pixels_cap)
{
    uint8_t *jpeg = s_ready->data;
    int jpeg_len = (int)s_ready->len;
    jpeg_dec_io_t io = {
        .inbuf = jpeg,
        .inbuf_len = jpeg_len,
        .inbuf_remain = jpeg_len,
    };
    jpeg_dec_header_info_t info = {0};
    jpeg_error_t ret = jpeg_dec_parse_header(decoder, &io, &info);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg header %d, frame %u len %u", ret, s_ready->frame_id, (unsigned)s_ready->len);
        return false;
    }
    /* OV2640 JPEG QVGA arrives as 240x320. 320x240 is the other legal size. */
    if (!((info.width == 240 && info.height == 320) || (info.width == 320 && info.height == 240))) {
        ESP_LOGW(TAG, "expected 240x320 or 320x240, got %ux%u", info.width, info.height);
        return false;
    }

    int out_w = 0;
    int out_h = 0;
    output_span(info.width, info.height, &out_w, &out_h);
    int out_len = out_w * out_h * (int)sizeof(uint16_t);
    if (out_len > pixels_cap) {
        return false;
    }

    /* parse_header consumes the buffer. A zero remainder makes process return -3. */
    if (io.inbuf == NULL || io.inbuf_remain <= 0) {
        io.inbuf = jpeg;
        io.inbuf_len = jpeg_len;
        io.inbuf_remain = jpeg_len;
    }
    io.outbuf = pixels;
    ret = jpeg_dec_process(decoder, &io);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg decode %d, len %d", ret, jpeg_len);
        return false;
    }

    lcd_draw_rgb565((const uint16_t *)pixels, out_w, out_h);
    return true;
}

static void decode_task(void *arg)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.rotate = decoder_rotate();

    jpeg_dec_handle_t decoder = NULL;
    jpeg_error_t ret = jpeg_dec_open(&cfg, &decoder);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg open %d", ret);
        vTaskDelete(NULL);
        return;
    }

    const int pixels_cap = LCD_H_RES * LCD_V_RES * (int)sizeof(uint16_t);
    uint8_t *pixels = jpeg_calloc_align(pixels_cap, 16);
    if (pixels == NULL) {
        pixels = heap_caps_aligned_calloc(16, 1, pixels_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (pixels == NULL) {
        ESP_LOGE(TAG, "no pixel buffer");
        vTaskDelete(NULL);
        return;
    }

    uint32_t shown = 0;
    uint32_t failed = 0;
    int64_t window_start = esp_timer_get_time();

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (decode_and_show(decoder, pixels, pixels_cap)) {
            shown++;
        } else {
            failed++;
        }

        taskENTER_CRITICAL(&s_ready_mux);
        s_decode_busy = false;
        taskEXIT_CRITICAL(&s_ready_mux);

        int64_t now = esp_timer_get_time();
        if (now - window_start >= 2000000) {
            float seconds = (now - window_start) / 1000000.0f;
            uint32_t busy = s_busy_drops;
            uint32_t gap = s_gap_drops;
            s_busy_drops = 0;
            s_gap_drops = 0;
            ESP_LOGI(TAG, "rx %.1f fps, decode fail %u, busy drop %u, gap %u",
                     shown / seconds, (unsigned)failed, (unsigned)busy, (unsigned)gap);
            shown = 0;
            failed = 0;
            window_start = now;
        }
    }
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    s_assembly = heap_caps_aligned_calloc(16, 1, sizeof(jpeg_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ready = heap_caps_aligned_calloc(16, 1, sizeof(jpeg_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_assembly == NULL || s_ready == NULL) {
        ESP_LOGE(TAG, "no PSRAM for JPEG slots");
        return;
    }

    ESP_ERROR_CHECK(lcd_start());
    lcd_color_bars();
    wifi_start();

    xTaskCreatePinnedToCore(decode_task, "decode", 10240, NULL, 4, &s_decode_task, 1);
    xTaskCreatePinnedToCore(receive_task, "video_rx", 5120, NULL, 6, NULL, 0);
}
