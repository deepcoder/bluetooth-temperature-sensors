# Bluetooth temperature sensors
Read Bluetooth Advertising Packets from BLE temperature sensors and publish data to MQTT. This C language program runs on Linux. I have successfully used it on Raspberry Pi OS, Ubuntu 18 and Ubuntu 20.

This program decodes the bluetooth advertising packets for the following BLE temperature and humidity sensors:
```
//  1 = Xiaomi LYWSD03MMC-ATC   https://github.com/atc1441/ATC_MiThermometer & https://github.com/pvvx/ATC_MiThermometer
//  2 = Govee H5052 (type 4 advertising packets)
//  3 = Govee H5072
//  4 = Govee H5102
//  5 = Govee H5075
//  6 = Govee H5074 (type 4 advertising packets)
// 99 = Display raw type 0 and type 4 advertising packets for this BLE MAC address
```
The program uses the bluetooth and mqtt client libraries, steps to image Raspberry Pi and install necessary libraries to compile program are show at bottom of this readme.

Presuming reasonably modern systemd based linux system you can build and run like this:
```
sudo apt install libssl-dev libbluetooth-dev libyaml-dev

git clone https://github.com/eclipse/paho.mqtt.c.git
cd paho.mqtt.c/
make
sudo make install
cd ..

git clone https://github.com/deepcoder/bluetooth-temperature-sensors.git
cd bluetooth-temperature-sensors
make
sudo make install

#edit for your config /etc/bluetooth-temperature-sensors.yaml
systemctl start ble_sensor_mqtt_pub

# if you want to start at boot
systemctl enable ble_sensor_mqtt_pub
```
 
## Example JSON published to MQTT topic if using legacy publishing (won't work with HA auto configuration):
```
topic:
homeassistant/sensor/ble-temp/A4:C1:38:22:13:D0

payload:
{"timestamp":"20201206025836","mac-address":"A4:C1:38:22:13:D0","rssi":-69,"temperature":64.4,"units":"F","temperature-celsius":18.0,"humidity":44.0,"battery-pct":93,"sensor-name":"","location":"H5072 Kitchen","sensor-type":"3"}
```

## Example JSON published to MQTT topic if using new style publishing:
```
topic:
homeassistant/sensor/ble-temp/th_kitchen/state

payload:
{"timestamp":"20201206025836","mac":"A4:C1:38:22:13:D0","rssi":-69,"tempf":64.4,"units":"F","tempc":18.0,"humidity":44.0,"batterypct":93,"name":"Kitchen Temp/Hum","location":"Kitchen","type":"3"}
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

The configuration file is normal YAML.  The included sample config has more detail but here is an example config with 4 sensors.

```
mqtt_server_url: "tcp://172.148.5.11:1883"
mqtt_base_topic: "homeassistant/sensor/ble-temp/"

mqtt_username: "mosquitto"
mqtt_password: "mosquitto_pass"

bluetooth_adapter: 0
scan_type: 1
scan_window: 100
scan_interval: 1000
logging_level: 3

publish_type: 1
auto_configure: 1
auto_conf_stats: 1
auto_conf_tempc: 1
auto_conf_tempf: 0
auto_conf_hum: 1
auto_conf_battery: 1
auto_conf_voltage: 0
auto_conf_signal: 1

sensors:
  - name: "Living Room Temp/Hum"
    unique: "th_living_room"
    location: "Living Room"
    type: 1
    mac: "DD:C1:38:70:0C:24"

  - name: "Shared Bathroom Temp/Hum"
    unique: "th_bathroom"
    location: "Shared Bathroom"
    type: 3
    mac: "DD:C1:38:AC:77:44"

  - name: "Attic Temp/Hum"
    unique: "th_attic"
    location: "Attic"
    type: 3
    mac: "DD:12:1D:22:80:77"

  - name: "LYWSD03MMC test"
    unique: "th_test"
    location: "Garage"
    type: 99
    mac: "DD:C1:38:AC:28:A2"

