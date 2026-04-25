/*
 * Slave-side handlers for the open PHY-control RPCs.
 *
 * Each handler receives the raw request body (already framed by
 * esp-hosted's pserial layer), looks at our phy_rpc_hdr_t for the
 * op_id, performs the action by calling the right phy_* symbol from
 * libphy.a, and answers with esp_hosted_send_custom_data().
 */

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"

#include "esp_hosted_peer_data.h"      /* slave-side analog of esp_hosted_misc.h */
#include "phy_rpc_proto.h"

static const char *TAG = "phy_rpc_slave";

extern void     phy_11p_set        (int enable, int reserved);
extern void     phy_change_channel  (int channel, int a, int b, int ht_mode);
extern void     phy_force_rx_gain   (int gain) __attribute__((weak));
extern void     phy_rx_gain_force   (int en)   __attribute__((weak));
extern void     phy_disable_agc     (void)     __attribute__((weak));
extern void     phy_enable_agc      (void)     __attribute__((weak));
extern void     phy_agc_max_gain_set(int g)    __attribute__((weak));
extern void     phy_disable_cca     (void)     __attribute__((weak));
extern void     phy_enable_cca      (void)     __attribute__((weak));
extern void     phy_set_cca_cnt     (uint32_t) __attribute__((weak));
extern uint32_t phy_get_cca_cnt     (void)     __attribute__((weak));
extern int      phy_get_rssi        (void)     __attribute__((weak));
extern int      phy_get_sigrssi     (void)     __attribute__((weak));
extern void     phy_disable_low_rate(void)     __attribute__((weak));
extern void     phy_enable_low_rate (void)     __attribute__((weak));
extern void     phy_bb_bss_cbw40    (int en)   __attribute__((weak));

/* Extra symbols covering the rest of the matrix. All weak: chips that
 * don't ship the symbol simply return NOT_SUPPORTED. */
extern void     phy_set_freq            (int freq_mhz)       __attribute__((weak));
extern void     phy_band_change         (int band)           __attribute__((weak));
extern void     phy_band_sel            (int band)           __attribute__((weak));
extern void     phy_chan_filt_set       (uint32_t mask)      __attribute__((weak));
extern void     phy_chan14_mic_cfg_new  (int enable)         __attribute__((weak));
extern void     phy_set_rate            (int rate)           __attribute__((weak));
extern void     phy_set_most_tpw        (int tpw)            __attribute__((weak));
extern int      phy_get_most_tpw        (void)               __attribute__((weak));
extern void     phy_set_rx_gain_table   (int idx)            __attribute__((weak));
extern int      phy_get_noise_floor     (void)               __attribute__((weak));
extern int      phy_xpd_tsens           (void)               __attribute__((weak));
extern void     phy_csidump_force_lltf_cfg(int enable)       __attribute__((weak));
extern void     phy_set_loopback_gain   (int gain)           __attribute__((weak));
extern void     phy_bt_set_tx_gain_new  (int gain)           __attribute__((weak));
extern void     phy_bt_tx_gain_set      (int gain)           __attribute__((weak));
extern void     phy_bt_filter_reg       (uint32_t reg)       __attribute__((weak));

/* Live state we report via GET_INFO */
static struct {
    uint8_t  channel;
    int8_t   tx_power_dbm;
    uint8_t  phy_11p_armed;
    uint8_t  cca_enabled;
    uint8_t  low_rate_enabled;
    uint8_t  agc_max_gain;
} s_state = {
    .channel = 180,
    .tx_power_dbm = 20,
    .phy_11p_armed = 1,
    .cca_enabled = 1,
    .low_rate_enabled = 0,
    .agc_max_gain = 255,
};

/* ---------- response shipper ------------------------------------- */

static void send_simple_resp(uint32_t resp_id, uint32_t op_id, esp_err_t status)
{
    phy_rpc_resp_hdr_t hdr = { .op_id = op_id, .status = (int32_t)status };
    esp_hosted_send_custom_data(resp_id, (uint8_t *)&hdr, sizeof(hdr));
}

