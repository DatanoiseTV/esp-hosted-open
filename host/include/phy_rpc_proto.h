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

/* Raw 802.11 TX / RX */
#define PHY_RPC_REQ_TX_80211          PHY_RPC_MSG_REQ(0x020)  /* arbitrary 802.11 frame, host -> slave */
#define PHY_RPC_REQ_SET_PROMISC       PHY_RPC_MSG_REQ(0x021)  /* uint8_t enable */
#define PHY_RPC_REQ_SET_PROMISC_FILTER PHY_RPC_MSG_REQ(0x022) /* WIFI_PROMIS_FILTER_MASK_* bitmap */

/* 2.4 GHz / general Wi-Fi knobs */
#define PHY_RPC_REQ_SET_PROTOCOL      PHY_RPC_MSG_REQ(0x023)  /* B|G|N|AX bitmask */
#define PHY_RPC_REQ_SET_MAC           PHY_RPC_MSG_REQ(0x024)  /* 6-byte MAC */

/* CSI capture (events stream out via PHY_RPC_EVT_CSI) */
#define PHY_RPC_REQ_SET_CSI_ENABLE    PHY_RPC_MSG_REQ(0x025)  /* uint8_t enable */

/* 802.11mc FTM ranging (initiator) */
#define PHY_RPC_REQ_FTM_INITIATE      PHY_RPC_MSG_REQ(0x026)  /* peer MAC + frames + period */

/* ESP-NOW (universal — all Wi-Fi chips) */
#define PHY_RPC_REQ_ESPNOW_INIT       PHY_RPC_MSG_REQ(0x030)
#define PHY_RPC_REQ_ESPNOW_DEINIT     PHY_RPC_MSG_REQ(0x031)
#define PHY_RPC_REQ_ESPNOW_ADD_PEER   PHY_RPC_MSG_REQ(0x032)  /* peer cfg */
#define PHY_RPC_REQ_ESPNOW_DEL_PEER   PHY_RPC_MSG_REQ(0x033)  /* peer mac */
#define PHY_RPC_REQ_ESPNOW_SEND       PHY_RPC_MSG_REQ(0x034)  /* peer mac + data */
#define PHY_RPC_REQ_ESPNOW_SET_PMK    PHY_RPC_MSG_REQ(0x035)  /* 16-byte PMK */

/* 802.15.4 (Thread / Zigbee — C6, H2, H4 only; NOT_SUPPORTED elsewhere) */
#define PHY_RPC_REQ_IEEE154_ENABLE    PHY_RPC_MSG_REQ(0x040)  /* uint8_t enable */
#define PHY_RPC_REQ_IEEE154_SET_CHAN  PHY_RPC_MSG_REQ(0x041)  /* uint8_t (11..26) */
#define PHY_RPC_REQ_IEEE154_SET_PANID PHY_RPC_MSG_REQ(0x042)  /* uint16_t */
#define PHY_RPC_REQ_IEEE154_SET_PROMISC PHY_RPC_MSG_REQ(0x043) /* uint8_t */
#define PHY_RPC_REQ_IEEE154_TX_RAW    PHY_RPC_MSG_REQ(0x044)  /* raw frame */

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

#define PHY_RPC_RESP_TX_80211             PHY_RPC_MSG_RESP(0x020)
#define PHY_RPC_RESP_SET_PROMISC          PHY_RPC_MSG_RESP(0x021)
#define PHY_RPC_RESP_SET_PROMISC_FILTER   PHY_RPC_MSG_RESP(0x022)
#define PHY_RPC_RESP_SET_PROTOCOL         PHY_RPC_MSG_RESP(0x023)
#define PHY_RPC_RESP_SET_MAC              PHY_RPC_MSG_RESP(0x024)
#define PHY_RPC_RESP_SET_CSI_ENABLE       PHY_RPC_MSG_RESP(0x025)
#define PHY_RPC_RESP_FTM_INITIATE         PHY_RPC_MSG_RESP(0x026)

