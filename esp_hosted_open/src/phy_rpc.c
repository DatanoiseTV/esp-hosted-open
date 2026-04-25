/*
 * phy_rpc.c — host-side dispatcher for the open PHY-control RPCs.
 *
 * Maps each esp_hosted_open_* function to a request/response round
 * trip over esp-hosted's generic custom-data channel. The slave's
 * phy_rpc_overlay component registers the matching handlers.
 */

#include <string.h>
#include <stdlib.h>

#include "esp_hosted_open.h"
#include "phy_rpc_proto.h"

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_hosted_misc.h"          /* esp_hosted_send_custom_data, register */

static const char *TAG = "phy_rpc";

#define MAX_INFLIGHT  4

static struct {
    SemaphoreHandle_t              done;
    uint32_t                       op_id;
    uint32_t                       want_resp_id;
    void                          *out_buf;
    size_t                         out_cap;
    size_t                         out_len;
    int32_t                        status;
    bool                           in_use;
} s_inflight[MAX_INFLIGHT];

static SemaphoreHandle_t s_inflight_mutex;
static uint32_t          s_next_op_id = 1;
static uint32_t          s_timeout_ms = 1500;

static esp_hosted_open_ocb_rx_cb_t s_ocb_cb;
static void                        *s_ocb_ctx;

static esp_hosted_open_raw_rx_cb_t  s_raw_cb;
static void                        *s_raw_ctx;
static esp_hosted_open_csi_cb_t     s_csi_cb;
static void                        *s_csi_ctx;
static esp_hosted_open_ftm_cb_t     s_ftm_cb;
static void                        *s_ftm_ctx;
static esp_hosted_open_espnow_rx_cb_t s_espnow_rx_cb;
static void                          *s_espnow_rx_ctx;
static esp_hosted_open_espnow_tx_cb_t s_espnow_tx_cb;
static void                          *s_espnow_tx_ctx;
static esp_hosted_open_ieee154_rx_cb_t s_ieee154_rx_cb;
static void                           *s_ieee154_rx_ctx;

/* ---------- request/response plumbing ------------------------------ */

static struct slot { /* alias */
    SemaphoreHandle_t done;
} *find_inflight_unused(void)
{
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        if (!s_inflight[i].in_use) return (struct slot *)&s_inflight[i];
    }
    return NULL;
}

static void on_response(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_resp_hdr_t)) {
        ESP_LOGW(TAG, "short response on msg %08" PRIx32, msg_id);
        return;
    }
    const phy_rpc_resp_hdr_t *rhdr = (const phy_rpc_resp_hdr_t *)data;

    xSemaphoreTake(s_inflight_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        if (s_inflight[i].in_use &&
            s_inflight[i].op_id == rhdr->op_id &&
            s_inflight[i].want_resp_id == msg_id) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        xSemaphoreGive(s_inflight_mutex);
        ESP_LOGD(TAG, "response op_id=%" PRIu32 " msg=%08" PRIx32 " — no waiter",
                 rhdr->op_id, msg_id);
        return;
    }
    s_inflight[slot].status = rhdr->status;
    if (s_inflight[slot].out_buf && len > sizeof(*rhdr)) {
        size_t copy = len - sizeof(*rhdr);
        if (copy > s_inflight[slot].out_cap) copy = s_inflight[slot].out_cap;
        memcpy(s_inflight[slot].out_buf, data + sizeof(*rhdr), copy);
        s_inflight[slot].out_len = copy;
    }
    xSemaphoreGive(s_inflight[slot].done);
    xSemaphoreGive(s_inflight_mutex);
}