static void send_resp_with_body(uint32_t resp_id, uint32_t op_id, esp_err_t status,
                                const void *body, size_t body_len)
{
    size_t total = sizeof(phy_rpc_resp_hdr_t) + body_len;
    uint8_t *buf = malloc(total);
    if (!buf) {
        send_simple_resp(resp_id, op_id, ESP_ERR_NO_MEM);
        return;
    }
    phy_rpc_resp_hdr_t *hdr = (phy_rpc_resp_hdr_t *)buf;
    hdr->op_id  = op_id;
    hdr->status = (int32_t)status;
    if (body_len) memcpy(buf + sizeof(*hdr), body, body_len);
    esp_hosted_send_custom_data(resp_id, buf, total);
    free(buf);
}

/* ---------- per-request handlers --------------------------------- */

#define UNPACK_REQ(type, var)                                               \
    if (len < sizeof(type)) { ESP_LOGW(TAG, #type " short"); return; }      \
    const type *var = (const type *)data;                                   \
    uint32_t _op = var->hdr.op_id

static void on_set_channel(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_channel_t, r);
    /* permissive country first (idempotent) */
    wifi_country_t cc = { .cc="01", .schan=1, .nchan=200, .max_tx_power=23, .policy=WIFI_COUNTRY_POLICY_MANUAL };
    esp_wifi_set_country(&cc);
    esp_err_t err = esp_wifi_set_channel(140, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) {
        phy_change_channel(r->channel, 1, 0, 0);
        s_state.channel = r->channel;
    }
    send_simple_resp(PHY_RPC_RESP_SET_CHANNEL, _op, err);
}

static void on_set_phy_11p(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_phy_11p_t, r);
    phy_11p_set(r->enable ? 1 : 0, 0);
    s_state.phy_11p_armed = r->enable ? 1 : 0;
    send_simple_resp(PHY_RPC_RESP_SET_PHY_11P, _op, ESP_OK);
}

static void on_set_tx_power(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_tx_power_t, r);
    int8_t q = r->dbm * 4;
    if (q < 8)  q = 8;
    if (q > 84) q = 84;
    esp_err_t err = esp_wifi_set_max_tx_power(q);
    if (err == ESP_OK) s_state.tx_power_dbm = r->dbm;
    send_simple_resp(PHY_RPC_RESP_SET_TX_POWER, _op, err);
}

static void on_set_rx_gain(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_rx_gain_t, r);
    esp_err_t st = ESP_OK;
    if (r->gain_index == 0xFF) {
        if (phy_enable_agc)    phy_enable_agc();
        if (phy_rx_gain_force) phy_rx_gain_force(0);
    } else {
        if (phy_disable_agc)   phy_disable_agc();
        if (phy_rx_gain_force) phy_rx_gain_force(1);
        if (!phy_force_rx_gain) st = ESP_ERR_NOT_SUPPORTED;
        else                    phy_force_rx_gain(r->gain_index);
    }
    send_simple_resp(PHY_RPC_RESP_SET_RX_GAIN, _op, st);
}

static void on_set_agc_max_gain(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_agc_max_gain_t, r);
    esp_err_t st = ESP_OK;
    if (!phy_agc_max_gain_set) st = ESP_ERR_NOT_SUPPORTED;
    else                        { phy_agc_max_gain_set(r->max_gain); s_state.agc_max_gain = r->max_gain; }
    send_simple_resp(PHY_RPC_RESP_SET_AGC_MAX_GAIN, _op, st);
}

static void on_set_cca(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_cca_t, r);
    esp_err_t st = ESP_OK;
    if (r->enable) {
        if (!phy_enable_cca) st = ESP_ERR_NOT_SUPPORTED; else phy_enable_cca();
    } else {
        if (!phy_disable_cca) st = ESP_ERR_NOT_SUPPORTED; else phy_disable_cca();
    }
    if (st == ESP_OK) s_state.cca_enabled = r->enable ? 1 : 0;
    send_simple_resp(PHY_RPC_RESP_SET_CCA, _op, st);
}

