/*
 * phy_rpc_wireless.c — ESP-NOW + 802.15.4 RPC handlers.
 *
 * ESP-NOW is universal across every Wi-Fi-capable Espressif chip;
 * 802.15.4 lives only on C6 / H2 / H4 (and is *not* present on the
 * C5). The 15.4 handlers are guarded with weak symbols so the
 * binary still links on chips without the radio — those calls
 * return ESP_ERR_NOT_SUPPORTED at runtime.
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_hosted_peer_data.h"
#include "phy_rpc_proto.h"

static const char *TAG = "phy_rpc_wireless";

/* --- shared response helpers (internal-link to phy_rpc_extras) --- */
static void send_simple_resp(uint32_t resp_id, uint32_t op_id, esp_err_t status)
{
    phy_rpc_resp_hdr_t hdr = { .op_id = op_id, .status = (int32_t)status };
    esp_hosted_send_custom_data(resp_id, (uint8_t *)&hdr, sizeof(hdr));
}

#define UNPACK(type, var)                                              \
    if (len < sizeof(type)) { ESP_LOGW(TAG, #type " short"); return; } \
    const type *var = (const type *)data;                              \
    uint32_t _op = var->hdr.op_id

/* ====================================================================
 * ESP-NOW
 * ================================================================== */

static void espnow_rx_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0 || len > 250) return;
    size_t total = sizeof(phy_rpc_evt_espnow_rx_t) + len;
    uint8_t *buf = malloc(total);
    if (!buf) return;
    phy_rpc_evt_espnow_rx_t *ev = (phy_rpc_evt_espnow_rx_t *)buf;
    memcpy(ev->src_mac, info->src_addr, 6);
    memcpy(ev->dst_mac, info->des_addr, 6);
    ev->rssi_dbm = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    ev->channel  = info->rx_ctrl ? info->rx_ctrl->channel : 0;
    ev->data_len = (uint16_t)len;
    memcpy(ev->data, data, len);
    esp_hosted_send_custom_data(PHY_RPC_EVT_ESPNOW_RX, buf, total);
    free(buf);
}

static void espnow_tx_cb(const wifi_tx_info_t *info, esp_now_send_status_t status)
{
    phy_rpc_evt_espnow_tx_status_t ev = {0};
    if (info && info->src_addr) memcpy(ev.peer_mac, info->src_addr, 6);
    ev.status = (status == ESP_NOW_SEND_SUCCESS) ? 0 : 1;
    esp_hosted_send_custom_data(PHY_RPC_EVT_ESPNOW_TX_STATUS,
                                (uint8_t *)&ev, sizeof(ev));
}

static void on_espnow_init(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    esp_err_t err = esp_now_init();
    if (err == ESP_OK) {
        esp_now_register_recv_cb(espnow_rx_cb);
        esp_now_register_send_cb(espnow_tx_cb);
    }
    send_simple_resp(PHY_RPC_RESP_ESPNOW_INIT, op, err);
}

static void on_espnow_deinit(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    esp_err_t err = esp_now_deinit();
    send_simple_resp(PHY_RPC_RESP_ESPNOW_DEINIT, op, err);
}

static void on_espnow_set_pmk(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_espnow_set_pmk_t, r);
    esp_err_t err = esp_now_set_pmk(r->pmk);
    send_simple_resp(PHY_RPC_RESP_ESPNOW_SET_PMK, _op, err);
}

static void on_espnow_add_peer(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_espnow_add_peer_t, r);
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, r->peer_mac, 6);
    memcpy(p.lmk, r->lmk, 16);
    p.channel = r->channel;
    p.ifidx   = (r->wifi_if == 1) ? WIFI_IF_AP : WIFI_IF_STA;
    p.encrypt = r->encrypt != 0;
    esp_err_t err = esp_now_add_peer(&p);
    send_simple_resp(PHY_RPC_RESP_ESPNOW_ADD_PEER, _op, err);
}

static void on_espnow_del_peer(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_espnow_del_peer_t, r);
    esp_err_t err = esp_now_del_peer(r->peer_mac);
    send_simple_resp(PHY_RPC_RESP_ESPNOW_DEL_PEER, _op, err);
}