#define PHY_RPC_RESP_ESPNOW_INIT          PHY_RPC_MSG_RESP(0x030)
#define PHY_RPC_RESP_ESPNOW_DEINIT        PHY_RPC_MSG_RESP(0x031)
#define PHY_RPC_RESP_ESPNOW_ADD_PEER      PHY_RPC_MSG_RESP(0x032)
#define PHY_RPC_RESP_ESPNOW_DEL_PEER      PHY_RPC_MSG_RESP(0x033)
#define PHY_RPC_RESP_ESPNOW_SEND          PHY_RPC_MSG_RESP(0x034)
#define PHY_RPC_RESP_ESPNOW_SET_PMK       PHY_RPC_MSG_RESP(0x035)

#define PHY_RPC_RESP_IEEE154_ENABLE       PHY_RPC_MSG_RESP(0x040)
#define PHY_RPC_RESP_IEEE154_SET_CHAN     PHY_RPC_MSG_RESP(0x041)
#define PHY_RPC_RESP_IEEE154_SET_PANID    PHY_RPC_MSG_RESP(0x042)
#define PHY_RPC_RESP_IEEE154_SET_PROMISC  PHY_RPC_MSG_RESP(0x043)
#define PHY_RPC_RESP_IEEE154_TX_RAW       PHY_RPC_MSG_RESP(0x044)

/* Async events (slave->host, no request/response correlation) */
#define PHY_RPC_EVT_OCB_FRAME        PHY_RPC_MSG_EVT(0x001)  /* OCB-filtered LLC payload (V2X) */
#define PHY_RPC_EVT_PHY_STATE        PHY_RPC_MSG_EVT(0x002)  /* periodic stats push */
#define PHY_RPC_EVT_RAW_FRAME        PHY_RPC_MSG_EVT(0x003)  /* generic 802.11 frame (matches promisc filter) */
#define PHY_RPC_EVT_CSI              PHY_RPC_MSG_EVT(0x004)  /* CSI capture */
#define PHY_RPC_EVT_FTM_REPORT       PHY_RPC_MSG_EVT(0x005)  /* FTM ranging measurements */
#define PHY_RPC_EVT_ESPNOW_RX        PHY_RPC_MSG_EVT(0x006)  /* ESP-NOW frame received   */
#define PHY_RPC_EVT_ESPNOW_TX_STATUS PHY_RPC_MSG_EVT(0x007)  /* ESP-NOW TX confirmation  */
#define PHY_RPC_EVT_IEEE154_RX       PHY_RPC_MSG_EVT(0x008)  /* 802.15.4 frame received  */

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

/* Raw TX: header + variable-length 802.11 frame. Caller fills the
 * full MAC header (FC, addresses, seq) plus payload; the slave does
 * not modify it before injecting via esp_wifi_80211_tx. */
typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        wifi_if;       /* 0 = STA, 1 = AP                */
    uint8_t        en_seq_nr;     /* 1 = let MAC override seq#      */
    uint16_t       reserved;
    uint16_t       frame_len;
    uint8_t        frame[];
} phy_rpc_req_tx_80211_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        enable;
} phy_rpc_req_set_promisc_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint32_t       filter_mask;   /* WIFI_PROMIS_FILTER_MASK_* OR'd */
} phy_rpc_req_set_promisc_filter_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        protocol_mask; /* WIFI_PROTOCOL_11B | _11G | _11N | _11AX | _LR */
    uint8_t        wifi_if;       /* 0 = STA, 1 = AP */
    uint8_t        reserved[2];
} phy_rpc_req_set_protocol_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        wifi_if;       /* 0 = STA, 1 = AP */
    uint8_t        mac[6];
    uint8_t        reserved;
} phy_rpc_req_set_mac_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        enable;
} phy_rpc_req_set_csi_enable_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        peer_mac[6];
    uint8_t        frames_count;  /* burst length */
    uint8_t        burst_period;  /* 100 ms units */
    uint8_t        channel;       /* IEEE channel; 0 = current */
    uint8_t        reserved[3];
} phy_rpc_req_ftm_initiate_t;

