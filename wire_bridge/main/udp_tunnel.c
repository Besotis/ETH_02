// udp_tunnel.c — Ethernet(L2) over UDP with fragmentation + robust reassembly
// ESP-IDF 6.x
//
// Fixes:
//  - TX queue holds pointers (low RAM)
//  - Reassembly uses fragment bitmap (works with >2 fragments, out-of-order)
//  - Simple timeout resets stuck reassembly

#include "udp_tunnel.h"

#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "wb_udp";

#define WB_MAGIC 0xBEEF
#define WB_VER   1

#define WB_MAX_FRAME    1600
#define WB_MTU          CONFIG_WB_MAX_PAYLOAD     // fragment payload bytes (400..1400)
#define WB_MAX_FRAGS    8                         // enough: 1600/400=4, 1600/200=8 etc.
#define WB_REASM_TO_MS  50                        // timeout for missing frags

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  flags;
    uint16_t seq;
    uint16_t frame_len;
    uint16_t frag_off;
    uint16_t frag_len;
} wb_hdr_t;

// Reassembly state
typedef struct {
    bool     in_use;
    uint16_t seq;
    uint16_t frame_len;
    uint8_t  total_frags;     // expected fragments count
    uint8_t  got_frags;       // how many received (unique)
    uint8_t  bitmap;          // bit i = fragment i received (WB_MAX_FRAGS<=8)
    int64_t  t_last_us;       // last fragment time
    uint8_t  buf[WB_MAX_FRAME];
} wb_reasm_t;

// TX queue item
typedef struct {
    uint16_t len;
    uint8_t *buf;             // malloc'd, freed in tx task
} tx_item_t;

static int s_sock = -1;
static struct sockaddr_in s_peer = {0};

static wb_frame_rx_cb_t s_rx_cb = NULL;
static void *s_rx_user = NULL;

static QueueHandle_t s_txq = NULL;

static wb_reasm_t s_re = {0};
static uint16_t s_seq = 1;

static uint32_t s_tx = 0, s_rx = 0, s_drop = 0;

uint32_t wb_udp_get_tx(void){ return s_tx; }
uint32_t wb_udp_get_rx(void){ return s_rx; }
uint32_t wb_udp_get_drop(void){ return s_drop; }

static uint8_t calc_total_frags(uint16_t frame_len)
{
    // ceil(frame_len / WB_MTU)
    uint32_t n = (frame_len + WB_MTU - 1) / WB_MTU;
    if (n == 0) n = 1;
    if (n > WB_MAX_FRAGS) return 0; // unsupported
    return (uint8_t)n;
}

static void reasm_reset(uint16_t seq, uint16_t frame_len)
{
    memset(&s_re, 0, sizeof(s_re));
    s_re.in_use = true;
    s_re.seq = seq;
    s_re.frame_len = frame_len;
    s_re.total_frags = calc_total_frags(frame_len);
    s_re.t_last_us = esp_timer_get_time();
    // if total_frags==0 -> will be dropped by handler
}

static void reasm_maybe_timeout(void)
{
    if (!s_re.in_use) return;
    int64_t now = esp_timer_get_time();
    if ((now - s_re.t_last_us) > (int64_t)WB_REASM_TO_MS * 1000) {
        // drop incomplete frame
        s_drop++;
        s_re.in_use = false;
    }
}

static void handle_packet(const uint8_t *p, int n)
{
    reasm_maybe_timeout();

    if (n < (int)sizeof(wb_hdr_t)) { s_drop++; return; }

    wb_hdr_t h;
    memcpy(&h, p, sizeof(h));

    if (h.magic != WB_MAGIC || h.ver != WB_VER) { s_drop++; return; }
    if (h.frame_len == 0 || h.frame_len > WB_MAX_FRAME) { s_drop++; return; }
    if ((int)(sizeof(wb_hdr_t) + h.frag_len) != n) { s_drop++; return; }
    if ((uint32_t)h.frag_off + (uint32_t)h.frag_len > (uint32_t)h.frame_len) { s_drop++; return; }
    if (h.frag_len == 0 || h.frag_len > WB_MTU) { s_drop++; return; }

    // determine fragment index
    if ((h.frag_off % WB_MTU) != 0) {
        // require aligned offsets (simplifies reassembly). last fragment still aligned by off
        s_drop++;
        return;
    }
    uint16_t frag_idx = (uint16_t)(h.frag_off / WB_MTU);
    if (frag_idx >= WB_MAX_FRAGS) { s_drop++; return; }

    const uint8_t *payload = p + sizeof(wb_hdr_t);

    // new frame
    if (!s_re.in_use || s_re.seq != h.seq || s_re.frame_len != h.frame_len) {
        reasm_reset(h.seq, h.frame_len);
        if (s_re.total_frags == 0) { // too many fragments needed
            s_drop++;
            s_re.in_use = false;
            return;
        }
    }

    // total_frags validation: index must be within expected
    if (frag_idx >= s_re.total_frags) { s_drop++; return; }

    // copy payload
    memcpy(&s_re.buf[h.frag_off], payload, h.frag_len);

    // mark received (avoid double-counting)
    uint8_t bit = (uint8_t)(1u << frag_idx);
    if ((s_re.bitmap & bit) == 0) {
        s_re.bitmap |= bit;
        s_re.got_frags++;
    }

    s_re.t_last_us = esp_timer_get_time();

    // complete when got all fragments
    if (s_re.got_frags >= s_re.total_frags) {
        // also sanity check last fragment length matches frame_len
        // (optional strictness)
        if (s_rx_cb) s_rx_cb(s_re.buf, s_re.frame_len, s_rx_user);
        s_re.in_use = false;
    }
}

