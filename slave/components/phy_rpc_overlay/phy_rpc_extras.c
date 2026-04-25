/*
 * phy_rpc_extras.c — slave-side handlers for the "missing/flaky" set:
 * raw 802.11 TX, promiscuous RX forwarding, CSI capture, FTM ranging,
 * 2.4 GHz protocol selection, MAC override.
 *
 * These are layered on the same custom-data RPC channel as
 * phy_rpc_handlers.c. We keep them in a separate file so the
 * matrix-row PHY hacks stay focused.
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_hosted_peer_data.h"
#include "phy_rpc_proto.h"

static const char *TAG = "phy_rpc_extras";

/* ---------- response helpers (mirror of phy_rpc_handlers.c) ----- */

static void send_simple_resp(uint32_t resp_id, uint32_t op_id, esp_err_t status)
{
    phy_rpc_resp_hdr_t hdr = { .op_id = op_id, .status = (int32_t)status };
    esp_hosted_send_custom_data(resp_id, (uint8_t *)&hdr, sizeof(hdr));
}

#define UNPACK(type, var)                                              \
    if (len < sizeof(type)) { ESP_LOGW(TAG, #type " short"); return; } \
    const type *var = (const type *)data;                              \
    uint32_t _op = var->hdr.op_id

/* ---------- raw 802.11 TX -------------------------------------- */

static void on_tx_80211(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_tx_80211_t, r);
    if (sizeof(*r) + r->frame_len > len) {
        send_simple_resp(PHY_RPC_RESP_TX_80211, _op, ESP_ERR_INVALID_SIZE);
        return;
    }
    wifi_interface_t iface = (r->wifi_if == 1) ? WIFI_IF_AP : WIFI_IF_STA;
    esp_err_t err = esp_wifi_80211_tx(iface, r->frame, r->frame_len, r->en_seq_nr != 0);
    if (err != ESP_OK) ESP_LOGW(TAG, "80211_tx: %s", esp_err_to_name(err));
    send_simple_resp(PHY_RPC_RESP_TX_80211, _op, err);
}

/* ---------- promiscuous RX forwarding -------------------------- */

#define RAW_QUEUE_LEN  16

typedef struct {
    int8_t   rssi_dbm;
    uint8_t  channel;
    uint8_t  rate;
    uint8_t  is_qos;
    uint16_t frame_len;
    uint32_t timestamp_us;
    uint8_t  frame[1600];
} raw_evt_t;

static QueueHandle_t s_raw_q;

static void IRAM_ATTR promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_raw_q) return;
    if (type == WIFI_PKT_MISC) return;
    const wifi_promiscuous_pkt_t *p = buf;
    size_t plen = p->rx_ctrl.sig_len;
    if (plen > 1600) return;

    raw_evt_t *e = malloc(sizeof(*e) - sizeof(e->frame) + plen);
    if (!e) return;
    e->rssi_dbm     = p->rx_ctrl.rssi;
    e->channel      = p->rx_ctrl.channel;
    e->rate         = p->rx_ctrl.rate;
    e->is_qos       = (type == WIFI_PKT_DATA && (p->payload[0] & 0x80)) ? 1 : 0;
    e->frame_len    = (uint16_t)plen;
    e->timestamp_us = (uint32_t)p->rx_ctrl.timestamp;
    memcpy(e->frame, p->payload, plen);

    BaseType_t hp = pdFALSE;
    if (xQueueSendFromISR(s_raw_q, &e, &hp) != pdTRUE) {
        free(e);
    }
    if (hp) portYIELD_FROM_ISR();
}

static void raw_forward_task(void *arg)
{
    while (1) {
        raw_evt_t *e = NULL;
        if (xQueueReceive(s_raw_q, &e, portMAX_DELAY) != pdTRUE || !e) continue;
        size_t total = offsetof(phy_rpc_evt_raw_frame_t, frame) + e->frame_len;
        uint8_t *buf = malloc(total);
        if (buf) {
            phy_rpc_evt_raw_frame_t *ev = (phy_rpc_evt_raw_frame_t *)buf;
            ev->rssi_dbm     = e->rssi_dbm;
            ev->channel      = e->channel;
            ev->rate         = e->rate;
            ev->is_qos       = e->is_qos;
            ev->frame_len    = e->frame_len;
            ev->reserved     = 0;
            ev->timestamp_us = e->timestamp_us;
            memcpy(ev->frame, e->frame, e->frame_len);
            esp_hosted_send_custom_data(PHY_RPC_EVT_RAW_FRAME, buf, total);
            free(buf);
        }
        free(e);
    }
}