static esp_err_t do_call(uint32_t req_msg_id, uint32_t resp_msg_id,
                         const void *req_body, size_t req_body_len,
                         void *resp_body, size_t resp_body_cap,
                         size_t *resp_body_len)
{
    if (!s_inflight_mutex) return ESP_ERR_INVALID_STATE;

    /* Build request: phy_rpc_hdr_t + body */
    size_t total = sizeof(phy_rpc_hdr_t) + req_body_len;
    uint8_t *buf = malloc(total);
    if (!buf) return ESP_ERR_NO_MEM;
    phy_rpc_hdr_t *hdr = (phy_rpc_hdr_t *)buf;

    /* Reserve a slot */
    xSemaphoreTake(s_inflight_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        if (!s_inflight[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        xSemaphoreGive(s_inflight_mutex);
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    hdr->op_id                 = s_next_op_id++;
    s_inflight[slot].op_id     = hdr->op_id;
    s_inflight[slot].want_resp_id = resp_msg_id;
    s_inflight[slot].out_buf   = resp_body;
    s_inflight[slot].out_cap   = resp_body_cap;
    s_inflight[slot].out_len   = 0;
    s_inflight[slot].status    = ESP_FAIL;
    s_inflight[slot].in_use    = true;
    xSemaphoreGive(s_inflight_mutex);

    if (req_body_len) memcpy(buf + sizeof(*hdr), req_body, req_body_len);

    esp_err_t err = esp_hosted_send_custom_data(req_msg_id, buf, total);
    free(buf);
    if (err != ESP_OK) {
        s_inflight[slot].in_use = false;
        return err;
    }

    if (xSemaphoreTake(s_inflight[slot].done, pdMS_TO_TICKS(s_timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "RPC timeout msg=%08" PRIx32, req_msg_id);
        s_inflight[slot].in_use = false;
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t st = (esp_err_t)s_inflight[slot].status;
    if (resp_body_len) *resp_body_len = s_inflight[slot].out_len;
    s_inflight[slot].in_use = false;
    return st;
}

/* ---------- async event channel ------------------------------------ */

static void on_event_ocb(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    if (len < sizeof(phy_rpc_evt_ocb_frame_t)) return;
    const phy_rpc_evt_ocb_frame_t *e = (const phy_rpc_evt_ocb_frame_t *)data;
    if (sizeof(*e) + e->llc_payload_len > len) return;

    if (s_ocb_cb) {
        esp_hosted_open_rx_meta_t m = {
            .rssi_dbm     = e->rssi_dbm,
            .channel      = e->channel,
            .timestamp_us = e->timestamp_us,
        };
        memcpy(m.src_mac, e->src_mac, 6);
        s_ocb_cb(e->data, e->llc_payload_len, &m, s_ocb_ctx);
    }
}

esp_err_t esp_hosted_open_register_ocb_rx_cb(esp_hosted_open_ocb_rx_cb_t cb, void *ctx)
{
    s_ocb_cb  = cb;
    s_ocb_ctx = ctx;
    return ESP_OK;
}

static void on_event_raw(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    if (len < sizeof(phy_rpc_evt_raw_frame_t)) return;
    const phy_rpc_evt_raw_frame_t *e = (const phy_rpc_evt_raw_frame_t *)data;
    if (sizeof(*e) + e->frame_len > len) return;
    if (!s_raw_cb) return;
    esp_hosted_open_raw_meta_t m = {
        .rssi_dbm     = e->rssi_dbm,
        .channel      = e->channel,
        .rate         = e->rate,
        .is_qos       = e->is_qos != 0,
        .timestamp_us = e->timestamp_us,
    };
    s_raw_cb(e->frame, e->frame_len, &m, s_raw_ctx);
}

static void on_event_csi(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    if (len < sizeof(phy_rpc_evt_csi_t)) return;
    const phy_rpc_evt_csi_t *e = (const phy_rpc_evt_csi_t *)data;
    if (sizeof(*e) + e->csi_len > len) return;
    if (!s_csi_cb) return;
    s_csi_cb(e->csi, e->csi_len, e->rssi_dbm, e->channel, e->src_mac,
             e->timestamp_us, s_csi_ctx);
}

static void on_event_ftm(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    if (len < sizeof(phy_rpc_evt_ftm_report_t)) return;
    const phy_rpc_evt_ftm_report_t *e = (const phy_rpc_evt_ftm_report_t *)data;
    if (!s_ftm_cb) return;
    esp_hosted_open_ftm_report_t r = {
        .status      = e->status,
        .rtt_raw_ps  = e->rtt_raw_ps,
        .rtt_est_ps  = e->rtt_est_ps,
        .dist_est_cm = e->dist_est_cm,
    };
    memcpy(r.peer_mac, e->peer_mac, 6);
    s_ftm_cb(&r, s_ftm_ctx);
}

esp_err_t esp_hosted_open_register_raw_rx_cb(esp_hosted_open_raw_rx_cb_t cb, void *ctx)
{ s_raw_cb = cb; s_raw_ctx = ctx; return ESP_OK; }

esp_err_t esp_hosted_open_register_csi_cb(esp_hosted_open_csi_cb_t cb, void *ctx)
{ s_csi_cb = cb; s_csi_ctx = ctx; return ESP_OK; }

esp_err_t esp_hosted_open_register_ftm_cb(esp_hosted_open_ftm_cb_t cb, void *ctx)
{ s_ftm_cb = cb; s_ftm_ctx = ctx; return ESP_OK; }

static void on_event_espnow_rx(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    if (len < sizeof(phy_rpc_evt_espnow_rx_t)) return;
    const phy_rpc_evt_espnow_rx_t *e = (const phy_rpc_evt_espnow_rx_t *)data;
    if (sizeof(*e) + e->data_len > len) return;
    if (!s_espnow_rx_cb) return;
    esp_hosted_open_espnow_meta_t m = {
        .rssi_dbm = e->rssi_dbm,
        .channel  = e->channel,
    };
    memcpy(m.src_mac, e->src_mac, 6);
    memcpy(m.dst_mac, e->dst_mac, 6);
    s_espnow_rx_cb(e->data, e->data_len, &m, s_espnow_rx_ctx);
}

static void on_event_espnow_tx(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    if (len < sizeof(phy_rpc_evt_espnow_tx_status_t)) return;
    const phy_rpc_evt_espnow_tx_status_t *e = (const phy_rpc_evt_espnow_tx_status_t *)data;
    if (s_espnow_tx_cb) s_espnow_tx_cb(e->peer_mac, e->status == 0, s_espnow_tx_ctx);
}

static void on_event_ieee154_rx(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    if (len < sizeof(phy_rpc_evt_ieee154_rx_t)) return;
    const phy_rpc_evt_ieee154_rx_t *e = (const phy_rpc_evt_ieee154_rx_t *)data;
    if (sizeof(*e) + e->frame_len > len) return;
    if (!s_ieee154_rx_cb) return;
    esp_hosted_open_ieee154_meta_t m = {
        .rssi_dbm     = e->rssi_dbm,
        .lqi          = e->lqi,
        .channel      = e->channel,
        .timestamp_us = e->timestamp_us,
    };
    s_ieee154_rx_cb(e->frame, e->frame_len, &m, s_ieee154_rx_ctx);
}

esp_err_t esp_hosted_open_register_espnow_rx_cb(esp_hosted_open_espnow_rx_cb_t cb, void *ctx)
{ s_espnow_rx_cb = cb; s_espnow_rx_ctx = ctx; return ESP_OK; }

esp_err_t esp_hosted_open_register_espnow_tx_cb(esp_hosted_open_espnow_tx_cb_t cb, void *ctx)
{ s_espnow_tx_cb = cb; s_espnow_tx_ctx = ctx; return ESP_OK; }

esp_err_t esp_hosted_open_register_ieee154_rx_cb(esp_hosted_open_ieee154_rx_cb_t cb, void *ctx)
{ s_ieee154_rx_cb = cb; s_ieee154_rx_ctx = ctx; return ESP_OK; }

/* ---------- init / teardown --------------------------------------- */

esp_err_t esp_hosted_open_init(void)
{
    if (s_inflight_mutex) return ESP_ERR_INVALID_STATE;
    s_inflight_mutex = xSemaphoreCreateMutex();
    if (!s_inflight_mutex) return ESP_ERR_NO_MEM;
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        s_inflight[i].done = xSemaphoreCreateBinary();
    }

    /* Register one callback per response/event id we care about. */
    static const uint32_t resp_ids[] = {
        PHY_RPC_RESP_SET_CHANNEL,        PHY_RPC_RESP_SET_PHY_11P,
        PHY_RPC_RESP_SET_TX_POWER,       PHY_RPC_RESP_SET_RX_GAIN,
        PHY_RPC_RESP_SET_AGC_MAX_GAIN,   PHY_RPC_RESP_SET_CCA,
        PHY_RPC_RESP_GET_CCA_COUNTERS,   PHY_RPC_RESP_RESET_CCA_COUNT,
        PHY_RPC_RESP_SET_LOW_RATE,       PHY_RPC_RESP_SET_BANDWIDTH,
        PHY_RPC_RESP_GET_PHY_RSSI,       PHY_RPC_RESP_SET_COUNTRY_PERM,
        PHY_RPC_RESP_GET_INFO,
        PHY_RPC_RESP_SET_FREQ,           PHY_RPC_RESP_SET_BAND,
        PHY_RPC_RESP_SET_CHAN_FILT,      PHY_RPC_RESP_SET_CHAN14_MIC,
        PHY_RPC_RESP_SET_RATE,           PHY_RPC_RESP_SET_MOST_TPW,
        PHY_RPC_RESP_GET_MOST_TPW,       PHY_RPC_RESP_RESET_RX_GAIN_TBL,
        PHY_RPC_RESP_GET_NOISE_FLOOR,    PHY_RPC_RESP_GET_TEMPERATURE,
        PHY_RPC_RESP_SET_CSI_DUMP,       PHY_RPC_RESP_SET_LOOPBACK,
        PHY_RPC_RESP_SET_BT_TX_GAIN,     PHY_RPC_RESP_SET_BT_FILTER,
        PHY_RPC_RESP_GET_CAPS,
        PHY_RPC_RESP_TX_80211,           PHY_RPC_RESP_SET_PROMISC,
        PHY_RPC_RESP_SET_PROMISC_FILTER, PHY_RPC_RESP_SET_PROTOCOL,
        PHY_RPC_RESP_SET_MAC,            PHY_RPC_RESP_SET_CSI_ENABLE,
        PHY_RPC_RESP_FTM_INITIATE,
        PHY_RPC_RESP_ESPNOW_INIT,        PHY_RPC_RESP_ESPNOW_DEINIT,
        PHY_RPC_RESP_ESPNOW_ADD_PEER,    PHY_RPC_RESP_ESPNOW_DEL_PEER,
        PHY_RPC_RESP_ESPNOW_SEND,        PHY_RPC_RESP_ESPNOW_SET_PMK,
        PHY_RPC_RESP_IEEE154_ENABLE,     PHY_RPC_RESP_IEEE154_SET_CHAN,
        PHY_RPC_RESP_IEEE154_SET_PANID,  PHY_RPC_RESP_IEEE154_SET_PROMISC,
        PHY_RPC_RESP_IEEE154_TX_RAW,
        PHY_RPC_RESP_SET_EVENT_MASK,
    };
    for (size_t i = 0; i < sizeof(resp_ids) / sizeof(*resp_ids); i++) {
        esp_err_t err = esp_hosted_register_custom_callback(resp_ids[i], on_response, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %08" PRIx32 ": %s", resp_ids[i], esp_err_to_name(err));
            return err;
        }
    }
    ESP_ERROR_CHECK(esp_hosted_register_custom_callback(PHY_RPC_EVT_OCB_FRAME,  on_event_ocb, NULL));
    ESP_ERROR_CHECK(esp_hosted_register_custom_callback(PHY_RPC_EVT_RAW_FRAME,  on_event_raw, NULL));
    ESP_ERROR_CHECK(esp_hosted_register_custom_callback(PHY_RPC_EVT_CSI,        on_event_csi, NULL));
    ESP_ERROR_CHECK(esp_hosted_register_custom_callback(PHY_RPC_EVT_FTM_REPORT, on_event_ftm, NULL));
    ESP_ERROR_CHECK(esp_hosted_register_custom_callback(PHY_RPC_EVT_ESPNOW_RX,        on_event_espnow_rx, NULL));
    ESP_ERROR_CHECK(esp_hosted_register_custom_callback(PHY_RPC_EVT_ESPNOW_TX_STATUS, on_event_espnow_tx, NULL));
    ESP_ERROR_CHECK(esp_hosted_register_custom_callback(PHY_RPC_EVT_IEEE154_RX,       on_event_ieee154_rx, NULL));

    ESP_LOGI(TAG, "esp-hosted-open PHY RPC layer up");
    return ESP_OK;
}

esp_err_t esp_hosted_open_deinit(void)
{
    /* esp-hosted doesn't expose a deregister-by-id; pass NULL callback. */
    static const uint32_t all_ids[] = {
        PHY_RPC_RESP_SET_CHANNEL, PHY_RPC_RESP_SET_PHY_11P, PHY_RPC_RESP_SET_TX_POWER,
        PHY_RPC_RESP_SET_RX_GAIN, PHY_RPC_RESP_SET_AGC_MAX_GAIN, PHY_RPC_RESP_SET_CCA,
        PHY_RPC_RESP_GET_CCA_COUNTERS, PHY_RPC_RESP_RESET_CCA_COUNT,
        PHY_RPC_RESP_SET_LOW_RATE, PHY_RPC_RESP_SET_BANDWIDTH,
        PHY_RPC_RESP_GET_PHY_RSSI, PHY_RPC_RESP_SET_COUNTRY_PERM, PHY_RPC_RESP_GET_INFO,
        PHY_RPC_RESP_SET_FREQ, PHY_RPC_RESP_SET_BAND, PHY_RPC_RESP_SET_CHAN_FILT,
        PHY_RPC_RESP_SET_CHAN14_MIC, PHY_RPC_RESP_SET_RATE, PHY_RPC_RESP_SET_MOST_TPW,
        PHY_RPC_RESP_GET_MOST_TPW, PHY_RPC_RESP_RESET_RX_GAIN_TBL,
        PHY_RPC_RESP_GET_NOISE_FLOOR, PHY_RPC_RESP_GET_TEMPERATURE,
        PHY_RPC_RESP_SET_CSI_DUMP, PHY_RPC_RESP_SET_LOOPBACK,
        PHY_RPC_RESP_SET_BT_TX_GAIN, PHY_RPC_RESP_SET_BT_FILTER,
        PHY_RPC_RESP_GET_CAPS,
        PHY_RPC_EVT_OCB_FRAME,
    };
    for (size_t i = 0; i < sizeof(all_ids) / sizeof(*all_ids); i++) {
        esp_hosted_register_custom_callback(all_ids[i], NULL, NULL);
    }
    if (s_inflight_mutex) { vSemaphoreDelete(s_inflight_mutex); s_inflight_mutex = NULL; }
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        if (s_inflight[i].done) vSemaphoreDelete(s_inflight[i].done);
    }
    return ESP_OK;
}

void esp_hosted_open_set_timeout_ms(uint32_t ms) { s_timeout_ms = ms; }

/* ---------- per-call wrappers ------------------------------------- */

#define ONE_BYTE_REQ(req_macro, resp_macro, type, value)                        \
    do {                                                                        \
        type body = (value);                                                    \
        return do_call((req_macro), (resp_macro), &body, sizeof(body),          \
                       NULL, 0, NULL);                                          \
    } while (0)

esp_err_t esp_hosted_open_set_channel(uint8_t ieee_channel)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_CHANNEL,      PHY_RPC_RESP_SET_CHANNEL,      uint8_t, ieee_channel); }

esp_err_t esp_hosted_open_set_phy_11p(bool enable)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_PHY_11P,      PHY_RPC_RESP_SET_PHY_11P,      uint8_t, enable ? 1 : 0); }

