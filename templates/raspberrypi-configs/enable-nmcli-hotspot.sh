#!/bin/bash

sudo nmcli con add \
        type wifi \
        ifname wlan0 \
        con-name espeon-hotspot \
        autoconnect yes \
        ssid "espeon"

sudo nmcli con modify espeon-hotspot \
        ipv4.method shared \
        ipv4.addresses "192.168.4.1/24"

sudo nmcli con modify espeon-hotspot \
        wifi-sec.key-mgmt wpa-psk \
        wifi-sec.psk "espeon123!"


sudo nmcli con modify espeon-hotspot \
        802-11-wireless.mode ap \
        802-11-wireless.band bg \
        802-11-wireless-security.pmf 1


sudo nmcli con up espeon-hotspot