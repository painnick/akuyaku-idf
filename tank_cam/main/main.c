#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_camera.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "video_link.h"

static const char *TAG = "tank_cam";

#define WIFI_CONNECTED_BIT BIT0

/*
 * AI-Thinker ESP32-CAM. Y2 is the camera D0 LSB.
 * GPIO0 is XCLK and also the download strap; it is driven only after boot.
 * GPIO16/17 stay with the onboard PSRAM.
 */
#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27
#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

static EventGroupHandle_t s_wifi_events;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnect reason %u, reconnecting", event->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "STA IP " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = VIDEO_LINK_SSID,
            .channel = VIDEO_LINK_CHANNEL,
            .threshold.authmode = WIFI_AUTH_OPEN,
            .scan_method = WIFI_FAST_SCAN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    /* AMPDU is off, so a fixed basic rate is allowed. High MCS frames were not ACKed and filled the TX queue. */
    ESP_ERROR_CHECK(esp_wifi_internal_set_fix_rate(WIFI_IF_STA, true, WIFI_PHY_RATE_6M));
    ESP_LOGI(TAG, "joining SoftAP %s ch %d", VIDEO_LINK_SSID, VIDEO_LINK_CHANNEL);
}

static esp_err_t camera_start(void)
{
    camera_config_t cfg = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = CONFIG_TANK_JPEG_QUALITY,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
    }
    return err;
}

static bool send_frame(int sock, const struct sockaddr_in *dest, const uint8_t *jpeg, size_t len, uint16_t frame_id)
{
    uint8_t packet[sizeof(video_link_hdr_t) + VIDEO_LINK_PAYLOAD];
    uint8_t frag_cnt = (uint8_t)((len + VIDEO_LINK_PAYLOAD - 1) / VIDEO_LINK_PAYLOAD);

    for (uint8_t i = 0; i < frag_cnt; i++) {
        size_t offset = (size_t)i * VIDEO_LINK_PAYLOAD;
        size_t chunk = len - offset;
        if (chunk > VIDEO_LINK_PAYLOAD) {
            chunk = VIDEO_LINK_PAYLOAD;
        }

        video_link_hdr_t hdr = {
            .magic = VIDEO_LINK_MAGIC,
            .frame_id = frame_id,
            .frag_idx = i,
            .frag_cnt = frag_cnt,
            .payload_len = (uint16_t)chunk,
        };
        memcpy(packet, &hdr, sizeof(hdr));
        memcpy(packet + sizeof(hdr), jpeg + offset, chunk);

        /* UDP sendto returns ENOMEM instead of waiting when the Wi-Fi TX queue is full. */
        int sent = -1;
        for (int attempt = 0; attempt < 4; attempt++) {
            sent = sendto(sock, packet, sizeof(hdr) + chunk, 0, (const struct sockaddr *)dest, sizeof(*dest));
            if (sent >= 0 || errno != ENOMEM) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (sent < 0) {
            ESP_LOGW(TAG, "sendto failed errno=%d len=%u", errno, (unsigned)len);
            return false;
        }
    }
    return true;
}

static void stream_task(void *arg)
{
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(VIDEO_LINK_PORT),
    };
    dest.sin_addr.s_addr = inet_addr(VIDEO_LINK_AP_IP);
    ESP_LOGI(TAG, "pushing JPEG to %s:%d", VIDEO_LINK_AP_IP, VIDEO_LINK_PORT);

    uint16_t frame_id = 0;
    uint32_t sent_frames = 0;
    uint32_t dropped = 0;
    int64_t window_start = esp_timer_get_time();
    int64_t next_frame_us = window_start;
    const int64_t frame_interval_us = 1000000 / 15;

    while (true) {
        if ((xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int64_t now = esp_timer_get_time();
        if (now < next_frame_us) {
            vTaskDelay(pdMS_TO_TICKS((next_frame_us - now) / 1000));
        }
        next_frame_us = esp_timer_get_time() + frame_interval_us;

        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGW(TAG, "capture failed");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (fb->format == PIXFORMAT_JPEG && fb->len > 0 && fb->len <= VIDEO_LINK_MAX_JPEG) {
            if (send_frame(sock, &dest, fb->buf, fb->len, frame_id)) {
                frame_id++;
                sent_frames++;
            } else {
                dropped++;
                /* Let the Wi-Fi retry queue drain instead of refilling it. */
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        } else {
            dropped++;
            ESP_LOGW(TAG, "skip frame format=%d len=%u", fb->format, (unsigned)fb->len);
        }
        esp_camera_fb_return(fb);

        now = esp_timer_get_time();
        if (now - window_start >= 2000000) {
            float seconds = (now - window_start) / 1000000.0f;
            ESP_LOGI(TAG, "tx %.1f fps, dropped %u", sent_frames / seconds, (unsigned)dropped);
            sent_frames = 0;
            dropped = 0;
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

    esp_err_t cam = camera_start();
    wifi_start();
    if (cam != ESP_OK) {
        ESP_LOGE(TAG, "camera sensor did not answer on GPIO4/GPIO5. Reseat the ribbon and reset.");
        return;
    }

    xTaskCreatePinnedToCore(stream_task, "stream", 6144, NULL, 5, NULL, 1);
}
