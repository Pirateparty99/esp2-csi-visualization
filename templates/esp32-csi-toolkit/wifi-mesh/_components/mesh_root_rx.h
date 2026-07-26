#ifndef MESH_ROOT_RX_H
#define MESH_ROOT_RX_H

#include <cstring>
#include "esp_mesh.h"
#include "csi_udp_sender.h"
#include "mesh_heartbeat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *MESH_RX_TAG = "mesh_root_rx";

// Leaves address their CSI to the root itself (NULL "to", flag 0), so it
// arrives on the normal mesh receive path and esp_mesh_recv() is the
// matching call. The root then forwards to the configured UDP target.
//
// See mesh_csi_sender.h for why the external-IP (MESH_DATA_TODS /
// esp_mesh_recv_toDS) form is not used.
static inline void mesh_root_rx_task(void *pv) {
    static uint8_t rx_buf[2048];
    mesh_data_t data;
    data.data = rx_buf;
    data.size = sizeof(rx_buf);

    // Forwarding happens silently, so without a periodic count there is no
    // way to tell "the mesh is delivering CSI" apart from "nothing is
    // arriving at all" by watching the root's console.
    uint32_t n_forwarded = 0, n_heartbeat = 0;
    int64_t last_report_us = esp_timer_get_time();
    const int64_t REPORT_INTERVAL_US = 5000000; // 5s

    for (;;) {
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_report_us >= REPORT_INTERVAL_US) {
            last_report_us = now_us;
            ESP_LOGI(MESH_RX_TAG, "forwarded %u CSI packets, dropped %u heartbeats",
                     n_forwarded, n_heartbeat);
            n_forwarded = 0;
            n_heartbeat = 0;
        }

        mesh_addr_t from;
        int flag = 0;
        data.size = sizeof(rx_buf); // reset each call -- recv shrinks this to actual received length
        // Bounded wait rather than portMAX_DELAY so the periodic report
        // above still fires when no traffic is arriving -- which is exactly
        // the case worth reporting.
        esp_err_t err = esp_mesh_recv(&from, &data, 1000, &flag, NULL, 0);
        if (err == ESP_OK) {
            if (data.proto == MESH_PROTO_JSON) {
                // Heartbeat packets exist purely to keep mesh links busy so
                // CSI keeps firing -- they carry no sensing data, so drop
                // them here instead of forwarding to the UDP consumer.
                if (data.size == MESH_HEARTBEAT_PAYLOAD_LEN &&
                    memcmp(data.data, MESH_HEARTBEAT_PAYLOAD, MESH_HEARTBEAT_PAYLOAD_LEN) == 0) {
                    n_heartbeat++;
                    continue;
                }
                csi_udp_sender_send_raw((const char *) data.data, data.size);
                n_forwarded++;
            } else {
                ESP_LOGW(MESH_RX_TAG, "Dropped non-JSON mesh packet: proto=0x%x size=%d",
                         data.proto, data.size);
            }
        } else if (err == ESP_ERR_MESH_TIMEOUT) {
            continue; // no traffic this interval -- expected, keeps reporting
        } else {
            ESP_LOGW(MESH_RX_TAG, "esp_mesh_recv error: 0x%x", err);
        }
    }
}

#endif // MESH_ROOT_RX_H