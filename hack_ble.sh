#!/bin/bash

trap "echo Exited!; exit;" SIGINT SIGTERM

if [ $EUID -ne 0 ]; then
    echo "This script should be run as root." > /dev/stderr
    exit 1
fi

while :
do
    echo 'Running ble_sensor_mqtt_pub'
    date '+%Y%m%d%H%M%S'
	echo "Press [CTRL+C] to stop.."
    cd /home/pi/atc-govee
	/home/pi/atc-govee/ble_sensor_mqtt_pub /home/pi/atc-govee/ble_sensor_mqtt_pub.yaml
done