static void on_get_cca_counters(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    if (!phy_get_cca_cnt) { send_simple_resp(PHY_RPC_RESP_GET_CCA_COUNTERS, op, ESP_ERR_NOT_SUPPORTED); return; }
    struct __attribute__((packed)) { uint32_t busy, total; } body = {
        .busy = phy_get_cca_cnt(),
        .total = 0,    /* slave doesn't track wall time here; leave 0 */
    };
    send_resp_with_body(PHY_RPC_RESP_GET_CCA_COUNTERS, op, ESP_OK, &body, sizeof(body));
}

static void on_reset_cca_counters(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    if (!phy_set_cca_cnt) { send_simple_resp(PHY_RPC_RESP_RESET_CCA_COUNT, op, ESP_ERR_NOT_SUPPORTED); return; }
    phy_set_cca_cnt(0);
    send_simple_resp(PHY_RPC_RESP_RESET_CCA_COUNT, op, ESP_OK);
}

static void on_set_low_rate(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_low_rate_t, r);
    esp_err_t st = ESP_OK;
    if (r->enable) {
        if (!phy_enable_low_rate) st = ESP_ERR_NOT_SUPPORTED; else phy_enable_low_rate();
    } else {
        if (!phy_disable_low_rate) st = ESP_ERR_NOT_SUPPORTED; else phy_disable_low_rate();
    }
    if (st == ESP_OK) s_state.low_rate_enabled = r->enable ? 1 : 0;
    send_simple_resp(PHY_RPC_RESP_SET_LOW_RATE, _op, st);
}

static void on_set_bandwidth(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_bandwidth_t, r);
    esp_err_t st = ESP_OK;
    if (!phy_bb_bss_cbw40) st = ESP_ERR_NOT_SUPPORTED;
    else if (r->bw_mhz == 40) phy_bb_bss_cbw40(1);
    else if (r->bw_mhz == 20) phy_bb_bss_cbw40(0);
    else st = ESP_ERR_INVALID_ARG;
    send_simple_resp(PHY_RPC_RESP_SET_BANDWIDTH, _op, st);
}

static void on_get_phy_rssi(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    int8_t rssi = INT8_MIN;
    if (phy_get_sigrssi) rssi = (int8_t)phy_get_sigrssi();
    else if (phy_get_rssi) rssi = (int8_t)phy_get_rssi();
    send_resp_with_body(PHY_RPC_RESP_GET_PHY_RSSI, op,
                        rssi == INT8_MIN ? ESP_ERR_NOT_SUPPORTED : ESP_OK,
                        &rssi, sizeof(rssi));
}

static void on_set_country_permissive(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    wifi_country_t cc = { .cc="01", .schan=1, .nchan=200, .max_tx_power=23, .policy=WIFI_COUNTRY_POLICY_MANUAL };
    esp_err_t err = esp_wifi_set_country(&cc);
    send_simple_resp(PHY_RPC_RESP_SET_COUNTRY_PERM, op, err);
}

static void on_get_info(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    struct __attribute__((packed)) {
        uint8_t channel;
        int8_t  tx_power_dbm;
        uint8_t phy_11p_armed;
        uint8_t cca_enabled;
        uint8_t low_rate_enabled;
        uint8_t agc_max_gain;
        uint8_t reserved[2];
        char    fw_version[32];
    } body = {
        .channel          = s_state.channel,
        .tx_power_dbm     = s_state.tx_power_dbm,
        .phy_11p_armed    = s_state.phy_11p_armed,
        .cca_enabled      = s_state.cca_enabled,
        .low_rate_enabled = s_state.low_rate_enabled,
        .agc_max_gain     = s_state.agc_max_gain,
    };
    snprintf(body.fw_version, sizeof(body.fw_version), "esp-hosted-open slave 0.1");
    send_resp_with_body(PHY_RPC_RESP_GET_INFO, op, ESP_OK, &body, sizeof(body));
}

/* ---------- new handlers covering the rest of the PHY surface ---- */

#define HOOK_OPT(sym, expr)  do { if (!sym) { st = ESP_ERR_NOT_SUPPORTED; } else { expr; } } while (0)

