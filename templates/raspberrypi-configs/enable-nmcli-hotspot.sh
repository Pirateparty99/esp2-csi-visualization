#!/bin/bash

# The channel is pinned, not auto-selected. CSI is frequency dependent, so an
# RTI baseline captured on one channel is meaningless on another, and
# node-to-node links require every node parked on the same channel. If this
# changes, it must change in the ESP firmware's WIFI_CHANNEL too, on every
# node, followed by a fresh --calibrate.
#
# Channel 11 chosen from a survey of this site (see docs/firmware.md):
# channels 1 and 6 each had ~12 APs within +/-4 channels, 11 had one at -75dBm.

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
        802-11-wireless.channel 11 \
        802-11-wireless-security.pmf 1


sudo nmcli con up espeon-hotspot