esp_err_t esp_hosted_open_set_tx_power(int8_t dbm)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_TX_POWER,     PHY_RPC_RESP_SET_TX_POWER,     int8_t, dbm); }

esp_err_t esp_hosted_open_set_rx_gain(uint8_t gain_index)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_RX_GAIN,      PHY_RPC_RESP_SET_RX_GAIN,      uint8_t, gain_index); }

esp_err_t esp_hosted_open_set_agc_max_gain(uint8_t max_gain)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_AGC_MAX_GAIN, PHY_RPC_RESP_SET_AGC_MAX_GAIN, uint8_t, max_gain); }

esp_err_t esp_hosted_open_set_cca(bool enable)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_CCA,          PHY_RPC_RESP_SET_CCA,          uint8_t, enable ? 1 : 0); }

esp_err_t esp_hosted_open_set_low_rate(bool enable)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_LOW_RATE,     PHY_RPC_RESP_SET_LOW_RATE,     uint8_t, enable ? 1 : 0); }

esp_err_t esp_hosted_open_set_bandwidth(uint8_t bw_mhz)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_BANDWIDTH,    PHY_RPC_RESP_SET_BANDWIDTH,    uint8_t, bw_mhz); }

esp_err_t esp_hosted_open_reset_cca_counters(void)
{
    return do_call(PHY_RPC_REQ_RESET_CCA_COUNT, PHY_RPC_RESP_RESET_CCA_COUNT,
                   NULL, 0, NULL, 0, NULL);
}

