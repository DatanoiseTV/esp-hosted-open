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
    };
    for (size_t i = 0; i < sizeof(resp_ids) / sizeof(*resp_ids); i++) {
        esp_err_t err = esp_hosted_register_custom_callback(resp_ids[i], on_response, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %08" PRIx32 ": %s", resp_ids[i], esp_err_to_name(err));
            return err;
        }
    }
    ESP_ERROR_CHECK(esp_hosted_register_custom_callback(PHY_RPC_EVT_OCB_FRAME, on_event_ocb, NULL));

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

esp_err_t esp_hosted_open_get_caps(uint8_t caps[4])
{
    if (!caps) return ESP_ERR_INVALID_ARG;
    size_t got = 0;
    return do_call(PHY_RPC_REQ_GET_CAPS, PHY_RPC_RESP_GET_CAPS,
                   NULL, 0, caps, 4, &got);
}

bool esp_hosted_open_has_capability(uint32_t request_msg_id, const uint8_t caps[4])
{
    /* Low byte of msg_id is the index into our cap bitmap (0x000–0x01F). */
    uint32_t low = request_msg_id & 0xFFu;
    if (low > 0x1F) return false;
    return (caps[low / 8] & (1u << (low % 8))) != 0;
}
