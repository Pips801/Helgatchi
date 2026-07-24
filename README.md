# Helgatchi
Handheld BLE + WiFi Hunter



## What is it?
A portable, hand-held BLE beacon and WiFi AP scanner/hunter, with built-in alerting, 1.69” full-color screen, and external SMA-RP antenna all powered by the XIAO ESP32-S3. 

The Helgatchi will passively scan for BLE devices and WiFi SSIDs/APs, and alert when specific Manufacturers, naming schemes, or services are discovered. 

It can be configured to alert anything you want, such as:
- Flock technology
- AXON technology
- Wearable health devices
- Any specific device
- Hidden WiFi/Bluetooth cameras

## Key Features
- Enclosed, portable, battery powered handheld rechargeable BLE and WiFi scanner and hunter that can live in your pocket.
- Passively scans for beacons in the background in-between sleeping, and actively scans when an alert is triggered.
- Powered by the XIAO ESP32-S3
- 6 RGB LEDs
- 1.69" rounded screen
- Vibration motor for alerts
- You can change
  - Color scheme
  - Alert settings
    - Vibrate
    - LEDs
    - Screen Wake
  - Scan settings
    - Sleep duration
    - Scan duration
  - Rules for alerts
    - MAC address (BLE, WiFi)
    - Manufacturer (BLE, WiFi OUI)
    - Device name (BLE name, WiFi SSID)
    - Service (BLE)

## Specifications

| Spec      | Info                                             	|
|----------	|---------------------------------------------------	|
| MCU      	| XIAO ESP32-S3                                     	|
| Flash    	| 8MB                                               	|
| Battery  	| 400mAh - 1200mAh 3.7v LiPo                        	|
| Screen   	| Waveshare 1.69” LCD - 240p × 280p                 	|
| Firmware 	| platform.io + Arduino                             	|
| Software 	| Graphics: LVGL BLE: NimBLE LEDs: FastLED          	|
| Antenna  	| XIAO -> u.FL > back PCB > SMA-RP > 2.4Ghz antenna 	|
| Size     	| Width: 50mm Length: 80mm Height: TBD              	|