/* ESP-NOW peer descriptor (matches IDF's esp_now_peer_info_t subset) */
typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        peer_mac[6];
    uint8_t        lmk[16];       /* zero = unencrypted */
    uint8_t        channel;       /* 0 = current */
    uint8_t        wifi_if;       /* 0 = STA, 1 = AP */
    uint8_t        encrypt;       /* 0/1 */
    uint8_t        reserved;
} phy_rpc_req_espnow_add_peer_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        peer_mac[6];
} phy_rpc_req_espnow_del_peer_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        peer_mac[6];   /* 0xff*6 = broadcast */
    uint16_t       data_len;      /* 1..250 */
    uint8_t        data[];
} phy_rpc_req_espnow_send_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        pmk[16];
} phy_rpc_req_espnow_set_pmk_t;

/* 802.15.4 */
typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        enable;
} phy_rpc_req_ieee154_enable_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        channel;       /* 11..26 (2.4 GHz O-QPSK) */
} phy_rpc_req_ieee154_set_chan_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint16_t       pan_id;
} phy_rpc_req_ieee154_set_panid_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        enable;
} phy_rpc_req_ieee154_set_promisc_t;

typedef struct __attribute__((packed)) {
    phy_rpc_hdr_t hdr;
    uint8_t        cca;           /* 1 = CCA before TX */
    uint8_t        reserved;
    uint16_t       frame_len;     /* including 2-byte FCS placeholder */
    uint8_t        frame[];
} phy_rpc_req_ieee154_tx_raw_t;

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

/* Generic 802.11 frame as captured by the slave's promiscuous callback.
 * frame[] starts at the 802.11 MAC header (no radiotap prefix). */
typedef struct __attribute__((packed)) {
    int8_t   rssi_dbm;
    uint8_t  channel;
    uint8_t  rate;                /* WIFI_PHY_RATE_* */
    uint8_t  is_qos;              /* QoS-Data subtype? */
    uint16_t frame_len;
    uint16_t reserved;
    uint32_t timestamp_us;
    uint8_t  frame[];
} phy_rpc_evt_raw_frame_t;

/* CSI capture. csi[] is the raw H matrix bytes; layout matches IDF's
 * wifi_csi_info_t.buf — see CSI section of the ESP-IDF Wi-Fi docs. */
typedef struct __attribute__((packed)) {
    int8_t   rssi_dbm;
    uint8_t  channel;
    uint8_t  src_mac[6];
    uint16_t csi_len;
    uint16_t reserved;
    uint32_t timestamp_us;
    uint8_t  csi[];
} phy_rpc_evt_csi_t;

/* FTM ranging report. Mirrors the useful fields of
 * wifi_event_ftm_report_t. */
typedef struct __attribute__((packed)) {
    uint8_t  peer_mac[6];
    uint8_t  status;              /* 0 = success */
    uint8_t  reserved;
    int32_t  rtt_raw_ps;          /* round-trip time, picoseconds */
    int32_t  rtt_est_ps;
    int32_t  dist_est_cm;
} phy_rpc_evt_ftm_report_t;

/* ESP-NOW received frame. */
typedef struct __attribute__((packed)) {
    uint8_t  src_mac[6];
    uint8_t  dst_mac[6];
    int8_t   rssi_dbm;
    uint8_t  channel;
    uint16_t data_len;
    uint8_t  data[];
} phy_rpc_evt_espnow_rx_t;

typedef struct __attribute__((packed)) {
    uint8_t  peer_mac[6];
    uint8_t  status;              /* 0 = success */
    uint8_t  reserved;
} phy_rpc_evt_espnow_tx_status_t;

/* 802.15.4 received frame. */
typedef struct __attribute__((packed)) {
    int8_t   rssi_dbm;
    uint8_t  lqi;
    uint8_t  channel;
    uint8_t  reserved;
    uint16_t frame_len;
    uint16_t reserved2;
    uint32_t timestamp_us;
    uint8_t  frame[];
} phy_rpc_evt_ieee154_rx_t;

#ifdef __cplusplus
}
#endif
