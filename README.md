# esp2-csi-visualization
This repo is intended to ingest CSI data from ESP32s to create a visualization of sensed objects. Very much a test/POC.

This repo adds UDP forwarding to the ESP CSI Toolkit project with templates to set the WiFi name/password and destination host/port.

This repo also includes a script to setup the ESP IDF with version 4.3.3 to build the firmware.

# Usage

1, Configure AP/STA settings
2. Build/flash ESP32 firmware
3. Calibrate the rti_aggreagotor.py visualization script:
   ```
   python visualizations/rti-aggregator.py --calibrate --calibrate-seconds 30 --room-width 4 --room-height 3
   ```
4. Run the rti_aggreagotor.py visualization script:
   ```
   python visualizations/rti-aggregator.py --room-width 4 --room-height 3
   ```

CSI Fields (In order of how the fields are sent in the JSON messages)
type,role,mac,rssi,rate,sig_mode,mcs,bandwidth,smoothing,not_sounding,aggregation,stbc,fec_coding,sgi,noise_floor,ampdu_cnt,channel,secondary_channel,local_timestamp,ant,sig_len,rx_state,real_time_set,real_timestamp,len,CSI_DATA
