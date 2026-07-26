#ifndef MESH_CSI_SENDER_H
#define MESH_CSI_SENDER_H

#include "esp_mesh.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "csi_udp_sender.h"   // reuse csi_to_json()

static const char *MESH_CSI_TAG = "mesh_csi";

// Filler traffic. Heartbeats carry no sensing data -- their only purpose is
// to keep every link in the mesh tree busy with real 802.11 frames. The
// ESP32's CSI hardware only fires on RX (including the automatic MAC-layer
// ACK a unicast send gets back), and ESP-MESH's own control-plane chatter is
// far too sparse to sustain a CSI stream: an otherwise-idle node produced one
// capture during association and none for the next 30+ seconds.
//
// mesh_root_rx.h recognizes this marker and drops it instead of forwarding.
#define MESH_HEARTBEAT_PAYLOAD "{\"type\":\"HEARTBEAT\"}"
#define MESH_HEARTBEAT_PAYLOAD_LEN (sizeof(MESH_HEARTBEAT_PAYLOAD) - 1)

// A serialized CSI reading waiting to go out. csi_to_json() emits at most
// CSI_UDP_MAX_VALUES samples, which fits comfortably here; snprintf truncates
// rather than overruns if that ever changes.
#define MESH_CSI_JSON_MAX    1200
#define MESH_CSI_QUEUE_DEPTH 8

typedef struct {
    uint16_t len;
    char json[MESH_CSI_JSON_MAX];
} mesh_csi_item_t;

static QueueHandle_t s_mesh_csi_queue = NULL;
static uint32_t s_mesh_csi_dropped = 0;

static inline void mesh_csi_sender_init(void) {
    if (s_mesh_csi_queue == NULL) {
        s_mesh_csi_queue = xQueueCreate(MESH_CSI_QUEUE_DEPTH, sizeof(mesh_csi_item_t));
    }
}

// Called from the Wi-Fi CSI callback, so this only serializes and enqueues --
// it never touches esp_mesh_send().
//
// Sending directly from here was tried and does not work: esp_mesh_send() is
// documented as not reentrant and blocks by default, so calling it from the
// callback (which runs on the Wi-Fi task holding csi_component.h's mutex)
// raced the sender task and wedged the CSI RX path. Guarding it with a mutex
// instead just moved the failure -- CSI is triggered *by* the heartbeat
// traffic, so captures land exactly while the sender holds the lock, and
// every CSI send then timed out on it.
static inline void mesh_csi_sender_send(const wifi_csi_info_t *data) {
    if (!esp_mesh_is_device_active()) {
        return;
    }
    if (esp_mesh_is_root()) {
        // Root is still a sensing node, but it has no parent to relay
        // through -- it owns the UDP socket, so its own captures go
        // straight out.
        csi_udp_sender_send(data);
        return;
    }
    if (s_mesh_csi_queue == NULL) {
        return;
    }

    // static rather than a ~1.2KB stack frame on the Wi-Fi task. Safe because
    // csi_component.h serializes callbacks behind its own mutex.
    static mesh_csi_item_t item;
    int len = csi_to_json(data, item.json, sizeof(item.json));
    if (len <= 0) {
        return;
    }
    item.len = (uint16_t) len;

    // Never block the callback: drop under backpressure instead.
    if (xQueueSend(s_mesh_csi_queue, &item, 0) != pdTRUE) {
        s_mesh_csi_dropped++;
    }
}

// Sole owner of esp_mesh_send(). Because the API is not reentrant, exactly
// one task may ever call it -- enforcing that single-owner rule is why this
// task exists, and why it needs no lock of its own.
//
// Draining CSI takes priority; heartbeats go out only when the queue is
// empty, so filler traffic naturally backs off once real captures flow.
static inline void mesh_tx_task(void *pv) {
    static mesh_csi_item_t item;
    uint32_t n_csi = 0, n_hb = 0, n_fail = 0;
    esp_err_t last_err = ESP_OK;
    int64_t last_report_us = esp_timer_get_time();
    const int64_t REPORT_INTERVAL_US = 5000000; // 5s

#if defined CONFIG_PACKET_RATE && (CONFIG_PACKET_RATE > 0)
    const TickType_t idle_wait = pdMS_TO_TICKS(1000 / CONFIG_PACKET_RATE);
#else
    const TickType_t idle_wait = pdMS_TO_TICKS(50);
#endif

    for (;;) {
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_report_us >= REPORT_INTERVAL_US) {
            last_report_us = now_us;
            ESP_LOGI(MESH_CSI_TAG,
                     "tx: %u CSI, %u heartbeats, %u failed (last 0x%x), %u dropped, layer:%d root:%d",
                     n_csi, n_hb, n_fail, last_err, s_mesh_csi_dropped,
                     esp_mesh_get_layer(), esp_mesh_is_root() ? 1 : 0);
            n_csi = 0;
            n_hb = 0;
            n_fail = 0;
            s_mesh_csi_dropped = 0;
        }

        if (!esp_mesh_is_device_active()) {
            vTaskDelay(idle_wait);
            continue;
        }

        if (esp_mesh_is_root()) {
            // No parent to relay through. Keep the root's own uplink busy so
            // it still captures CSI; anything from below is handled by
            // mesh_root_rx_task.
            csi_udp_sender_ping();
            vTaskDelay(idle_wait);
            continue;
        }

        mesh_data_t pkt;
        // Doubles as this loop's pacing: with nothing queued we wait here for
        // one heartbeat interval before emitting filler.
        bool have_csi = (s_mesh_csi_queue != NULL &&
                         xQueueReceive(s_mesh_csi_queue, &item, idle_wait) == pdTRUE);
        if (have_csi) {
            pkt.data = (uint8_t *) item.json;
            pkt.size = item.len;
        } else {
            pkt.data = (uint8_t *) MESH_HEARTBEAT_PAYLOAD;
            pkt.size = MESH_HEARTBEAT_PAYLOAD_LEN;
        }
        pkt.proto = MESH_PROTO_JSON;
        pkt.tos = MESH_TOS_P2P;

        // Addressed to the root itself: NULL "to" with flag 0 is the pairing
        // esp_mesh_send() defines for that, and the root reads it back with
        // esp_mesh_recv(). The external-IP form (MESH_DATA_TODS +
        // esp_mesh_recv_toDS) was tried first and never delivered, even with a
        // root holding a DHCP lease and posting toDS reachability.
        //
        // NONBLOCK so a closed upstream window drops a packet rather than
        // stalling this task indefinitely.
        esp_err_t err = esp_mesh_send(NULL, &pkt, MESH_DATA_NONBLOCK, NULL, 0);
        if (err == ESP_OK) {
            if (have_csi) {
                n_csi++;
            } else {
                n_hb++;
            }
        } else {
            n_fail++;
            last_err = err;
        }
    }
}

#endif // MESH_CSI_SENDER_H
