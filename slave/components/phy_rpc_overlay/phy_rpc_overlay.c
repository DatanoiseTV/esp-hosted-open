/*
 * phy_rpc_overlay — slave-side PHY hack and app_main override.
 *
 * Sequencing:
 *   1. NVS init.
 *   2. esp_hosted_coprocessor_init()  (upstream's slave bring-up).
 *   3. slave_phy_apply()              (our 11p hack on top).
 *
 * Upstream's own app_main is disabled via
 * CONFIG_ESP_HOSTED_COPROCESSOR_APP_MAIN=n in sdkconfig.defaults, so
 * only ours is linked.
 */

#include "esp_log.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_hosted_coprocessor.h"

extern void phy_rpc_handlers_register(void);
extern void phy_rpc_extras_register(void);
extern void phy_rpc_wireless_register(void);

static const char *TAG = "phy_rpc_slave";

extern void phy_11p_set        (int enable, int reserved);
extern void phy_change_channel  (int channel, int a, int b, int ht_mode);

static esp_err_t slave_phy_apply(uint8_t ieee_chan)
{
    wifi_country_t country = {
        .cc           = "01",
        .schan        = 1,
        .nchan        = 200,
        .max_tx_power = 23,
        .policy       = WIFI_COUNTRY_POLICY_MANUAL,
    };
    esp_err_t err = esp_wifi_set_country(&country);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_country: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "phy_11p_set(1, 0)");
    phy_11p_set(1, 0);

    err = esp_wifi_set_channel(140, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bootstrap set_channel(140): %s", esp_err_to_name(err));
        return err;
    }

    phy_change_channel(ieee_chan, 1, 0, 0);
    ESP_LOGI(TAG, "tuned to ITS channel %u (~%u MHz)",
             ieee_chan, 5000 + 5 * ieee_chan);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

#if CONFIG_PHY_RPC_OVERLAY_REAPPLY_ON_WIFI_READY
static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_WIFI_READY) {
        ESP_LOGI(TAG, "Wi-Fi ready — re-applying 11p hack");
        slave_phy_apply((uint8_t)CONFIG_PHY_RPC_OVERLAY_CHANNEL);
    }
}
#endif

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "starting esp-hosted coprocessor (vendored upstream)");
    ESP_ERROR_CHECK(esp_hosted_coprocessor_init());

#if CONFIG_PHY_RPC_OVERLAY_REAPPLY_ON_WIFI_READY
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
#endif

    ESP_ERROR_CHECK(slave_phy_apply((uint8_t)CONFIG_PHY_RPC_OVERLAY_CHANNEL));

    /* Register the host-driven control RPCs (set channel, gain, CCA,
     * etc.) on top of esp-hosted's custom-data channel. Requires
     * CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y on both sides. */
    phy_rpc_handlers_register();
    phy_rpc_extras_register();
    phy_rpc_wireless_register();

    ESP_LOGI(TAG, "slave ready: ITS ch %u, 11p mode armed, RPCs live",
             CONFIG_PHY_RPC_OVERLAY_CHANNEL);

    while (1) vTaskDelay(pdMS_TO_TICKS(60000));
}
