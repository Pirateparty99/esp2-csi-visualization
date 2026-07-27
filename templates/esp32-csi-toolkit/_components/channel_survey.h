#ifndef CHANNEL_SURVEY_H
#define CHANNEL_SURVEY_H

#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"

// Reports how busy each 2.4GHz channel looks, once, at boot.
//
// Deliberately read-only: it never changes channel. CSI is frequency
// dependent, so a baseline captured on one channel says nothing about
// another -- a node that reroamed would silently produce phantom detections
// rather than an error. Node-to-node links additionally require every node
// parked on the same channel, since promiscuous capture only hears the
// current one. So the survey exists to inform a deployment-time decision that
// is then pinned, not to drive runtime switching.
//
// Run before associating: scanning while connected disrupts the link.

static const char *CHANNEL_SURVEY_TAG = "chan_survey";

// Bounds the scan allocation. Dense environments can return far more; the
// extras are dropped rather than sized for, since this is advisory output.
#define CHANNEL_SURVEY_MAX_APS 48
#define CHANNEL_SURVEY_MAX_CHANNEL 14

static inline void channel_survey_run(void) {
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.ssid = NULL;
    scan_cfg.bssid = NULL;
    scan_cfg.channel = 0;          // all channels
    scan_cfg.show_hidden = true;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(CHANNEL_SURVEY_TAG, "scan failed: 0x%x (skipping survey)", err);
        return;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        ESP_LOGI(CHANNEL_SURVEY_TAG, "no neighbouring APs detected");
        return;
    }

    uint16_t want = found > CHANNEL_SURVEY_MAX_APS ? CHANNEL_SURVEY_MAX_APS : found;
    wifi_ap_record_t *records =
        (wifi_ap_record_t *) malloc(want * sizeof(wifi_ap_record_t));
    if (records == NULL) {
        ESP_LOGW(CHANNEL_SURVEY_TAG, "out of memory for %u records; skipping survey", want);
        esp_wifi_scan_stop();
        return;
    }

    if (esp_wifi_scan_get_ap_records(&want, records) != ESP_OK) {
        ESP_LOGW(CHANNEL_SURVEY_TAG, "could not read scan records; skipping survey");
        free(records);
        return;
    }

    int ap_count[CHANNEL_SURVEY_MAX_CHANNEL + 1] = {0};
    int strongest[CHANNEL_SURVEY_MAX_CHANNEL + 1];
    for (int i = 0; i <= CHANNEL_SURVEY_MAX_CHANNEL; i++) {
        strongest[i] = -127;
    }

    for (int i = 0; i < want; i++) {
        int ch = records[i].primary;
        if (ch < 1 || ch > CHANNEL_SURVEY_MAX_CHANNEL) {
            continue;
        }
        ap_count[ch]++;
        if (records[i].rssi > strongest[ch]) {
            strongest[ch] = records[i].rssi;
        }
    }
    free(records);

    ESP_LOGI(CHANNEL_SURVEY_TAG, "---- 2.4GHz channel survey (%u APs%s) ----",
             found, found > want ? ", truncated" : "");
    ESP_LOGI(CHANNEL_SURVEY_TAG, "  ch | APs | strongest | note");

    for (int ch = 1; ch <= CHANNEL_SURVEY_MAX_CHANNEL; ch++) {
        if (ap_count[ch] == 0) {
            continue;
        }
        // 1/6/11 are the non-overlapping choices on 2.4GHz; anything else
        // overlaps two of them and is a poor pick regardless of its own count.
        const char *note = (ch == 1 || ch == 6 || ch == 11) ? "non-overlapping" : "overlaps";
        ESP_LOGI(CHANNEL_SURVEY_TAG, "  %2d | %3d | %4d dBm  | %s",
                 ch, ap_count[ch], strongest[ch], note);
    }

    // Occupancy on a channel is not just its own APs: 2.4GHz channels are
    // 20MHz wide on 5MHz spacing, so traffic within +/-4 bleeds in. Compare
    // 1/6/11 on that basis rather than on their own counts alone.
    int best = 0, best_load = 0;
    for (int i = 0; i < 3; i++) {
        const int candidates[3] = {1, 6, 11};
        int ch = candidates[i];
        int load = 0;
        for (int c = ch - 4; c <= ch + 4; c++) {
            if (c >= 1 && c <= CHANNEL_SURVEY_MAX_CHANNEL) {
                load += ap_count[c];
            }
        }
        ESP_LOGI(CHANNEL_SURVEY_TAG, "  channel %2d: %d APs within +/-4", ch, load);
        if (best == 0 || load < best_load) {
            best = ch;
            best_load = load;
        }
    }

    ESP_LOGI(CHANNEL_SURVEY_TAG,
             "quietest non-overlapping channel looks like %d (%d APs nearby)",
             best, best_load);
    ESP_LOGI(CHANNEL_SURVEY_TAG,
             "advisory only -- channel is NOT changed. To move: set it on the AP "
             "and in WIFI_CHANNEL, reflash every node, then re-run --calibrate.");
}

#endif // CHANNEL_SURVEY_H
