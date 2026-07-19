#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "../../_components/nvs_component.h"
#include "../../_components/sd_component.h"
#include "../../_components/csi_component.h"
#include "../../_components/time_component.h"
#include "../../_components/input_component.h"
#include "../../_components/sockets_component.h"
#include "../../_components/csi_udp_sender.h"
#include "../../_components/mesh_csi_sender.h"
#include "../../_components/mesh_root_rx.h"

#define ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD

#ifdef CONFIG_WIFI_CHANNEL
#define WIFI_CHANNEL CONFIG_WIFI_CHANNEL
#else
#define WIFI_CHANNEL 6
#endif

#ifdef CONFIG_SHOULD_COLLECT_CSI
#define SHOULD_COLLECT_CSI 1
#else
#define SHOULD_COLLECT_CSI 0
#endif

#ifdef CONFIG_SHOULD_COLLECT_ONLY_LLTF
#define SHOULD_COLLECT_ONLY_LLTF 1
#else
#define SHOULD_COLLECT_ONLY_LLTF 0
#endif

#ifdef CONFIG_SEND_CSI_TO_SERIAL
#define SEND_CSI_TO_SERIAL 1
#else
#define SEND_CSI_TO_SERIAL 0
#endif

#ifdef CONFIG_SEND_CSI_TO_SD
#define SEND_CSI_TO_SD 1
#else
#define SEND_CSI_TO_SD 0
#endif

// Fallback so this compiles even if CONFIG_MESH_* isn't defined yet
// (e.g. before the first menuconfig pass after adding the Kconfig block).
#ifndef CONFIG_MESH_ID
#define CONFIG_MESH_ID "77:77:77:77:77:77"
#endif
#ifndef CONFIG_MESH_MAX_LAYER
#define CONFIG_MESH_MAX_LAYER 6
#endif
#ifndef CONFIG_MESH_AP_PASSWORD
#define CONFIG_MESH_AP_PASSWORD "meshpass123"
#endif

static EventGroupHandle_t s_wifi_event_group;
static const char *TAG = "Active CSI collection (Mesh)";

static bool s_is_mesh_root = false;
static TaskHandle_t s_mesh_root_rx_handle = NULL;

// Parses "aa:bb:cc:dd:ee:ff" style hex string from Kconfig into a mesh_addr_t.
static void mesh_id_from_string(const char *str, mesh_addr_t *out) {
    unsigned int bytes[6];
    sscanf(str, "%x:%x:%x:%x:%x:%x",
           &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]);
    for (int i = 0; i < 6; i++) {
        out->addr[i] = (uint8_t) bytes[i];
    }
}

static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // Only fires when using an external router uplink
        // (CONFIG_MESH_ROUTER_SSID set). In no-router/standalone mode
        // this never fires -- the root's own softAP interface handles
        // the UDP send to the host PC instead.
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Root got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        return;
    }

    switch (event_id) {
        case MESH_EVENT_STARTED: {
            ESP_LOGI(TAG, "Mesh started, layer:%d", esp_mesh_get_layer());
            break;
        }
        case MESH_EVENT_PARENT_CONNECTED: {
            mesh_event_connected_t *connected = (mesh_event_connected_t *) event_data;
            ESP_LOGI(TAG, "Parent connected, layer:%d%s",
                     connected->self_layer,
                     esp_mesh_is_root() ? " (this node is ROOT)" : "");
            break;
        }
        case MESH_EVENT_PARENT_DISCONNECTED: {
            ESP_LOGW(TAG, "Parent disconnected -- mesh will attempt reconnect");
            break;
        }
        case MESH_EVENT_ROOT_ADDRESS: {
            mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *) event_data;
            ESP_LOGI(TAG, "Root address: " MACSTR, MAC2STR(root_addr->addr));
            break;
        }
        default:
            break;
    }

    // Role can change at runtime under auto-election -- re-check on every
    // event rather than latching a decision made once at boot.
    bool now_root = esp_mesh_is_root();
    if (now_root && !s_is_mesh_root) {
        ESP_LOGI(TAG, "This node became ROOT -- starting UDP sender + mesh RX task");
        csi_udp_sender_init();
        xTaskCreatePinnedToCore(&mesh_root_rx_task, "mesh_root_rx", 4096,
                                 NULL, 5, &s_mesh_root_rx_handle, 1);
        s_is_mesh_root = true;
    } else if (!now_root && s_is_mesh_root) {
        ESP_LOGW(TAG, "This node lost ROOT role -- stopping mesh RX task");
        if (s_mesh_root_rx_handle) {
            vTaskDelete(s_mesh_root_rx_handle);
            s_mesh_root_rx_handle = NULL;
        }
        s_is_mesh_root = false;
    }
}

