#ifndef MESH_HEARTBEAT_H
#define MESH_HEARTBEAT_H

#include "esp_mesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "csi_udp_sender.h"
#include "mesh_csi_sender.h"   // reuse mesh_send_locked()

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
static const char *MESH_HEARTBEAT_TAG = "mesh_heartbeat";

#define MESH_HEARTBEAT_PAYLOAD "{\"type\":\"HEARTBEAT\"}"
#define MESH_HEARTBEAT_PAYLOAD_LEN (sizeof(MESH_HEARTBEAT_PAYLOAD) - 1)

static inline void mesh_heartbeat_task(void *pv) {
    // Counterpart to the root's forwarding report: without this there is no
    // way to tell a leaf that is sending fine from one whose sends are all
    // failing, since heartbeat failures are otherwise ignored by design.
    uint32_t n_ok = 0, n_fail = 0;
    esp_err_t last_err = ESP_OK;
    int64_t last_report_us = esp_timer_get_time();
    const int64_t REPORT_INTERVAL_US = 5000000; // 5s

    for (;;) {
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_report_us >= REPORT_INTERVAL_US) {
            last_report_us = now_us;
            ESP_LOGI(MESH_HEARTBEAT_TAG,
                     "heartbeat sends: %u ok, %u failed (last err 0x%x), layer:%d root:%d",
                     n_ok, n_fail, last_err, esp_mesh_get_layer(),
                     esp_mesh_is_root() ? 1 : 0);
            n_ok = 0;
            n_fail = 0;
        }

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

        // Addressed to the root (NULL "to" with flag 0), matching
        // mesh_csi_sender_send().
        //
        // MESH_DATA_NONBLOCK is essential, not an optimization:
        // esp_mesh_send() blocks by default, and if the upstream window
        // never opens the very first call never returns -- the task then
        // makes exactly one attempt and hangs forever, which is what was
        // observed (no heartbeat report ever printed on a leaf).
        //
        // Best-effort: sends can transiently fail (e.g. with
        // ESP_ERR_MESH_NO_ROUTE_FOUND right as a parent link comes up).
        // That's fine here -- this traffic exists only to trigger CSI, not
        // to deliver anything, so failures only feed the counters above.
        esp_err_t err = mesh_send_locked(NULL, &pkt, MESH_DATA_NONBLOCK, 0);
        if (err == ESP_OK) {
            n_ok++;
        } else {
            n_fail++;
            last_err = err;
        }
    }
}

#endif // MESH_HEARTBEAT_H
