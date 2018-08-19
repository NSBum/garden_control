#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "nvs_flash.h"
//	si7021 i2c temp/humidity component
#include "si7021.h"
#include "json.h"

#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"

#define I2C_SDA	(CONFIG_I2C_SDA)  //    default GPIO_NUM_21
#define I2C_SCL (CONFIG_I2C_SCL)  //	default GPIO_NUM_22
#define RELAY_CONTROL (CONFIG_RELAY_CONTROL)

#define GPIO_DS18B20_0       (CONFIG_ONE_WIRE_GPIO)
#define MAX_DEVICES          (8)
#define DS18B20_RESOLUTION   (DS18B20_RESOLUTION_12_BIT)
#define SAMPLE_PERIOD        (1000)   // milliseconds

static const char *TAG="sta_mode_tcp_server";

#define LISTENQ 2
#define MAX_RELAY 5

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

//	global temp and humidity
float temp,hum, outside_temp;

//  State for commands incoming over TCP connection
typedef enum {
    cmd_status_idle,
    cmd_status_report,
    cmd_status_reportcase,
    cmd_status_relayon,
    cmd_status_relayoff,
    cmd_status_relayquery
} cmd_status_t;

cmd_status_t cmd_status;

//  index of relay addressed by an incoming TCP command
uint8_t addressed_relay;
//  state of nth relay
int relay_state[6];
//  GPIO numbers for connections to individual relays
gpio_num_t relay_pin[6];

//    event handler for wifi task
static esp_err_t event_handler(void *ctx, system_event_t *event) {
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        printf("got ip\n");
        printf("netmask: " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.netmask));
        printf("gw: " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.gw));
        printf("\n");
        fflush(stdout);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

