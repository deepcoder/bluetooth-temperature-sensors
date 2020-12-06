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
ble_sensor_mqtt_pub <bluetooth adapter> <scan type> <scan window> <scan interval>

bluetooth adapter = integer number of bluetooth devices, run hciconfig to see your adapters
scan type = 0 for passive, 1 for active advertising scan, some BLE sensors only share data on type 4 response active advertising packets
scan window = integer number that is multiplied by 0.625 to set advertising scanning window in milliseconds. Try 100 to start.
scan interval = integer number that is multiplied by 0.625 to set advertising scanning interval in milliseconds. Try 1000 to start.
 ```
 
Example JSON published to MQTT topic:
```
{"timestamp":"20201206025836","mac-address":"A4:D4:38:25:BD:61","rssi":-69,"temperature":64.4,"units":"F","temperature-celsius":18.0,"humidity":44.0,"battery-pct":93,"sensor-name":"","location":"H5072 Kitchen","sensor-type":"3"}
```

Configuration file:

see included configuration file, this sets MQTT server, MQTT base topic and details about each BLE sensor. Key info you need to have is MAC Address of each sensor and the device type of each.

Dumping raw advertising packets to console:

If you add the MAC address of a BLE device to the configuration file and give it a type of '99', the program will dislay the raw type 0 and 4 advertising packet data to the console. Useful to help figure out the data format of a new temperature and humidity sensor.