esp_err_t esp_hosted_open_set_country_permissive(void)
{
    return do_call(PHY_RPC_REQ_SET_COUNTRY_PERM, PHY_RPC_RESP_SET_COUNTRY_PERM,
                   NULL, 0, NULL, 0, NULL);
}

esp_err_t esp_hosted_open_get_cca_counters(uint32_t *busy_us, uint32_t *total_us)
{
    phy_rpc_resp_get_cca_counters_t body;
    size_t got = 0;
    esp_err_t err = do_call(PHY_RPC_REQ_GET_CCA_COUNTERS, PHY_RPC_RESP_GET_CCA_COUNTERS,
                            NULL, 0,
                            (uint8_t *)&body + sizeof(phy_rpc_resp_hdr_t),
                            sizeof(body) - sizeof(phy_rpc_resp_hdr_t), &got);
    if (err != ESP_OK) return err;
    if (busy_us)  *busy_us  = body.busy_us;
    if (total_us) *total_us = body.total_us;
    return ESP_OK;
}

esp_err_t esp_hosted_open_get_phy_rssi(int8_t *rssi_dbm)
{
    int8_t v = 0;
    size_t got = 0;
    esp_err_t err = do_call(PHY_RPC_REQ_GET_PHY_RSSI, PHY_RPC_RESP_GET_PHY_RSSI,
                            NULL, 0, &v, sizeof(v), &got);
    if (err != ESP_OK) return err;
    if (rssi_dbm) *rssi_dbm = v;
    return ESP_OK;
}

