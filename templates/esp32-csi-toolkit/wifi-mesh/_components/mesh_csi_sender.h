#ifndef MESH_CSI_SENDER_H
#define MESH_CSI_SENDER_H

#include <cstring>
#include "esp_mesh.h"
#include "esp_wifi_types.h"
#include "lwip/inet.h"
#include "csi_udp_sender.h"   // reuse csi_to_json()

static const char *MESH_CSI_TAG = "mesh_csi";
#define MESH_CSI_JSON_BUF_SIZE 2048

// Destination for upstream mesh traffic sent with MESH_DATA_TODS.
//
// esp_mesh_send() treats the "to" argument and the flags as a matched pair:
// a NULL "to" means "deliver to the root itself" and pairs with flag 0
// (read back via esp_mesh_recv), while MESH_DATA_TODS means "deliver to an
// external IP network" and requires "to" to carry the IPv4:PORT of that
// destination (read back via esp_mesh_recv_toDS). Passing NULL together
// with MESH_DATA_TODS mixes the two and leaves the packet without a usable
// external destination.
//
// Our root ignores the address it receives and forwards to its own
// configured UDP target, but the mesh stack still needs a well-formed one
// here to route the packet as toDS traffic at all.
static inline void mesh_csi_udp_target(mesh_addr_t *out) {
    memset(out, 0, sizeof(*out));
    out->mip.ip4.addr = ipaddr_addr(CONFIG_UDP_TARGET_IP);
    out->mip.port = htons(CONFIG_UDP_TARGET_PORT);
}

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

    mesh_addr_t to;
    mesh_csi_udp_target(&to);

    esp_err_t err = esp_mesh_send(&to, &mesh_pkt, MESH_DATA_TODS, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(MESH_CSI_TAG, "esp_mesh_send failed: 0x%x", err);
    }
}

#endif // MESH_CSI_SENDER_H