void mesh_csi_init(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    static esp_netif_t *s_mesh_netif_sta = NULL;
    static esp_netif_t *s_mesh_netif_ap = NULL;
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&s_mesh_netif_sta, &s_mesh_netif_ap));

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_mesh_init());

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &mesh_event_handler, NULL, NULL));

    mesh_cfg_t mesh_cfg = MESH_INIT_CONFIG_DEFAULT();
    mesh_id_from_string(CONFIG_MESH_ID, &mesh_cfg.mesh_id);
    mesh_cfg.channel = WIFI_CHANNEL;
    mesh_cfg.mesh_ap.max_connection = 6;
    strlcpy((char *) mesh_cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWORD,
            sizeof(mesh_cfg.mesh_ap.password));

#if defined(CONFIG_MESH_ROUTER_SSID)
    if (strlen(CONFIG_MESH_ROUTER_SSID) > 0) {
        strlcpy((char *) mesh_cfg.router.ssid, CONFIG_MESH_ROUTER_SSID,
                sizeof(mesh_cfg.router.ssid));
        strlcpy((char *) mesh_cfg.router.password, CONFIG_MESH_ROUTER_PASSWORD,
                sizeof(mesh_cfg.router.password));
    } else {
        ESP_ERROR_CHECK(esp_mesh_fix_root(false));  // empty SSID - no-router mode
        ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK));
    }
#else
    ESP_ERROR_CHECK(esp_mesh_fix_root(false));  // auto root-election, no-router mode
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK));
#endif

    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_cfg));
    ESP_ERROR_CHECK(esp_mesh_start());

    ESP_LOGI(TAG, "mesh_csi_init finished. mesh_id:%s channel:%d",
             CONFIG_MESH_ID, WIFI_CHANNEL);
}

void config_print() {
    printf("\n\n\n\n\n\n\n\n");
    printf("-----------------------\n");
    printf("ESP32 CSI Tool Settings\n");
    printf("-----------------------\n");
    printf("PROJECT_NAME: %s\n", "ACTIVE_MESH");
    printf("CONFIG_ESPTOOLPY_MONITOR_BAUD: %d\n", CONFIG_ESPTOOLPY_MONITOR_BAUD);
    printf("CONFIG_ESP_CONSOLE_UART_BAUDRATE: %d\n", CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    printf("IDF_VER: %s\n", IDF_VER);
    printf("-----------------------\n");
    printf("WIFI_CHANNEL: %d\n", WIFI_CHANNEL);
    printf("MESH_ID: %s\n", CONFIG_MESH_ID);
    printf("MESH_MAX_LAYER: %d\n", CONFIG_MESH_MAX_LAYER);
    printf("SHOULD_COLLECT_CSI: %d\n", SHOULD_COLLECT_CSI);
    printf("SHOULD_COLLECT_ONLY_LLTF: %d\n", SHOULD_COLLECT_ONLY_LLTF);
    printf("SEND_CSI_TO_SERIAL: %d\n", SEND_CSI_TO_SERIAL);
    printf("SEND_CSI_TO_SD: %d\n", SEND_CSI_TO_SD);
#if CONFIG_SEND_CSI_TO_UDP
    printf("SEND_CSI_TO_UDP (root only): 1 (%s:%d)\n",
           CONFIG_UDP_TARGET_IP, CONFIG_UDP_TARGET_PORT);
#else
    printf("SEND_CSI_TO_UDP: 0\n");
#endif
    printf("-----------------------\n");
    printf("\n\n\n\n\n\n\n\n");
}

extern "C" void app_main() {
    config_print();
    nvs_init();
    sd_init();
    mesh_csi_init();

#if !(SHOULD_COLLECT_CSI)
    printf("CSI will not be collected. Check `idf.py menuconfig  # > ESP32 CSI Tool Config` to enable CSI");
#endif

    csi_init((char *) "MESH");
}