esp_err_t esp_hosted_open_get_info(esp_hosted_open_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    struct __attribute__((packed)) {
        uint8_t channel;
        int8_t  tx_power_dbm;
        uint8_t phy_11p_armed;
        uint8_t cca_enabled;
        uint8_t low_rate_enabled;
        uint8_t agc_max_gain;
        uint8_t reserved[2];
        char    fw_version[32];
    } body;
    size_t got = 0;
    esp_err_t err = do_call(PHY_RPC_REQ_GET_INFO, PHY_RPC_RESP_GET_INFO,
                            NULL, 0, &body, sizeof(body), &got);
    if (err != ESP_OK) return err;
    out->channel          = body.channel;
    out->tx_power_dbm     = body.tx_power_dbm;
    out->phy_11p_armed    = body.phy_11p_armed != 0;
    out->cca_enabled      = body.cca_enabled   != 0;
    out->low_rate_enabled = body.low_rate_enabled != 0;
    out->agc_max_gain     = body.agc_max_gain;
    memcpy(out->fw_version, body.fw_version, sizeof(out->fw_version));
    out->fw_version[sizeof(out->fw_version) - 1] = 0;
    return ESP_OK;
}

/* ---------- new wrappers covering the rest of the matrix --------- */

esp_err_t esp_hosted_open_set_freq(uint32_t freq_mhz)
{
    return do_call(PHY_RPC_REQ_SET_FREQ, PHY_RPC_RESP_SET_FREQ,
                   &freq_mhz, sizeof(freq_mhz), NULL, 0, NULL);
}

