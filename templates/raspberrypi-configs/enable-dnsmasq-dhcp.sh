#!/bin/bash

sudo apt install -y dnsmasq udhcpd

sudo cp dnsmasq.conf /etc/dnsmasq/dnsmasq.conf

sudo systemctl enable --now dnsmasq

sudo sysctl -w net.ipv4.ip_forward=1
echo "net.ipv4.ip_forward=1" | sudo tee -a /etc/sysctl.conf