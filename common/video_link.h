#pragma once

#include <stdint.h>

/* Both boards are little-endian. The header is sent in native byte order. */

#define VIDEO_LINK_MAGIC 0xA55A
#define VIDEO_LINK_PORT 5000
#define VIDEO_LINK_PAYLOAD 1400
#define VIDEO_LINK_MAX_FRAGS 46
#define VIDEO_LINK_MAX_JPEG (VIDEO_LINK_PAYLOAD * VIDEO_LINK_MAX_FRAGS)

#define VIDEO_LINK_SSID "tank-rx"
#define VIDEO_LINK_PASS "tank-rx-pass"
#define VIDEO_LINK_CHANNEL 6
#define VIDEO_LINK_AP_IP "192.168.4.1"

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint16_t frame_id;
    uint8_t frag_idx;
    uint8_t frag_cnt;
    uint16_t payload_len;
} video_link_hdr_t;
