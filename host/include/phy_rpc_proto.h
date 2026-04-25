#pragma once

/*
 * phy_rpc_proto.h — wire protocol between the host MCU and the
 * Espressif co-processor running esp-hosted-open's slave overlay.
 *
 * Both sides include this header. Single source of truth for
 * msg_id allocations and on-the-wire payload layout for every RPC.
 *
 * Transport: esp_hosted_send_custom_data() + esp_hosted_register_custom_callback()
 * (gated by CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER on both sides).
 *
 * Each request from the host carries a unique op_id; the slave's
 * response event echoes the same op_id so the host can route it back
 * to the waiting caller.
 *
 * Coverage: every row in docs/symbol-reference.md that's reachable
 * from libphy.a on the target chip. RPCs that hit a missing symbol
 * return ESP_ERR_NOT_SUPPORTED (extern declared __attribute__((weak))
 * on the slave).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* msg_id namespace: 0xC175xxxx so we don't collide with anything
 * Espressif might add later. Top nibble of the lower 16 bits encodes
 * direction:
 *   0x0xxx — host->slave request
 *   0x8xxx — slave->host response (matches the request's low 12 bits)
 *   0xFxxx — async event (unsolicited slave->host) */
#define PHY_RPC_MSG_BASE             0xC1750000u
#define PHY_RPC_MSG_REQ(n)           (PHY_RPC_MSG_BASE | (n))
#define PHY_RPC_MSG_RESP(n)          (PHY_RPC_MSG_BASE | 0x8000u | (n))
#define PHY_RPC_MSG_EVT(n)           (PHY_RPC_MSG_BASE | 0xF000u | (n))

/* ---------- Request IDs (host -> slave) -------------------------- */

/* Channel / band / frequency */
#define PHY_RPC_REQ_SET_CHANNEL       PHY_RPC_MSG_REQ(0x001)  /* uint8_t IEEE */
#define PHY_RPC_REQ_SET_FREQ          PHY_RPC_MSG_REQ(0x010)  /* uint32_t MHz */
#define PHY_RPC_REQ_SET_BAND          PHY_RPC_MSG_REQ(0x011)  /* uint8_t 24|50 */
#define PHY_RPC_REQ_SET_BANDWIDTH     PHY_RPC_MSG_REQ(0x00A)  /* uint8_t (20|40) */
#define PHY_RPC_REQ_SET_CHAN_FILT     PHY_RPC_MSG_REQ(0x012)  /* uint32_t bitmask */
#define PHY_RPC_REQ_SET_COUNTRY_PERM  PHY_RPC_MSG_REQ(0x00C)
#define PHY_RPC_REQ_SET_CHAN14_MIC    PHY_RPC_MSG_REQ(0x013)  /* uint8_t enable */

/* PHY mode / rate */
#define PHY_RPC_REQ_SET_PHY_11P       PHY_RPC_MSG_REQ(0x002)  /* uint8_t (0/1) */
#define PHY_RPC_REQ_SET_RATE          PHY_RPC_MSG_REQ(0x014)  /* uint8_t rate idx */
#define PHY_RPC_REQ_SET_LOW_RATE      PHY_RPC_MSG_REQ(0x009)  /* uint8_t (0/1) */

/* TX power */
#define PHY_RPC_REQ_SET_TX_POWER      PHY_RPC_MSG_REQ(0x003)  /* int8_t dBm — esp_wifi public API */
#define PHY_RPC_REQ_SET_MOST_TPW      PHY_RPC_MSG_REQ(0x015)  /* int8_t — phy_set_most_tpw direct */
#define PHY_RPC_REQ_GET_MOST_TPW      PHY_RPC_MSG_REQ(0x016)

/* RX gain / AGC */
#define PHY_RPC_REQ_SET_RX_GAIN       PHY_RPC_MSG_REQ(0x004)  /* uint8_t (0xFF = release) */
#define PHY_RPC_REQ_SET_AGC_MAX_GAIN  PHY_RPC_MSG_REQ(0x005)  /* uint8_t */
#define PHY_RPC_REQ_RESET_RX_GAIN_TBL PHY_RPC_MSG_REQ(0x017)

/* CCA / channel busy */
#define PHY_RPC_REQ_SET_CCA           PHY_RPC_MSG_REQ(0x006)  /* uint8_t (0/1) */
#define PHY_RPC_REQ_GET_CCA_COUNTERS  PHY_RPC_MSG_REQ(0x007)
#define PHY_RPC_REQ_RESET_CCA_COUNT   PHY_RPC_MSG_REQ(0x008)

/* Diagnostics */
#define PHY_RPC_REQ_GET_PHY_RSSI      PHY_RPC_MSG_REQ(0x00B)
#define PHY_RPC_REQ_GET_NOISE_FLOOR   PHY_RPC_MSG_REQ(0x018)
#define PHY_RPC_REQ_GET_TEMPERATURE   PHY_RPC_MSG_REQ(0x019)  /* phy_xpd_tsens */

