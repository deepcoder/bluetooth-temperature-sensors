.POSIX:
.SUFFIXES:

CC = gcc
#CFLAGS = -Wall -Wextra -pedantic -std=c99 -O2
CFLAGS = -Wall -Wextra -O2


all: ble_sensor_mqtt_pub

ble_sensor_mqtt_pub : ble_sensor_mqtt_pub.c 
	$(CC) $(CFLAGS) $< -lyaml -lbluetooth  -lpaho-mqtt3c -o $@

clean :
	rm ble_sensor_mqtt_pub

.PHONY : clean