static void on_set_freq(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_freq_t, r);
    esp_err_t st = ESP_OK;
    HOOK_OPT(phy_set_freq, phy_set_freq((int)r->freq_mhz));
    send_simple_resp(PHY_RPC_RESP_SET_FREQ, _op, st);
}

static void on_set_band(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_band_t, r);
    esp_err_t st = ESP_OK;
    /* Two possible symbol names; try whichever resolved. */
    if (phy_band_change)   phy_band_change(r->band);
    else if (phy_band_sel) phy_band_sel(r->band);
    else                   st = ESP_ERR_NOT_SUPPORTED;
    send_simple_resp(PHY_RPC_RESP_SET_BAND, _op, st);
}

static void on_set_chan_filt(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_chan_filt_t, r);
    esp_err_t st = ESP_OK;
    HOOK_OPT(phy_chan_filt_set, phy_chan_filt_set(r->filter));
    send_simple_resp(PHY_RPC_RESP_SET_CHAN_FILT, _op, st);
}

static void on_set_chan14_mic(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_chan14_mic_t, r);
    esp_err_t st = ESP_OK;
    HOOK_OPT(phy_chan14_mic_cfg_new, phy_chan14_mic_cfg_new(r->enable));
    send_simple_resp(PHY_RPC_RESP_SET_CHAN14_MIC, _op, st);
}

static void on_set_rate(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_rate_t, r);
    esp_err_t st = ESP_OK;
    HOOK_OPT(phy_set_rate, phy_set_rate(r->rate));
    send_simple_resp(PHY_RPC_RESP_SET_RATE, _op, st);
}

static void on_set_most_tpw(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_most_tpw_t, r);
    esp_err_t st = ESP_OK;
    HOOK_OPT(phy_set_most_tpw, phy_set_most_tpw(r->tpw));
    send_simple_resp(PHY_RPC_RESP_SET_MOST_TPW, _op, st);
}

static void on_get_most_tpw(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    if (!phy_get_most_tpw) { send_simple_resp(PHY_RPC_RESP_GET_MOST_TPW, op, ESP_ERR_NOT_SUPPORTED); return; }
    int8_t v = (int8_t)phy_get_most_tpw();
    send_resp_with_body(PHY_RPC_RESP_GET_MOST_TPW, op, ESP_OK, &v, sizeof(v));
}

static void on_reset_rx_gain_table(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    esp_err_t st = ESP_OK;
    HOOK_OPT(phy_set_rx_gain_table, phy_set_rx_gain_table(0));
    send_simple_resp(PHY_RPC_RESP_RESET_RX_GAIN_TBL, op, st);
}

static void on_get_noise_floor(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    if (!phy_get_noise_floor) { send_simple_resp(PHY_RPC_RESP_GET_NOISE_FLOOR, op, ESP_ERR_NOT_SUPPORTED); return; }
    int8_t v = (int8_t)phy_get_noise_floor();
    send_resp_with_body(PHY_RPC_RESP_GET_NOISE_FLOOR, op, ESP_OK, &v, sizeof(v));
}

static void on_get_temperature(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    if (!phy_xpd_tsens) { send_simple_resp(PHY_RPC_RESP_GET_TEMPERATURE, op, ESP_ERR_NOT_SUPPORTED); return; }
    int8_t v = (int8_t)phy_xpd_tsens();
    send_resp_with_body(PHY_RPC_RESP_GET_TEMPERATURE, op, ESP_OK, &v, sizeof(v));
}

static void on_set_csi_dump(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_csi_dump_t, r);
    esp_err_t st = ESP_OK;
    HOOK_OPT(phy_csidump_force_lltf_cfg, phy_csidump_force_lltf_cfg(r->enable));
    send_simple_resp(PHY_RPC_RESP_SET_CSI_DUMP, _op, st);
}

static void on_set_loopback(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_loopback_t, r);
    esp_err_t st = ESP_OK;
    HOOK_OPT(phy_set_loopback_gain, phy_set_loopback_gain(r->gain));
    send_simple_resp(PHY_RPC_RESP_SET_LOOPBACK, _op, st);
}

