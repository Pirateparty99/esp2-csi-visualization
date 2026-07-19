sudo cp csi-udp-relay.py /opt/csi-udp-relay.py
sudo chmod +x /opt/csi-udp-relay.py

sudo cp csi-relay.service /etc/systemd/system/csi-relay.service 

sudo systemctl daemon-reload
sudo systemctl enable --now csi-relay