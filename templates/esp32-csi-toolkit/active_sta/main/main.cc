#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/inet.h"          // <-- add: for ipaddr_addr()
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/inet.h"     // <-- add: for ipaddr_addr()
#include "esp_netif.h"      // <-- add: for esp_netif_set_ip_info/hostname
#include "esp_netif.h"           // <-- add: for esp_netif_set_ip_info/hostname
#include "dhcpserver/dhcpserver.h" // <-- add: for dhcps_lease_t

#include "../../_components/nvs_component.h"
#include "../../_components/sd_component.h"
#include "../../_components/csi_component.h"
#include "../../_components/time_component.h"
#include "../../_components/input_component.h"
#include "../../_components/sockets_component.h"
#include "../../_components/csi_udp_sender.h"

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

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = NULL;   // <-- add
const int WIFI_CONNECTED_BIT = BIT0;

static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

static const char *TAG = "Active CSI collection (Station)";

esp_err_t _http_event_handle(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (!real_time_set) {
                    char *data = (char *) malloc(evt->data_len + 1);
                    strncpy(data, (char *) evt->data, evt->data_len);
                    data[evt->data_len + 1] = '\0';
                    time_set(data);
                    free(data);
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t esp_wifi_80211_tx(wifi_interface_t ifx, const void *buffer, int len, bool en_sys_seq);

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
#if CONFIG_STA_STATIC_IP_ENABLE
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(s_sta_netif));

        esp_netif_ip_info_t sta_ip_info = {0};
        sta_ip_info.ip.addr      = ipaddr_addr(CONFIG_STA_IP_ADDR);
        sta_ip_info.gw.addr      = ipaddr_addr(CONFIG_STA_GATEWAY_ADDR);
        sta_ip_info.netmask.addr = ipaddr_addr(CONFIG_STA_NETMASK_ADDR);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(s_sta_netif, &sta_ip_info));

        ESP_LOGI(TAG, "Static IP set: " IPSTR, IP2STR(&sta_ip_info.ip));

        // With a static IP there's no DHCP handshake, so IP_EVENT_STA_GOT_IP
        // never fires on its own -- set the bit and start the UDP sender here.
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
#if CONFIG_SEND_CSI_TO_UDP
        csi_udp_sender_init();
#endif
#endif
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Retry connecting to the AP");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

#if CONFIG_SEND_CSI_TO_UDP
        csi_udp_sender_init();
#endif
    }
}

bool is_wifi_connected() {
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT);
}

void station_init() {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_sta_config_t wifi_sta_config = {};
    wifi_sta_config.channel = WIFI_CHANNEL;
    wifi_config_t wifi_config = {
            .sta = wifi_sta_config,
    };

    strlcpy((char *) wifi_config.sta.ssid, ESP_WIFI_SSID, sizeof(ESP_WIFI_SSID));
    strlcpy((char *) wifi_config.sta.password, ESP_WIFI_PASS, sizeof(ESP_WIFI_PASS));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "connect to ap SSID:%s password:%s", ESP_WIFI_SSID, ESP_WIFI_PASS);
}

TaskHandle_t xHandle = NULL;

void vTask_socket_transmitter_sta_loop(void *pvParameters) {
    for (;;) {
        socket_transmitter_sta_loop(&is_wifi_connected);
    }
}

void config_print() {
    printf("\n\n\n\n\n\n\n\n");
    printf("-----------------------\n");
    printf("ESP32 CSI Tool Settings\n");
    printf("-----------------------\n");
    printf("PROJECT_NAME: %s\n", "ACTIVE_STA");
    printf("CONFIG_ESPTOOLPY_MONITOR_BAUD: %d\n", CONFIG_ESPTOOLPY_MONITOR_BAUD);
    printf("CONFIG_ESP_CONSOLE_UART_BAUDRATE: %d\n", CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    printf("IDF_VER: %s\n", IDF_VER);
    printf("-----------------------\n");
    printf("WIFI_CHANNEL: %d\n", WIFI_CHANNEL);
    printf("ESP_WIFI_SSID: %s\n", ESP_WIFI_SSID);
    printf("ESP_WIFI_PASSWORD: %s\n", ESP_WIFI_PASS);
    printf("ESP_WIFI_PASSWORD: %s\n", ESP_WIFI_PASS);
    printf("STA_HOSTNAME: %s\n", CONFIG_STA_HOSTNAME);
#if CONFIG_STA_STATIC_IP_ENABLE
    printf("STA_STATIC_IP: %s (gw %s / mask %s)\n",
           CONFIG_STA_IP_ADDR, CONFIG_STA_GATEWAY_ADDR, CONFIG_STA_NETMASK_ADDR);
#else
    printf("STA_STATIC_IP: disabled (using DHCP)\n");
#endif
    printf("PACKET_RATE: %i\n", CONFIG_PACKET_RATE);
    printf("SHOULD_COLLECT_CSI: %d\n", SHOULD_COLLECT_CSI);
    printf("SHOULD_COLLECT_ONLY_LLTF: %d\n", SHOULD_COLLECT_ONLY_LLTF);
    printf("SEND_CSI_TO_SERIAL: %d\n", SEND_CSI_TO_SERIAL);
    printf("SEND_CSI_TO_SD: %d\n", SEND_CSI_TO_SD);
#if CONFIG_SEND_CSI_TO_UDP
    printf("SEND_CSI_TO_UDP: 1 (%s:%d)\n", CONFIG_UDP_TARGET_IP, CONFIG_UDP_TARGET_PORT);
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
    station_init();
    csi_init((char *) "STA");

#if !(SHOULD_COLLECT_CSI)
    printf("CSI will not be collected. Check `idf.py menuconfig  # > ESP32 CSI Tool Config` to enable CSI");
#endif

    xTaskCreatePinnedToCore(&vTask_socket_transmitter_sta_loop, "socket_transmitter_sta_loop",
                            10000, (void *) &is_wifi_connected, 100, &xHandle, 1);
}