#ifndef MESH_ROOT_RX_H
#define MESH_ROOT_RX_H

#include <cstring>
#include "esp_mesh.h"
#include "csi_udp_sender.h"
#include "mesh_heartbeat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *MESH_RX_TAG = "mesh_root_rx";

static inline void mesh_root_rx_task(void *pv) {
    static uint8_t rx_buf[2048];
    mesh_data_t data;
    data.data = rx_buf;
    data.size = sizeof(rx_buf);

    for (;;) {
        mesh_addr_t from;
        int flag = 0;
        data.size = sizeof(rx_buf); // reset each call -- esp_mesh_recv shrinks this to actual received length
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err == ESP_OK) {
            if (data.proto == MESH_PROTO_JSON) {
                // Heartbeat packets exist purely to keep mesh links busy so
                // CSI keeps firing -- they carry no sensing data, so drop
                // them here instead of forwarding to the UDP consumer.
                if (data.size == MESH_HEARTBEAT_PAYLOAD_LEN &&
                    memcmp(data.data, MESH_HEARTBEAT_PAYLOAD, MESH_HEARTBEAT_PAYLOAD_LEN) == 0) {
                    continue;
                }
                csi_udp_sender_send_raw((const char *) data.data, data.size);
            } else {
                ESP_LOGW(MESH_RX_TAG, "Dropped non-JSON mesh packet: proto=0x%x size=%d",
                         data.proto, data.size);
            }
        } else {
            ESP_LOGW(MESH_RX_TAG, "esp_mesh_recv error: 0x%x", err);
        }
    }
}

#endif // MESH_ROOT_RX_H