/*
 * Integration tests for the host dispatcher (phy_rpc.c). Stands up the
 * mock esp-hosted channel, runs synchronous RPC wrappers against it,
 * checks request/response correlation + timeout behaviour.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_hosted_open.h"
#include "phy_rpc_proto.h"

extern void mock_set_slave_handler(int (*h)(uint32_t, const uint8_t *, size_t, uint32_t));
extern void mock_post_response(uint32_t resp_id, uint32_t op_id, int32_t status,
                               const void *body, size_t body_len);
extern void mock_post_event(uint32_t evt_id, const void *body, size_t body_len);

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "    FAIL: %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            return -1;                                                        \
        }                                                                     \
    } while (0)

/* ---------- bring-up ------------------------------------------- */

static int s_init_done;

static int ensure_init(void)
{
    if (s_init_done) return 0;
    if (esp_hosted_open_init() != ESP_OK) return -1;
    /* Fast timeout so a stuck test doesn't hang CI. */
    esp_hosted_open_set_timeout_ms(200);
    s_init_done = 1;
    return 0;
}

/* ---------- 1. echo ECHO: default mock auto-replies OK ---------- */

int test_dispatcher_echo_set_channel(void)
{
    EXPECT(ensure_init() == 0);
    mock_set_slave_handler(NULL);                 /* default echo */
    /* Default mock returns ESP_OK. */
    EXPECT(esp_hosted_open_set_channel(180) == ESP_OK);
    EXPECT(esp_hosted_open_set_phy_11p(true) == ESP_OK);
    EXPECT(esp_hosted_open_set_low_rate(false) == ESP_OK);
    return 0;
}

/* ---------- 2. body-bearing response ---------------------------- */

static int handler_get_phy_rssi(uint32_t req_id, const uint8_t *body,
                                size_t body_len, uint32_t op_id)
{
    if (req_id == PHY_RPC_REQ_GET_PHY_RSSI) {
        int8_t rssi = -42;
        mock_post_response(PHY_RPC_RESP_GET_PHY_RSSI, op_id, ESP_OK,
                           &rssi, sizeof(rssi));
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

int test_dispatcher_get_phy_rssi(void)
{
    EXPECT(ensure_init() == 0);
    mock_set_slave_handler(handler_get_phy_rssi);
    int8_t rssi = 0;
    EXPECT(esp_hosted_open_get_phy_rssi(&rssi) == ESP_OK);
    EXPECT(rssi == -42);
    return 0;
}

/* ---------- 3. error code propagation --------------------------- */

static int handler_returns_invalid(uint32_t req_id, const uint8_t *body,
                                   size_t body_len, uint32_t op_id)
{
    /* Always replies with ESP_ERR_INVALID_STATE regardless of msg_id. */
    uint32_t resp_id = (req_id & 0xFFFu) | (PHY_RPC_MSG_BASE | 0x8000u);
    mock_post_response(resp_id, op_id, ESP_ERR_INVALID_STATE, NULL, 0);
    return ESP_OK;
}

int test_dispatcher_error_propagation(void)
{
    EXPECT(ensure_init() == 0);
    mock_set_slave_handler(handler_returns_invalid);
    EXPECT(esp_hosted_open_set_channel(180) == ESP_ERR_INVALID_STATE);
    EXPECT(esp_hosted_open_set_phy_11p(true) == ESP_ERR_INVALID_STATE);
    return 0;
}

/* ---------- 4. timeout when slave never responds --------------- */

static int handler_drops_everything(uint32_t req_id, const uint8_t *body,
                                    size_t body_len, uint32_t op_id)
{
    /* Black hole — no response, no event. The host's timeout should fire. */
    return ESP_OK;
}

int test_dispatcher_timeout(void)
{
    EXPECT(ensure_init() == 0);
    mock_set_slave_handler(handler_drops_everything);
    EXPECT(esp_hosted_open_set_channel(180) == ESP_ERR_TIMEOUT);
    return 0;
}

/* ---------- 5. caps bitmap correctly delivers all 16 bytes ----- */

static int handler_get_caps_full(uint32_t req_id, const uint8_t *body,
                                 size_t body_len, uint32_t op_id)
{
    if (req_id == PHY_RPC_REQ_GET_CAPS) {
        uint8_t caps[PHY_RPC_CAPS_BYTES];
        for (int i = 0; i < PHY_RPC_CAPS_BYTES; i++) caps[i] = (uint8_t)(0x10 + i);
        mock_post_response(PHY_RPC_RESP_GET_CAPS, op_id, ESP_OK,
                           caps, sizeof(caps));
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

int test_dispatcher_caps_full_16_bytes(void)
{
    EXPECT(ensure_init() == 0);
    mock_set_slave_handler(handler_get_caps_full);
    uint8_t caps[16] = {0};
    EXPECT(esp_hosted_open_get_caps(caps) == ESP_OK);
    /* Verify the *full* 16-byte body round-trips, not just the old
     * 4 bytes — that's the bug we fixed earlier. */
    for (int i = 0; i < 16; i++) {
        EXPECT(caps[i] == (uint8_t)(0x10 + i));
    }
    return 0;
}

/* ---------- 6. async event delivery ---------------------------- */

static int   s_evt_count;
static int8_t s_last_rssi;
static uint8_t s_last_src[6];

static void on_evt_raw(const uint8_t *frame, size_t len,
                       const esp_hosted_open_raw_meta_t *meta, void *ctx)
{
    s_evt_count++;
    s_last_rssi = meta->rssi_dbm;
    /* Frame's first 6 bytes happen to be addr1 in our shim setup,
     * but we encode the source MAC at offset 10 in a real 802.11
     * frame. Test posts a synthetic body with src in meta. */
}

int test_dispatcher_event_dispatch(void)
{
    EXPECT(ensure_init() == 0);
    EXPECT(esp_hosted_open_register_raw_rx_cb(on_evt_raw, NULL) == ESP_OK);

    s_evt_count = 0;
    /* Build a phy_rpc_evt_raw_frame_t-shaped body. */
    struct __attribute__((packed)) {
        int8_t   rssi_dbm;
        uint8_t  channel;
        uint8_t  rate;
        uint8_t  is_qos;
        uint16_t frame_len;
        uint16_t reserved;
        uint32_t timestamp_us;
        uint8_t  frame[16];
    } body = {
        .rssi_dbm     = -55,
        .channel      = 180,
        .rate         = 11,
        .is_qos       = 1,
        .frame_len    = 16,
        .reserved     = 0,
        .timestamp_us = 12345,
        .frame        = "AAAA\xff\xff\xff\xff\xff\xff\xaa\xbb\xcc\xdd\xee\xff",
    };
    mock_post_event(PHY_RPC_EVT_RAW_FRAME, &body, sizeof(body));

    EXPECT(s_evt_count == 1);
    EXPECT(s_last_rssi == -55);
    return 0;
}
