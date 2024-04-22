// ble_sensor_mqtt_pub.c
// gcc -o ble_sensor_mqtt_pub ble_sensor_mqtt_pub.c -l bluetooth  -l paho-mqtt3c
// 202102030607
//
// decode BLE temperature sensor temperature and humidity data from BLE advertising packets
// and publish to MQTT
// sensors supported:
// 1 = Xiaomi LYWSD03MMC-ATC   https://github.com/atc1441/ATC_MiThermometer
// 2 = Govee H5052
// 3 = Govee H5072
// 4 = Govee H5102
// 5 = Govee H5075
// 6 = Govee H5074
//
// based on work by:
//  Intel Edison Playground
//  Copyright (c) 2015 Damian Kołakowski. All rights reserved.
//
// YAML config parsing based on:
// https://github.com/smavros/yaml-to-struct
//

#define VERSION_MAJOR 3
#define VERSION_MINOR 0
// why is it so hard to get the base name of the program withOUT the .c extension!!!!!!!

#define PROGRAM_NAME "ble_sensor_mqtt_pub"
// program configuration file,
// holds list of BLE sensors to track
#define CONFIGURATION_FILE "/etc/ble_sensor_mqtt_pub.yaml"

#define MAX_SENSORS 64

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <yaml.h>
#include "MQTTClient.h"

// logging setup
// LOG_EMERG
// A panic condition was reported to all processes.
// LOG_ALERT
// A condition that should be corrected immediately.
// LOG_CRIT
// A critical condition.
// LOG_ERR
// An error message.
// LOG_WARNING
// A warning message.
// LOG_NOTICE
// A condition requiring special handling.
// LOG_INFO
// A general information message.
// LOG_DEBUG
// A message useful for debugging programs.

// int logging_level = LOG_INFO;
// int logging_level = LOG_ERR;
int logging_level = LOG_DEBUG;

#define RSYSLOG_ADDRESS "192.168.2.5"
#define LOGMESSAGESIZE 512
char log_message[LOGMESSAGESIZE];

// Paho MQTT setup
//#define ADDRESS     "tcp://192.168.2.242:1883"
char mqtt_server_address[128];
// #define CLIENTID    PROGRAM_NAME
#define MQTTCLIENTIDSIZE 128
char z_client_id_mqtt[MQTTCLIENTIDSIZE];

#define QOS 1
#define TIMEOUT 10000L

// MONITOR THIS AS YOU ADD MORE UNITS!!!!!!!!!!!!!!!!!
#define MAXIMUM_JSON_MESSAGE 2048

// MQTT topic definitions
// code to publish topic will append mac address of unit to this base
// base topic:
// each sensor with publish it's data under this base, example:
// homeassistant/sensor/ble-temp/A4:C1:38:DB:64:96
//char topic_base[128];
//const char topic_base[] = "homeassistant/sensor/ble-temp/";
// under the base topic, this sub topic will publish statistics
// topic for hourly statistics
const char topic_statistics[] = "$SYS/hour-stats";

struct hci_request ble_hci_request(uint16_t ocf, int clen, void *status, void *cparam)
{
    struct hci_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.ogf = OGF_LE_CTL;
    rq.ocf = ocf;
    rq.cparam = cparam;
    rq.clen = clen;
    rq.rparam = status;
    rq.rlen = 1;
    return rq;
}

typedef struct
{
    int type;
    char mac[19];
    char location[64];
    char name[64];
    char unique[64];
    char my_id[64];
    char make[64];
    char model[64];
    int readings_per_hour;
} sensor_t;

typedef struct
{
    char mqtt_server_url[128];
    char mqtt_base_topic[128];
    char mqtt_username[64];
    char mqtt_password[64];
    int bluetooth_adapter;
    int scan_type;
    int scan_window;
    int scan_interval;
    int publish_type;
    int auto_configure;
    int auto_conf_stats;
    int auto_conf_tempf;
    int auto_conf_tempc;
    int auto_conf_hum;
    int auto_conf_battery;
    int auto_conf_voltage;
    int auto_conf_signal;
    char syslog_address[64];
    int logging_level;
    sensor_t sensors[MAX_SENSORS];
} config_t;

/* Global parser */
unsigned int parser(config_t *config, char **argv);

/* Parser utilities */
void init_prs(FILE *fp, yaml_parser_t *parser);
void parse_next(yaml_parser_t *parser, yaml_event_t *event);
void clean_prs(FILE *fp, yaml_parser_t *parser, yaml_event_t *event);

/* Parser actions */
void event_switch(bool *seq_status, unsigned int *map_seq, config_t *config,
                  yaml_parser_t *parser, yaml_event_t *event, FILE *fp);
void to_data(bool *seq_status, unsigned int *map_seq, config_t *config,
             yaml_parser_t *parser, yaml_event_t *event, FILE *fp);
void to_data_from_map(char *buf, unsigned int *map_seq, config_t *config,
                      yaml_parser_t *parser, yaml_event_t *event, FILE *fp);

/* Post parsing utilities */
void print_data(unsigned int sensor_count, config_t *config);

// returns a structure with info about each bluetooth adapter found on system and returns number of adapters found
static int hci_devlist(struct hci_dev_info **di, int *num)
{
    int i;

    if ((*di = malloc(HCI_MAX_DEV * sizeof(**di))) == NULL)
    {
        printf("Couldn't allocated memory for hci_devlist: %s", strerror(errno));
        exit(1);
    }

    for (i = *num = 0; i < HCI_MAX_DEV; i++)
        if (hci_devinfo(i, &(*di)[*num]) == 0)
            (*num)++;

    return 0;
}

char advertising_packet_type_desc[9][30] =
    {
        "ADV_IND         0 (0000)",
        "ADV_DIRECT_IND  1 (0001)",
        "ADV_NONCONN_IND 2 (0010)",
        "SCAN_REQ        3 (0011)",
        "SCAN_RSP        4 (0100)",
        "CONNECT_REQ     5 (0101)",
        "ADV_SCAN_IND    6 (0110)",
        "ADV_EXT_IND     7 (0111)",
        "AUX_CONNECT_RSP 8 (1000)"};

// used for printing packet info during debugging
// https://stackoverflow.com/questions/111928/is-there-a-printf-converter-to-print-in-binary-format
#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY(byte)       \
    (byte & 0x80 ? '1' : '0'),     \
        (byte & 0x40 ? '1' : '0'), \
        (byte & 0x20 ? '1' : '0'), \
        (byte & 0x10 ? '1' : '0'), \
        (byte & 0x08 ? '1' : '0'), \
        (byte & 0x04 ? '1' : '0'), \
        (byte & 0x02 ? '1' : '0'), \
        (byte & 0x01 ? '1' : '0')

// this function sends a log message to a remote syslog server
// call with:
//  log level
//  hostname of logging server
//  program name that is sending log message
//  message

void send_remote_syslog_message(int log_level, char *hostname, char *program_name, char *message)
{
    int sockfd, n;
    int serverlen;
    int message_length;
    struct sockaddr_in serveraddr;
    struct hostent *server;
#define LOGBUFFERSIZE 1024
    char syslogbuf[LOGBUFFERSIZE];
#define RSYSLOGPORT 514

    // socket: create the socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        fprintf(stderr, "ERROR opening socket for remote syslog write");
        exit(1);
    }

    // gethostbyname: get the server's DNS entry
    server = gethostbyname(hostname);
    if (server == NULL)
    {
        fprintf(stderr, "ERROR, no such host as %s for remote syslog write\n", hostname);
        exit(1);
    }

    // build the server's Internet address
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
          (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(RSYSLOGPORT);

    // build the syslog message
    message_length = snprintf(syslogbuf, LOGBUFFERSIZE, "<%d>%s %s", LOG_USER + log_level, program_name, message);

    // fprintf(stdout, "%s\n", syslogbuf);

    // send the message to the server
    serverlen = sizeof(serveraddr);
    n = sendto(sockfd, syslogbuf, strlen(syslogbuf), 0, (struct sockaddr *)&serveraddr, serverlen);
    if (n < 0)
    {
        fprintf(stderr, "ERROR in sendto for remote syslog write\n");
        exit(1);
    }

    return;
}

// catch <ctr>-c to exit program
static volatile bool keep_running = true;

void intHandler(int dummy)
{
    keep_running = false;
}

// MQTT async routines

volatile MQTTClient_deliveryToken deliveredtoken;

void delivered(void *context, MQTTClient_deliveryToken dt)
{
    // printf("Message with token value %d delivery confirmed\n", dt);
    deliveredtoken = dt;
}