```

## Use auto device configuration with Home Assistant
There is full support for Home Assintant's MQTT Discovery features (https://www.home-assistant.io/docs/mqtt/discovery).  This will allow auto creation of all configured entities in HA as well as grouping them together properly into Devices (which doesn't seem possible to do currently without using auto discovery).  Make sure the MQTT integration is enabled either via YAML or the UI.  Make sure both publish_type & auto_configure are set to 1 and then pick which entities you'd like created for each sensor from the auto_conf_* options.  Enabling auto_conf_battery will integrate with HA's battery function for devices (auto_conf_voltage will only be informational).


## Example Home Assistant manual MQTT sensor configuration:
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

# install libs if not already installed
sudo apt-get install libssl-dev

sudo apt-get install libbluetooth-dev

sudo apt-get install libyaml-dev

git clone https://github.com/eclipse/paho.mqtt.c.git
cd paho.mqtt.c/
make
sudo make install

cd /home/pi/bluetooth-temperature-sensors
make
sudo make install

pi@pi-ble-02:~/bluetooth-temperature-sensors $ lsusb
Bus 001 Device 005: ID 0a5c:21e8 Broadcom Corp. BCM20702A0 Bluetooth 4.0

pi@pi-ble-02:~/bluetooth-temperature-sensors $ hciconfig
hci0:	Type: Primary  Bus: USB
	BD Address: 00:02:72:DC:31:2F  ACL MTU: 1021:8  SCO MTU: 64:1
	UP RUNNING
	RX bytes:980 acl:0 sco:0 events:51 errors:0
	TX bytes:2446 acl:0 sco:0 commands:51 errors:0

# Edit config file
pi@pi-ble-02:~/bluetooth-temperature-sensors $ sudo nano /etc/ble_sensor_mqtt_pub.yaml

# Run in the foreground
pi@pi-ble-02:~/bluetooth-temperature-sensors $ sudo /usr/bin/ble_sensor_mqtt_pub /etc/ble_sensor_mqtt_pub.yaml

# Start the service
pi@pi-ble-02:~/bluetooth-temperature-sensors $ sudo systemctl start ble_sensor_mqtt_pub

# Check status of the service
pi@pi-ble-02:~/bluetooth-temperature-sensors $ sudo systemctl status ble_sensor_mqtt_pub

# Follow the log file, ctrl-c to exit
pi@pi-ble-02:~/bluetooth-temperature-sensors $ sudo journalctl -efu ble_sensor_mqtt_pub

# Enable service to run at boot
pi@pi-ble-02:~/bluetooth-temperature-sensors $ sudo systemctl enable ble_sensor_mqtt_pub

```
## Comparison of number of sensor reading
There is not much that can be done to control the number of sensor reading taken and then collected for each of the BLE devices. These graphs show a comparison of some of the sensors over a 24 hour period. Some sensors take readings often, others much less often. The Govee 5074 appears to takes both temperature and humidity readings several times per minute, where as sensors like the Govee 5075 go multiple minutes between readings. It is difficult to gauge the specific sampling rate. Note that some sensors seems to sample temperature and humidity at different intervals, for example the Xiaomi LYWSD03MMC w/ custom firmware does this. In addition to sampling rate of the sensor, you have to account for how many of the BLE advertising packets you are collecting. If a sensor has a poor RF signal, even if it is collecting and transmitting samples at one rate, your BLE collecting device might only be capturing a subset of the packets. In addition to RF signal considerations, the BLE parameters for scan window and scan interval will effect the number of advertising packets and therefor the number of samples you receive. The table below shows that for the test sensors, with BLE scan window of 125 ms and scan interval of 312.5 ms, the collector Raspberry PI captured on the order of 300 to 500 advertising packets per hour. Again, it is difficult to deduce the number of samples per hour each sensor was taking.

![alt text](https://github.com/deepcoder/bluetooth-temperature-sensors/blob/main/sensor-temperature-24h.png?raw=true)

![alt text](https://github.com/deepcoder/bluetooth-temperature-sensors/blob/main/sensor-humidity-24h.png?raw=true)

![alt text](https://github.com/deepcoder/bluetooth-temperature-sensors/blob/main/sensor-readings-per-hour-24h.png?raw=true)