static void on_espnow_send(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_espnow_send_t, r);
    if (sizeof(*r) + r->data_len > len) {
        send_simple_resp(PHY_RPC_RESP_ESPNOW_SEND, _op, ESP_ERR_INVALID_SIZE);
        return;
    }
    esp_err_t err = esp_now_send(r->peer_mac, r->data, r->data_len);
    send_simple_resp(PHY_RPC_RESP_ESPNOW_SEND, _op, err);
}

/* ====================================================================
 * 802.15.4 — weak-link the API so we still build on chips without it
 * ================================================================== */

extern esp_err_t esp_ieee802154_enable        (void)             __attribute__((weak));
extern esp_err_t esp_ieee802154_disable       (void)             __attribute__((weak));
extern esp_err_t esp_ieee802154_set_channel   (uint8_t ch)       __attribute__((weak));
extern esp_err_t esp_ieee802154_set_panid     (uint16_t pan)     __attribute__((weak));
extern esp_err_t esp_ieee802154_set_promiscuous(bool en)         __attribute__((weak));
extern esp_err_t esp_ieee802154_transmit      (const uint8_t *frame, bool cca) __attribute__((weak));

/* The 15.4 IDF API delivers received frames via this weak callback
 * (we override the default empty one). */
__attribute__((weak)) void esp_ieee802154_receive_done(uint8_t *frame, void *frame_info)
{
    /* frame[0] is the PHY-level length byte (PHR). */
    if (!frame) return;
    uint8_t plen = frame[0];
    if (plen < 2 || plen > 127) return;
    size_t total = sizeof(phy_rpc_evt_ieee154_rx_t) + plen;
    uint8_t *buf = malloc(total);
    if (!buf) return;
    phy_rpc_evt_ieee154_rx_t *ev = (phy_rpc_evt_ieee154_rx_t *)buf;
    memset(ev, 0, sizeof(*ev));
    /* frame_info is opaque (esp_ieee802154_frame_info_t*); fields we care
     * about (rssi, lqi, channel, timestamp) are accessed via inline
     * helpers in IDF. We stay protocol-agnostic and leave them at 0
     * if the helpers aren't available — host gets the raw frame. */
    ev->frame_len    = plen;
    memcpy(ev->frame, frame + 1, plen);
    esp_hosted_send_custom_data(PHY_RPC_EVT_IEEE154_RX, buf, total);
    free(buf);
}

#define I154_OR_NS(call_expr)  do { if (!esp_ieee802154_enable) { st = ESP_ERR_NOT_SUPPORTED; } else { st = (call_expr); } } while (0)

static void on_i154_enable(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_ieee154_enable_t, r);
    esp_err_t st = ESP_OK;
    if (r->enable) I154_OR_NS(esp_ieee802154_enable());
    else if (esp_ieee802154_disable) st = esp_ieee802154_disable();
    else st = ESP_ERR_NOT_SUPPORTED;
    send_simple_resp(PHY_RPC_RESP_IEEE154_ENABLE, _op, st);
}

static void on_i154_set_chan(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_ieee154_set_chan_t, r);
    esp_err_t st = ESP_OK;
    if (!esp_ieee802154_set_channel) st = ESP_ERR_NOT_SUPPORTED;
    else if (r->channel < 11 || r->channel > 26) st = ESP_ERR_INVALID_ARG;
    else st = esp_ieee802154_set_channel(r->channel);
    send_simple_resp(PHY_RPC_RESP_IEEE154_SET_CHAN, _op, st);
}

static void on_i154_set_panid(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_ieee154_set_panid_t, r);
    esp_err_t st = esp_ieee802154_set_panid ? esp_ieee802154_set_panid(r->pan_id) : ESP_ERR_NOT_SUPPORTED;
    send_simple_resp(PHY_RPC_RESP_IEEE154_SET_PANID, _op, st);
}

static void on_i154_set_promisc(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_ieee154_set_promisc_t, r);
    esp_err_t st = esp_ieee802154_set_promiscuous ?
                   esp_ieee802154_set_promiscuous(r->enable != 0) :
                   ESP_ERR_NOT_SUPPORTED;
    send_simple_resp(PHY_RPC_RESP_IEEE154_SET_PROMISC, _op, st);
}

