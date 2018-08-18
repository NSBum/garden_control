#include <stdio.h>
#include <string.h>
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

#define I2C_SDA	(CONFIG_I2C_SDA)  //    default GPIO_NUM_21
#define I2C_SCL (CONFIG_I2C_SCL)  //	default GPIO_NUM_22
#define RELAY_CONTROL (CONFIG_RELAY_CONTROL)

static const char *TAG="sta_mode_tcp_server";

#define LISTENQ 2

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

//	global temp and humidity
float temp,hum;

typedef enum {
    cmd_status_idle,
    cmd_status_report,
    cmd_status_reportcase,
    cmd_status_relayon,
    cmd_status_relayon0,
    cmd_status_relayoff,
    cmd_status_relayoff0
} cmd_status_t;

cmd_status_t cmd_status;

int relay_status;

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
            //set O_NONBLOCK so that recv will return, otherwise we need to impliment message end
            //detection logic. If know the client message format you should instead impliment logic
            //detect the end of message
            //fcntl(cs,F_SETFL,O_NONBLOCK);
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
                    else if( recv_buf[0] == 'N' && recv_buf[1] == '0') {
                        cmd_status = cmd_status_relayon0;
                        break;
                    }
                    else if( recv_buf[0] == 'F' && recv_buf[1] == '0') {
                        cmd_status = cmd_status_relayoff0;
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
            if( cmd_status == cmd_status_reportcase ) {
                //sprintf(str,"T0:%0.1f,H0:%0.1f",temp,hum);
                char *s = create_json_response_th(temp,hum);
                strcpy(str,s);
                cmd_status = cmd_status_idle;
            }
            else if( cmd_status == cmd_status_relayon0 ) {
                gpio_set_level(RELAY_CONTROL, 1);
                relay_status = true;
                char *s = create_json_response_relay(relay_status,0);
                strcpy(str,s);
                cmd_status = cmd_status_idle;
            }
            else if( cmd_status == cmd_status_relayoff0 ) {
                gpio_set_level(RELAY_CONTROL, 0);
                relay_status = false;
                char *s = create_json_response_relay(relay_status,0);
                strcpy(str,s);
                cmd_status = cmd_status_idle;
            }
            else {
                sprintf(str,"Unknown command: %d",memcmp("RC",recv_buf,2));
            }
            if( write(cs , str , strlen(str)) < 0)
            {
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
    
     // 
    gpio_set_direction(RELAY_CONTROL, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_CONTROL,0);
    ESP_LOGV(TAG,"Relay off at start");
    relay_status = false;

    xTaskCreate(&printWiFiIP,"printWiFiIP",2048,NULL,5,NULL);
    xTaskCreate(&tcp_server,"tcp_server",4096,NULL,5,NULL);
    xTaskCreate(&query_sensor, "sensor_task", 2048, NULL, 5,NULL);
}