esp_err_t esp_hosted_open_set_band(uint8_t band)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_BAND, PHY_RPC_RESP_SET_BAND, uint8_t, band); }

esp_err_t esp_hosted_open_set_chan_filt(uint32_t mask)
{
    return do_call(PHY_RPC_REQ_SET_CHAN_FILT, PHY_RPC_RESP_SET_CHAN_FILT,
                   &mask, sizeof(mask), NULL, 0, NULL);
}

esp_err_t esp_hosted_open_set_chan14_micro(bool enable)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_CHAN14_MIC, PHY_RPC_RESP_SET_CHAN14_MIC, uint8_t, enable ? 1 : 0); }

esp_err_t esp_hosted_open_set_rate(uint8_t rate_idx)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_RATE, PHY_RPC_RESP_SET_RATE, uint8_t, rate_idx); }

esp_err_t esp_hosted_open_set_most_tpw(int8_t tpw)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_MOST_TPW, PHY_RPC_RESP_SET_MOST_TPW, int8_t, tpw); }

esp_err_t esp_hosted_open_get_most_tpw(int8_t *tpw)
{
    int8_t v = 0;
    size_t got = 0;
    esp_err_t err = do_call(PHY_RPC_REQ_GET_MOST_TPW, PHY_RPC_RESP_GET_MOST_TPW,
                            NULL, 0, &v, sizeof(v), &got);
    if (err != ESP_OK) return err;
    if (tpw) *tpw = v;
    return ESP_OK;
}

esp_err_t esp_hosted_open_reset_rx_gain_table(void)
{
    return do_call(PHY_RPC_REQ_RESET_RX_GAIN_TBL, PHY_RPC_RESP_RESET_RX_GAIN_TBL,
                   NULL, 0, NULL, 0, NULL);
}

esp_err_t esp_hosted_open_get_noise_floor(int8_t *dbm)
{
    int8_t v = 0;
    size_t got = 0;
    esp_err_t err = do_call(PHY_RPC_REQ_GET_NOISE_FLOOR, PHY_RPC_RESP_GET_NOISE_FLOOR,
                            NULL, 0, &v, sizeof(v), &got);
    if (err != ESP_OK) return err;
    if (dbm) *dbm = v;
    return ESP_OK;
}

esp_err_t esp_hosted_open_get_temperature(int8_t *celsius)
{
    int8_t v = 0;
    size_t got = 0;
    esp_err_t err = do_call(PHY_RPC_REQ_GET_TEMPERATURE, PHY_RPC_RESP_GET_TEMPERATURE,
                            NULL, 0, &v, sizeof(v), &got);
    if (err != ESP_OK) return err;
    if (celsius) *celsius = v;
    return ESP_OK;
}

esp_err_t esp_hosted_open_set_csi_dump(bool enable)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_CSI_DUMP, PHY_RPC_RESP_SET_CSI_DUMP, uint8_t, enable ? 1 : 0); }

esp_err_t esp_hosted_open_set_loopback(uint8_t gain)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_LOOPBACK, PHY_RPC_RESP_SET_LOOPBACK, uint8_t, gain); }

esp_err_t esp_hosted_open_set_bt_tx_gain(uint8_t gain)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_BT_TX_GAIN, PHY_RPC_RESP_SET_BT_TX_GAIN, uint8_t, gain); }

esp_err_t esp_hosted_open_set_bt_filter(uint32_t reg_value)
{
    return do_call(PHY_RPC_REQ_SET_BT_FILTER, PHY_RPC_RESP_SET_BT_FILTER,
                   &reg_value, sizeof(reg_value), NULL, 0, NULL);
}

esp_err_t esp_hosted_open_get_caps(uint8_t caps[16])
{
    if (!caps) return ESP_ERR_INVALID_ARG;
    memset(caps, 0, 16);
    size_t got = 0;
    return do_call(PHY_RPC_REQ_GET_CAPS, PHY_RPC_RESP_GET_CAPS,
                   NULL, 0, caps, PHY_RPC_CAPS_BYTES, &got);
}

bool esp_hosted_open_has_capability(uint32_t request_msg_id, const uint8_t caps[16])
{
    /* Low byte of msg_id is the bit index into our cap bitmap. 16 bytes
     * × 8 bits = 128 slots, covering msg ids 0x00–0x7F. */
    uint32_t low = request_msg_id & 0xFFu;
    if (low >= PHY_RPC_CAPS_BYTES * 8) return false;
    return (caps[low / 8] & (1u << (low % 8))) != 0;
}