// MQTT received message handler
int msgarrvd(void *context, char *topicName, int topicLen, MQTTClient_message *message)
{
    int i;
    char *payloadptr;
    fprintf(stdout, "Message arrived\n");
    fprintf(stdout, "     topic: %s\n", topicName);
    fprintf(stdout, "     message: ");
    payloadptr = message->payload;
    for (i = 0; i < message->payloadlen; i++)
    {
        putc(*payloadptr++, stdout);
    }
    fprintf(stdout, "\n");
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

// MQTT connection to server lost handler
void connlost(void *context, char *cause)
{
    snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d MQTT Server Connection lost", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
    send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
    syslog(LOG_ERR, "%s", log_message);
    fprintf(stderr, "MQTT Server Connection lost, cause: %s\n", cause);
    exit(1);
}

// for reading configuration file
// read a field from the input line
char *getfield(char *line, int num)
{
    char *tok;
    for (tok = strtok(line, ",");
         tok && *tok;
         tok = strtok(NULL, ",\n"))
    {
        if (!--num)
            return tok;
    }
    return NULL;
}

// trim leading and trailing white space from a character string
// https://stackoverflow.com/questions/122616/how-do-i-trim-leading-trailing-whitespace-in-a-standard-way
char *trim(char *str)
{
    int isspace(int);
    size_t len = 0;
    char *frontp = str;
    char *endp = NULL;

    if (str == NULL)
    {
        return NULL;
    }
    if (str[0] == '\0')
    {
        return str;
    }

    len = strlen(str);
    endp = str + len;

    // Move the front and back pointers to address the first non-whitespace
    // characters from each end.
    while (isspace((unsigned char)*frontp))
    {
        ++frontp;
    }
    if (endp != frontp)
    {
        while (isspace((unsigned char)*(--endp)) && endp != frontp)
        {
        }
    }

    if (frontp != str && endp == frontp)
        *str = '\0';
    else if (str + len - 1 != endp)
        *(endp + 1) = '\0';

    // Shift the string so that it starts at str so that if it's dynamically
    // allocated, we can still free it on the returned pointer.  Note the reuse
    // of endp to mean the front of the string buffer now.
    endp = str;
    if (frontp != str)
    {
        while (*frontp)
        {
            *endp++ = *frontp++;
        }
        *endp = '\0';
    }

    return str;
}

int main(int argc, char *argv[])
{

    // startup
    fprintf(stdout, "%s v%2d.%02d\n", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);

    // handle signals, SIGINT
    struct sigaction act;
    act.sa_handler = intHandler;
    sigaction(SIGINT, &act, NULL);

    if (argc != 2)
    {
        snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Start program with a single argument pointing to yaml config file\n", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
        send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
        syslog(LOG_ERR, "%s", log_message);
        fprintf(stderr, "Start program with a single argument pointing to yaml config file\n");
        exit(1);
    }

    config_t config;

    int sensor_count;
    sensor_count = parser(&config, argv);

    int x;
    for (x = 0; x < sensor_count; x++)
    {
        if (config.publish_type)
        {
            strcpy(config.sensors[x].my_id, config.sensors[x].unique);
        }
        else
        {
            strcpy(config.sensors[x].my_id, config.sensors[x].mac);
        }
        switch (config.sensors[x].type)
        {
        case 1:
            strcpy(config.sensors[x].make, "Xiaomi");
            strcpy(config.sensors[x].model, "LYWSD03MMC-ATC");
            break;
        case 2:
            strcpy(config.sensors[x].make, "Govee");
            strcpy(config.sensors[x].model, "H5052");
            break;
        case 3:
            strcpy(config.sensors[x].make, "Govee");
            strcpy(config.sensors[x].model, "H5072");
            break;
        case 4:
            strcpy(config.sensors[x].make, "Govee");
            strcpy(config.sensors[x].model, "H5102");
            break;
        case 5:
            strcpy(config.sensors[x].make, "Govee");
            strcpy(config.sensors[x].model, "H5075");
            break;
        case 6:
            strcpy(config.sensors[x].make, "Govee");
            strcpy(config.sensors[x].model, "H5074");
            break;
        default:
            strcpy(config.sensors[x].make, "");
            strcpy(config.sensors[x].model, "");
        }
    }
    logging_level = config.logging_level;

    if (logging_level > LOG_NOTICE)
    {
        print_data(sensor_count, &config);
    }

    int hci_devs_num;
    struct hci_dev_info *hci_devs;

    int ret, status;

    // bluetooth adapter mac address
    char bluetooth_adapter_mac[19];
    // adapter number
    int bluetooth_adapter_number;

    // get the info about each of the bluetooth adapters in system
    if (hci_devlist(&hci_devs, &hci_devs_num))
    {

        snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Couldn't enumerate HCI devices: %s", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR, strerror(errno));
        send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
        syslog(LOG_ERR, "%s", log_message);
        fprintf(stderr, "Couldn't enumerate HCI devices: %s", strerror(errno));
        exit(1);
    }
    else
    {
        fprintf(stdout, "%u Bluetooth adapter(s) in system.\n", hci_devs_num);
    }

    // get requested adapter number from command line
    bluetooth_adapter_number = config.bluetooth_adapter;

    int ble_scan_type; // 0 = passive, 1 = active scan

    ble_scan_type = config.scan_type;

    if (ble_scan_type != 1)
    {
        ble_scan_type = 0;
    }

    //int ble_scan_interval = 65; // value * 0.625 ms, scan every 41 milliseconds
    //int ble_scan_window = 750; // value * 0.625 ms, window 469 milliseconds
    int ble_scan_window = 48;     // value * 0.625 ms, window 30 milliseconds
    int ble_scan_interval = 1500; // value * 0.625 ms, scan every 975 milliseconds

    ble_scan_window = config.scan_window;

    if (ble_scan_window == 0)
    {
        ble_scan_window = 48;
    }

    ble_scan_interval = config.scan_interval;

    if (ble_scan_interval == 0)
    {
        ble_scan_interval = 1500;
    }

    if (bluetooth_adapter_number < 0 || bluetooth_adapter_number > hci_devs_num - 1)
    {
        snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Enter bluetooth adapter number between 0 and %u !!\n", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR, hci_devs_num - 1);
        send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
        syslog(LOG_ERR, "%s", log_message);
        fprintf(stderr, "Enter bluetooth adapter number between 0 and %u !!\n", hci_devs_num - 1);
        exit(1);
    }

    // get MAC address for adapter selected
    strcpy(bluetooth_adapter_mac, batostr(&hci_devs[bluetooth_adapter_number].bdaddr));

    // log startup of program
    // strcpy(log_message, "test message *****");
    setlogmask(LOG_UPTO(LOG_INFO));
    openlog(PROGRAM_NAME, LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
    snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Starting.", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
    send_remote_syslog_message(LOG_INFO, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
    syslog(LOG_INFO, "%s", log_message);

    // maximum number of sensors
    // #define MAXIMUM_UNITS 40
    // number of devices read from configuration file
    int mac_total;

    int sensor_data_start;

    // total number of devices read from configuration file
    mac_total = sensor_count;

    fprintf(stdout, "Total devices in configuration file : %d\n", mac_total);

    // // create the MQTT topic from the base topic string and the MAC address of sensor
    // int topic_length;
    // char topic_buffer[200];
    // int topic_buffer_size = 200;

    // initialize MQTT
    MQTTClient client;
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;

    // set MQTT client ID to program name plus bluetooth mac address, to allow multiple instances on one machine
    snprintf(z_client_id_mqtt, MQTTCLIENTIDSIZE, "%s-%s", PROGRAM_NAME, bluetooth_adapter_mac);
    fprintf(stdout, "MQTT client name : %s\n", z_client_id_mqtt);
    MQTTClient_create(&client, config.mqtt_server_url, z_client_id_mqtt, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    //     MQTTClient_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    conn_opts.username = config.mqtt_username;
    conn_opts.password = config.mqtt_password;
    MQTTClient_setCallbacks(client, NULL, connlost, msgarrvd, delivered);
    if ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS)
    {
        snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d failed to connect to MQTT server", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
        send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
        syslog(LOG_ERR, "%s", log_message);
        fprintf(stderr, "Failed to connect to MQTT server, return code %d\n", rc);
        exit(1);
    }

    // Get HCI device.

    const int bluetooth_device = hci_open_dev(hci_get_route(&hci_devs[bluetooth_adapter_number].bdaddr));

    snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Bluetooth Adapter : %u has MAC address : %s\n", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR, bluetooth_adapter_number, bluetooth_adapter_mac);
    send_remote_syslog_message(LOG_INFO, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
    syslog(LOG_INFO, "%s", log_message);
    fprintf(stdout, "Bluetooth Adapter : %u has MAC address : %s\n", bluetooth_adapter_number, bluetooth_adapter_mac);

    if (bluetooth_device < 0)
    {
        snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d failed to open HCI device", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
        send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
        syslog(LOG_ERR, "%s", log_message);
        fprintf(stderr, "Failed to open HCI device, return code %d\n", bluetooth_device);
        exit(1);
    }

    // Set BLE scan parameters

    snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Advertising scan type (0=passive, 1=active): %u\n", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR, ble_scan_type);
    send_remote_syslog_message(LOG_INFO, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
    syslog(LOG_INFO, "%s", log_message);
    fprintf(stdout, "Advertising scan type (0=passive, 1=active): %u\n", ble_scan_type);

    snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Advertising scan window : %u %.1f ms\n", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR, ble_scan_window, ble_scan_window * 0.625);
    send_remote_syslog_message(LOG_INFO, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
    syslog(LOG_INFO, "%s", log_message);
    fprintf(stdout, "Advertising scan window   : %4u, %4.1f ms\n", ble_scan_window, ble_scan_window * 0.625);

    snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Advertising scan interval : %u %.1f ms\n", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR, ble_scan_interval, ble_scan_interval * 0.625);
    send_remote_syslog_message(LOG_INFO, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
    syslog(LOG_INFO, "%s", log_message);
    fprintf(stdout, "Advertising scan interval : %4u, %4.1f ms\n", ble_scan_interval, ble_scan_interval * 0.625);

    le_set_scan_parameters_cp scan_params_cp;
    memset(&scan_params_cp, 0, sizeof(scan_params_cp));
    // BLE PASSIVE OR ACTIVE SCAN ***************************************************************************************
    scan_params_cp.type = ble_scan_type; // 0x00 for passive scan, 0x01 for active scan (to get scan response packets)
    scan_params_cp.interval = htobs(ble_scan_interval);
    scan_params_cp.window = htobs(ble_scan_window);
    //     scan_params_cp.interval         = htobs(0x0010);
    //     scan_params_cp.window           = htobs(0x0010);
    scan_params_cp.own_bdaddr_type = 0x00; // Public Device Address (default).
    scan_params_cp.filter = 0x00;          // Accept all.

    struct hci_request scan_params_rq = ble_hci_request(OCF_LE_SET_SCAN_PARAMETERS, LE_SET_SCAN_PARAMETERS_CP_SIZE, &status, &scan_params_cp);

    ret = hci_send_req(bluetooth_device, &scan_params_rq, 1000);
    if (ret < 0)
    {
        hci_close_dev(bluetooth_device);
        snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Failed to set scan parameters data", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
        send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
        syslog(LOG_ERR, "%s", log_message);
        fprintf(stderr, "Failed to set scan parameters data, you must run this program as ROOT, return code %d\n", ret);
        exit(1);
    }

    // Set BLE events report mask.

    le_set_event_mask_cp event_mask_cp;
    memset(&event_mask_cp, 0, sizeof(le_set_event_mask_cp));
    int i = 0;
    for (i = 0; i < 8; i++)
        event_mask_cp.mask[i] = 0xFF;

    struct hci_request set_mask_rq = ble_hci_request(OCF_LE_SET_EVENT_MASK, LE_SET_EVENT_MASK_CP_SIZE, &status, &event_mask_cp);
    ret = hci_send_req(bluetooth_device, &set_mask_rq, 1000);
    if (ret < 0)
    {
        hci_close_dev(bluetooth_device);
        snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Failed to set event mask", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
        send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
        syslog(LOG_ERR, "%s", log_message);
        fprintf(stderr, "Failed to set event mask, return code %d\n", ret);
        exit(1);
    }

    // Enable scanning.

    le_set_scan_enable_cp scan_cp;
    memset(&scan_cp, 0, sizeof(scan_cp));
    scan_cp.enable = 0x01;     // Enable flag.
    scan_cp.filter_dup = 0x00; // Filtering disabled.

    struct hci_request enable_adv_rq = ble_hci_request(OCF_LE_SET_SCAN_ENABLE, LE_SET_SCAN_ENABLE_CP_SIZE, &status, &scan_cp);

    ret = hci_send_req(bluetooth_device, &enable_adv_rq, 1000);
    if (ret < 0)
    {
        hci_close_dev(bluetooth_device);
        snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Failed to enable scan", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
        send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
        syslog(LOG_ERR, "%s", log_message);
        fprintf(stderr, "Failed to enable scan, return code %d\n", ret);
        exit(1);
    }

    // Get Results.

    struct hci_filter nf;
    hci_filter_clear(&nf);
    hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
    hci_filter_set_event(EVT_LE_META_EVENT, &nf);
    ret = setsockopt(bluetooth_device, SOL_HCI, HCI_FILTER, &nf, sizeof(nf));
    if (ret < 0)
    {
        hci_close_dev(bluetooth_device);
        snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Could not set socket options", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
        send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
        syslog(LOG_ERR, "%s", log_message);
        fprintf(stderr, "Could not set socket options, return code %d\n", ret);
        exit(1);
    }

    snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Scanning....", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
    send_remote_syslog_message(LOG_INFO, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
    syslog(LOG_INFO, "%s", log_message);
    //     fprintf(stdout, "%s v%2d.%02d\n", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
    fprintf(stdout, "Scanning....\n");
    fflush(stdout);

    // bluetooth advertising packet buffer
    uint8_t ble_adv_buf[HCI_MAX_EVENT_SIZE];
    evt_le_meta_event *meta_event;
    le_advertising_info *adv_info;
    int bluetooth_adv_packet_length;

    // create the MQTT topic from the base topic string and the MAC address of sensor
    int topic_length;
    char topic_buffer[200];
    int topic_buffer_size = 200;

    // MQTT payload buffer
    int payload_length;
    char payload_buffer[MAXIMUM_JSON_MESSAGE];

    // int payload_buff_size = 300;

    // holds current time of current advertising packet that is received
    time_t rawtime = time(NULL);
    struct tm tm = *gmtime(&rawtime);
    struct tm *time_packet_received;

    // get the current hour, keep track every time we roll over to a new hour
    int hour_current;
    int hour_last;

    time_t gmt_time_now;
    struct tm tnp = *gmtime(&gmt_time_now);

    time(&gmt_time_now);
    tnp = *gmtime(&gmt_time_now);
    hour_current = tnp.tm_hour;

    if (hour_current == 0)
    {
        hour_last = 23;
    }
    else
    {
        hour_last = hour_current - 1;
    }

    fprintf(stdout, "current hour (GMT) = %d\n", hour_current);
    fprintf(stdout, "last    hour (GMT) = %d\n", hour_last);

    if (config.auto_configure)
    {
        if (logging_level > LOG_INFO)
        {
            fprintf(stdout, "=========\n");
            fprintf(stdout, "Begining auto configuration of devices\n");
        }

        if (config.auto_conf_stats)
        {

            payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                      "{\"~\":\"%s$SYS/hour-stats\",\"name\":\"BLE Temperature Reading Hourly Stats\",\"uniq_id\":\"ble-tmp-hourly-stats\",\"stat_t\":\"~\",\"unit_of_meas\":\"Pkts\",\"val_tpl\":\"{{value_json.total_adv_packets}}\"}",
                                      config.mqtt_base_topic);

            if (payload_length >= MAXIMUM_JSON_MESSAGE)
            // if (payload_length >= payload_buff_size)
            {
                fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                exit(-1);
            }

            // // create the MQTT topic from the base topic string and the MAC address of sensor
            // int topic_length;
            // char topic_buffer[200];
            // int topic_buffer_size = 200;

            topic_length = snprintf(topic_buffer, topic_buffer_size, "%shourly-stats/config", config.mqtt_base_topic);

            // publish the message and wait for success
            pubmsg.payload = payload_buffer;
            pubmsg.payloadlen = payload_length;
            pubmsg.qos = QOS;
            pubmsg.retained = 1;
            deliveredtoken = 0;
            MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

            // wait for messqge to be delivered to server
            // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
            while (deliveredtoken != token)
                ;
        }

        for (x = 0; x < sensor_count; x++)
        {
            if (logging_level > LOG_INFO)
            {
                fprintf(stdout, "  Configuring: %s\n", config.sensors[x].my_id);
            }

            if (config.sensors[x].type != 99)
            {
                // configure temp F sensor
                if (config.auto_conf_tempf)
                {
                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                              "{\"~\":\"%s%s\",\"dev_cla\":\"temperature\",\"name\":\"%s-F\",\"uniq_id\":\"%s-F\",\"stat_t\":\"~/state\",\"unit_of_meas\":\"°F\",\"val_tpl\":\"{{value_json.tempf}}\",\"dev\":{\"name\":\"%s\",\"ids\":\"%s\",\"sa\":\"%s\",\"cns\":[[\"mac\", \"%s\"]],\"mf\":\"%s\",\"mdl\":\"%s\"}  }",
                                              config.mqtt_base_topic,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].location,
                                              config.sensors[x].mac,
                                              config.sensors[x].make,
                                              config.sensors[x].model);

                    if (payload_length >= MAXIMUM_JSON_MESSAGE)
                    // if (payload_length >= payload_buff_size)
                    {
                        fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                        exit(-1);
                    }

                    // // create the MQTT topic from the base topic string and the MAC address of sensor

                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%sF/config", config.mqtt_base_topic, config.sensors[x].my_id);

                    // publish the message and wait for success
                    pubmsg.payload = payload_buffer;
                    pubmsg.payloadlen = payload_length;
                    pubmsg.qos = QOS;
                    pubmsg.retained = 1;
                    deliveredtoken = 0;
                    MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                    // wait for messqge to be delivered to server
                    // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                    while (deliveredtoken != token)
                        ;
                }

                // configure temp C sensor
                if (config.auto_conf_tempc)
                {
                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                              "{\"~\":\"%s%s\",\"dev_cla\":\"temperature\",\"name\":\"%s-T\",\"uniq_id\":\"%s-T\",\"stat_t\":\"~/state\",\"unit_of_meas\":\"°C\",\"val_tpl\":\"{{value_json.tempc}}\",\"dev\":{\"name\":\"%s\",\"ids\":\"%s\",\"sa\":\"%s\",\"cns\":[[\"mac\", \"%s\"]],\"mf\":\"%s\",\"mdl\":\"%s\"}  }",
                                              config.mqtt_base_topic,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].location,
                                              config.sensors[x].mac,
                                              config.sensors[x].make,
                                              config.sensors[x].model);

                    if (payload_length >= MAXIMUM_JSON_MESSAGE)
                    // if (payload_length >= payload_buff_size)
                    {
                        fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                        exit(-1);
                    }

                    // // create the MQTT topic from the base topic string and the MAC address of sensor

                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%sT/config", config.mqtt_base_topic, config.sensors[x].my_id);

                    // publish the message and wait for success
                    pubmsg.payload = payload_buffer;
                    pubmsg.payloadlen = payload_length;
                    pubmsg.qos = QOS;
                    pubmsg.retained = 1;
                    deliveredtoken = 0;
                    MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                    // wait for messqge to be delivered to server
                    // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                    while (deliveredtoken != token)
                        ;
                }

                // configure hum sensor
                if (config.auto_conf_hum)
                {
                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                              "{\"~\":\"%s%s\",\"dev_cla\":\"humidity\",\"name\":\"%s-H\",\"uniq_id\":\"%s-H\",\"stat_t\":\"~/state\",\"unit_of_meas\":\"%%\",\"val_tpl\":\"{{value_json.humidity}}\",\"dev\":{\"name\":\"%s\",\"ids\":\"%s\",\"sa\":\"%s\",\"cns\":[[\"mac\", \"%s\"]],\"mf\":\"%s\",\"mdl\":\"%s\"}  }",
                                              config.mqtt_base_topic,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].location,
                                              config.sensors[x].mac,
                                              config.sensors[x].make,
                                              config.sensors[x].model);

                    if (payload_length >= MAXIMUM_JSON_MESSAGE)
                    // if (payload_length >= payload_buff_size)
                    {
                        fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                        exit(-1);
                    }

                    // // create the MQTT topic from the base topic string and the MAC address of sensor

                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%sH/config", config.mqtt_base_topic, config.sensors[x].my_id);

                    // publish the message and wait for success
                    pubmsg.payload = payload_buffer;
                    pubmsg.payloadlen = payload_length;
                    pubmsg.qos = QOS;
                    pubmsg.retained = 1;
                    deliveredtoken = 0;
                    MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                    // wait for messqge to be delivered to server
                    // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                    while (deliveredtoken != token)
                        ;
                }

                // configure battery sensor
                if (config.auto_conf_battery)
                {
                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                              "{\"~\":\"%s%s\",\"dev_cla\":\"battery\",\"name\":\"%s-B\",\"uniq_id\":\"%s-B\",\"stat_t\":\"~/state\",\"unit_of_meas\":\"%%\",\"val_tpl\":\"{{value_json.batterypct}}\",\"dev\":{\"name\":\"%s\",\"ids\":\"%s\",\"sa\":\"%s\",\"cns\":[[\"mac\", \"%s\"]],\"mf\":\"%s\",\"mdl\":\"%s\"}  }",
                                              config.mqtt_base_topic,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].location,
                                              config.sensors[x].mac,
                                              config.sensors[x].make,
                                              config.sensors[x].model);

                    if (payload_length >= MAXIMUM_JSON_MESSAGE)
                    // if (payload_length >= payload_buff_size)
                    {
                        fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                        exit(-1);
                    }

                    // // create the MQTT topic from the base topic string and the MAC address of sensor

                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%sB/config", config.mqtt_base_topic, config.sensors[x].my_id);

                    // publish the message and wait for success
                    pubmsg.payload = payload_buffer;
                    pubmsg.payloadlen = payload_length;
                    pubmsg.qos = QOS;
                    pubmsg.retained = 1;
                    deliveredtoken = 0;
                    MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                    // wait for messqge to be delivered to server
                    // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                    while (deliveredtoken != token)
                        ;
                }

                // configure voltage sensor only an option for sensor type 1
                if (config.auto_conf_voltage && config.sensors[x].type == 1)
                {
                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                              "{\"~\":\"%s%s\",\"dev_cla\":\"voltage\",\"name\":\"%s-V\",\"uniq_id\":\"%s-V\",\"stat_t\":\"~/state\",\"unit_of_meas\":\"mV\",\"val_tpl\":\"{{value_json.batterymv}}\",\"dev\":{\"name\":\"%s\",\"ids\":\"%s\",\"sa\":\"%s\",\"cns\":[[\"mac\", \"%s\"]],\"mf\":\"%s\",\"mdl\":\"%s\"}  }",
                                              config.mqtt_base_topic,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].location,
                                              config.sensors[x].mac,
                                              config.sensors[x].make,
                                              config.sensors[x].model);

                    if (payload_length >= MAXIMUM_JSON_MESSAGE)
                    // if (payload_length >= payload_buff_size)
                    {
                        fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                        exit(-1);
                    }

                    // // create the MQTT topic from the base topic string and the MAC address of sensor
                    // int topic_length;
                    // char topic_buffer[200];
                    // int topic_buffer_size = 200;

                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%sV/config", config.mqtt_base_topic, config.sensors[x].my_id);

                    // publish the message and wait for success
                    pubmsg.payload = payload_buffer;
                    pubmsg.payloadlen = payload_length;
                    pubmsg.qos = QOS;
                    pubmsg.retained = 1;
                    deliveredtoken = 0;
                    MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                    // wait for messqge to be delivered to server
                    // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                    while (deliveredtoken != token)
                        ;
                }

                // configure signal sensor
                if (config.auto_conf_signal)
                {
                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                              "{\"~\":\"%s%s\",\"dev_cla\":\"signal_strength\",\"name\":\"%s-S\",\"uniq_id\":\"%s-S\",\"stat_t\":\"~/state\",\"unit_of_meas\":\"dBm\",\"val_tpl\":\"{{value_json.rssi}}\",\"dev\":{\"name\":\"%s\",\"ids\":\"%s\",\"sa\":\"%s\",\"cns\":[[\"mac\", \"%s\"]],\"mf\":\"%s\",\"mdl\":\"%s\"}  }",
                                              config.mqtt_base_topic,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].name,
                                              config.sensors[x].my_id,
                                              config.sensors[x].location,
                                              config.sensors[x].mac,
                                              config.sensors[x].make,
                                              config.sensors[x].model);

                    if (payload_length >= MAXIMUM_JSON_MESSAGE)
                    // if (payload_length >= payload_buff_size)
                    {
                        fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                        exit(-1);
                    }

                    // // create the MQTT topic from the base topic string and the MAC address of sensor
                    // int topic_length;
                    // char topic_buffer[200];
                    // int topic_buffer_size = 200;

                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%sS/config", config.mqtt_base_topic, config.sensors[x].my_id);

                    // publish the message and wait for success
                    pubmsg.payload = payload_buffer;
                    pubmsg.payloadlen = payload_length;
                    pubmsg.qos = QOS;
                    pubmsg.retained = 1;
                    deliveredtoken = 0;
                    MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                    // wait for messqge to be delivered to server
                    // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                    while (deliveredtoken != token)
                        ;
                }
            }
        }
        if (logging_level > LOG_INFO)
        {
            fprintf(stdout, " Auto configuration complete\n");
        }
        fflush(stdout);
    }

    // loop until SIGINT received
    while (keep_running)
    {

        // check if we have rolled over to a new hour, if so send report of advertising packets receive in last hour
        time(&gmt_time_now);
        tnp = *gmtime(&gmt_time_now);

        if (hour_current != tnp.tm_hour)
        {
            hour_current = tnp.tm_hour;
            // check if new day
            if (hour_current == 0)
            {
                hour_last = 23;
            }
            else
            {
                hour_last = hour_current - 1;
            }

            // don't publish right at top of hour, wait a few seconds
            sleep(10);

            fprintf(stdout, "*********** =========\n");
            fprintf(stdout, "HOUR ROLLOVER\n");
            fprintf(stdout, "current hour (GMT) = %d\n", hour_current);
            fprintf(stdout, "last    hour (GMT) = %d\n", hour_last);

            time(&gmt_time_now);
            tnp = *gmtime(&gmt_time_now);

            // create JSON string with timestamp, count and location for each known device
            payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                      "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",",
                                      tnp.tm_year + 1900, tnp.tm_mon + 1, tnp.tm_mday, tnp.tm_hour, tnp.tm_min, tnp.tm_sec);

            int count_string_length;
            char count_string_buffer[MAXIMUM_JSON_MESSAGE] = "";
            int count_string_size = MAXIMUM_JSON_MESSAGE;

            // this builds a string contains the readings for each device concatenated together
            int total_advertising_packets = 0;
            int n;
            for (n = 0; n <= mac_total - 1; n++)
            {
                count_string_length = snprintf(count_string_buffer, count_string_size, "\"%s\":{\"count\":%d, \"location\":\"%s\"},", config.sensors[n].mac, config.sensors[n].readings_per_hour, config.sensors[n].location);
                strcat(payload_buffer, count_string_buffer);
                // if ( n < mac_total - 1 )
                //     strcat(payload_buffer, ",");

                fprintf(stderr, "Location : %s packets received in last hour : %d %s\n", config.sensors[n].mac, config.sensors[n].readings_per_hour, config.sensors[n].location);
                total_advertising_packets = total_advertising_packets + config.sensors[n].readings_per_hour;
                config.sensors[n].readings_per_hour = 0;
            }

            // append the total of all advertising packets for all sensors of this type in last hour
            count_string_length = snprintf(count_string_buffer, count_string_size, "\"total_adv_packets\":%d}", total_advertising_packets);
            strcat(payload_buffer, count_string_buffer);

            // get length of MQTT payload after concatinating all the individual string together
            payload_length = strlen(payload_buffer);

            fprintf(stdout, "payload_buffer JSON : %s\n", payload_buffer);

            if (payload_length >= MAXIMUM_JSON_MESSAGE)
            {
                fprintf(stderr, "MQTT payload too long: %d\n", payload_length);
                snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d MQTT payload too long: %d\n", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR, payload_length);
                send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
                syslog(LOG_ERR, "%s", log_message);
                exit(1);
            }

            // publish it to a statistics topic under the root topic
            topic_length = snprintf(topic_buffer, topic_buffer_size,
                                    "%s%s",
                                    config.mqtt_base_topic, topic_statistics);

            // publish the message and wait for success
            pubmsg.payload = payload_buffer;
            pubmsg.payloadlen = payload_length;
            pubmsg.qos = QOS;
            pubmsg.retained = 0;
            deliveredtoken = 0;
            MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

            // wait for messqge to be delivered to server
            // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
            while (deliveredtoken != token)
                ;
        }

        // get the bluetooth packet
        bluetooth_adv_packet_length = read(bluetooth_device, ble_adv_buf, sizeof(ble_adv_buf));
        // apparently there can be multiple advertisement packets with the packet received
        if (bluetooth_adv_packet_length >= HCI_EVENT_HDR_SIZE)
        {
            meta_event = (evt_le_meta_event *)(ble_adv_buf + HCI_EVENT_HDR_SIZE + 1);
            if (meta_event->subevent == EVT_LE_ADVERTISING_REPORT)
            {

                uint8_t reports_count = meta_event->data[0];
                void *offset = meta_event->data + 1;
                while (reports_count--)
                {
                    // this is the advertising specific data within the packet
                    adv_info = (le_advertising_info *)offset;

                    // get the MAC address of the device that sent the advertising packet
                    char addr[18];
                    ba2str(&(adv_info->bdaddr), addr);

                    // check the MAC address of the BLE device and see if it is in out list of deies to monitor

                    int mac_match = 0;
                    int mac_index;
                    int i_match;

                    for (i_match = 0; i_match < mac_total; ++i_match)
                    {
                        if (strcmp(addr, config.sensors[i_match].mac) == 0)
                        {
                            mac_match = 1;
                            // keep a pointer to the mac address we matched
                            mac_index = i_match;
                        }
                    }

                    // found the mac address in our list we are interested in, so decipher it's data
                    // if (1 == 1)
                    if (mac_match == 1)
                    {

                        // different processing based on the device type, each type has different formats of advertising packets

                        // 1 = Xiaomi LYWSD03MMC-ATC
                        if (config.sensors[mac_index].type == 1)
                        {
                            int advertising_packet_type; // type of advertising packet
                            //printf("=========\n");
                            //get the time that we received the scan response packet
                            time(&rawtime);
                            tm = *gmtime(&rawtime);
                            time_packet_received = localtime(&rawtime);

                            // printf ( "Current local time and date: %s", asctime (timeinfo) );

                            // printf("mac address =  %s  location = %s ", addr, config.sensors[mac_index].location);

                            advertising_packet_type = (unsigned int)ble_adv_buf[5];
                            // printf("advertising_packet_type = %03d\n", advertising_packet_type);
                            // length of packet and subpacket

                            // printf("full packet length      = %3d %02X\n", bluetooth_adv_packet_length, bluetooth_adv_packet_length);
                            // printf("sub packet length       = %3d %02X\n", adv_info->length, adv_info->length);

                            // printf("rssi         = %03d %02X\n", (int8_t)adv_info->data[adv_info->length], adv_info->data[adv_info->length]);

                            // handle the specific data for each advertising packet type
                            // The Advertising and Scan Response data is sent in advertising events. The
                            // Advertising Data is placed in the AdvData field of ADV_IND,
                            // ADV_NONCONN_IND, and ADV_SCAN_IND packets. The Scan Response
                            // data is sent in the ScanRspData field of SCAN_RSP packets.
                            // https://www.libelium.com/forum/libelium_files/bt4_core_spec_adv_data_reference.pdf

                            if (advertising_packet_type == 0)
                            {

                                int ad_length; // length of ADV_IND packet type
                                int ad_type;   // attribute of ADV_IND packet type

                                int8_t rssi_int = adv_info->data[adv_info->length];

                                if (logging_level == LOG_DEBUG)
                                {
                                    fprintf(stdout, "=========\n");
                                    fprintf(stdout, "Current local time and date: %s", asctime(time_packet_received));
                                    fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                    fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);
                                    fprintf(stdout, "rssi         = %03d\n", rssi_int);
                                }

                                // length of packet and subpacket

                                //printf("full packet length      = %3d %02X\n", bluetooth_adv_packet_length, bluetooth_adv_packet_length);
                                //printf("sub packet length       = %3d %02X\n", adv_info->length, adv_info->length);

                                ad_length = (unsigned int)adv_info->data[0];
                                //printf("ADV_IND AD Data Length  = %3d %02X\n", ad_length, ad_length);
                                ad_type = (unsigned int)adv_info->data[1];
                                //printf("ADV_IND AD Type         = %3d %02X\n", ad_type, ad_type);

                                int16_t temperature_int;
                                double temperature_celsius;
                                double temperature_fahrenheit;
                                int16_t humidity_int;
                                double humidity;
                                uint8_t battery_pct_int;
                                uint16_t battery_mv_int;
                                uint8_t frame_int;

                                // check for pvvx firmware custom format
                                if (bluetooth_adv_packet_length == 34)
                                {
                                    if (logging_level == LOG_DEBUG)
                                    {
                                        fprintf(stdout, "Parsing as PVVX Firmware\n");
                                    }

                                    temperature_int = (adv_info->data[10]) | (adv_info->data[11] << 8);
                                    temperature_celsius = (double)temperature_int / 100.0;

                                    temperature_fahrenheit = temperature_celsius * 9.0 / 5.0 + 32.0;

                                    humidity_int = (adv_info->data[12]) | (adv_info->data[13] << 8);
                                    humidity = (double)humidity_int / 100.0;

                                    battery_pct_int = adv_info->data[16];

                                    battery_mv_int = (adv_info->data[14]) | (adv_info->data[15] << 8);

                                    frame_int = adv_info->data[17];
                                }
                                else
                                {
                                    if (logging_level == LOG_DEBUG)
                                    {
                                        fprintf(stdout, "Parsing as ATC Firmware\n");
                                    }

                                    temperature_int = (adv_info->data[10] << 8) | adv_info->data[11];
                                    temperature_celsius = (double)temperature_int / 10.0;

                                    temperature_fahrenheit = temperature_celsius * 9.0 / 5.0 + 32.0;

                                    humidity_int = adv_info->data[12];
                                    humidity = (double)humidity_int;

                                    battery_pct_int = adv_info->data[13];

                                    battery_mv_int = (adv_info->data[14] << 8) | adv_info->data[15];

                                    frame_int = adv_info->data[16];
                                }

                                if (logging_level == LOG_DEBUG)
                                {
                                    fprintf(stdout, "temp c       =  %.1f\n", temperature_celsius);
                                    fprintf(stdout, "temp f       =  %.1f\n", temperature_fahrenheit);
                                    fprintf(stdout, "humidity pct =  %.1f\n", humidity);
                                    fprintf(stdout, "battery pct  = %3d\n", battery_pct_int);
                                    fprintf(stdout, "battery mv   =  %4d\n", battery_mv_int);
                                    fprintf(stdout, "frame        =   %3d\n", frame_int);
                                }
                                // count the number of advertising packets we get from each unit

                                config.sensors[mac_index].readings_per_hour = config.sensors[mac_index].readings_per_hour + 1;

                                if (config.publish_type == 1)
                                {
                                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                              "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac\":\"%s\",\"rssi\":%d,\"tempf\":%#.1F,\"units\":\"F\",\"tempc\":%#.1F,\"humidity\":%#.1F,\"batterypct\":%i,\"batterymv\":%i,\"frame\":%i,\"name\":\"%s\",\"location\":\"%s\",\"type\":\"%d\"}",
                                                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                              addr, rssi_int, temperature_fahrenheit,
                                                              temperature_celsius,
                                                              humidity, battery_pct_int, battery_mv_int, frame_int,
                                                              config.sensors[mac_index].name,
                                                              config.sensors[mac_index].location,
                                                              config.sensors[mac_index].type);
                                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s/state", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                }
                                else
                                {
                                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                              "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac-address\":\"%s\",\"rssi\":%d,\"temperature\":%#.1F,\"units\":\"F\",\"temperature-celsius\":%#.1F,\"humidity\":%.0F,\"battery-pct\":%i,\"battery-mv\":%i,\"frame\":%i,\"sensor-name\":\"%s\",\"location\":\"%s\",\"sensor-type\":\"%d\"}",
                                                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                              addr, rssi_int, temperature_fahrenheit,
                                                              temperature_celsius,
                                                              humidity, battery_pct_int, battery_mv_int, frame_int,
                                                              config.sensors[mac_index].name,
                                                              config.sensors[mac_index].location,
                                                              config.sensors[mac_index].type);
                                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                }

                                if (payload_length >= MAXIMUM_JSON_MESSAGE)
                                // if (payload_length >= payload_buff_size)
                                {
                                    fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                                    exit(-1);
                                }

                                // publish the message and wait for success
                                pubmsg.payload = payload_buffer;
                                pubmsg.payloadlen = payload_length;
                                pubmsg.qos = QOS;
                                pubmsg.retained = 0;
                                deliveredtoken = 0;
                                MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                                // wait for messqge to be delivered to server
                                // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                                while (deliveredtoken != token)
                                    ;
                            }

                            if (advertising_packet_type == 4)
                            {
                            }
                            fflush(stdout);
                        }
                        // end device type 1

                        // 2 = Govee H5052
                        if (config.sensors[mac_index].type == 2)
                        {
                            //get the time that we received the scan response packet
                            time(&rawtime);
                            tm = *gmtime(&rawtime);
                            time_packet_received = localtime(&rawtime);

                            int advertising_packet_type; // type of advertising packet

                            advertising_packet_type = (unsigned int)ble_adv_buf[5];

                            if (advertising_packet_type == 0)
                            {
                            }

                            // sensor data is broadcast in type 4 advertising message by the H5052
                            if (advertising_packet_type == 4)
                            {
                                // get rssi
                                int rssi_int = (signed char)(int8_t)adv_info->data[adv_info->length];

                                if (logging_level == LOG_DEBUG)
                                {
                                    fprintf(stdout, "=========\n");
                                    fprintf(stdout, "Current local time and date: %s", asctime(time_packet_received));
                                    fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                    fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);
                                    fprintf(stdout, "rssi         = %03d\n", rssi_int);
                                }

                                //                                 fprintf(stdout, "=========\n");
                                //                                 fprintf(stdout, "Current local time and date: %s", asctime (time_packet_received) );
                                //                                 fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].location);
                                //
                                //                                 fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);

                                sensor_data_start = 5;

                                // get the lsb msb byte pairs for temperature and humidity and convert them to an integer value (in 100's)
                                // temperature is a signed 16 bit integer to allow for temperatures below and above 0 degrees celsius
                                signed short int temperature_int = adv_info->data[sensor_data_start + 0] | adv_info->data[sensor_data_start + 1] << 8;
                                int humidity_int = adv_info->data[sensor_data_start + 2] | adv_info->data[sensor_data_start + 3] << 8;

                                // convert the integer * 100 value for temperature and humidity to degrees fahrenheit and celsius (for homekit) and humidity percentage

                                double temperature_fahrenheit;
                                double temperature_celsius;
                                temperature_fahrenheit = (temperature_int / 100.0) * 9.0 / 5.0 + 32.0;
                                temperature_celsius = (temperature_int / 100.0);

                                double humidity = humidity_int / 100.0;

                                // get battery level percentage
                                int battery_precentage_int = (signed char)adv_info->data[sensor_data_start + 4];

                                if (logging_level == LOG_DEBUG)
                                {
                                    fprintf(stdout, "temp c       =  %.1f\n", temperature_celsius);
                                    fprintf(stdout, "temp f       =  %.1f\n", temperature_fahrenheit);
                                    fprintf(stdout, "humidity pct =  %.1f\n", humidity);
                                    fprintf(stdout, "battery pct  = %3d\n", battery_precentage_int);
                                }

                                // count the number of advertising packets we get from each unit

                                config.sensors[mac_index].readings_per_hour = config.sensors[mac_index].readings_per_hour + 1;

                                if (config.publish_type == 1)
                                {
                                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                              "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac\":\"%s\",\"rssi\":%d,\"tempf\":%#.1F,\"units\":\"F\",\"tempc\":%#.1F,\"humidity\":%#.1F,\"batterypct\":%i,\"name\":\"%s\",\"location\":\"%s\",\"type\":\"%d\"}",
                                                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                              addr, rssi_int, temperature_fahrenheit,
                                                              temperature_celsius,
                                                              humidity, battery_precentage_int,
                                                              config.sensors[mac_index].name,
                                                              config.sensors[mac_index].location,
                                                              config.sensors[mac_index].type);
                                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s/state", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                }
                                else
                                {
                                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                              "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac-address\":\"%s\",\"rssi\":%d,\"temperature\":%#.1F,\"units\":\"F\",\"temperature-celsius\":%#.1F,\"humidity\":%#.1F,\"battery-pct\":%i,\"sensor-name\":\"%s\",\"location\":\"%s\",\"sensor-type\":\"%d\"}",
                                                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                              addr, rssi_int, temperature_fahrenheit,
                                                              temperature_celsius,
                                                              humidity, battery_precentage_int,
                                                              config.sensors[mac_index].name,
                                                              config.sensors[mac_index].location,
                                                              config.sensors[mac_index].type);
                                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                }

                                if (payload_length >= MAXIMUM_JSON_MESSAGE)
                                // if (payload_length >= payload_buff_size)
                                {
                                    fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                                    exit(-1);
                                }

                                // publish the message and wait for success
                                pubmsg.payload = payload_buffer;
                                pubmsg.payloadlen = payload_length;
                                pubmsg.qos = QOS;
                                pubmsg.retained = 0;
                                deliveredtoken = 0;
                                MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                                // wait for messqge to be delivered to server
                                // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                                while (deliveredtoken != token)
                                    ;
                            }

                            fflush(stdout);
                        }
                        //end device type 2

                        // device type 3 = Govee H5072
                        if (config.sensors[mac_index].type == 3)
                        {
                            //get the time that we received the scan response packet
                            time(&rawtime);
                            tm = *gmtime(&rawtime);
                            time_packet_received = localtime(&rawtime);

                            int advertising_packet_type; // type of advertising packet

                            advertising_packet_type = (unsigned int)ble_adv_buf[5];

                            // sensor data is broadcast in type 0 advertising message by the H5072
                            if (advertising_packet_type == 0)
                            {
                                // get rssi
                                int rssi_int = (signed char)(int8_t)adv_info->data[adv_info->length];

                                if (logging_level == LOG_DEBUG)
                                {
                                    fprintf(stdout, "=========\n");
                                    fprintf(stdout, "Current local time and date: %s", asctime(time_packet_received));
                                    fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                    fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);
                                    fprintf(stdout, "rssi         = %03d\n", rssi_int);
                                }

                                //                                 fprintf(stdout, "=========\n");
                                //                                 fprintf(stdout, "Current local time and date: %s", asctime (time_packet_received) );
                                //                                 fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                //
                                //                                 fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);

                                sensor_data_start = 26;

                                int below_32;
                                int msb;

                                if ((adv_info->data[sensor_data_start + 0] & (1 << 7)) != 0)
                                {
                                    below_32 = 1;
                                    msb = adv_info->data[sensor_data_start + 0];
                                    msb &= ~(1UL << 7);
                                }
                                else
                                {
                                    below_32 = 0;
                                    msb = adv_info->data[sensor_data_start + 0];
                                }

                                unsigned int sensor_data = adv_info->data[sensor_data_start + 2] | adv_info->data[sensor_data_start + 1] << 8 | msb << 16;

                                signed int temperature_int = sensor_data / 10000;

                                // this crap code works for values below 0 degrees C but seems to bottomout about 12.2 degrees F, display shows values lower
                                // but not very accurate. Manual says range is 14 degrees F to 140 degrees F
                                // Humidity seems accurate thru range however
                                if (below_32 == 1)
                                {
                                    temperature_int = temperature_int * -1;
                                }

                                unsigned int humidity_int = (sensor_data % 1000) / 10;

                                int32_t answer;
                                answer = (((int32_t)((int8_t)adv_info->data[sensor_data_start + 0])) << 16) + (((int32_t)adv_info->data[sensor_data_start + 1]) << 8) + adv_info->data[sensor_data_start + 2];

                                // convert the integer * 100 value for temperature and humidity to degrees fahrenheit and celsius (for homekit) and humidity percentage

                                double temperature_fahrenheit;
                                double temperature_celsius;
                                temperature_fahrenheit = temperature_int * 9.0 / 5.0 + 32.0;
                                temperature_celsius = temperature_int;

                                double humidity = humidity_int;

                                // get battery level percentage
                                int battery_precentage_int = (signed char)adv_info->data[sensor_data_start + 3];

                                if (logging_level == LOG_DEBUG)
                                {
                                    fprintf(stdout, "temp c       =  %.1f\n", temperature_celsius);
                                    fprintf(stdout, "temp f       =  %.1f\n", temperature_fahrenheit);
                                    fprintf(stdout, "humidity pct =  %.1f\n", humidity);
                                    fprintf(stdout, "battery pct  = %3d\n", battery_precentage_int);
                                }

                                // count the number of advertising packets we get from each unit

                                config.sensors[mac_index].readings_per_hour = config.sensors[mac_index].readings_per_hour + 1;

                                if (config.publish_type == 1)
                                {
                                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                              "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac\":\"%s\",\"rssi\":%d,\"tempf\":%#.1F,\"units\":\"F\",\"tempc\":%#.1F,\"humidity\":%#.1F,\"batterypct\":%i,\"name\":\"%s\",\"location\":\"%s\",\"type\":\"%d\"}",
                                                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                              addr, rssi_int, temperature_fahrenheit,
                                                              temperature_celsius,
                                                              humidity, battery_precentage_int,
                                                              config.sensors[mac_index].name,
                                                              config.sensors[mac_index].location,
                                                              config.sensors[mac_index].type);
                                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s/state", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                }
                                else
                                {
                                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                              "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac-address\":\"%s\",\"rssi\":%d,\"temperature\":%#.1F,\"units\":\"F\",\"temperature-celsius\":%#.1F,\"humidity\":%#.1F,\"battery-pct\":%i,\"sensor-name\":\"%s\",\"location\":\"%s\",\"sensor-type\":\"%d\"}",
                                                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                              addr, rssi_int, temperature_fahrenheit,
                                                              temperature_celsius,
                                                              humidity, battery_precentage_int,
                                                              config.sensors[mac_index].name,
                                                              config.sensors[mac_index].location,
                                                              config.sensors[mac_index].type);
                                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                }

                                if (payload_length >= MAXIMUM_JSON_MESSAGE)
                                // if (payload_length >= payload_buff_size)
                                {
                                    fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                                    exit(-1);
                                }

                                // publish the message and wait for success
                                pubmsg.payload = payload_buffer;
                                pubmsg.payloadlen = payload_length;
                                pubmsg.qos = QOS;
                                pubmsg.retained = 0;
                                deliveredtoken = 0;
                                MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                                // wait for messqge to be delivered to server
                                // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                                while (deliveredtoken != token)
                                    ;
                            }

                            if (advertising_packet_type == 4)
                            {
                            }
                            fflush(stdout);
                        }
                        // end device type 3

                        // device type 4 = Govee H5102
                        if (config.sensors[mac_index].type == 4)
                        {
                            //get the time that we received the scan response packet
                            time(&rawtime);
                            tm = *gmtime(&rawtime);
                            time_packet_received = localtime(&rawtime);

                            int advertising_packet_type; // type of advertising packet

                            advertising_packet_type = (unsigned int)ble_adv_buf[5];

                            // sensor data is broadcast in type 0 advertising message by the H5072
                            if (advertising_packet_type == 0)
                            {
                                // get rssi
                                int rssi_int = (signed char)(int8_t)adv_info->data[adv_info->length];

                                if (logging_level == LOG_DEBUG)
                                {
                                    fprintf(stdout, "=========\n");
                                    fprintf(stdout, "Current local time and date: %s", asctime(time_packet_received));
                                    fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                    fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);
                                    fprintf(stdout, "rssi         = %03d\n", rssi_int);
                                }

                                //                                 fprintf(stdout, "=========\n");
                                //                                 fprintf(stdout, "Current local time and date: %s", asctime (time_packet_received) );
                                //                                 fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                //
                                //                                 fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);

                                sensor_data_start = 27;

                                int below_32;
                                int msb;

                                if ((adv_info->data[sensor_data_start + 0] & (1 << 7)) != 0)
                                {
                                    below_32 = 1;
                                    msb = adv_info->data[sensor_data_start + 0];
                                    msb &= ~(1UL << 7);
                                }
                                else
                                {
                                    below_32 = 0;
                                    msb = adv_info->data[sensor_data_start + 0];
                                }

                                unsigned int sensor_data = adv_info->data[sensor_data_start + 2] | adv_info->data[sensor_data_start + 1] << 8 | msb << 16;

                                signed int temperature_int = sensor_data / 10000;

                                // this crap code works for values below 0 degrees C but seems to bottomout about 12.2 degrees F, display shows values lower
                                // but not very accurate. Manual says range is 14 degrees F to 140 degrees F
                                // Humidity seems accurate thru range however
                                if (below_32 == 1)
                                {
                                    temperature_int = temperature_int * -1;
                                }

                                unsigned int humidity_int = (sensor_data % 1000) / 10;

                                int32_t answer;
                                answer = (((int32_t)((int8_t)adv_info->data[sensor_data_start + 0])) << 16) + (((int32_t)adv_info->data[sensor_data_start + 1]) << 8) + adv_info->data[sensor_data_start + 2];

                                // convert the integer * 100 value for temperature and humidity to degrees fahrenheit and celsius (for homekit) and humidity percentage

                                double temperature_fahrenheit;
                                double temperature_celsius;
                                temperature_fahrenheit = temperature_int * 9.0 / 5.0 + 32.0;
                                temperature_celsius = temperature_int;

                                double humidity = humidity_int;
                                // get battery level percentage
                                int battery_precentage_int = (signed char)adv_info->data[sensor_data_start + 3];

                                if (logging_level == LOG_DEBUG)
                                {
                                    fprintf(stdout, "temp c       =  %.1f\n", temperature_celsius);
                                    fprintf(stdout, "temp f       =  %.1f\n", temperature_fahrenheit);
                                    fprintf(stdout, "humidity pct =  %.1f\n", humidity);
                                    fprintf(stdout, "battery pct  = %3d\n", battery_precentage_int);
                                }

                                // count the number of advertising packets we get from each unit

                                config.sensors[mac_index].readings_per_hour = config.sensors[mac_index].readings_per_hour + 1;

                                if (config.publish_type == 1)
                                {
                                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                              "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac\":\"%s\",\"rssi\":%d,\"tempf\":%#.1F,\"units\":\"F\",\"tempc\":%#.1F,\"humidity\":%#.1F,\"batterypct\":%i,\"name\":\"%s\",\"location\":\"%s\",\"type\":\"%d\"}",
                                                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                              addr, rssi_int, temperature_fahrenheit,
                                                              temperature_celsius,
                                                              humidity, battery_precentage_int,
                                                              config.sensors[mac_index].name,
                                                              config.sensors[mac_index].location,
                                                              config.sensors[mac_index].type);
                                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s/state", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                }
                                else
                                {
                                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                              "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac-address\":\"%s\",\"rssi\":%d,\"temperature\":%#.1F,\"units\":\"F\",\"temperature-celsius\":%#.1F,\"humidity\":%#.1F,\"battery-pct\":%i,\"sensor-name\":\"%s\",\"location\":\"%s\",\"sensor-type\":\"%d\"}",
                                                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                              addr, rssi_int, temperature_fahrenheit,
                                                              temperature_celsius,
                                                              humidity, battery_precentage_int,
                                                              config.sensors[mac_index].name,
                                                              config.sensors[mac_index].location,
                                                              config.sensors[mac_index].type);
                                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                }

                                if (payload_length >= MAXIMUM_JSON_MESSAGE)
                                // if (payload_length >= payload_buff_size)
                                {
                                    fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                                    exit(-1);
                                }

                                // publish the message and wait for success
                                pubmsg.payload = payload_buffer;
                                pubmsg.payloadlen = payload_length;
                                pubmsg.qos = QOS;
                                pubmsg.retained = 0;
                                deliveredtoken = 0;
                                MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                                // wait for messqge to be delivered to server
                                // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                                while (deliveredtoken != token)
                                    ;
                            }

                            if (advertising_packet_type == 4)
                            {
                            }
                            fflush(stdout);
                        }
                        // end device type 4

                        // device type 5 = Govee H5075
                        if (config.sensors[mac_index].type == 5)
                        {
                            //get the time that we received the scan response packet
                            time(&rawtime);
                            tm = *gmtime(&rawtime);
                            time_packet_received = localtime(&rawtime);

                            int advertising_packet_type; // type of advertising packet

                            advertising_packet_type = (unsigned int)ble_adv_buf[5];

                            // sensor data is broadcast in type 0 advertising message by the H5072
                            if (advertising_packet_type == 0)
                            {
                                // get rssi
                                int rssi_int = (signed char)(int8_t)adv_info->data[adv_info->length];

                                if (logging_level == LOG_DEBUG)
                                {
                                    fprintf(stdout, "=========\n");
                                    fprintf(stdout, "Current local time and date: %s", asctime(time_packet_received));
                                    fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                    fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);
                                    fprintf(stdout, "rssi         = %03d\n", rssi_int);
                                }

                                //                                 fprintf(stdout, "=========\n");
                                //                                 fprintf(stdout, "Current local time and date: %s", asctime (time_packet_received) );
                                //                                 fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                //
                                //                                 fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);

                                sensor_data_start = 26;

                                int below_32;
                                int msb;

                                if ((adv_info->data[sensor_data_start + 0] & (1 << 7)) != 0)
                                {
                                    below_32 = 1;
                                    msb = adv_info->data[sensor_data_start + 0];
                                    msb &= ~(1UL << 7);
                                }
                                else
                                {
                                    below_32 = 0;
                                    msb = adv_info->data[sensor_data_start + 0];
                                }

                                unsigned int sensor_data = adv_info->data[sensor_data_start + 2] | adv_info->data[sensor_data_start + 1] << 8 | msb << 16;

                                double temperature_double = sensor_data / 1000 / 10.0;

                                // this crap code works for values below 0 degrees C but seems to bottomout about 12.2 degrees F, display shows values lower
                                // but not very accurate. Manual says range is 14 degrees F to 140 degrees F
                                // Humidity seems accurate thru range however
                                if (below_32 == 1)
                                {
                                    temperature_double =  temperature_double * -1;
                                }

                                double humidity_int = (sensor_data % 1000) / 10.0;

                                int32_t answer;
                                answer = (((int32_t)((int8_t)adv_info->data[sensor_data_start + 0])) << 16) + (((int32_t)adv_info->data[sensor_data_start + 1]) << 8) + adv_info->data[sensor_data_start + 2];

                                // convert the values for temperature and humidity to degrees fahrenheit and celsius (for homekit) and humidity percentage

                                double temperature_fahrenheit;
                                double temperature_celsius;
                                temperature_fahrenheit = temperature_double * 9.0 / 5.0 + 32.0;
                                temperature_celsius = temperature_double;

                                double humidity = humidity_int;
                                // get battery level percentage
                                int battery_precentage_int = (signed char)adv_info->data[sensor_data_start + 3];

                                if (logging_level == LOG_DEBUG)
                                {
                                    fprintf(stdout, "temp c       =  %.1f\n", temperature_celsius);
                                    fprintf(stdout, "temp f       =  %.1f\n", temperature_fahrenheit);
                                    fprintf(stdout, "humidity pct =  %.1f\n", humidity);
                                    fprintf(stdout, "battery pct  = %3d\n", battery_precentage_int);
                                }

                                // count the number of advertising packets we get from each unit

                                config.sensors[mac_index].readings_per_hour = config.sensors[mac_index].readings_per_hour + 1;

                                if (config.publish_type == 1)
                                {
                                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                              "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac\":\"%s\",\"rssi\":%d,\"tempf\":%#.1F,\"units\":\"F\",\"tempc\":%#.1F,\"humidity\":%#.1F,\"batterypct\":%i,\"name\":\"%s\",\"location\":\"%s\",\"type\":\"%d\"}",
                                                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                              addr, rssi_int, temperature_fahrenheit,
                                                              temperature_celsius,
                                                              humidity, battery_precentage_int,
                                                              config.sensors[mac_index].name,
                                                              config.sensors[mac_index].location,
                                                              config.sensors[mac_index].type);
                                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s/state", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                }
                                else
                                {
                                    payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                              "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac-address\":\"%s\",\"rssi\":%d,\"temperature\":%#.1F,\"units\":\"F\",\"temperature-celsius\":%#.1F,\"humidity\":%#.1F,\"battery-pct\":%i,\"sensor-name\":\"%s\",\"location\":\"%s\",\"sensor-type\":\"%d\"}",
                                                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                              addr, rssi_int, temperature_fahrenheit,
                                                              temperature_celsius,
                                                              humidity, battery_precentage_int,
                                                              config.sensors[mac_index].name,
                                                              config.sensors[mac_index].location,
                                                              config.sensors[mac_index].type);
                                    topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                }

                                if (payload_length >= MAXIMUM_JSON_MESSAGE)
                                // if (payload_length >= payload_buff_size)
                                {
                                    fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                                    exit(-1);
                                }

                                // publish the message and wait for success
                                pubmsg.payload = payload_buffer;
                                pubmsg.payloadlen = payload_length;
                                pubmsg.qos = QOS;
                                pubmsg.retained = 0;
                                deliveredtoken = 0;
                                MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                                // wait for messqge to be delivered to server
                                // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                                while (deliveredtoken != token)
                                    ;
                            }

                            if (advertising_packet_type == 4)
                            {
                            }
                            fflush(stdout);
                        }
                        // end device type 5

                        // device type 6 = Govee H5074
                        if (config.sensors[mac_index].type == 6)
                        {
                            //get the time that we received the scan response packet
                            time(&rawtime);
                            tm = *gmtime(&rawtime);
                            time_packet_received = localtime(&rawtime);

                            int advertising_packet_type; // type of advertising packet

                            advertising_packet_type = (unsigned int)ble_adv_buf[5];

                            // sensor data is broadcast in type 0 advertising message by the H5072
                            if (advertising_packet_type == 0)
                            {
                            }

                            if (advertising_packet_type == 4)
                            {
                                // get rssi
                                int rssi_int = (signed char)(int8_t)adv_info->data[adv_info->length];

                                // this device sends sensor data only on this type of scan response advertising packet

                                if ((int8_t)adv_info->data[0] == 0x0a)
                                {

                                    if (logging_level == LOG_DEBUG)
                                    {
                                        fprintf(stdout, "=========\n");
                                        fprintf(stdout, "Current local time and date: %s", asctime(time_packet_received));
                                        fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                        fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);
                                        fprintf(stdout, "rssi         = %03d\n", rssi_int);
                                    }

                                    //                                     fprintf(stdout, "=========\n");
                                    //                                     fprintf(stdout, "Current local time and date: %s", asctime (time_packet_received) );
                                    //                                     fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                    //
                                    //                                     fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);

                                    sensor_data_start = 5;

                                    // get the lsb msb byte pairs for temperature and humidity and convert them to an integer value (in 100's)
                                    // temperature is a signed 16 bit integer to allow for temperatures below and above 0 degrees celsius
                                    signed short int temperature_int = adv_info->data[sensor_data_start + 0] | adv_info->data[sensor_data_start + 1] << 8;
                                    int humidity_int = adv_info->data[sensor_data_start + 2] | adv_info->data[sensor_data_start + 3] << 8;

                                    // convert the integer * 100 value for temperature and humidity to degrees fahrenheit and celsius (for homekit) and humidity percentage

                                    double temperature_fahrenheit;
                                    double temperature_celsius;
                                    temperature_fahrenheit = (temperature_int / 100.0) * 9.0 / 5.0 + 32.0;
                                    temperature_celsius = (temperature_int / 100.0);

                                    double humidity = humidity_int / 100.0;

                                    // get battery level percentage
                                    int battery_precentage_int = (signed char)adv_info->data[sensor_data_start + 4];

                                    if (logging_level == LOG_DEBUG)
                                    {
                                        fprintf(stdout, "temp c       =  %.1f\n", temperature_celsius);
                                        fprintf(stdout, "temp f       =  %.1f\n", temperature_fahrenheit);
                                        fprintf(stdout, "humidity pct =  %.1f\n", humidity);
                                        fprintf(stdout, "battery pct  = %3d\n", battery_precentage_int);
                                    }

                                    // count the number of advertising packets we get from each unit

                                    config.sensors[mac_index].readings_per_hour = config.sensors[mac_index].readings_per_hour + 1;

                                    if (config.publish_type == 1)
                                    {
                                        payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                                  "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac\":\"%s\",\"rssi\":%d,\"tempf\":%#.1F,\"units\":\"F\",\"tempc\":%#.1F,\"humidity\":%#.1F,\"batterypct\":%i,\"name\":\"%s\",\"location\":\"%s\",\"type\":\"%d\"}",
                                                                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                                  addr, rssi_int, temperature_fahrenheit,
                                                                  temperature_celsius,
                                                                  humidity, battery_precentage_int,
                                                                  config.sensors[mac_index].name,
                                                                  config.sensors[mac_index].location,
                                                                  config.sensors[mac_index].type);
                                        topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s/state", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                    }
                                    else
                                    {
                                        payload_length = snprintf(payload_buffer, MAXIMUM_JSON_MESSAGE,
                                                                  "{\"timestamp\":\"%04d%02d%02d%02d%02d%02d\",\"mac-address\":\"%s\",\"rssi\":%d,\"temperature\":%#.1F,\"units\":\"F\",\"temperature-celsius\":%#.1F,\"humidity\":%#.1F,\"battery-pct\":%i,\"sensor-name\":\"%s\",\"location\":\"%s\",\"sensor-type\":\"%d\"}",
                                                                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                                                  addr, rssi_int, temperature_fahrenheit,
                                                                  temperature_celsius,
                                                                  humidity, battery_precentage_int,
                                                                  config.sensors[mac_index].name,
                                                                  config.sensors[mac_index].location,
                                                                  config.sensors[mac_index].type);
                                        topic_length = snprintf(topic_buffer, topic_buffer_size, "%s%s", config.mqtt_base_topic, config.sensors[mac_index].my_id);
                                    }

                                    if (payload_length >= MAXIMUM_JSON_MESSAGE)
                                    // if (payload_length >= payload_buff_size)
                                    {
                                        fprintf(stderr, "MQTT payload too long, %d\n", payload_length);
                                        exit(-1);
                                    }

                                    // publish the message and wait for success
                                    pubmsg.payload = payload_buffer;
                                    pubmsg.payloadlen = payload_length;
                                    pubmsg.qos = QOS;
                                    pubmsg.retained = 0;
                                    deliveredtoken = 0;
                                    MQTTClient_publishMessage(client, topic_buffer, &pubmsg, &token);

                                    // wait for messqge to be delivered to server
                                    // printf("Waiting for publication of %s\n" "on topic %s for client with ClientID: %s\n", payload_buffer, topic_buffer, z_client_id_mqtt);
                                    while (deliveredtoken != token)
                                        ;
                                }
                            }
                            fflush(stdout);
                        }
                        // end device type 6

                        // device type 99 = decoding
                        if (config.sensors[mac_index].type == 99)
                        {
                            //get the time that we received the scan response packet
                            time(&rawtime);
                            tm = *gmtime(&rawtime);
                            time_packet_received = localtime(&rawtime);

                            int advertising_packet_type; // type of advertising packet

                            advertising_packet_type = (unsigned int)ble_adv_buf[5];

                            if (advertising_packet_type == 0)
                            {
                                // counter for printing
                                int n;

                                // get rssi
                                int rssi_int = (signed char)(int8_t)adv_info->data[adv_info->length];

                                if (logging_level == LOG_DEBUG)
                                {
                                    fprintf(stdout, "=========\n");
                                    fprintf(stdout, "Current local time and date: %s", asctime(time_packet_received));
                                    fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                    fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);
                                    // print whole packet
                                    printf("==>0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2 2 2 2 2 2 2 2 2 2 3 3 3 3 3 3 3 3 3 3 4 4 4 4 4 4 4 4 4 4 5 5 5 5 5 5 5 5 5 5 6 \n");
                                    printf("==>0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 \n");
                                    printf("==>                            0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2\n");
                                    printf("==>                            0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 \n");
                                    printf("==>");
                                    for (n = 0; n < bluetooth_adv_packet_length; n++)
                                        printf("%02X", (unsigned char)ble_adv_buf[n]);
                                    printf("\n");
                                    printf("==>__________ad________________________mmmmmmmmmmmmtttthhbbzbzbccrr\n");
                                    fprintf(stdout, "rssi         = %03d\n", rssi_int);
                                }

                                //                                 fprintf(stdout, "=========\n");
                                //                                 fprintf(stdout, "Current local time and date: %s", asctime (time_packet_received) );
                                //                                 fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                //
                                //                                 fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);
                            }

                            if (advertising_packet_type == 4)
                            {
                                // counter for printing
                                int n;

                                // get rssi
                                int rssi_int = (signed char)(int8_t)adv_info->data[adv_info->length];

                                fprintf(stdout, "=========\n");
                                fprintf(stdout, "Current local time and date: %s", asctime(time_packet_received));
                                fprintf(stdout, "mac address =  %s  location = %s device type = %d ", addr, config.sensors[mac_index].location, config.sensors[mac_index].type);
                                fprintf(stdout, "advertising_packet_type = %03d\n", advertising_packet_type);
                                // print whole packet
                                printf("==>0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2 2 2 2 2 2 2 2 2 2 3 3 3 3 3 3 3 3 3 3 4 4 4 4 4 4 4 4 4 4 5 5 5 5 5 5 5 5 5 5 6 \n");
                                printf("==>0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 \n");
                                printf("==>                            0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 2\n");
                                printf("==>                            0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 \n");
                                printf("==>");
                                for (n = 0; n < bluetooth_adv_packet_length; n++)
                                    printf("%02X", (unsigned char)ble_adv_buf[n]);
                                printf("\n");
                                printf("==>__________ad________________________mmmmmmmmmmmmtttthhbbzbzbccrr\n");
                                fprintf(stdout, "rssi         = %03d\n", rssi_int);
                            }
                            fflush(stdout);
                        }
                        // end device type 99

                    } // end of Matched MAC address

                    // if there are multiple advertising packets loop thru them
                    offset = adv_info->data + adv_info->length + 2;
                }
            }
        }
    }

    // <ctrl>-c to exit program received
    fprintf(stdout, "\n<ctrl>-c signal received, exiting.\n");
    snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d <ctrl>-c signal received, exiting.", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
    send_remote_syslog_message(LOG_INFO, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
    syslog(LOG_INFO, "%s", log_message);

    // Disable scanning.
    memset(&scan_cp, 0, sizeof(scan_cp));
    scan_cp.enable = 0x00; // Disable flag.

    struct hci_request disable_adv_rq = ble_hci_request(OCF_LE_SET_SCAN_ENABLE, LE_SET_SCAN_ENABLE_CP_SIZE, &status, &scan_cp);
    ret = hci_send_req(bluetooth_device, &disable_adv_rq, 1000);
    if (ret < 0)
    {
        hci_close_dev(bluetooth_device);
        snprintf(log_message, LOGMESSAGESIZE, "%s v: %d.%d Failed to disable scan", PROGRAM_NAME, VERSION_MAJOR, VERSION_MINOR);
        send_remote_syslog_message(LOG_ERR, RSYSLOG_ADDRESS, PROGRAM_NAME, log_message);
        syslog(LOG_ERR, "%s", log_message);
        fprintf(stdout, "Failed to disable scan, return code %d\n", ret);
        exit(1);
    }

    hci_close_dev(bluetooth_device);

    // end MQTT session
    MQTTClient_disconnect(client, 10000);
    MQTTClient_destroy(&client);

    exit(0);
}

