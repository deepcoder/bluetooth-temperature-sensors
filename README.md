# Bluetooth temperature sensors
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
The program uses the bluetooth and mqtt client libraries, steps to image Raspberry Pi and install necessary libraries to compile program are show at bottom of this readme:

To compile:

```
gcc -o ble_sensor_mqtt_pub ble_sensor_mqtt_pub.c -lbluetooth  -l paho-mqtt3c

```
To run:
```
sudo ble_sensor_mqtt_pub <bluetooth adapter> <scan type> <scan window> <scan interval>

bluetooth adapter = integer number of bluetooth devices, run hciconfig to see your adapters, 1st adapter is referenced as 0 in this program
scan type = 0 for passive, 1 for active advertising scan, some BLE sensors only share data on type 4 response active advertising packets
scan window = integer number that is multiplied by 0.625 to set advertising scanning window in milliseconds. Try 100 to start.
scan interval = integer number that is multiplied by 0.625 to set advertising scanning interval in milliseconds. Try 1000 to start.
BLE scanning requires root equivalent rights, therefore sudo is necessary.
 ```
 
## Example JSON published to MQTT topic:
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

## Configuration file:

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

## Example Home Assistant MQTT sensor configuration:
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

## Finding your sensor's MAC address:

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

if you run the program without the pipe to the grep command, you can see all the Bluetooth LE devices that are visible and advertising:

```
sudo hcitool -i hci0 lescan
```

## Dumping raw advertising packets to console:

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


## Steps to set up raspberry pi as bluetooth sensor MQTT collector

imaged with:
2020-08-20-raspios-buster-armhf-full.img

```
sudo apt-get update
sudo apt-get full-upgrade

reboot
uname -a
Linux pi-ble-02 5.4.79-v7+ #1373 SMP Mon Nov 23 13:22:33 GMT 2020 armv7l GNU/Linux
Pi Model 2B  V1.1
Revision           : a21041
SoC                : BCM2836
RAM                : 1024Mb

git clone https://github.com/deepcoder/bluetooth-temperature-sensors.git

sudo apt-get install libbluetooth-dev

git clone https://github.com/eclipse/paho.mqtt.c.git
cd paho.mqtt.c/
make
sudo make install

cd /home/pi/bluetooth-temperature-sensors
gcc -o ble_sensor_mqtt_pub ble_sensor_mqtt_pub.c -lbluetooth  -l paho-mqtt3c

pi@pi-ble-02:~/bluetooth-temperature-sensors $ lsusb
Bus 001 Device 005: ID 0a5c:21e8 Broadcom Corp. BCM20702A0 Bluetooth 4.0

pi@pi-ble-02:~/bluetooth-temperature-sensors $ hciconfig
hci0:	Type: Primary  Bus: USB
	BD Address: 00:02:72:DC:31:2F  ACL MTU: 1021:8  SCO MTU: 64:1
	UP RUNNING
	RX bytes:980 acl:0 sco:0 events:51 errors:0
	TX bytes:2446 acl:0 sco:0 commands:51 errors:0

pi@pi-ble-02:~/bluetooth-temperature-sensors $ sudo ./ble_sensor_mqtt_pub 0 1 100 1000
ble_sensor_mqtt_pub v 2.11
1 Bluetooth adapter(s) in system.
Reading configuration file : ble_sensor_mqtt_pub.csv
MQTT server : tcp://172.168.2.22:1883
MQTT topic  : homeassistant/sensor/ble-temp/
Header      |MAC Address      |Type|Location                      |
Unit  :   0 |58:2D:34:3B:44:16|  99|MJ_HT_V1_LYWSDCGQ             |
Total devices in configuration file : 1
MQTT client name : ble_sensor_mqtt_pub-2F:31:FC:13:02:00
Bluetooth Adapter : 0 has MAC address : 2F:31:FC:13:02:00
Advertising scan type (0=passive, 1=active): 1
Advertising scan window   :  100, 62.5 ms
Advertising scan interval : 1000, 625.0 ms
Scanning....
current hour (GMT) = 23
last    hour (GMT) = 22

=========
Current local time and date: Sun Dec  6 15:30:29 2020
mac address =  58:2D:34:3B:44:16  location = MJ_HT_V1_LYWSDCGQ device type = 99 advertising_packet_type = 000
==>0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2 2 2 2 2 2 2 2 2 2 3 3 3 3 3 3 3 3 3 3 4 4 4 4 4 4 4 4 4 4 5 5 5 5 5 5 5 5 5 5 6
==>0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
==>                            0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2
==>                            0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
==>043E230201000056723B342D5817020106131695FE5020AA015456723B342D58061002CD01B9
==>__________ad________________________mmmmmmmmmmmmtttthhbbzbzbccrr
rssi         = -71
=========
Current local time and date: Sun Dec  6 15:30:29 2020
mac address =  58:2D:34:3B:44:16  location = MJ_HT_V1_LYWSDCGQ device type = 99 advertising_packet_type = 004
==>0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2 2 2 2 2 2 2 2 2 2 3 3 3 3 3 3 3 3 3 3 4 4 4 4 4 4 4 4 4 4 5 5 5 5 5 5 5 5 5 5 6
==>0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
==>                            0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2
==>                            0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
==>043E260201040056723B342D581A09094D4A5F48545F563105030F180A180916FFFFC3FB2C6D28B1B9
==>__________ad________________________mmmmmmmmmmmmtttthhbbzbzbccrr
rssi         = -71
```
## Comparison of number of sensor reading
There is not much that can be done to control the number of sensor reading taken and then collected for each of the BLE devices. These graphs show a comparison of some of the sensors over a 24 hour period. Some sensors take readings often, others much less often. The Govee 5074 appears to takes both temperature and humidity readings several times per minute, where as sensors like the Govee 5075 go multiple minutes between readings. It is difficult to gauge the specific sampling rate. Note that some sensors seems to sample temperature and humidity at different intervals, for example the Xiaomi LYWSD03MMC w/ custom firmware does this. In addition to sampling rate of the sensor, you have to account for how many of the BLE advertising packets you are collecting. If a sensor has a poor RF signal, even if it is collecting and transmitting samples at one rate, your BLE collecting device might only be capturing a subset of the packets. In addition to RF signal considerations, the BLE parameters for scan window and scan interval will effect the number of advertising packets and therefor the number of samples you receive. The table below shows that for the test sensors, with BLE scan window of 125 ms and scan interval of 312.5 ms, the collector Raspberry PI captured on the order of 300 to 500 advertising packets per hour. Again, it is difficult to deduce the number of samples per hour each sensor was taking.

![alt text](https://github.com/deepcoder/bluetooth-temperature-sensors/blob/main/sensor-temperature-24h.png?raw=true)
