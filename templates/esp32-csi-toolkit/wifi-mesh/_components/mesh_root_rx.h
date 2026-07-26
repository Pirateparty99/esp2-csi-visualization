#ifndef MESH_ROOT_RX_H
#define MESH_ROOT_RX_H

#include <cstring>
#include "esp_mesh.h"
#include "csi_udp_sender.h"
#include "mesh_heartbeat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *MESH_RX_TAG = "mesh_root_rx";

// Leaf nodes send CSI upstream with the MESH_DATA_TODS flag, which marks a
// packet as destined for the external IP network rather than for another
// node inside the mesh. Those land in the root's separate toDS queue and are
// only drained by esp_mesh_recv_toDS() -- plain esp_mesh_recv() never sees
// them. Draining the wrong queue leaves the toDS queue to fill up and stalls
// the mesh's upstream flow-control window, which shows up on leaves as
// repeating "[WND-RX] ... 1200 ms timeout" warnings with a climbing
// timeout_count.
static inline void mesh_root_rx_task(void *pv) {
    static uint8_t rx_buf[2048];
    mesh_data_t data;
    data.data = rx_buf;
    data.size = sizeof(rx_buf);

    for (;;) {
        mesh_addr_t from;
        mesh_addr_t to;
        int flag = 0;
        data.size = sizeof(rx_buf); // reset each call -- recv shrinks this to actual received length
        esp_err_t err = esp_mesh_recv_toDS(&from, &to, &data, portMAX_DELAY, &flag, NULL, 0);
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
        } else if (err == ESP_ERR_MESH_RECV_RELEASE) {
            // Normal control signal (the stack is releasing a pending
            // toDS read, e.g. around a root change), not a failure.
            continue;
        } else {
            ESP_LOGW(MESH_RX_TAG, "esp_mesh_recv_toDS error: 0x%x", err);
        }
    }
}

#endif // MESH_ROOT_RX_H