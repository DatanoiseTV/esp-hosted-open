#pragma once

/*
 * Host-side wrappers around the CITS custom RPCs that run on the C5
 * slave. These functions hide the op_id correlation, request/response
 * marshalling, and the wait-for-reply semantics behind a normal sync
 * C API.
 *
 * Build: link this component on the host (P4) side; the slave's
 * phy_rpc_overlay registers the matching handlers. Both sides need
 * CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y.
 *
 * All calls block on the SDIO RPC round-trip (~ms). They are safe to
 * call from any task except the esp-hosted RX task.
 */

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the hosted RPC layer. Call once after esp_hosted_init().
 * Registers the response/event callbacks with esp-hosted's custom-data
 * channel and spins up the dispatcher. */
esp_err_t esp_hosted_open_init(void);
esp_err_t esp_hosted_open_deinit(void);

/* Default per-call timeout. Override by calling
 * esp_hosted_open_set_timeout_ms() before issuing requests. */
void esp_hosted_open_set_timeout_ms(uint32_t ms);

/* ---------- PHY control --------------------------------------- */

/* Channel / band / frequency */
esp_err_t esp_hosted_open_set_channel       (uint8_t ieee_channel);
esp_err_t esp_hosted_open_set_freq          (uint32_t freq_mhz);
esp_err_t esp_hosted_open_set_band          (uint8_t band);          /* 24 | 50 */
esp_err_t esp_hosted_open_set_bandwidth     (uint8_t bw_mhz);        /* 20 | 40 */
esp_err_t esp_hosted_open_set_chan_filt     (uint32_t mask);
esp_err_t esp_hosted_open_set_chan14_micro  (bool enable);
esp_err_t esp_hosted_open_set_country_permissive(void);

/* PHY mode / rate */
esp_err_t esp_hosted_open_set_phy_11p       (bool enable);
esp_err_t esp_hosted_open_set_rate          (uint8_t rate_idx);
esp_err_t esp_hosted_open_set_low_rate      (bool enable);

/* TX power */
esp_err_t esp_hosted_open_set_tx_power      (int8_t dbm);            /* via esp_wifi public API */
esp_err_t esp_hosted_open_set_most_tpw      (int8_t tpw);            /* phy_set_most_tpw raw    */
esp_err_t esp_hosted_open_get_most_tpw      (int8_t *tpw);

/* RX gain / AGC */
esp_err_t esp_hosted_open_set_rx_gain       (uint8_t gain_index);    /* 0xFF = release          */
esp_err_t esp_hosted_open_set_agc_max_gain  (uint8_t max_gain);
esp_err_t esp_hosted_open_reset_rx_gain_table(void);

/* CCA / channel busy */
esp_err_t esp_hosted_open_set_cca           (bool enable);
esp_err_t esp_hosted_open_get_cca_counters  (uint32_t *busy_us, uint32_t *total_us);
esp_err_t esp_hosted_open_reset_cca_counters(void);

/* Diagnostics */
esp_err_t esp_hosted_open_get_phy_rssi      (int8_t *rssi_dbm);
esp_err_t esp_hosted_open_get_noise_floor   (int8_t *dbm);
esp_err_t esp_hosted_open_get_temperature   (int8_t *celsius);

/* CSI / loopback / self-test */
esp_err_t esp_hosted_open_set_csi_dump      (bool enable);
esp_err_t esp_hosted_open_set_loopback      (uint8_t gain);          /* 0 = disable */

/* BT-radio knobs (nRF24L01+-style hacks; only on chips with the
 * relevant phy_bt_* exports) */
esp_err_t esp_hosted_open_set_bt_tx_gain    (uint8_t gain);
esp_err_t esp_hosted_open_set_bt_filter     (uint32_t reg_value);

/* Capability discovery — bitmap indexed by request msg_id low byte;
 * tells the host which RPCs the slave's libphy.a actually exposes
 * on this chip. */
esp_err_t esp_hosted_open_get_caps          (uint8_t caps[4]);
bool      esp_hosted_open_has_capability    (uint32_t request_msg_id, const uint8_t caps[4]);

typedef struct {
    uint8_t  channel;
    int8_t   tx_power_dbm;
    bool     phy_11p_armed;
    bool     cca_enabled;
    bool     low_rate_enabled;
    uint8_t  agc_max_gain;
    char     fw_version[32];
} esp_hosted_open_info_t;

esp_err_t esp_hosted_open_get_info(esp_hosted_open_info_t *out);

/* ---------- Async events from slave ----------------------------- */

/* Called when the slave receives an OCB frame (CAM, DENM, anything
 * matching ETSI/WSMP EtherType + LLC SNAP). Payload starts at the
 * GeoNetworking PDU. Runs on the dispatcher task — keep it short. */
typedef struct {
    int8_t   rssi_dbm;
    uint8_t  channel;
    uint8_t  src_mac[6];
    uint32_t timestamp_us;
} esp_hosted_open_rx_meta_t;

typedef void (*esp_hosted_open_ocb_rx_cb_t)(const uint8_t *llc_payload,
                                            size_t                       len,
                                            const esp_hosted_open_rx_meta_t *meta,
                                            void                         *ctx);

esp_err_t esp_hosted_open_register_ocb_rx_cb(esp_hosted_open_ocb_rx_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