static void on_set_bt_tx_gain(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_bt_tx_gain_t, r);
    esp_err_t st = ESP_OK;
    if (phy_bt_set_tx_gain_new) phy_bt_set_tx_gain_new(r->gain);
    else if (phy_bt_tx_gain_set) phy_bt_tx_gain_set(r->gain);
    else                          st = ESP_ERR_NOT_SUPPORTED;
    send_simple_resp(PHY_RPC_RESP_SET_BT_TX_GAIN, _op, st);
}

static void on_set_bt_filter(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    UNPACK_REQ(phy_rpc_req_set_bt_filter_t, r);
    esp_err_t st = ESP_OK;
    HOOK_OPT(phy_bt_filter_reg, phy_bt_filter_reg(r->reg_value));
    send_simple_resp(PHY_RPC_RESP_SET_BT_FILTER, _op, st);
}

/* GET_CAPS — inspect the weak-symbol resolution at runtime to tell
 * the host which RPCs are actually wired on this chip. */
static void on_get_caps(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len < sizeof(phy_rpc_hdr_t)) return;
    uint32_t op = ((phy_rpc_hdr_t *)data)->op_id;
    uint8_t caps[4] = {0};
    /* Helper: if the underlying symbol resolved, set the bit for this
     * request id. Bit n of caps[] is request id (BASE | n) availability. */
    #define MARK(req_id, present)                                            \
        do { uint32_t low = (req_id) & 0xFFu;                                \
             if (low <= 0x1F && (present)) caps[low / 8] |= 1u << (low % 8); } while (0)

    /* Always-on: covered by esp_wifi public API. */
    MARK(PHY_RPC_REQ_SET_CHANNEL,        phy_change_channel != NULL);
    MARK(PHY_RPC_REQ_SET_PHY_11P,        phy_11p_set != NULL);
    MARK(PHY_RPC_REQ_SET_TX_POWER,       1);
    MARK(PHY_RPC_REQ_SET_COUNTRY_PERM,   1);
    MARK(PHY_RPC_REQ_GET_INFO,           1);
    MARK(PHY_RPC_REQ_GET_CAPS,           1);

    MARK(PHY_RPC_REQ_SET_RX_GAIN,        phy_force_rx_gain != NULL);
    MARK(PHY_RPC_REQ_SET_AGC_MAX_GAIN,   phy_agc_max_gain_set != NULL);
    MARK(PHY_RPC_REQ_RESET_RX_GAIN_TBL,  phy_set_rx_gain_table != NULL);
    MARK(PHY_RPC_REQ_SET_CCA,            phy_disable_cca != NULL);
    MARK(PHY_RPC_REQ_GET_CCA_COUNTERS,   phy_get_cca_cnt != NULL);
    MARK(PHY_RPC_REQ_RESET_CCA_COUNT,    phy_set_cca_cnt != NULL);
    MARK(PHY_RPC_REQ_SET_LOW_RATE,       phy_disable_low_rate != NULL);
    MARK(PHY_RPC_REQ_SET_BANDWIDTH,      phy_bb_bss_cbw40 != NULL);
    MARK(PHY_RPC_REQ_GET_PHY_RSSI,       (phy_get_sigrssi != NULL || phy_get_rssi != NULL));
    MARK(PHY_RPC_REQ_SET_FREQ,           phy_set_freq != NULL);
    MARK(PHY_RPC_REQ_SET_BAND,           (phy_band_change != NULL || phy_band_sel != NULL));
    MARK(PHY_RPC_REQ_SET_CHAN_FILT,      phy_chan_filt_set != NULL);
    MARK(PHY_RPC_REQ_SET_CHAN14_MIC,     phy_chan14_mic_cfg_new != NULL);
    MARK(PHY_RPC_REQ_SET_RATE,           phy_set_rate != NULL);
    MARK(PHY_RPC_REQ_SET_MOST_TPW,       phy_set_most_tpw != NULL);
    MARK(PHY_RPC_REQ_GET_MOST_TPW,       phy_get_most_tpw != NULL);
    MARK(PHY_RPC_REQ_GET_NOISE_FLOOR,    phy_get_noise_floor != NULL);
    MARK(PHY_RPC_REQ_GET_TEMPERATURE,    phy_xpd_tsens != NULL);
    MARK(PHY_RPC_REQ_SET_CSI_DUMP,       phy_csidump_force_lltf_cfg != NULL);
    MARK(PHY_RPC_REQ_SET_LOOPBACK,       phy_set_loopback_gain != NULL);
    MARK(PHY_RPC_REQ_SET_BT_TX_GAIN,     (phy_bt_set_tx_gain_new != NULL || phy_bt_tx_gain_set != NULL));
    MARK(PHY_RPC_REQ_SET_BT_FILTER,      phy_bt_filter_reg != NULL);
    #undef MARK

    send_resp_with_body(PHY_RPC_RESP_GET_CAPS, op, ESP_OK, caps, sizeof(caps));
}