/* CSI / loopback / self-test */
#define PHY_RPC_REQ_SET_CSI_DUMP      PHY_RPC_MSG_REQ(0x01A)  /* uint8_t (0/1) — phy_csidump_force_lltf_cfg */
#define PHY_RPC_REQ_SET_LOOPBACK      PHY_RPC_MSG_REQ(0x01B)  /* uint8_t gain */

/* BT-radio knobs (nRF24L01+-style hacks) */
#define PHY_RPC_REQ_SET_BT_TX_GAIN    PHY_RPC_MSG_REQ(0x01C)  /* uint8_t */
#define PHY_RPC_REQ_SET_BT_FILTER     PHY_RPC_MSG_REQ(0x01D)  /* uint32_t reg value */

/* Meta */
#define PHY_RPC_REQ_GET_INFO          PHY_RPC_MSG_REQ(0x00D)
#define PHY_RPC_REQ_GET_CAPS          PHY_RPC_MSG_REQ(0x01E)  /* which RPCs are wired on this chip? */

/* Response IDs (slave->host, sent via esp_hosted_send_custom_data
 * AFTER processing the matching request). Each carries a header with
 * the original op_id and an int32 status. */
#define PHY_RPC_RESP_SET_CHANNEL      PHY_RPC_MSG_RESP(0x001)
#define PHY_RPC_RESP_SET_PHY_11P      PHY_RPC_MSG_RESP(0x002)
#define PHY_RPC_RESP_SET_TX_POWER     PHY_RPC_MSG_RESP(0x003)
#define PHY_RPC_RESP_SET_RX_GAIN      PHY_RPC_MSG_RESP(0x004)
#define PHY_RPC_RESP_SET_AGC_MAX_GAIN PHY_RPC_MSG_RESP(0x005)
#define PHY_RPC_RESP_SET_CCA          PHY_RPC_MSG_RESP(0x006)
#define PHY_RPC_RESP_GET_CCA_COUNTERS PHY_RPC_MSG_RESP(0x007)
#define PHY_RPC_RESP_RESET_CCA_COUNT  PHY_RPC_MSG_RESP(0x008)
#define PHY_RPC_RESP_SET_LOW_RATE     PHY_RPC_MSG_RESP(0x009)
#define PHY_RPC_RESP_SET_BANDWIDTH    PHY_RPC_MSG_RESP(0x00A)
#define PHY_RPC_RESP_GET_PHY_RSSI     PHY_RPC_MSG_RESP(0x00B)
#define PHY_RPC_RESP_SET_COUNTRY_PERM PHY_RPC_MSG_RESP(0x00C)
#define PHY_RPC_RESP_GET_INFO         PHY_RPC_MSG_RESP(0x00D)

#define PHY_RPC_RESP_SET_FREQ           PHY_RPC_MSG_RESP(0x010)
#define PHY_RPC_RESP_SET_BAND           PHY_RPC_MSG_RESP(0x011)
#define PHY_RPC_RESP_SET_CHAN_FILT      PHY_RPC_MSG_RESP(0x012)
#define PHY_RPC_RESP_SET_CHAN14_MIC     PHY_RPC_MSG_RESP(0x013)
#define PHY_RPC_RESP_SET_RATE           PHY_RPC_MSG_RESP(0x014)
#define PHY_RPC_RESP_SET_MOST_TPW       PHY_RPC_MSG_RESP(0x015)
#define PHY_RPC_RESP_GET_MOST_TPW       PHY_RPC_MSG_RESP(0x016)
#define PHY_RPC_RESP_RESET_RX_GAIN_TBL  PHY_RPC_MSG_RESP(0x017)
#define PHY_RPC_RESP_GET_NOISE_FLOOR    PHY_RPC_MSG_RESP(0x018)
#define PHY_RPC_RESP_GET_TEMPERATURE    PHY_RPC_MSG_RESP(0x019)
#define PHY_RPC_RESP_SET_CSI_DUMP       PHY_RPC_MSG_RESP(0x01A)
#define PHY_RPC_RESP_SET_LOOPBACK       PHY_RPC_MSG_RESP(0x01B)
#define PHY_RPC_RESP_SET_BT_TX_GAIN     PHY_RPC_MSG_RESP(0x01C)
#define PHY_RPC_RESP_SET_BT_FILTER      PHY_RPC_MSG_RESP(0x01D)
#define PHY_RPC_RESP_GET_CAPS           PHY_RPC_MSG_RESP(0x01E)

/* Async events (slave->host, no request/response correlation) */
#define PHY_RPC_EVT_OCB_FRAME        PHY_RPC_MSG_EVT(0x001)  /* received & filtered LLC payload */
#define PHY_RPC_EVT_PHY_STATE        PHY_RPC_MSG_EVT(0x002)  /* periodic stats push */

