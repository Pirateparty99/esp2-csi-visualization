#ifndef MESH_HEARTBEAT_H
#define MESH_HEARTBEAT_H

#include "esp_mesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "csi_udp_sender.h"

// Heartbeat packets carry no sensing data -- their only purpose is to keep
// every link in the mesh tree busy with real 802.11 traffic. The ESP32's CSI
// hardware only fires on RX (including the automatic MAC-layer ACK a unicast
// send gets back), and ESP-MESH's own control-plane chatter alone is far too
// sparse to produce a continuous CSI stream (observed: one CSI capture during
// association, then none for 30+ seconds of otherwise-idle connection).
//
// Every non-root node pings its own parent upstream, which alone covers
// every parent-child edge in the tree -- the ACK that comes back triggers
// CSI at the child, and the data frame arriving triggers CSI at the parent.
// Root has no mesh parent to ping, so it pings its UDP target (the
// router/host) directly instead, which keeps root's own CSI flowing too.
//
// mesh_root_rx_task recognizes this marker and silently drops it instead of
// forwarding it to the UDP consumer.
#define MESH_HEARTBEAT_PAYLOAD "{\"type\":\"HEARTBEAT\"}"
#define MESH_HEARTBEAT_PAYLOAD_LEN (sizeof(MESH_HEARTBEAT_PAYLOAD) - 1)

static inline void mesh_heartbeat_task(void *pv) {
    for (;;) {
#if defined CONFIG_PACKET_RATE && (CONFIG_PACKET_RATE > 0)
        vTaskDelay(pdMS_TO_TICKS(1000 / CONFIG_PACKET_RATE));
#else
        vTaskDelay(pdMS_TO_TICKS(50));
#endif

        if (!esp_mesh_is_device_active()) {
            continue;
        }

        if (esp_mesh_is_root()) {
            csi_udp_sender_ping();
            continue;
        }

        mesh_data_t pkt;
        pkt.data = (uint8_t *) MESH_HEARTBEAT_PAYLOAD;
        pkt.size = MESH_HEARTBEAT_PAYLOAD_LEN;
        pkt.proto = MESH_PROTO_JSON;
        pkt.tos = MESH_TOS_P2P;

        // Best-effort: esp_mesh_send() can transiently fail (e.g. with
        // ESP_ERR_MESH_NO_ROUTE_FOUND right as a parent link comes up).
        // That's fine here -- this traffic exists only to trigger CSI, not
        // to deliver anything, so failures are silently ignored.
        esp_mesh_send(NULL, &pkt, MESH_DATA_TODS, NULL, 0);
    }
}

#endif // MESH_HEARTBEAT_H