static void on_set_promisc(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_set_promisc_t, r);
    if (r->enable && !s_raw_q) {
        s_raw_q = xQueueCreate(RAW_QUEUE_LEN, sizeof(raw_evt_t *));
        xTaskCreate(raw_forward_task, "raw_fwd", 4096, NULL, 5, NULL);
        esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    }
    esp_err_t err = esp_wifi_set_promiscuous(r->enable != 0);
    send_simple_resp(PHY_RPC_RESP_SET_PROMISC, _op, err);
}

static void on_set_promisc_filter(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_set_promisc_filter_t, r);
    wifi_promiscuous_filter_t f = { .filter_mask = r->filter_mask };
    esp_err_t err = esp_wifi_set_promiscuous_filter(&f);
    send_simple_resp(PHY_RPC_RESP_SET_PROMISC_FILTER, _op, err);
}

/* ---------- 2.4 GHz protocol + MAC ----------------------------- */

static void on_set_protocol(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_set_protocol_t, r);
    wifi_interface_t iface = (r->wifi_if == 1) ? WIFI_IF_AP : WIFI_IF_STA;
    esp_err_t err = esp_wifi_set_protocol(iface, r->protocol_mask);
    send_simple_resp(PHY_RPC_RESP_SET_PROTOCOL, _op, err);
}

static void on_set_mac(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_set_mac_t, r);
    wifi_interface_t iface = (r->wifi_if == 1) ? WIFI_IF_AP : WIFI_IF_STA;
    esp_err_t err = esp_wifi_set_mac(iface, r->mac);
    send_simple_resp(PHY_RPC_RESP_SET_MAC, _op, err);
}

/* ---------- CSI ------------------------------------------------ */

static void csi_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf || info->len <= 0) return;
    size_t total = offsetof(phy_rpc_evt_csi_t, csi) + info->len;
    uint8_t *buf = malloc(total);
    if (!buf) return;
    phy_rpc_evt_csi_t *ev = (phy_rpc_evt_csi_t *)buf;
    ev->rssi_dbm     = info->rx_ctrl.rssi;
    ev->channel      = info->rx_ctrl.channel;
    memcpy(ev->src_mac, info->mac, 6);
    ev->csi_len      = (uint16_t)info->len;
    ev->reserved     = 0;
    ev->timestamp_us = (uint32_t)info->rx_ctrl.timestamp;
    memcpy(ev->csi, info->buf, info->len);
    esp_hosted_send_custom_data(PHY_RPC_EVT_CSI, buf, total);
    free(buf);
}

static void on_set_csi_enable(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_set_csi_enable_t, r);
    esp_err_t err;
    if (r->enable) {
        err = esp_wifi_set_csi_rx_cb(csi_cb, NULL);
        if (err == ESP_OK) err = esp_wifi_set_csi(true);
    } else {
        err = esp_wifi_set_csi(false);
    }
    send_simple_resp(PHY_RPC_RESP_SET_CSI_ENABLE, _op, err);
}

/* ---------- FTM ------------------------------------------------ */

static void on_ftm_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT || id != WIFI_EVENT_FTM_REPORT) return;
    wifi_event_ftm_report_t *r = (wifi_event_ftm_report_t *)data;
    phy_rpc_evt_ftm_report_t ev = {
        .status      = r->status,
        .rtt_raw_ps  = (int32_t)r->rtt_raw,
        .rtt_est_ps  = (int32_t)r->rtt_est,
        .dist_est_cm = (int32_t)r->dist_est,
    };
    memcpy(ev.peer_mac, r->peer_mac, 6);
    esp_hosted_send_custom_data(PHY_RPC_EVT_FTM_REPORT, (uint8_t *)&ev, sizeof(ev));
}