void printWiFiIP(void *pvParam) {
    printf("print_WiFiIP task started \n");
    xEventGroupWaitBits(wifi_event_group,CONNECTED_BIT,false,true,portMAX_DELAY);
    tcpip_adapter_ip_info_t ip_info;
    ESP_ERROR_CHECK(tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info));
    printf("IP :  %s\n", ip4addr_ntoa(&ip_info.ip));
    vTaskDelete( NULL );
}
void tcp_server(void *pvParam) {
    ESP_LOGI(TAG,"tcp_server task started \n");

    struct sockaddr_in tcpServerAddr;
    tcpServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    tcpServerAddr.sin_family = AF_INET;
    tcpServerAddr.sin_port = htons( 3000 );

    int s;
    char recv_buf[64];
    static struct sockaddr_in remote_addr;
    static unsigned int socklen;
    socklen = sizeof(remote_addr);
    int cs;//client socket

    xEventGroupWaitBits(wifi_event_group,CONNECTED_BIT,false,true,portMAX_DELAY);

    while( 1 ) {
        s = socket(AF_INET, SOCK_STREAM, 0);
        if(s < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.\n");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... allocated socket\n");
        if(bind(s, (struct sockaddr *)&tcpServerAddr, sizeof(tcpServerAddr)) != 0) {
            ESP_LOGE(TAG, "... socket bind failed errno=%d \n", errno);
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... socket bind done \n");
        if( listen(s, LISTENQ) != 0 ) {
            ESP_LOGE(TAG, "... socket listen failed errno=%d \n", errno);
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        while( 1 ) {
            cs = accept(s,(struct sockaddr *)&remote_addr, &socklen);
            ESP_LOGI(TAG,"New connection request,Request data:");
            bzero(recv_buf, sizeof(recv_buf));
            int sizeUsed = 0;
            while( 1 ) {
                ssize_t sizeRead = recv(cs, recv_buf + sizeUsed, sizeof(recv_buf)-sizeUsed, 0);
                if( sizeRead < 0 ) {
                    ESP_LOGE(TAG, "recv: %d %s", sizeRead, strerror(errno));
                    break;
                }
                if( sizeRead == 0 ) {
                    break;
                }
                sizeUsed += sizeRead;
                if( sizeUsed >= 2 ) {
                    ESP_LOGI(TAG,"Two byte message received\n");
                    if( recv_buf[0] == 'R' && recv_buf[1] == 'C' ) {
                        cmd_status = cmd_status_reportcase;
                        break;
                    }
                    else if( recv_buf[0] == 'N' ) {
                        addressed_relay = recv_buf[1] - '0';
                        cmd_status = cmd_status_relayon;
                        break;
                    }
                    else if( recv_buf[0] == 'F') {
                        addressed_relay = recv_buf[1] - '0';
                        cmd_status = cmd_status_relayoff;
                        break;
                    }
                    else if( recv_buf[0] == 'Q' ) {
                        addressed_relay = recv_buf[1] - '0';
                        cmd_status = cmd_status_relayquery;
                        break;
                    }
                }

            }
            ESP_LOGI(TAG, "\nDone reading from socket");
            for( int i = 0; i < sizeUsed; i++ ) {
                putchar(recv_buf[i]);
                //printf("Received: %02d: %02X\n",i,recv_buf[i]);
            }

            char str[1024];
            switch(cmd_status) {
                case cmd_status_reportcase:
                    char *s = create_json_response_th(temp,hum);
                    strcpy(str,s);
                    cmd_status = cmd_status_idle;
                    break;
                case cmd_status_relayon:
                    gpio_set_level(relay_pin[addressed_relay], 1);
                    relay_state[addressed_relay] = true;
                    char *s = create_json_response_relay(relay_state[addressed_relay],0);
                    strcpy(str,s);
                    cmd_status = cmd_status_idle;
                    break;
                case cmd_status_relayoff:
                    gpio_set_level(relay_pin[addressed_relay], 0);
                    relay_state[addressed_relay] = false;
                    char *s = create_json_response_relay(relay_state[addressed_relay],0);
                    strcpy(str,s);
                    cmd_status = cmd_status_idle;
                    break;
                case cmd_status_relayquery:
                    char *s = create_json_response_relay(relay_state[addressed_relay],0);
                    strcpy(str,s);
                    cmd_status = cmd_status_idle;
                    break;
                default:
                    char *s = create_json_response_error(memcmp("RC",recv_buf,2))
                    strcpy(str,s);
                    cmd_status = cmd_status_idle;
            }
            if( write(cs , str , strlen(str)) < 0) {
                ESP_LOGE(TAG, "Send failed \n");
                close(s);
                vTaskDelay(4000 / portTICK_PERIOD_MS);
                continue;
            }
            ESP_LOGI(TAG, "Socket send success\n");
            close(cs);
        }
        ESP_LOGI(TAG, "Server will be opened in 5 seconds\n");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
    ESP_LOGI(TAG, "tcp_client task closed\n");
}

void query_sensor(void *pvParameter) {
    while(1) {
        temp = si7021_read_temperature();
        hum = si7021_read_humidity();

        printf("%0.2f degrees C, %0.2f%% RH\n", temp, hum);
        vTaskDelay(5000 / portTICK_RATE_MS);
    }
}

void app_main()
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    initialise_wifi();

    //	initialize I2C driver/device
    ret = si7021_init(I2C_NUM_0, I2C_SDA,I2C_SCL,GPIO_PULLUP_DISABLE,GPIO_PULLUP_DISABLE);
    ESP_ERROR_CHECK(ret);
    printf("I2C driver initialized\n");

    cmd_status = cmd_status_idle;

    relay_pin[0] = CONFIG_RELAY_0_CONTROL;
    relay_pin[1] = CONFIG_RELAY_1_CONTROL;
    relay_pin[2] = CONFIG_RELAY_2_CONTROL;
    relay_pin[3] = CONFIG_RELAY_3_CONTROL;
    relay_pin[4] = CONFIG_RELAY_4_CONTROL;

    for( uint8_t i = 0; i < 5; i++ ) {
        gpio_set_direction(relay_pin[i], GPIO_MODE_OUTPUT);
        gpio_set_level(relay_pin[i], 0 );
        relay_state[i] = false;
    }
    ESP_LOGV(TAG,"Relays off at start");

    // Allow bus to stabilize a bit before communicating
    vTaskDelay(2000.0 / portTICK_PERIOD_MS);

    xTaskCreate(&owb_search_task,"owbsearch",4096,NULL,5,NULL);
    xTaskCreate(&printWiFiIP,"printWiFiIP",2048,NULL,5,NULL);
    xTaskCreate(&tcp_server,"tcp_server",4096,NULL,5,NULL);
    xTaskCreate(&query_sensor, "sensor_task", 2048, NULL, 5,NULL);
    xTaskCreate(&owb_get_temps,"gettemp",4096,&outside_temp,5,NULL);
}
