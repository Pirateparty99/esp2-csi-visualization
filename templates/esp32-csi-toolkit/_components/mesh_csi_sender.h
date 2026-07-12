#ifndef MESH_CSI_SENDER_H
#define MESH_CSI_SENDER_H

#include "esp_mesh.h"
#include "esp_wifi_types.h"
#include "csi_udp_sender.h"   // reuse csi_to_json()

static const char *MESH_CSI_TAG = "mesh_csi";
#define MESH_CSI_JSON_BUF_SIZE 2048

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

    esp_err_t err = esp_mesh_send(NULL, &mesh_pkt, MESH_DATA_TODS, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(MESH_CSI_TAG, "esp_mesh_send failed: 0x%x", err);
    }
}

#endif // MESH_CSI_SENDER_H