/* ---------- common request header ---------------------------------- */

/* Every request carries a 4-byte header in front of its body so the
 * slave can correlate the response. Pack: little-endian. */
typedef struct __attribute__((packed)) {
    uint32_t op_id;        /* host-chosen, monotonic; echoed in response */
} phy_rpc_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t op_id;        /* matches request */
    int32_t  status;       /* esp_err_t value */
} phy_rpc_resp_hdr_t;

/* ---------- per-call payloads -------------------------------------- */

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        channel;       /* IEEE channel: 172/174/176/178/180/182/184 */
} phy_rpc_req_set_channel_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        enable;
} phy_rpc_req_set_phy_11p_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    int8_t         dbm;
} phy_rpc_req_set_tx_power_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        gain_index;    /* 0xFF = release lock / AGC */
} phy_rpc_req_set_rx_gain_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        max_gain;
} phy_rpc_req_set_agc_max_gain_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        enable;
} phy_rpc_req_set_cca_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        enable;
} phy_rpc_req_set_low_rate_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        bw_mhz;        /* 20 or 40 */
} phy_rpc_req_set_bandwidth_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint32_t       freq_mhz;
} phy_rpc_req_set_freq_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        band;          /* 24 = 2.4 GHz, 50 = 5 GHz */
} phy_rpc_req_set_band_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint32_t       filter;        /* opaque, chip-specific */
} phy_rpc_req_set_chan_filt_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        enable;
} phy_rpc_req_set_chan14_mic_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        rate;          /* IEEE rate index, chip-dependent */
} phy_rpc_req_set_rate_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    int8_t         tpw;           /* phy_set_most_tpw raw value (1/4 dBm) */
} phy_rpc_req_set_most_tpw_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        enable;
} phy_rpc_req_set_csi_dump_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        gain;          /* 0 = disable */
} phy_rpc_req_set_loopback_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        gain;
} phy_rpc_req_set_bt_tx_gain_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint32_t       reg_value;
} phy_rpc_req_set_bt_filter_t;

/* Response bodies (after phy_rpc_resp_hdr_t) */

typedef struct __attribute__((packed)) {
    phy_rpc_resp_hdr_t hdr;
    uint32_t            busy_us;
    uint32_t            total_us;
} phy_rpc_resp_get_cca_counters_t;

typedef struct __attribute__((packed)) {
    phy_rpc_resp_hdr_t hdr;
    int8_t              rssi_dbm;
} phy_rpc_resp_get_phy_rssi_t;

typedef struct __attribute__((packed)) {
    phy_rpc_resp_hdr_t hdr;
    int8_t              dbm;
} phy_rpc_resp_get_most_tpw_t;

typedef struct __attribute__((packed)) {
    phy_rpc_resp_hdr_t hdr;
    int8_t              dbm;
} phy_rpc_resp_get_noise_floor_t;

typedef struct __attribute__((packed)) {
    phy_rpc_resp_hdr_t hdr;
    int8_t              celsius;
} phy_rpc_resp_get_temperature_t;

/* GET_CAPS reports which RPCs the slave's libphy.a actually exposes
 * on this chip — bit n is set iff request id (PHY_RPC_MSG_BASE | n) is
 * wired and the underlying symbol resolved at link time. The host
 * uses this to gracefully degrade. caps[0] covers msg ids 0x000-0x007,
 * caps[1] covers 0x008-0x00F, ..., caps[3] covers 0x018-0x01F. */
typedef struct __attribute__((packed)) {
    phy_rpc_resp_hdr_t hdr;
    uint8_t             caps[4];
} phy_rpc_resp_get_caps_t;

typedef struct __attribute__((packed)) {
    phy_rpc_resp_hdr_t hdr;
    uint8_t             channel;
    int8_t              tx_power_dbm;
    uint8_t             phy_11p_armed;
    uint8_t             cca_enabled;
    uint8_t             low_rate_enabled;
    uint8_t             agc_max_gain;
    uint8_t             reserved[2];
    char                fw_version[32];
} phy_rpc_resp_get_info_t;

/* ---------- async events ------------------------------------------ */

/* Slave hands the host a ready-to-parse LLC payload (post-LLC/SNAP)
 * with frame metadata. The data[] tail is the GeoNetworking PDU
 * exactly as it would appear in the standalone build's RX callback. */
typedef struct __attribute__((packed)) {
    int8_t   rssi_dbm;
    uint8_t  channel;
    uint8_t  src_mac[6];
    uint16_t llc_payload_len;
    uint32_t timestamp_us;
    uint8_t  data[];              /* llc_payload_len bytes */
} phy_rpc_evt_ocb_frame_t;

#ifdef __cplusplus
}
#endif