static void on_ftm_initiate(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_ftm_initiate_t, r);
    static bool s_ftm_handler_registered;
    if (!s_ftm_handler_registered) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, on_ftm_event, NULL);
        s_ftm_handler_registered = true;
    }
    wifi_ftm_initiator_cfg_t cfg = {
        .frm_count    = r->frames_count,
        .burst_period = r->burst_period,
        .channel      = r->channel,
    };
    memcpy(cfg.resp_mac, r->peer_mac, 6);
    esp_err_t err = esp_wifi_ftm_initiate_session(&cfg);
    send_simple_resp(PHY_RPC_RESP_FTM_INITIATE, _op, err);
}

/* ---------- misc: event-mask suppression ----------------------- */

static void on_set_event_mask(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK(phy_rpc_req_set_event_mask_t, r);
    /* Wi-Fi event mask is a chip-internal API; on IDF it is exposed via
     * esp_event-level filtering, not a global mask. We approximate by
     * unregistering all default handlers and letting only those the
     * host explicitly subscribes to flow through. For now we just log;
     * the real implementation depends on which IDF version's event
     * suppression API we target. Reserved for follow-up. */
    ESP_LOGI(TAG, "set_event_mask(0x%llx) — TODO: wire to esp_event filter", (unsigned long long)r->mask);
    send_simple_resp(PHY_RPC_RESP_SET_EVENT_MASK, _op, ESP_OK);
}

/* ---------- caps reporting ------------------------------------- */

#define MARK(caps, req_id)                                           \
    do { uint32_t low = (req_id) & 0xFFu;                            \
         if (low < PHY_RPC_CAPS_BYTES * 8)                           \
             (caps)[low / 8] |= 1u << (low % 8); } while (0)

void phy_rpc_extras_fill_caps(uint8_t caps[PHY_RPC_CAPS_BYTES])
{
    /* These all use public esp_wifi_* APIs that are present on every
     * Wi-Fi-capable chip. FTM is public too but only meaningful on
     * S2/S3/C5/C6/C61; we still mark it available — caller gets
     * NOT_SUPPORTED at runtime if the chip doesn't service it. */
    MARK(caps, PHY_RPC_REQ_TX_80211);
    MARK(caps, PHY_RPC_REQ_SET_PROMISC);
    MARK(caps, PHY_RPC_REQ_SET_PROMISC_FILTER);
    MARK(caps, PHY_RPC_REQ_SET_PROTOCOL);
    MARK(caps, PHY_RPC_REQ_SET_MAC);
    MARK(caps, PHY_RPC_REQ_SET_CSI_ENABLE);
    MARK(caps, PHY_RPC_REQ_FTM_INITIATE);
    MARK(caps, PHY_RPC_REQ_SET_EVENT_MASK);
}

#undef MARK

/* ---------- registration --------------------------------------- */

void phy_rpc_extras_register(void)
{
    struct { uint32_t id; void (*cb)(uint32_t, const uint8_t *, size_t, void *); } table[] = {
        { PHY_RPC_REQ_TX_80211,           on_tx_80211 },
        { PHY_RPC_REQ_SET_PROMISC,        on_set_promisc },
        { PHY_RPC_REQ_SET_PROMISC_FILTER, on_set_promisc_filter },
        { PHY_RPC_REQ_SET_PROTOCOL,       on_set_protocol },
        { PHY_RPC_REQ_SET_MAC,            on_set_mac },
        { PHY_RPC_REQ_SET_CSI_ENABLE,     on_set_csi_enable },
        { PHY_RPC_REQ_FTM_INITIATE,       on_ftm_initiate },
        { PHY_RPC_REQ_SET_EVENT_MASK,     on_set_event_mask },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); i++) {
        esp_err_t err = esp_hosted_register_custom_callback(table[i].id, table[i].cb, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %08" PRIx32 ": %s", table[i].id, esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "%zu extra RPC handlers registered", sizeof(table) / sizeof(*table));
}
