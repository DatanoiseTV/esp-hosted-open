#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t esp_hosted_send_custom_data(uint32_t msg_id_to_send,
                                      const uint8_t *data_to_send,
                                      size_t data_len_to_send);

esp_err_t esp_hosted_register_custom_callback(
    uint32_t msg_id_exp,
    void (*callback)(uint32_t msg_id_recvd, const uint8_t *data_recvd,
                     size_t data_len_recvd, void *local_context),
    void *local_context);