unsigned int
parser(config_t *config, char **argv)
{
    /* Open file & declare libyaml types */
    FILE *fp = fopen(argv[1], "r");

    if (fp == NULL)
    {
        fprintf(stdout, "ERROR: Failed to open config file: %s\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    yaml_parser_t parser;
    yaml_event_t event;

    bool seq_status = 0;      /* IN or OUT of sequence index, init to OUT */
    unsigned int map_seq = 0; /* Index of mapping inside sequence */

    init_prs(fp, &parser); /* Initiliaze parser & open file */

    do
    {
        parse_next(&parser, &event); /* Parse new event */

        /* Decide what to do with each event */
        event_switch(&seq_status, &map_seq, config, &parser, &event, fp);

        if (event.type != YAML_STREAM_END_EVENT)
        {
            yaml_event_delete(&event);
        }

        if (map_seq > MAX_SENSORS)
        {
            break;
        }

    } while (event.type != YAML_STREAM_END_EVENT);

    clean_prs(fp, &parser, &event); /* clean parser & close file */

    return map_seq;
}

void event_switch(bool *seq_status, unsigned int *map_seq, config_t *config,
                  yaml_parser_t *parser, yaml_event_t *event, FILE *fp)
{
    switch (event->type)
    {
    case YAML_STREAM_START_EVENT:
        break;
    case YAML_STREAM_END_EVENT:
        break;
    case YAML_DOCUMENT_START_EVENT:
        break;
    case YAML_DOCUMENT_END_EVENT:
        break;
    case YAML_SEQUENCE_START_EVENT:
        (*seq_status) = true;
        break;
    case YAML_SEQUENCE_END_EVENT:
        (*seq_status) = false;
        break;
    case YAML_MAPPING_START_EVENT:
        if (*seq_status == 1)
        {
            (*map_seq)++;
        }
        break;
    case YAML_MAPPING_END_EVENT:
        break;
    case YAML_ALIAS_EVENT:
        printf(" ERROR: Got alias (anchor %s)\n",
               event->data.alias.anchor);
        exit(EXIT_FAILURE);
        break;
    case YAML_SCALAR_EVENT:
        to_data(seq_status, map_seq, config, parser, event, fp);
        break;
    case YAML_NO_EVENT:
        puts(" ERROR: No event!");
        exit(EXIT_FAILURE);
        break;
    }
}

void to_data(bool *seq_status, unsigned int *map_seq, config_t *config,
             yaml_parser_t *parser, yaml_event_t *event, FILE *fp)
{
    char *buf = (char *)event->data.scalar.value;

    /* Dictionary */
    char *mqtt_server_url = "mqtt_server_url";
    char *mqtt_base_topic = "mqtt_base_topic";
    char *mqtt_username = "mqtt_username";
    char *mqtt_password = "mqtt_password";
    char *bluetooth_adapter = "bluetooth_adapter";
    char *scan_type = "scan_type";
    char *scan_window = "scan_window";
    char *scan_interval = "scan_interval";
    char *publish_type = "publish_type";
    char *auto_configure = "auto_configure";
    char *auto_conf_stats = "auto_conf_stats";
    char *auto_conf_tempf = "auto_conf_tempf";
    char *auto_conf_tempc = "auto_conf_tempc";
    char *auto_conf_hum = "auto_conf_hum";
    char *auto_conf_battery = "auto_conf_battery";
    char *auto_conf_voltage = "auto_conf_voltage";
    char *auto_conf_signal = "auto_conf_signal";
    char *syslog_address = "syslog_address";
    char *logging_level = "logging_level";
    char *sensors = "sensors";

    if (!strcmp(buf, mqtt_server_url))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        strcpy(config->mqtt_server_url, (char *)event->data.scalar.value);
    }
    else if (!strcmp(buf, mqtt_base_topic))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        strcpy(config->mqtt_base_topic, (char *)event->data.scalar.value);
    }
    else if (!strcmp(buf, mqtt_username))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        strcpy(config->mqtt_username, (char *)event->data.scalar.value);
    }
    else if (!strcmp(buf, mqtt_password))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        strcpy(config->mqtt_password, (char *)event->data.scalar.value);
    }
    else if (!strcmp(buf, bluetooth_adapter))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->bluetooth_adapter = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, scan_type))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->scan_type = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, scan_window))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->scan_window = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, scan_interval))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->scan_interval = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, publish_type))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->publish_type = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, auto_configure))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->auto_configure = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, auto_conf_stats))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->auto_conf_stats = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, auto_conf_tempf))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->auto_conf_tempf = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, auto_conf_tempc))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->auto_conf_tempc = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, auto_conf_hum))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->auto_conf_hum = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, auto_conf_battery))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->auto_conf_battery = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, auto_conf_voltage))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->auto_conf_voltage = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, auto_conf_signal))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->auto_conf_signal = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, syslog_address))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        strcpy(config->syslog_address, (char *)event->data.scalar.value);
    }
    else if (!strcmp(buf, logging_level))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->logging_level = strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if ((*seq_status) == true)
    {
        /* Data from sequence of sensors */
        to_data_from_map(buf, map_seq, config, parser, event, fp);
    }
    else if (!strcmp(buf, sensors))
    {
        /* Do nothing, "sensors" is just the label of mapping's sequence */
    }
    else
    {
        printf("\n -ERROR: Unknow variable in config file: %s\n", buf);
        clean_prs(fp, parser, event);
        exit(EXIT_FAILURE);
    }
}

