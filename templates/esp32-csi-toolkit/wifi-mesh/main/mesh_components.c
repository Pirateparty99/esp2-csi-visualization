#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "esp_wifi.h"
#include "esp_netif.h"

static mesh_addr_t s_mesh_id = {0};

void mesh_csi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_mesh_sta();  // creates the mesh's internal netif

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_mesh_init());

    mesh_cfg_t mesh_cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy(&mesh_cfg.mesh_id, &s_mesh_id, sizeof(mesh_addr_t));
    mesh_cfg.channel = CONFIG_WIFI_CHANNEL;
    mesh_cfg.mesh_ap.max_connection = 6;
    strlcpy((char *)mesh_cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWORD,
            sizeof(mesh_cfg.mesh_ap.password));

#if defined(CONFIG_MESH_ROUTER_SSID) && CONFIG_MESH_ROUTER_SSID[0] != '\0'
    strlcpy((char *)mesh_cfg.router.ssid, CONFIG_MESH_ROUTER_SSID,
            sizeof(mesh_cfg.router.ssid));
    strlcpy((char *)mesh_cfg.router.password, CONFIG_MESH_ROUTER_PASSWORD,
            sizeof(mesh_cfg.router.password));
#else
    ESP_ERROR_CHECK(esp_mesh_fix_root(false));
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK));
#endif

    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_cfg));
    ESP_ERROR_CHECK(esp_mesh_start());
}