/* ---------- raw TX / promisc / protocol / MAC / CSI / FTM -------- */

esp_err_t esp_hosted_open_tx_80211(uint8_t wifi_if, const uint8_t *frame,
                                   size_t len, bool en_seq_nr)
{
    if (!frame || !len || len > 1500) return ESP_ERR_INVALID_ARG;
    /* Build header(8) + frame */
    size_t total = 8 + len;
    uint8_t *body = malloc(total);
    if (!body) return ESP_ERR_NO_MEM;
    body[0] = wifi_if;
    body[1] = en_seq_nr ? 1 : 0;
    body[2] = body[3] = 0;          /* reserved */
    body[4] = (uint8_t)(len & 0xFF);
    body[5] = (uint8_t)((len >> 8) & 0xFF);
    body[6] = body[7] = 0;
    /* But we need to send the cits header (op_id) too, so use do_call's
     * own framing. The first 4 bytes added by do_call's wrapper are the
     * op_id; what we put in here is the 8-byte body header + frame. */
    /* Combine into one buffer for the do_call body argument. */
    uint8_t *full = malloc(8 + len);
    if (!full) { free(body); return ESP_ERR_NO_MEM; }
    full[0] = wifi_if;
    full[1] = en_seq_nr ? 1 : 0;
    full[2] = full[3] = 0;
    full[4] = (uint8_t)(len & 0xFF);
    full[5] = (uint8_t)((len >> 8) & 0xFF);
    full[6] = full[7] = 0;
    memcpy(full + 8, frame, len);
    free(body);
    esp_err_t err = do_call(PHY_RPC_REQ_TX_80211, PHY_RPC_RESP_TX_80211,
                            full, 8 + len, NULL, 0, NULL);
    free(full);
    return err;
}

esp_err_t esp_hosted_open_set_promisc(bool enable)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_PROMISC, PHY_RPC_RESP_SET_PROMISC, uint8_t, enable ? 1 : 0); }

esp_err_t esp_hosted_open_set_promisc_filter(uint32_t filter_mask)
{
    return do_call(PHY_RPC_REQ_SET_PROMISC_FILTER, PHY_RPC_RESP_SET_PROMISC_FILTER,
                   &filter_mask, sizeof(filter_mask), NULL, 0, NULL);
}

esp_err_t esp_hosted_open_set_protocol(uint8_t wifi_if, uint8_t protocol_mask)
{
    struct __attribute__((packed)) { uint8_t mask, iface, _r[2]; } body =
        { protocol_mask, wifi_if, {0, 0} };
    return do_call(PHY_RPC_REQ_SET_PROTOCOL, PHY_RPC_RESP_SET_PROTOCOL,
                   &body, sizeof(body), NULL, 0, NULL);
}

esp_err_t esp_hosted_open_set_mac(uint8_t wifi_if, const uint8_t mac[6])
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    struct __attribute__((packed)) { uint8_t iface, m[6], _r; } body;
    body.iface = wifi_if;
    memcpy(body.m, mac, 6);
    body._r = 0;
    return do_call(PHY_RPC_REQ_SET_MAC, PHY_RPC_RESP_SET_MAC,
                   &body, sizeof(body), NULL, 0, NULL);
}

esp_err_t esp_hosted_open_set_csi_enable(bool enable)
{ ONE_BYTE_REQ(PHY_RPC_REQ_SET_CSI_ENABLE, PHY_RPC_RESP_SET_CSI_ENABLE, uint8_t, enable ? 1 : 0); }

esp_err_t esp_hosted_open_ftm_initiate(const uint8_t peer_mac[6],
                                       uint8_t frames_count,
                                       uint8_t burst_period_100ms,
                                       uint8_t channel)
{
    if (!peer_mac) return ESP_ERR_INVALID_ARG;
    struct __attribute__((packed)) {
        uint8_t mac[6], frames, period, ch, _r[3];
    } body;
    memcpy(body.mac, peer_mac, 6);
    body.frames = frames_count;
    body.period = burst_period_100ms;
    body.ch     = channel;
    body._r[0] = body._r[1] = body._r[2] = 0;
    return do_call(PHY_RPC_REQ_FTM_INITIATE, PHY_RPC_RESP_FTM_INITIATE,
                   &body, sizeof(body), NULL, 0, NULL);
}

/* ---------- ESP-NOW -------------------------------------------- */

esp_err_t esp_hosted_open_espnow_init(void)
{ return do_call(PHY_RPC_REQ_ESPNOW_INIT, PHY_RPC_RESP_ESPNOW_INIT, NULL, 0, NULL, 0, NULL); }

esp_err_t esp_hosted_open_espnow_deinit(void)
{ return do_call(PHY_RPC_REQ_ESPNOW_DEINIT, PHY_RPC_RESP_ESPNOW_DEINIT, NULL, 0, NULL, 0, NULL); }