static void on_i154_tx_raw(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_ieee154_tx_raw_t, r);
    esp_err_t st = ESP_OK;
    if (sizeof(*r) + r->frame_len > len) st = ESP_ERR_INVALID_SIZE;
    else if (!esp_ieee802154_transmit) st = ESP_ERR_NOT_SUPPORTED;
    else {
        /* IEEE 802.15.4 frames need a leading PHR (PHY-level length byte). */
        uint8_t buf[128];
        if (r->frame_len > 127) { st = ESP_ERR_INVALID_SIZE; }
        else {
            buf[0] = (uint8_t)r->frame_len;
            memcpy(buf + 1, r->frame, r->frame_len);
            st = esp_ieee802154_transmit(buf, r->cca != 0);
        }
    }
    send_simple_resp(PHY_RPC_RESP_IEEE154_TX_RAW, _op, st);
}

/* ---------- caps reporting ------------------------------------- */

#define MARK(caps, req_id)                                           \
    do { uint32_t low = (req_id) & 0xFFu;                            \
         if (low < PHY_RPC_CAPS_BYTES * 8)                           \
             (caps)[low / 8] |= 1u << (low % 8); } while (0)

void phy_rpc_wireless_fill_caps(uint8_t caps[PHY_RPC_CAPS_BYTES])
{
    /* ESP-NOW: public API, present on every Wi-Fi chip. */
    MARK(caps, PHY_RPC_REQ_ESPNOW_INIT);
    MARK(caps, PHY_RPC_REQ_ESPNOW_DEINIT);
    MARK(caps, PHY_RPC_REQ_ESPNOW_ADD_PEER);
    MARK(caps, PHY_RPC_REQ_ESPNOW_DEL_PEER);
    MARK(caps, PHY_RPC_REQ_ESPNOW_SEND);
    MARK(caps, PHY_RPC_REQ_ESPNOW_SET_PMK);

    /* 802.15.4: weak-linked. Only mark present on chips that ship the
     * radio (C6/H2/H4 — esp_ieee802154_enable resolves there). */
    if (esp_ieee802154_enable) {
        MARK(caps, PHY_RPC_REQ_IEEE154_ENABLE);
        MARK(caps, PHY_RPC_REQ_IEEE154_SET_CHAN);
        MARK(caps, PHY_RPC_REQ_IEEE154_SET_PANID);
        MARK(caps, PHY_RPC_REQ_IEEE154_SET_PROMISC);
        MARK(caps, PHY_RPC_REQ_IEEE154_TX_RAW);
    }
}

#undef MARK

/* ---------- registration --------------------------------------- */

void phy_rpc_wireless_register(void)
{
    struct { uint32_t id; void (*cb)(uint32_t, const uint8_t *, size_t, void *); } table[] = {
        { PHY_RPC_REQ_ESPNOW_INIT,        on_espnow_init },
        { PHY_RPC_REQ_ESPNOW_DEINIT,      on_espnow_deinit },
        { PHY_RPC_REQ_ESPNOW_SET_PMK,     on_espnow_set_pmk },
        { PHY_RPC_REQ_ESPNOW_ADD_PEER,    on_espnow_add_peer },
        { PHY_RPC_REQ_ESPNOW_DEL_PEER,    on_espnow_del_peer },
        { PHY_RPC_REQ_ESPNOW_SEND,        on_espnow_send },
        { PHY_RPC_REQ_IEEE154_ENABLE,     on_i154_enable },
        { PHY_RPC_REQ_IEEE154_SET_CHAN,   on_i154_set_chan },
        { PHY_RPC_REQ_IEEE154_SET_PANID,  on_i154_set_panid },
        { PHY_RPC_REQ_IEEE154_SET_PROMISC, on_i154_set_promisc },
        { PHY_RPC_REQ_IEEE154_TX_RAW,     on_i154_tx_raw },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); i++) {
        esp_err_t err = esp_hosted_register_custom_callback(table[i].id, table[i].cb, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %08" PRIx32 ": %s", table[i].id, esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "%zu wireless RPC handlers registered (ESP-NOW + 802.15.4)",
             sizeof(table) / sizeof(*table));
}
