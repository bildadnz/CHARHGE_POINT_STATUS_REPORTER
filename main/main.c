#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <cJSON.h>

#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/uart_vfs.h"
#include "driver/uart.h"

#define WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif

#if CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#endif

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
static esp_mqtt_client_handle_t mqttClient;
/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static const char *TAG = "charge station";
static int s_retry_num = 0;
static int isConnected = 0;
static const int RX_BUF_SIZE = 128;

typedef struct {
    char charge_state[20];
    int uptime_s;
    float current_A;
    float voltage_V;
} shared_data_t;
shared_data_t device_stats = { .charge_state = "IDLE", .uptime_s = 0, .current_A = 0.0, 
    .voltage_V = 0.0 };
QueueHandle_t xQueue = NULL;
QueueHandle_t pwm_queue = NULL;
QueueHandle_t simQueue = NULL;

static int chargetime = 0;
void parse_json(const char *json_string) {   
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) return;
    shared_data_t data;
    // 1. Extract values into the struct
    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "charge_state");
    if (cJSON_IsString(state) && state->valuestring) {
        if (strcmp(state->valuestring, "START_CHARGING") == 0) {
            if(strncmp(data.charge_state, "CHARGING", 8) == 0)
                ESP_LOGI("STATE", "Received CHARGING command");
            strncpy(data.charge_state, "CHARGING", sizeof("CHARGING") - 1);
            chargetime = 1;
        }else if (strcmp(state->valuestring, "STOP_CHARGING") == 0) {
            if(strncmp(device_stats.charge_state, "CHARGING", 8) == 0 && chargetime > 0){
                chargetime = 0;
                data.uptime_s = 0;
                ESP_LOGI("STATE", "Received STOP_CHARGING command");
                strncpy(data.charge_state, "IDLE", sizeof("IDLE") - 1);
            } else {
                data.uptime_s = 0; // No charging time if we weren't charging
                strncpy(data.charge_state, "IDLE", sizeof("IDLE") - 1);
            }
        }else {
            strncpy(data.charge_state, "IDLE", sizeof("IDLE") - 1);
        }
    }

    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current_A");
    if (cJSON_IsNumber(current)) {
        data.current_A = current->valuedouble;
    }

    // 2. Send the struct to the queue
    if (xQueue != NULL) {
        if (xQueueSend(xQueue, &data, (TickType_t)0) != pdPASS) {
            ESP_LOGE(TAG, "Queue full, could not send data");
        }
    }else{
        ESP_LOGE(TAG, "Failed to create queue");
    }
    if (pwm_queue != NULL) {
        if (xQueueSend(pwm_queue, &data, (TickType_t)0) != pdPASS) {
            ESP_LOGE(TAG, "Queue full, could not send data");
        }
    }else{
        ESP_LOGE(TAG, "Failed to create queue");
    }
    if (simQueue != NULL) {
        if (xQueueSend(simQueue, &data, (TickType_t)0) != pdPASS) {
            ESP_LOGE(TAG, "Queue full, could not send data");
        }
    }else{
        ESP_LOGE(TAG, "Failed to create queue");
    }
    cJSON_Delete(root);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        isConnected = 1;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID,&event_handler,NULL,&instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP,
                    &event_handler,NULL,&instance_got_ip));

    // Read WiFi credentials from NVS, with defaults
    char current_ssid[64] = {0};
    char current_pass[64] = {0};
    nvs_handle_t nvs_handle;
    if (nvs_open("wifi_config", NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t len;
        if (nvs_get_str(nvs_handle, "ssid", NULL, &len) == ESP_OK) {
            nvs_get_str(nvs_handle, "ssid", current_ssid, &len);
        }
        if (nvs_get_str(nvs_handle, "password", NULL, &len) == ESP_OK) {
            nvs_get_str(nvs_handle, "password", current_pass, &len);
        }
        nvs_close(nvs_handle);
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };
    memcpy(wifi_config.sta.ssid, current_ssid, strlen(current_ssid));
    memcpy(wifi_config.sta.password, current_pass, strlen(current_pass));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,pdFALSE,pdFALSE,portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", current_ssid);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", current_ssid);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void mqtt_publish_task(void *pvParameters) {
    char jsonPayload[128];
    const TickType_t xTicksToWait = pdMS_TO_TICKS(5000);
    while (1) {
        // 1. Wait for data, but only for 5 seconds
        // If data arrives, it returns pdPASS. If 5s passes, it returns pdFALSE.
        BaseType_t xStatus = xQueueReceive(xQueue, &device_stats, xTicksToWait);
        if (xStatus == pdPASS)continue;
        // 2. Format the JSON (using either new or last-known data)
        snprintf(jsonPayload, sizeof(jsonPayload),
                 "{\"device_id\":\"chtest001\", \"uptime_s\": %d,\"voltage_V\": %.1f, \"current_A\": %.1f, \"charge_state\": \"%s\"}",
                 device_stats.uptime_s, device_stats.voltage_V, device_stats.current_A, device_stats.charge_state);
        // 3. Publish
        int msg_id = esp_mqtt_client_publish(mqttClient, "chaji/charger/<chtest_001>/status", jsonPayload, 0, 0, 0);
        if (msg_id == -1) {
            ESP_LOGE(TAG, "Failed to publish MQTT message");
        }        
        // No vTaskDelay needed! xQueueReceive handles the timing.
    }
}