static void udp_rx_task(void *arg)
{
    (void)arg;
    uint8_t rxbuf[2048];

    while (1) {
        int n = recv(s_sock, rxbuf, sizeof(rxbuf), 0);
        if (n <= 0) continue;
        s_rx++;
        handle_packet(rxbuf, n);
    }
}

static void udp_tx_task(void *arg)
{
    (void)arg;
    tx_item_t it;

    while (1) {
        if (xQueueReceive(s_txq, &it, portMAX_DELAY) != pdTRUE) continue;

        if (!it.buf || it.len == 0 || it.len > WB_MAX_FRAME) {
            s_drop++;
            if (it.buf) free(it.buf);
            continue;
        }

        uint16_t seq = s_seq++;
        uint16_t frame_len = it.len;

        for (uint16_t off = 0; off < frame_len; ) {
            uint16_t frag = (uint16_t)(frame_len - off);
            if (frag > WB_MTU) frag = WB_MTU;

            uint8_t out[sizeof(wb_hdr_t) + WB_MTU];

            wb_hdr_t h = {
                .magic = WB_MAGIC,
                .ver = WB_VER,
                .flags = 1,
                .seq = seq,
                .frame_len = frame_len,
                .frag_off = off,
                .frag_len = frag,
            };

            memcpy(out, &h, sizeof(h));
            memcpy(out + sizeof(h), it.buf + off, frag);

            int sent = sendto(s_sock, out, (int)(sizeof(h) + frag), 0,
                              (struct sockaddr*)&s_peer, sizeof(s_peer));
            if (sent > 0) s_tx++;
            else s_drop++;

            off = (uint16_t)(off + frag);
        }

        free(it.buf);
    }
}

void wb_udp_start(wb_frame_rx_cb_t cb, void *user)
{
    s_rx_cb = cb;
    s_rx_user = user;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        return;
    }

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(CONFIG_WB_UDP_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_sock, (struct sockaddr*)&local, sizeof(local)) != 0) {
        ESP_LOGE(TAG, "bind() failed");
        return;
    }

    s_peer.sin_family = AF_INET;
    s_peer.sin_port = htons(CONFIG_WB_UDP_PORT);

#if CONFIG_WB_ROLE_AP
    s_peer.sin_addr.s_addr = inet_addr("192.168.50.2");
#else
    s_peer.sin_addr.s_addr = inet_addr("192.168.50.1");
#endif

    s_txq = xQueueCreate(16, sizeof(tx_item_t));
    if (!s_txq) {
        ESP_LOGE(TAG, "xQueueCreate failed (no RAM)");
        return;
    }

    xTaskCreate(udp_rx_task, "wb_udp_rx", 4096, NULL, 18, NULL);
    xTaskCreate(udp_tx_task, "wb_udp_tx", 4096, NULL, 18, NULL);

    ESP_LOGI(TAG, "UDP tunnel: port=%d peer=%s payload=%d",
             CONFIG_WB_UDP_PORT,
#if CONFIG_WB_ROLE_AP
             "192.168.50.2"
#else
             "192.168.50.1"
#endif
             , WB_MTU);
}

bool wb_udp_send_frame(const uint8_t *frame, size_t len)
{
    if (!s_txq || !frame || len == 0 || len > WB_MAX_FRAME) return false;

    tx_item_t it = {0};
    it.len = (uint16_t)len;
    it.buf = (uint8_t*)malloc(len);
    if (!it.buf) {
        s_drop++;
        return false;
    }
    memcpy(it.buf, frame, len);

    if (xQueueSend(s_txq, &it, 0) == pdTRUE) return true;

    free(it.buf);
    s_drop++;
    return false;
}
