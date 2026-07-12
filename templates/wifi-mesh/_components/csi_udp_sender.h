#ifndef CSI_UDP_SENDER_H
#define CSI_UDP_SENDER_H

#include <cstring>
#include "esp_log.h"
#include "esp_wifi_types.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#ifndef CONFIG_UDP_TARGET_IP
#define CONFIG_UDP_TARGET_IP "0.0.0.0"
#endif
#ifndef CONFIG_UDP_TARGET_PORT
#define CONFIG_UDP_TARGET_PORT 0
#endif

static const char *CSI_UDP_TAG = "csi_udp";
static int csi_udp_sock = -1;
static struct sockaddr_in csi_udp_dest_addr;

#define CSI_UDP_MAX_VALUES    256
#define CSI_UDP_JSON_BUF_SIZE 2048

static inline void csi_udp_sender_init(void) {
    if (csi_udp_sock >= 0) {
        close(csi_udp_sock);
        csi_udp_sock = -1;
    }

    csi_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (csi_udp_sock < 0) {
        ESP_LOGE(CSI_UDP_TAG, "Unable to create UDP socket: errno %d", errno);
        return;
    }

    memset(&csi_udp_dest_addr, 0, sizeof(csi_udp_dest_addr));
    csi_udp_dest_addr.sin_family = AF_INET;
    csi_udp_dest_addr.sin_port = htons(CONFIG_UDP_TARGET_PORT);
    inet_pton(AF_INET, CONFIG_UDP_TARGET_IP, &csi_udp_dest_addr.sin_addr);

    ESP_LOGI(CSI_UDP_TAG, "UDP CSI sender targeting %s:%d",
             CONFIG_UDP_TARGET_IP, CONFIG_UDP_TARGET_PORT);
}

// --- NEW: pure encoder, no socket dependency. Reusable by both the
// direct-UDP path (active_ap/active_sta, unchanged) and the mesh path
// (non-root nodes encode here, then hand bytes to esp_mesh_send()).
// Returns the encoded length, or 0 on truncation/error.
static inline int csi_to_json(const wifi_csi_info_t *data, char *out_buf, size_t out_buf_size) {
    int offset = 0;

    int n = data->len;
    if (n > CSI_UDP_MAX_VALUES) {
        n = CSI_UDP_MAX_VALUES;
    }

    offset += snprintf(out_buf + offset, out_buf_size - offset,
        "{\"type\":\"CSI_DATA\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"rssi\":%d,\"len\":%d,\"csi\":[",
        data->mac[0], data->mac[1], data->mac[2],
        data->mac[3], data->mac[4], data->mac[5],
        data->rx_ctrl.rssi,
        data->len);

    for (int i = 0; i < n; i++) {
        offset += snprintf(out_buf + offset, out_buf_size - offset,
                            "%s%d", (i == 0 ? "" : ","), (int) data->buf[i]);
    }

    offset += snprintf(out_buf + offset, out_buf_size - offset, "]}");

    if (offset < 0 || (size_t)offset >= out_buf_size) {
        ESP_LOGE(CSI_UDP_TAG, "csi_to_json: buffer too small, payload truncated");
        return 0;
    }
    return offset;
}

// --- NEW: raw transmit, no encoding. This is what the mesh root's
// receive task calls directly with bytes it got from esp_mesh_recv() --
// those bytes are already JSON, encoded by the relaying node's csi_to_json().
static inline void csi_udp_sender_send_raw(const char *payload, size_t len) {
    if (csi_udp_sock < 0) {
        return;
    }
    int err = sendto(csi_udp_sock, payload, len, 0,
                      (struct sockaddr *) &csi_udp_dest_addr, sizeof(csi_udp_dest_addr));
    if (err < 0) {
        ESP_LOGE(CSI_UDP_TAG, "sendto failed: errno %d", errno);
    }
}

// Unchanged signature/behavior for existing active_ap/active_sta callers --
// now just a thin wrapper: encode, then send.
static inline void csi_udp_sender_send(const wifi_csi_info_t *data) {
    static char json_buf[CSI_UDP_JSON_BUF_SIZE];
    int len = csi_to_json(data, json_buf, sizeof(json_buf));
    if (len > 0) {
        csi_udp_sender_send_raw(json_buf, (size_t)len);
    }
}

#endif // CSI_UDP_SENDER_H