void to_data_from_map(char *buf, unsigned int *map_seq, config_t *config,
                      yaml_parser_t *parser, yaml_event_t *event, FILE *fp)
{
    /* Dictionary */
    char *name = "name";
    char *type = "type";
    char *mac = "mac";
    char *location = "location";
    char *unique = "unique";

    if (!strcmp(buf, name))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        strcpy(config->sensors[(*map_seq) - 1].name,
               (char *)event->data.scalar.value);
    }
    else if (!strcmp(buf, type))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->sensors[(*map_seq) - 1].type =
            strtol((char *)event->data.scalar.value, NULL, 10);
    }
    else if (!strcmp(buf, mac))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        config->sensors[(*map_seq) - 1].readings_per_hour = 0;
        strcpy(config->sensors[(*map_seq) - 1].mac,
               (char *)event->data.scalar.value);
    }
    else if (!strcmp(buf, location))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        strcpy(config->sensors[(*map_seq) - 1].location,
               (char *)event->data.scalar.value);
    }
    else if (!strcmp(buf, unique))
    {
        yaml_event_delete(event);
        parse_next(parser, event);
        strcpy(config->sensors[(*map_seq) - 1].unique,
               (char *)event->data.scalar.value);
    }
    else
    {
        printf("\n -ERROR: Unknow variable in config file: %s\n", buf);
        clean_prs(fp, parser, event);
        exit(EXIT_FAILURE);
    }
}

