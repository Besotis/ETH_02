// udp_tunnel.c — Ethernet(L2) over UDP with simple fragmentation + safe reassembly
// ESP-IDF 6.x
//
// Key fixes vs earlier version:
//  - TX queue stores pointers (small queue, no huge RAM usage)
//  - Reassembly is "correct": does NOT assume got-bytes means complete
//    We handle 1-fragment frames and 2-fragment frames reliably (enough for Ethernet MTU).
//
// Recommended: set CONFIG_WB_MAX_PAYLOAD to 1200..1400
// NOTE: With payload >= 800, 1600-byte frames are at most 2 fragments.

#include "udp_tunnel.h"
#include "bridge_cfg.h"

#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "wb_udp";

#define WB_MAGIC 0xBEEF
#define WB_VER   1

#define WB_MAX_FRAME   1600                  // accept up to typical Ethernet frame sizes
#define WB_MTU         CONFIG_WB_MAX_PAYLOAD  // fragment payload bytes

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  flags;       // bit0=frag (always 1 in this simple tunnel)
    uint16_t seq;         // frame sequence
    uint16_t frame_len;   // full ethernet frame length
    uint16_t frag_off;    // offset in bytes
    uint16_t frag_len;    // fragment bytes
} wb_hdr_t;

// Reassembly: 1 or 2 fragments (works for Ethernet MTU with WB_MTU >= 800)
typedef struct {
    uint16_t seq;
    uint16_t frame_len;
    uint8_t  mask;        // bit0: frag at off=0 received, bit1: frag at off>0 received
    uint8_t  buf[WB_MAX_FRAME];
    bool     in_use;
} wb_reasm_t;

// TX queue item (small): payload buffer allocated per frame
typedef struct {
    uint16_t len;
    uint8_t *buf;         // malloc'd, freed in tx task
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

static void reasm_reset(uint16_t seq, uint16_t frame_len)
{
    s_re.seq = seq;
    s_re.frame_len = frame_len;
    s_re.mask = 0;
    s_re.in_use = true;
}

static void handle_packet(const uint8_t *p, int n)
{
    if (n < (int)sizeof(wb_hdr_t)) { s_drop++; return; }

    wb_hdr_t h;
    memcpy(&h, p, sizeof(h));

    if (h.magic != WB_MAGIC || h.ver != WB_VER) { s_drop++; return; }

    if (h.frame_len == 0 || h.frame_len > WB_MAX_FRAME) { s_drop++; return; }
    if ((int)(sizeof(wb_hdr_t) + h.frag_len) != n) { s_drop++; return; }
    if ((uint32_t)h.frag_off + (uint32_t)h.frag_len > (uint32_t)h.frame_len) { s_drop++; return; }

    const uint8_t *payload = p + sizeof(wb_hdr_t);

    // New frame or frame changed -> reset
    if (!s_re.in_use || s_re.seq != h.seq || s_re.frame_len != h.frame_len) {
        reasm_reset(h.seq, h.frame_len);
    }

    // Copy fragment into buffer
    memcpy(&s_re.buf[h.frag_off], payload, h.frag_len);

    // Mark which fragment we got (we only need 2-fragment logic for Ethernet MTU)
    if (h.frag_off == 0) s_re.mask |= 0x01;
    else                 s_re.mask |= 0x02;

    // 1-fragment frame: offset 0 and frag_len == frame_len
    if ((s_re.mask & 0x01) && h.frag_off == 0 && h.frag_len == h.frame_len) {
        if (s_rx_cb) s_rx_cb(s_re.buf, s_re.frame_len, s_rx_user);
        s_re.in_use = false;
        return;
    }

    // 2-fragment frame: require both parts
    if (s_re.mask == 0x03) {
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

    // Small queue: only pointers + length
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

    // Queue full
    free(it.buf);
    s_drop++;
    return false;
}