void pwm_task(void *pvParameters) {    
    while (1) {
        // Check for new data, but don't block forever so we can keep blinking
        if (xQueueReceive(pwm_queue, &device_stats, 0) == pdPASS) {
            continue;
        }
        if (strncmp(device_stats.charge_state, "IDLE", 4) == 0) {\
            // 2Hz = 250ms ON, 250ms OFF
            gpio_set_level(13, 0);
            gpio_set_level(12, 0);
            gpio_set_level(14, 1);
            vTaskDelay(pdMS_TO_TICKS(250));
            gpio_set_level(14, 0);
            vTaskDelay(pdMS_TO_TICKS(250));
        } else if (strncmp(device_stats.charge_state, "CHARGING", 8) == 0) {
            gpio_set_level(14, 0);
            gpio_set_level(13, 0);
            gpio_set_level(12, 1);
            vTaskDelay(pdMS_TO_TICKS(250));
            gpio_set_level(12, 0);
            vTaskDelay(pdMS_TO_TICKS(250));
        } else {
            gpio_set_level(14, 0);
            gpio_set_level(12, 0);
            gpio_set_level(13, 1);
            vTaskDelay(pdMS_TO_TICKS(250));
            gpio_set_level(13, 0);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}

void simulation_task(void *pvParameters) {
    float voltage = 230.0f; // Start at nominal
    float current = 0.0f; // Start with last known current
    while (1){
        if (xQueueReceive(simQueue, &device_stats, 0) == pdPASS) {
            ESP_LOGI("SIM", "State updated to: %s", device_stats.charge_state);
            current = device_stats.current_A; // Update current based on received command
        }
        // 1. Simulate random fluctuations (+/- 10.5V and +/- 0.5A)
        if (strncmp(device_stats.charge_state, "CHARGING", 8) == 0) {
            float change = (float)((esp_random() % 100) - 50.0f) / 4.8f;
            voltage += change;
            device_stats.uptime_s = chargetime*10; // Simulate uptime increment
            chargetime++;
            strncpy(device_stats.charge_state, "CHARGING", sizeof("CHARGING") - 1);
            device_stats.charge_state[sizeof("CHARGING") - 1] = '\0';
        }else {
            voltage = 220.0f + (float)(esp_random() % 100) / 5.0f;
            if(strncmp(device_stats.charge_state, "FAULT", 5) == 0 && 
            (voltage >= 220.0f && voltage <= 240.0f) ){
                if(device_stats.uptime_s > 0) {
                    strncpy(device_stats.charge_state, "CHARGING", sizeof("CHARGING") - 1);
                    device_stats.charge_state[sizeof("CHARGING") - 1] = '\0';
                    device_stats.uptime_s -= 10;
                    ESP_LOGW("SIM", "Voltage back to normal. Setting state to CHARGING.");
                }else{
                    strncpy(device_stats.charge_state, "IDLE", sizeof("IDLE") - 1);
                    device_stats.charge_state[sizeof("IDLE") - 1] = '\0';
                    ESP_LOGW("SIM", "Voltage back to normal. Setting state to IDLE.");
                }
            }
        }
        // 2. Logic: Determine state based on Voltage thresholds
        if(voltage < 220.0f || voltage > 240.0f){
            strncpy(device_stats.charge_state, "FAULT", sizeof("FAULT") - 1);
            device_stats.charge_state[sizeof("FAULT") - 1] = '\0';
            ESP_LOGW("SIM", "Voltage out of range! Setting state to FAULT.");
             // Ensure null termination
        }
        // 3. Package data into the struct
        device_stats.current_A = current;
        device_stats.voltage_V = voltage;
        //printf("SIMULATION: Voltage=%.1fV, Current=%.1fA, State=%s, Uptime=%d s\n", device_stats.voltage_V, device_stats.current_A, device_stats.charge_state, device_stats.uptime_s);
        // 4. Send to BOTH queues (MQTT and PWM)
        if (xQueue!= NULL) xQueueSend(xQueue, &device_stats, 0);
        else ESP_LOGE(TAG, "Failed to create MQTT queue");
        if (pwm_queue != NULL)  xQueueSend(pwm_queue, &device_stats, 0);
        else ESP_LOGE(TAG, "Failed to create PWM queue");
        // 5. Update interval (e.g., every 10 seconds)
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, "chaji/charger/<chtest_001>/cmd", 0);
        xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, NULL, 5, NULL);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d, return code=0x%02x ", event->msg_id,
             (uint8_t)*event->data);
        break;
        
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        parse_json(event->data);
        break;
        
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGI(TAG, "Last captured errno : %d (%s)",  event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        } else {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;
        
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = CONFIG_BROKER_URI,
        },
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    mqttClient = client;    
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

static void rx_task(void *arg)
{
    uart_vfs_dev_use_driver(UART_NUM_0);
    static const char *RX_TASK_TAG = "SERIAL_RX";
    esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);
    uint8_t* data = (uint8_t*) malloc(RX_BUF_SIZE+1);
    uint8_t* pass_data = (uint8_t*) malloc(RX_BUF_SIZE + 1);
    char buff;
    while(1) {
        ESP_LOGI(RX_TASK_TAG, "Do you want to change WiFi credentials? Type y/n :");
        if (uart_read_bytes(UART_NUM_0, &buff, 1, 5000/ portTICK_PERIOD_MS) > 0) 
            ESP_LOGI(RX_TASK_TAG, "Received: %c", buff);
        if (buff == 'y' || buff == 'Y') {
            ESP_LOGI(RX_TASK_TAG, "Changing WiFi credentials. SSID:");
            // Read new credentials from UART and update the config
            const int rxBytes = uart_read_bytes(UART_NUM_0, data, RX_BUF_SIZE, 10000 / portTICK_PERIOD_MS);
            if (rxBytes > 0) {
                data[rxBytes] = 0;
                ESP_LOGI(RX_TASK_TAG, "wifi ssid: '%s' size '%d' bytes", data, rxBytes);
                // Now read password
                ESP_LOGI(RX_TASK_TAG, "Password:");
                const int passBytes = uart_read_bytes(UART_NUM_0, pass_data, RX_BUF_SIZE, 10000 / portTICK_PERIOD_MS);
                if (passBytes > 0) {
                    pass_data[passBytes] = 0;
                    ESP_LOGI(RX_TASK_TAG, "wifi password received, size '%d' bytes", passBytes);
                    // Save to NVS
                    nvs_handle_t nvs_handle;
                    if (nvs_open("wifi_config", NVS_READWRITE, &nvs_handle) == ESP_OK) {
                        nvs_set_str(nvs_handle, "ssid", (char*)data);
                        nvs_set_str(nvs_handle, "password", (char*)pass_data);
                        nvs_commit(nvs_handle);
                        nvs_close(nvs_handle);
                        ESP_LOGI(RX_TASK_TAG, "WiFi credentials updated in NVS.");
                        break;
                    } else {
                        ESP_LOGE(RX_TASK_TAG, "Failed to open NVS for writing.");
                    }
                } else {
                    ESP_LOGI(RX_TASK_TAG, "No password received.");
                }
                free(pass_data);
            } else {
                ESP_LOGI(RX_TASK_TAG, "No SSID received.");
            }
        } else {
            // Save to NVS
            nvs_handle_t nvs_handle;
            if (nvs_open("wifi_config", NVS_READWRITE, &nvs_handle) == ESP_OK) {
                nvs_set_str(nvs_handle, "ssid", WIFI_SSID);
                nvs_set_str(nvs_handle, "password", WIFI_PASS);
                nvs_commit(nvs_handle);
                nvs_close(nvs_handle);
                ESP_LOGI(RX_TASK_TAG, "Keeping existing WiFi credentials.");
                break;
            } 
            break;
        }
    }
    free(data);
}

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    // Seed the random number generator once
    srand(esp_random());
    // Create queues before starting tasks
    xQueue = xQueueCreate(10, sizeof(shared_data_t));
    pwm_queue = xQueueCreate(10, sizeof(shared_data_t));
    simQueue = xQueueCreate(10, sizeof(shared_data_t));
    //initialize UART for debugging
    if (uart_driver_install(UART_NUM_0, 2 * 1024, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Driver installation failed");
        vTaskDelete(NULL);
    }
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_NUM_0, &uart_config);
    //initialize GPIO for PWM simulation
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << 12) | (1ULL << 13) | (1ULL << 14),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);
    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
    }
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    rx_task(NULL); // Run the UART task first to allow credential updates before WiFi starts
    wifi_init_sta();
    if(isConnected == 0){
        while(1){
            rx_task(NULL); // Run uart again to allow credential updates before WiFi starts
            wifi_init_sta();
            if(isConnected == 1){
                break;
            }
            ESP_LOGI(TAG, "Retrying WiFi connection...");
        }
    }   
    if (isConnected){
        mqtt_start();
        xTaskCreate(pwm_task, "pwm_task", 4096, NULL, 5, NULL);
        xTaskCreate(simulation_task, "simulation_task", 4096, NULL, 5, NULL);
    }
}