void parse_next(yaml_parser_t *parser, yaml_event_t *event)
{
    /* Parse next scalar. if wrong exit with error */
    if (!yaml_parser_parse(parser, event))
    {
        printf("Parser error %d\n", parser->error);
        exit(EXIT_FAILURE);
    }
}

void init_prs(FILE *fp, yaml_parser_t *parser)
{
    /* Parser initilization */
    if (!yaml_parser_initialize(parser))
    {
        fputs("Failed to initialize parser!\n", stderr);
    }

    if (fp == NULL)
    {
        fputs("Failed to open file!\n", stderr);
    }

    yaml_parser_set_input_file(parser, fp);
}

void clean_prs(FILE *fp, yaml_parser_t *parser, yaml_event_t *event)
{
    yaml_event_delete(event);   /* Delete event */
    yaml_parser_delete(parser); /* Delete parser */
    fclose(fp);                 /* Close file */
}

void print_data(unsigned int sensor_count, config_t *config)
{
    puts("\n --- data structure after parsing ---");
    printf(" mqtt_server_url = %s\n", config->mqtt_server_url);
    printf(" mqtt_base_topic = %s\n", config->mqtt_base_topic);
    printf(" mqtt_username = %s\n", config->mqtt_username);
    printf(" mqtt_password = %s\n", config->mqtt_password);
    printf(" bluetooth_adapter = %i\n", config->bluetooth_adapter);
    printf(" scan_type = %i\n", config->scan_type);
    printf(" scan_window = %i\n", config->scan_window);
    printf(" scan_interval = %i\n", config->scan_interval);
    printf(" publish_type = %i\n", config->publish_type);
    printf(" auto_configure = %i\n", config->auto_configure);
    printf(" auto_conf_stats = %i\n", config->auto_conf_stats);
    printf(" auto_conf_tempf = %i\n", config->auto_conf_tempf);
    printf(" auto_conf_tempc = %i\n", config->auto_conf_tempc);
    printf(" auto_conf_hum = %i\n", config->auto_conf_hum);
    printf(" auto_conf_battery = %i\n", config->auto_conf_battery);
    printf(" auto_conf_voltage = %i\n", config->auto_conf_voltage);
    printf(" auto_conf_signal = %i\n", config->auto_conf_signal);
    printf(" syslog_address = %s\n", config->syslog_address);
    printf(" logging_level = %i\n", config->logging_level);

    puts(" sensor configs:");
    puts("\t -----------------");
    for (int i = 0; i < (int)sensor_count; i++)
    {
        printf("\t name = %s\n", config->sensors[i].name);
        printf("\t unique = %s\n", config->sensors[i].unique);
        printf("\t location = %s\n", config->sensors[i].location);
        printf("\t type = %i\n", config->sensors[i].type);
        printf("\t mac = %s\n", config->sensors[i].mac);
        puts("\t -----------------");
    }
}