/* ---------- registration ----------------------------------------- */

void phy_rpc_handlers_register(void)
{
    struct { uint32_t id; void (*cb)(uint32_t, const uint8_t *, size_t, void *); } table[] = {
        { PHY_RPC_REQ_SET_CHANNEL,      on_set_channel },
        { PHY_RPC_REQ_SET_PHY_11P,      on_set_phy_11p },
        { PHY_RPC_REQ_SET_TX_POWER,     on_set_tx_power },
        { PHY_RPC_REQ_SET_RX_GAIN,      on_set_rx_gain },
        { PHY_RPC_REQ_SET_AGC_MAX_GAIN, on_set_agc_max_gain },
        { PHY_RPC_REQ_SET_CCA,          on_set_cca },
        { PHY_RPC_REQ_GET_CCA_COUNTERS, on_get_cca_counters },
        { PHY_RPC_REQ_RESET_CCA_COUNT,  on_reset_cca_counters },
        { PHY_RPC_REQ_SET_LOW_RATE,     on_set_low_rate },
        { PHY_RPC_REQ_SET_BANDWIDTH,    on_set_bandwidth },
        { PHY_RPC_REQ_GET_PHY_RSSI,     on_get_phy_rssi },
        { PHY_RPC_REQ_SET_COUNTRY_PERM, on_set_country_permissive },
        { PHY_RPC_REQ_GET_INFO,         on_get_info },
        { PHY_RPC_REQ_SET_FREQ,         on_set_freq },
        { PHY_RPC_REQ_SET_BAND,         on_set_band },
        { PHY_RPC_REQ_SET_CHAN_FILT,    on_set_chan_filt },
        { PHY_RPC_REQ_SET_CHAN14_MIC,   on_set_chan14_mic },
        { PHY_RPC_REQ_SET_RATE,         on_set_rate },
        { PHY_RPC_REQ_SET_MOST_TPW,     on_set_most_tpw },
        { PHY_RPC_REQ_GET_MOST_TPW,     on_get_most_tpw },
        { PHY_RPC_REQ_RESET_RX_GAIN_TBL, on_reset_rx_gain_table },
        { PHY_RPC_REQ_GET_NOISE_FLOOR,  on_get_noise_floor },
        { PHY_RPC_REQ_GET_TEMPERATURE,  on_get_temperature },
        { PHY_RPC_REQ_SET_CSI_DUMP,     on_set_csi_dump },
        { PHY_RPC_REQ_SET_LOOPBACK,     on_set_loopback },
        { PHY_RPC_REQ_SET_BT_TX_GAIN,   on_set_bt_tx_gain },
        { PHY_RPC_REQ_SET_BT_FILTER,    on_set_bt_filter },
        { PHY_RPC_REQ_GET_CAPS,         on_get_caps },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); i++) {
        esp_err_t err = esp_hosted_register_custom_callback(table[i].id, table[i].cb, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %08" PRIx32 ": %s", table[i].id, esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "%zu PHY RPC handlers registered", sizeof(table) / sizeof(*table));
}
