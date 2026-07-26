#ifndef MESH_CSI_SENDER_H
#define MESH_CSI_SENDER_H

#include "esp_mesh.h"
#include "esp_wifi_types.h"
#include "csi_udp_sender.h"   // reuse csi_to_json()

static const char *MESH_CSI_TAG = "mesh_csi";
#define MESH_CSI_JSON_BUF_SIZE 2048

// esp_mesh_send() is documented as not reentrant, and there are two
// independent callers here: the heartbeat task and the Wi-Fi CSI callback.
// Without serializing them the second caller enters while the first is still
// inside the API. Observed symptom: a leaf's heartbeat task stopped after its
// very first send and never printed again, while the root received nothing.
//
// Bounded try-take rather than a blocking take, so no caller is parked here
// indefinitely. Callers pass their own budget: CSI is the actual payload and
// waits briefly, while the heartbeat is only filler traffic and gives up
// immediately rather than starving CSI of the lock.
static SemaphoreHandle_t s_mesh_send_mutex = xSemaphoreCreateMutex();

static inline esp_err_t mesh_send_locked(const mesh_addr_t *to, mesh_data_t *data,
                                          int flag, uint32_t wait_ms) {
    if (s_mesh_send_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mesh_send_mutex, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = esp_mesh_send(to, data, flag, NULL, 0);
    xSemaphoreGive(s_mesh_send_mutex);
    return err;
}

// Upstream sends address the root directly rather than an external IP.
//
// esp_mesh_send() treats "to" and the flags as a matched pair: a NULL "to"
// with flag 0 means "deliver to the root itself" (read back with
// esp_mesh_recv), while MESH_DATA_TODS means "deliver to an external IP
// network" and needs "to" to carry that IPv4:PORT (read back with
// esp_mesh_recv_toDS).
//
// The toDS form was tried first and did not deliver: with a root that had a
// DHCP lease and had posted toDS reachability, the root still received
// nothing while leaves logged continuous "[WND-RX] ... 1200 ms timeout"
// warnings -- their upstream window never opened. Addressing the root
// directly avoids the toDS window machinery altogether, and the root still
// forwards to the configured UDP target itself.

static inline void mesh_csi_sender_send(const wifi_csi_info_t *data) {
    if (!esp_mesh_is_device_active()) {
        return;
    }
    if (esp_mesh_is_root()) {
        // Root captures CSI locally too (it's still a sensing node) --
        // send straight to UDP instead of looping it through the mesh.
        csi_udp_sender_send(data);
        return;
    }

    static char json_buf[MESH_CSI_JSON_BUF_SIZE];
    int len = csi_to_json(data, json_buf, sizeof(json_buf));
    if (len <= 0) {
        return;
    }

    mesh_data_t mesh_pkt;
    mesh_pkt.data = (uint8_t *) json_buf;
    mesh_pkt.size = (uint16_t) len;
    mesh_pkt.proto = MESH_PROTO_JSON;
    mesh_pkt.tos = MESH_TOS_P2P;

    // MESH_DATA_NONBLOCK is required here. This runs inside the Wi-Fi CSI
    // callback while csi_component.h holds its mutex, so a blocking send
    // that never completes wedges the whole CSI RX path, not just this
    // packet. Dropping a sample under backpressure is the right trade.
    esp_err_t err = mesh_send_locked(NULL, &mesh_pkt, MESH_DATA_NONBLOCK, 30);
    if (err != ESP_OK) {
        // Rate-limited: under sustained backpressure this fires per capture.
        static int64_t last_warn_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_warn_us >= 5000000) {
            last_warn_us = now_us;
            ESP_LOGW(MESH_CSI_TAG, "esp_mesh_send failed: 0x%x", err);
        }
    }
}

#endif // MESH_CSI_SENDER_H