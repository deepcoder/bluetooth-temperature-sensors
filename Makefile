.POSIX:
.SUFFIXES:

CC = gcc
#CFLAGS = -Wall -Wextra -pedantic -std=c99 -O2
CFLAGS = -Wall -Wextra -O2

all: ble_sensor_mqtt_pub

ble_sensor_mqtt_pub : ble_sensor_mqtt_pub.c 
	$(CC) $(CFLAGS) $< -lyaml -lbluetooth  -lpaho-mqtt3c -o $@


.PHONY : install
install:
	install -m 755 ble_sensor_mqtt_pub /usr/bin/
	install -m 644 ble_sensor_mqtt_pub.service /etc/systemd/system/
ifeq (,$(wildcard /etc/ble_sensor_mqtt_pub.yaml))
	install -m 600 ble_sensor_mqtt_pub.yaml /etc/ 
	# Config using /etc/ble_sensor_mqtt_pub.yaml
else
	# Leaving existing config at /etc/ble_sensor_mqtt_pub.yaml
endif
	# Start service with 'systemctl start ble_sensor_mqtt_pub'
	# Set service to start at boot with 'systemctl enable ble_sensor_mqtt_pub'

.PHONY : uninstall
uninstall:
	systemctl stop ble_sensor_mqtt_pub
	systemctl disable ble_sensor_mqtt_pub
	rm -f /usr/bin/ble_sensor_mqtt_pub
	rm -f /etc/systemd/system/ble_sensor_mqtt_pub.service
	# Leaving config file if it exists

.PHONY : clean
clean :
	rm ble_sensor_mqtt_pub
