# bluetooth-temperature-sensors
Read Bluetooth Advertising Packets from BLE temperature sensors and publish data to MQTT. This C language program runs on Linux. I have successfully used it on Raspberry Pi OS, Ubuntu 18 and Ubuntu 20.

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
BLE scanning requires root equivalent rights, therefore sudo is necessary.
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

Please note the program is very UNFORGIVING of format mistakes in this file! Lines must match format, the third line of column headers is required. See included configuration file, this sets MQTT server, MQTT base topic and details about each BLE sensor. Key info you need to have is MAC Address of each sensor and the device type of each.

```
tcp://172.148.5.11:1883
homeassistant/sensor/ble-temp/
mac address, type, location
DD:C1:38:70:0C:24,  1, Living Room
DD:C1:38:AC:77:44,  3, Shared Bathroom
DD:12:1D:22:80:77,  6, Attic
DD:C1:38:AC:28:A2, 99, LYWSD03MMC test

```

Example Home Assistant MQTT sensor configuration:
```
  - platform: mqtt
    name: "BLE Temperature Reading Hourly Stats"
    state_topic: "homeassistant/sensor/ble-temp/$SYS/hour-stats"
    value_template: "{{ value_json.total_adv_packets }}"
    unit_of_measurement: "Pkts"
    json_attributes_topic: "homeassistant/sensor/ble-temp/$SYS/hour-stats"

  - platform: mqtt
    name: "Backyard ATC_MI Temperature"
    state_topic: "homeassistant/sensor/ble-temp/A4:C1:38:DD:10:20"
    value_template: "{{ value_json.temperature }}"
    unit_of_measurement: "°F"
    json_attributes_topic: "homeassistant/sensor/ble-temp/A4:C1:38:DD:10:20"

  - platform: mqtt
    name: "Refrigerator Govee Humidity"
    state_topic: "homeassistant/sensor/ble-temp/3F:46:0D:31:70:28"
    value_template: "{{ value_json.humidity }}"
    unit_of_measurement: "%"
    json_attributes_topic: "homeassistant/sensor/ble-temp/3F:46:0D:31:70:28"

```

Finding your sensor's MAC address:

Considering how important this unique address is, especially when you have multiple units that all look identical, finding this number is a pain at times. Most of the sensors do NOT have the MAC address listed physically on them.

Keep a list of your known sensors MAC address. I write the address with a Sharpie on my units.

I do a two step process, you can find a name of the unit, sometimes with the last 2 or 3 digit pairs of the MAC address using a bluetooth low energy scanning tool like 'Light Blue', 'BLE Scanner' on iOS or Android. Or 'BlueSee' or 'Bluetooth Explorer' on Mac OS. Basically you are looking for the 'new' guy. Unplug the sensors battery, clear the scanning list. Start the scanner, plug the battery in to the sensor and watch for a new unit to appear. You are looking for this 'short' name that the unit broadcasts. Yes a pain.

With this short name in hand, you will now use another tool to find the full MAC address. On the Linux machine where you will run the scanning program use the 'hcitool' BLE command as follows to find the full MAC address:

examples:
```
sudo hcitool -i hci0 lescan | grep "MJ_HT_V1"
sudo hcitool -i hci0 lescan | grep "80:27"
sudo hcitool -i hci0 lescan | grep "ATC_71CC86"
sudo hcitool -i hci0 lescan | grep "Govee_H5074_B0A7"
```

this will return a line with the full MAC address of the unit, similar to one of the ones shown below. Record this MAC address:

```
58:2D:34:3B:72:56 MJ_HT_V1
A4:C1:38:71:CC:86 ATC_71CC86
E0:12:1D:22:B0:A7 Govee_H5074_B0A7
```

if you run the program with the pipe to the grep command, you can see all the Bluetooth LE devices that are visible and advertising:

```
sudo hcitool -i hci0 lescan
```

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
