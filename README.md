# bluetooth-temperature-sensors
Read Bluetooth Advertising Packets from BLE temperature sensors and publish data to MQTT

This program decodes the bluetooth advertising packets for the following BLE temperature and humidity sensors:
```
//  1 = Xiaomi LYWSD03MMC-ATC   https://github.com/atc1441/ATC_MiThermometer
//  2 = Govee H5052 (type 4 advertising packets)
//  3 = Govee H5072
//  4 = Govee H5102
//  5 = Govee H5075
//  6 = Govee H5074 (type 4 advertising packets)
// 99 = Display raw type 0 and type 4 advertising packets for this BLE MAC address
```

The program uses the bluetooth and mqtt client libraries, which may need to be installed to compile and possibly run the program.

To compile:

```
gcc -o ble_sensor_mqtt_pub ble_sensor_mqtt_pub.c -lbluetooth  -l paho-mqtt3c
```

To run:
```
sudo ble_sensor_mqtt_pub <bluetooth adapter> <scan type> <scan window> <scan interval>

bluetooth adapter = integer number of bluetooth devices, run hciconfig to see your adapters
scan type = 0 for passive, 1 for active advertising scan, some BLE sensors only share data on type 4 response active advertising packets
scan window = integer number that is multiplied by 0.625 to set advertising scanning window in milliseconds. Try 100 to start.
scan interval = integer number that is multiplied by 0.625 to set advertising scanning interval in milliseconds. Try 1000 to start.
BLE scanning requires root equivalent rights, therefor sudo is necessary.
 ```
 
Example JSON published to MQTT topic:
```
topic:
homeassistant/sensor/ble-temp/A4:C1:38:22:13:D0

payload:
{"timestamp":"20201206025836","mac-address":"A4:C1:38:22:13:D0","rssi":-69,"temperature":64.4,"units":"F","temperature-celsius":18.0,"humidity":44.0,"battery-pct":93,"sensor-name":"","location":"H5072 Kitchen","sensor-type":"3"}
```
At the top of each hour the program will publish a count of the total number of advertising packets seen for each sensor in the prior hour to MQTT. This is useful to check the bluetooth frequency reception for each sensor as well as the quality and frequency of readings for each sensor type. The sub topic for this is:
```
$SYS/hour-stats
```
Example:
```
{
  "timestamp": "20201206110010",
  "aa:bb:cc:dd:ee:ff": {
    "count": 365,
    "location": "LYWSD03MMC Living Room"
  },
  "aa:bb:cc:dd:ee:ff": {
    "count": 140,
    "location": "LYWSD03MMC Shared Bathroom"
  },
.
.
.
.

  },
  "aa:bb:cc:dd:ee:ff": {
    "count": 384,
    "location": "LYWSD03MMC Dining Room"
  },
  "aa:bb:cc:dd:ee:ff": {
    "count": 397,
    "location": "LYWSD03MMC Attic"
  },
  "aa:bb:cc:dd:ee:ff": {
    "count": 288,
    "location": "H5052 Refrigerator"
  },
  "aa:bb:cc:dd:ee:ff": {
    "count": 767,
    "location": "H5052 Backyard"
  },
  "aa:bb:cc:dd:ee:ff": {
    "count": 680,
    "location": "H5052 Freezer"
  },
  .
  .
  .
  .
    "aa:bb:cc:dd:ee:ff": {
    "count": 330,
    "location": "H5072 Kitchen"
  },
  "aa:bb:cc:dd:ee:ff": {
    "count": 351,
    "location": "H5102 test unit"
  },
  "aa:bb:cc:dd:ee:ff": {
    "count": 344,
    "location": "H5075 test unit"
  },
  "aa:bb:cc:dd:ee:ff": {
    "count": 433,
    "location": "H5074 test unit"
  },
  "total_adv_packets": 7900
}

```

Configuration file:

see included configuration file, this sets MQTT server, MQTT base topic and details about each BLE sensor. Key info you need to have is MAC Address of each sensor and the device type of each.

Dumping raw advertising packets to console:

If you add the MAC address of a BLE device to the configuration file and give it a type of '99', the program will display the raw type 0 and 4 advertising packet data to the console. Useful to help figure out the data format of a new temperature and humidity sensor.

Example dump:

```
=========
Current local time and date: Sun Dec  6 13:55:55 2020
mac address =  E0:12:1D:33:82:11  location = H5074 test unit device type = 99 advertising_packet_type = 000
==>0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2 2 2 2 2 2 2 2 2 2 3 3 3 3 3 3 3 3 3 3 4 4 4 4 4 4 4 4 4 4 5 5 5 5 5 5 5 5 5 5 6
==>0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
==>                            0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2
==>                            0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
==>043E29020100002780221D12E01D02010607030A18F5FE88EC1109476F7665655F48353037345F38303237BC
==>__________ad________________________mmmmmmmmmmmmtttthhbbzbzbccrr
rssi         = -68
=========
Current local time and date: Sun Dec  6 13:55:55 2020
mac address =  E0:12:1D:33:82:11  location = H5074 test unit device type = 99 advertising_packet_type = 004
==>0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2 2 2 2 2 2 2 2 2 2 3 3 3 3 3 3 3 3 3 3 4 4 4 4 4 4 4 4 4 4 5 5 5 5 5 5 5 5 5 5 6
==>0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
==>                            0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2
==>                            0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
==>043E17020104002780221D12E00B0AFF88EC005806D0106402C0
==>__________ad________________________mmmmmmmmmmmmtttthhbbzbzbccrr
rssi         = -64
=========
```
