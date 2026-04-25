/*
 * Wire-protocol sanity for the host<->slave RPC framing.
 *
 * The packed structs in phy_rpc_proto.h are written to and parsed
 * from a network buffer on two different chips. Their layout has to
 * be byte-stable across both compilers and across releases.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "phy_rpc_proto.h"

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "    FAIL: %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            return -1;                                                        \
        }                                                                     \
    } while (0)

int test_proto_sizes(void)
{
    /* Common headers. */
    EXPECT(sizeof(phy_rpc_hdr_t)           == 4);
    EXPECT(sizeof(phy_rpc_resp_hdr_t)      == 8);

    /* Single-byte-body requests are header(4) + 1. */
    EXPECT(sizeof(phy_rpc_req_set_channel_t)       == 5);
    EXPECT(sizeof(phy_rpc_req_set_phy_11p_t)       == 5);
    EXPECT(sizeof(phy_rpc_req_set_tx_power_t)      == 5);
    EXPECT(sizeof(phy_rpc_req_set_rx_gain_t)       == 5);
    EXPECT(sizeof(phy_rpc_req_set_agc_max_gain_t)  == 5);
    EXPECT(sizeof(phy_rpc_req_set_cca_t)           == 5);
    EXPECT(sizeof(phy_rpc_req_set_low_rate_t)      == 5);
    EXPECT(sizeof(phy_rpc_req_set_bandwidth_t)     == 5);

    /* Response bodies (after the 8-byte resp header). */
    EXPECT(sizeof(phy_rpc_resp_get_phy_rssi_t)        == 8 + 1);
    EXPECT(sizeof(phy_rpc_resp_get_cca_counters_t)    == 8 + 8);

    /* Field offsets we'll rely on cross-compiler. */
    EXPECT(offsetof(phy_rpc_resp_hdr_t,    op_id)   == 0);
    EXPECT(offsetof(phy_rpc_resp_hdr_t,    status)  == 4);
    /* OCB-frame event: rssi(1) channel(1) src_mac(6) llc_len(2)
     *                  timestamp(4) data[]
     * Total fixed prefix = 14, then variable-length payload. */
    EXPECT(offsetof(phy_rpc_evt_ocb_frame_t, src_mac)         == 2);
    EXPECT(offsetof(phy_rpc_evt_ocb_frame_t, llc_payload_len) == 8);
    EXPECT(offsetof(phy_rpc_evt_ocb_frame_t, timestamp_us)    == 10);
    EXPECT(offsetof(phy_rpc_evt_ocb_frame_t, data)            == 14);

    return 0;
}

int test_proto_msgid_namespace(void)
{
    /* All CITS msg_ids live in 0xC175.... */
    EXPECT((PHY_RPC_REQ_SET_CHANNEL  & 0xFFFF0000u) == PHY_RPC_MSG_BASE);
    EXPECT((PHY_RPC_RESP_SET_CHANNEL & 0xFFFF0000u) == PHY_RPC_MSG_BASE);
    EXPECT((PHY_RPC_EVT_OCB_FRAME    & 0xFFFF0000u) == PHY_RPC_MSG_BASE);

    /* Direction encoding. */
    EXPECT((PHY_RPC_REQ_SET_CHANNEL  & 0x0000F000u) == 0x0000);
    EXPECT((PHY_RPC_RESP_SET_CHANNEL & 0x0000F000u) == 0x8000);
    EXPECT((PHY_RPC_EVT_OCB_FRAME    & 0x0000F000u) == 0xF000);

    /* Request and response share the same low nibble. */
    EXPECT((PHY_RPC_REQ_SET_CHANNEL  & 0xFFFu) == (PHY_RPC_RESP_SET_CHANNEL & 0xFFFu));
    EXPECT((PHY_RPC_REQ_GET_PHY_RSSI & 0xFFFu) == (PHY_RPC_RESP_GET_PHY_RSSI & 0xFFFu));

    /* Specific known values — fail loudly on accidental renumbering. */
    EXPECT(PHY_RPC_REQ_SET_CHANNEL    == 0xC1750001u);
    EXPECT(PHY_RPC_RESP_SET_CHANNEL   == 0xC1758001u);
    EXPECT(PHY_RPC_REQ_GET_INFO       == 0xC175000Du);
    EXPECT(PHY_RPC_RESP_GET_INFO      == 0xC175800Du);
    EXPECT(PHY_RPC_EVT_OCB_FRAME      == 0xC175F001u);

    /* Expanded surface — every wrap landed in the 0x010-0x01E range. */
    EXPECT(PHY_RPC_REQ_SET_FREQ          == 0xC1750010u);
    EXPECT(PHY_RPC_REQ_SET_BAND          == 0xC1750011u);
    EXPECT(PHY_RPC_REQ_SET_RATE          == 0xC1750014u);
    EXPECT(PHY_RPC_REQ_GET_NOISE_FLOOR   == 0xC1750018u);
    EXPECT(PHY_RPC_REQ_GET_TEMPERATURE   == 0xC1750019u);
    EXPECT(PHY_RPC_REQ_SET_BT_TX_GAIN    == 0xC175001Cu);
    EXPECT(PHY_RPC_REQ_GET_CAPS          == 0xC175001Eu);

    return 0;
}

/* The slave's GET_CAPS response packs availability of each request id
 * into a 4-byte bitmap: bit (id & 0xFF) of caps[(id&0xFF)/8]. Make sure
 * the host's helper agrees with that layout. */
int test_proto_caps_bitmap(void)
{
    /* Pretend the slave replied "everything available". */
    uint8_t all_on[4] = {0xff, 0xff, 0xff, 0xff};

    /* Pretend the slave replied "nothing in 0x10-0x1F" (e.g. an older
     * chip with only the original 13 RPCs). */
    uint8_t old_chip[4] = {0xff, 0x3f, 0x00, 0x00};

    /* SET_CHANNEL (id 0x001) -> bit 1 of caps[0] */
    EXPECT((all_on[0]    & (1u << 1)) != 0);
    EXPECT((old_chip[0]  & (1u << 1)) != 0);

    /* GET_CAPS (id 0x01E) -> bit 6 of caps[3] */
    EXPECT((all_on[3]    & (1u << 6)) != 0);
    EXPECT((old_chip[3]  & (1u << 6)) == 0);

    /* SET_FREQ (id 0x010) -> bit 0 of caps[2] */
    EXPECT((all_on[2]    & (1u << 0)) != 0);
    EXPECT((old_chip[2]  & (1u << 0)) == 0);

    return 0;
}
