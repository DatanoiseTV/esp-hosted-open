/*
 * host_p4_demo — minimal P4 application that uses esp_hosted_open
 * over SDIO to talk to an esp-hosted-open slave.
 *
 * What it does at boot:
 *   1. esp_hosted_init()             — bring up SDIO transport
 *   2. esp_hosted_open_init()        — register CITS custom-RPC handlers
 *   3. get_caps()                    — see which RPCs the slave actually
 *                                      exposes on this chip
 *   4. get_info()                    — current slave state
 *   5. set_channel(180)              — tune the C5 to CCH (5.900 GHz)
 *   6. get_phy_rssi()                — single-shot RSSI poll
 *   7. esp_now_init() + add_peer +
 *      send                           — fire one ESP-NOW broadcast
 *
 * Each call uses *only* SDIO — no UART / serial console traffic, no
 * SPI fallback. SDIO transport is asserted in sdkconfig.defaults.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_hosted.h"
#include "esp_hosted_open.h"
#include "phy_rpc_proto.h"      /* PHY_RPC_REQ_* ids for capability checks */

static const char *TAG = "host_demo";

static void on_espnow_rx(const uint8_t *data, size_t len,
                         const esp_hosted_open_espnow_meta_t *m, void *ctx)
{
    ESP_LOGI(TAG, "ESP-NOW rx %u B from %02x:%02x:%02x:%02x:%02x:%02x  rssi=%d",
             (unsigned)len,
             m->src_mac[0], m->src_mac[1], m->src_mac[2],
             m->src_mac[3], m->src_mac[4], m->src_mac[5],
             m->rssi_dbm);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* Bring up the SDIO transport to the slave. */
    ESP_LOGI(TAG, "starting esp-hosted SDIO link…");
    if (esp_hosted_init() != 0) {
        ESP_LOGE(TAG, "esp_hosted_init failed");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* Register the CITS custom-RPC layer. */
    ESP_ERROR_CHECK(esp_hosted_open_init());
    ESP_ERROR_CHECK(esp_hosted_open_register_espnow_rx_cb(on_espnow_rx, NULL));

    /* Capability discovery — works for the full 0x000-0x07F namespace
     * thanks to the 16-byte bitmap. */
    uint8_t caps[16];
    if (esp_hosted_open_get_caps(caps) == ESP_OK) {
        ESP_LOGI(TAG, "slave caps:");
        ESP_LOGI(TAG, "  channel set:      %d", esp_hosted_open_has_capability(PHY_RPC_REQ_SET_CHANNEL,    caps));
        ESP_LOGI(TAG, "  802.11p:          %d", esp_hosted_open_has_capability(PHY_RPC_REQ_SET_PHY_11P,    caps));
        ESP_LOGI(TAG, "  raw 802.11 TX:    %d", esp_hosted_open_has_capability(PHY_RPC_REQ_TX_80211,       caps));
        ESP_LOGI(TAG, "  ESP-NOW:          %d", esp_hosted_open_has_capability(PHY_RPC_REQ_ESPNOW_INIT,    caps));
        ESP_LOGI(TAG, "  802.15.4:         %d", esp_hosted_open_has_capability(PHY_RPC_REQ_IEEE154_ENABLE, caps));
    }

    esp_hosted_open_info_t info;
    if (esp_hosted_open_get_info(&info) == ESP_OK) {
        ESP_LOGI(TAG, "slave info: ch=%u tx=%d dBm 11p=%d cca=%d  fw=%s",
                 info.channel, info.tx_power_dbm,
                 info.phy_11p_armed, info.cca_enabled, info.fw_version);
    }

    /* Tune to ITS CCH (5.900 GHz). NOT_SUPPORTED on chips without 5 GHz. */
    if (esp_hosted_open_has_capability(PHY_RPC_REQ_SET_CHANNEL, caps)) {
        esp_err_t err = esp_hosted_open_set_channel(180);
        ESP_LOGI(TAG, "set_channel(180): %s", esp_err_to_name(err));
    }

    int8_t rssi;
    if (esp_hosted_open_get_phy_rssi(&rssi) == ESP_OK) {
        ESP_LOGI(TAG, "PHY RSSI = %d dBm", rssi);
    }

    /* ESP-NOW broadcast — universal across every Wi-Fi-capable chip. */
    if (esp_hosted_open_has_capability(PHY_RPC_REQ_ESPNOW_INIT, caps)) {
        ESP_ERROR_CHECK(esp_hosted_open_espnow_init());
        static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        esp_hosted_open_espnow_add_peer(broadcast, NULL, 0, 0, false);
        const char *msg = "hello from p4";
        esp_err_t err = esp_hosted_open_espnow_send(broadcast,
                                                    (const uint8_t *)msg,
                                                    strlen(msg));
        ESP_LOGI(TAG, "espnow_send broadcast: %s", esp_err_to_name(err));
    }

    /* Idle. SDIO transport drives everything via interrupts. */
    while (1) vTaskDelay(pdMS_TO_TICKS(60000));
}