esp_err_t esp_hosted_open_espnow_set_pmk(const uint8_t pmk[16])
{
    if (!pmk) return ESP_ERR_INVALID_ARG;
    return do_call(PHY_RPC_REQ_ESPNOW_SET_PMK, PHY_RPC_RESP_ESPNOW_SET_PMK,
                   pmk, 16, NULL, 0, NULL);
}

esp_err_t esp_hosted_open_espnow_add_peer(const uint8_t peer_mac[6],
                                          const uint8_t lmk[16],
                                          uint8_t channel, uint8_t wifi_if, bool encrypt)
{
    if (!peer_mac) return ESP_ERR_INVALID_ARG;
    struct __attribute__((packed)) {
        uint8_t mac[6]; uint8_t lmk[16]; uint8_t ch; uint8_t iface; uint8_t enc; uint8_t _r;
    } body;
    memcpy(body.mac, peer_mac, 6);
    if (lmk) memcpy(body.lmk, lmk, 16); else memset(body.lmk, 0, 16);
    body.ch = channel; body.iface = wifi_if; body.enc = encrypt ? 1 : 0; body._r = 0;
    return do_call(PHY_RPC_REQ_ESPNOW_ADD_PEER, PHY_RPC_RESP_ESPNOW_ADD_PEER,
                   &body, sizeof(body), NULL, 0, NULL);
}

esp_err_t esp_hosted_open_espnow_del_peer(const uint8_t peer_mac[6])
{
    if (!peer_mac) return ESP_ERR_INVALID_ARG;
    return do_call(PHY_RPC_REQ_ESPNOW_DEL_PEER, PHY_RPC_RESP_ESPNOW_DEL_PEER,
                   peer_mac, 6, NULL, 0, NULL);
}

esp_err_t esp_hosted_open_espnow_send(const uint8_t peer_mac[6],
                                      const uint8_t *data, size_t len)
{
    if (!peer_mac || !data || !len || len > 250) return ESP_ERR_INVALID_ARG;
    size_t total = 6 + 2 + len;
    uint8_t *body = malloc(total);
    if (!body) return ESP_ERR_NO_MEM;
    memcpy(body, peer_mac, 6);
    body[6] = (uint8_t)(len & 0xFF);
    body[7] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(body + 8, data, len);
    esp_err_t err = do_call(PHY_RPC_REQ_ESPNOW_SEND, PHY_RPC_RESP_ESPNOW_SEND,
                            body, total, NULL, 0, NULL);
    free(body);
    return err;
}

/* ---------- 802.15.4 ------------------------------------------- */

esp_err_t esp_hosted_open_ieee154_enable(bool enable)
{ ONE_BYTE_REQ(PHY_RPC_REQ_IEEE154_ENABLE, PHY_RPC_RESP_IEEE154_ENABLE, uint8_t, enable ? 1 : 0); }

esp_err_t esp_hosted_open_ieee154_set_channel(uint8_t channel)
{ ONE_BYTE_REQ(PHY_RPC_REQ_IEEE154_SET_CHAN, PHY_RPC_RESP_IEEE154_SET_CHAN, uint8_t, channel); }

esp_err_t esp_hosted_open_ieee154_set_pan_id(uint16_t pan_id)
{
    return do_call(PHY_RPC_REQ_IEEE154_SET_PANID, PHY_RPC_RESP_IEEE154_SET_PANID,
                   &pan_id, sizeof(pan_id), NULL, 0, NULL);
}

esp_err_t esp_hosted_open_ieee154_set_promiscuous(bool enable)
{ ONE_BYTE_REQ(PHY_RPC_REQ_IEEE154_SET_PROMISC, PHY_RPC_RESP_IEEE154_SET_PROMISC, uint8_t, enable ? 1 : 0); }

esp_err_t esp_hosted_open_set_event_mask(uint64_t mask)
{
    return do_call(PHY_RPC_REQ_SET_EVENT_MASK, PHY_RPC_RESP_SET_EVENT_MASK,
                   &mask, sizeof(mask), NULL, 0, NULL);
}

esp_err_t esp_hosted_open_ieee154_tx_raw(const uint8_t *frame, size_t len, bool cca)
{
    if (!frame || !len || len > 127) return ESP_ERR_INVALID_ARG;
    size_t total = 4 + len;
    uint8_t *body = malloc(total);
    if (!body) return ESP_ERR_NO_MEM;
    body[0] = cca ? 1 : 0;
    body[1] = 0;
    body[2] = (uint8_t)(len & 0xFF);
    body[3] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(body + 4, frame, len);
    esp_err_t err = do_call(PHY_RPC_REQ_IEEE154_TX_RAW, PHY_RPC_RESP_IEEE154_TX_RAW,
                            body, total, NULL, 0, NULL);
    free(body);
    return err;
}
