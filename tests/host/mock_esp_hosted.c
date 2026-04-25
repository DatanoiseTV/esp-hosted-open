/*
 * mock_esp_hosted.c — host-side stand-in for esp-hosted's peer-data
 * channel. Lets us run the entire host dispatcher (phy_rpc.c) under
 * gcc with a fake slave in the same process, no IDF and no chip.
 *
 * Real flow (on hardware):
 *
 *   host                         slave
 *    │                             │
 *    │ esp_hosted_send_custom_data │
 *    │ ──────────── SDIO ─────────►│  callback registered for msg_id
 *    │                             │  ... handler runs ...
 *    │             SDIO            │  esp_hosted_send_custom_data
 *    │ ◄───────────────────────────│  (response with status + body)
 *    │ on_response invoked         │
 *    │ in_flight slot signalled    │
 *
 * This shim wires the two halves together in-process.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "esp_err.h"

/* ---------- ultra-minimal callback registry --------------------- */

#define MAX_CB 64

typedef void (*custom_cb_t)(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx);

static struct {
    uint32_t    msg_id;
    custom_cb_t cb;
    void       *ctx;
    int         used;
} s_table[MAX_CB];

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

esp_err_t esp_hosted_register_custom_callback(
    uint32_t msg_id,
    void (*cb)(uint32_t, const uint8_t *, size_t, void *),
    void *ctx)
{
    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < MAX_CB; i++) {
        if (s_table[i].used && s_table[i].msg_id == msg_id) {
            s_table[i].cb  = cb;
            s_table[i].ctx = ctx;
            if (!cb) s_table[i].used = 0;
            pthread_mutex_unlock(&s_mtx);
            return ESP_OK;
        }
    }
    if (cb) {
        for (int i = 0; i < MAX_CB; i++) {
            if (!s_table[i].used) {
                s_table[i].msg_id = msg_id;
                s_table[i].cb     = cb;
                s_table[i].ctx    = ctx;
                s_table[i].used   = 1;
                pthread_mutex_unlock(&s_mtx);
                return ESP_OK;
            }
        }
    }
    pthread_mutex_unlock(&s_mtx);
    return ESP_ERR_NO_MEM;
}

static custom_cb_t lookup(uint32_t msg_id, void **ctx_out)
{
    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < MAX_CB; i++) {
        if (s_table[i].used && s_table[i].msg_id == msg_id) {
            custom_cb_t cb = s_table[i].cb;
            *ctx_out = s_table[i].ctx;
            pthread_mutex_unlock(&s_mtx);
            return cb;
        }
    }
    pthread_mutex_unlock(&s_mtx);
    return NULL;
}

/* ---------- the fake slave -------------------------------------- */

#include "phy_rpc_proto.h"

/* External hook so a test can install its own slave-side handler for
 * a given request id. Default is the auto-reply ECHO logic below. */
typedef int (*mock_slave_handler_t)(uint32_t req_id,
                                    const uint8_t *body, size_t body_len,
                                    uint32_t op_id);

static mock_slave_handler_t s_slave_handler;

void mock_set_slave_handler(mock_slave_handler_t h) { s_slave_handler = h; }

/* Helper for tests: post a response back to the host as if the slave
 * had replied. Wraps the cits_rpc_resp_hdr_t framing. */
void mock_post_response(uint32_t resp_id, uint32_t op_id, int32_t status,
                        const void *body, size_t body_len)
{
    size_t total = sizeof(phy_rpc_resp_hdr_t) + body_len;
    uint8_t *buf = malloc(total);
    if (!buf) return;
    phy_rpc_resp_hdr_t *hdr = (phy_rpc_resp_hdr_t *)buf;
    hdr->op_id  = op_id;
    hdr->status = status;
    if (body_len) memcpy(buf + sizeof(*hdr), body, body_len);

    void *ctx;
    custom_cb_t cb = lookup(resp_id, &ctx);
    if (cb) cb(resp_id, buf, total, ctx);
    free(buf);
}

/* mock_post_event: same as response but for async events (no op_id
 * correlation, body-only). */
void mock_post_event(uint32_t evt_id, const void *body, size_t body_len)
{
    void *ctx;
    custom_cb_t cb = lookup(evt_id, &ctx);
    if (cb) cb(evt_id, (const uint8_t *)body, body_len, ctx);
}

/* esp_hosted_send_custom_data is what the host dispatcher calls to
 * send a request. We treat it as "request received by slave" — pull
 * the op_id from the body, dispatch to our slave handler. */
esp_err_t esp_hosted_send_custom_data(uint32_t msg_id,
                                      const uint8_t *data, size_t len)
{
    if (len < sizeof(phy_rpc_hdr_t)) return ESP_ERR_INVALID_SIZE;
    const phy_rpc_hdr_t *hdr = (const phy_rpc_hdr_t *)data;
    uint32_t op = hdr->op_id;
    const uint8_t *body = data + sizeof(*hdr);
    size_t  body_len    = len - sizeof(*hdr);

    if (s_slave_handler) {
        return (esp_err_t)s_slave_handler(msg_id, body, body_len, op);
    }
    /* Default: ECHO — reply OK with the request body. The mock posts
     * the response back on the matching response id. */
    uint32_t resp_id = (msg_id & ~0xF000u) | 0x8000u;
    mock_post_response(resp_id, op, ESP_OK, body, body_len);
    return ESP_OK;
}
