#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "bmp280.h"

// --- НАСТРОЙКИ ---
#define WIFI_SSID      "it-academy"
#define WIFI_PASS      "446RKTMbb"
#define MQTT_BROKER    "mqtt://172.16.22.167:1883"
#define TOPIC_SENSORS  "iot_proj/sensors"
#define TOPIC_ACTIONS  "iot_proj/actions"

// Пины I2C для BME280
#define SDA_GPIO 14
#define SCL_GPIO 27

static const char *TAG = "IOT_NODE";
esp_mqtt_client_handle_t mqtt_client = NULL;
bmp280_t dev;

// --- ОБРАБОТЧИК СОБЫТИЙ MQTT ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            esp_mqtt_client_subscribe(event->client, TOPIC_ACTIONS, 0);
            ESP_LOGI(TAG, "Subscribed to %s", TOPIC_ACTIONS);
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected");
            break;
            
        case MQTT_EVENT_DATA:
            if (strncmp(event->topic, TOPIC_ACTIONS, event->topic_len) == 0) {
                // Парсим JSON из топика actions
                cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
                if (root) {
                    cJSON *action_item = cJSON_GetObjectItem(root, "action");
                    if (action_item) {
                        int action = action_item->valueint;
                        ESP_LOGI(TAG, "=== ПОЛУЧЕНО ДЕЙСТВИЕ: %d ===", action);
                        ESP_LOGI(TAG, "  Обогреватель: %s", (action & (1 << 5)) ? "ВКЛ" : "ВЫКЛ");
                        ESP_LOGI(TAG, "  Кондиционер:  %s", (action & (1 << 4)) ? "ВКЛ" : "ВЫКЛ");
                        ESP_LOGI(TAG, "  Увлажнитель:  %s", (action & (1 << 3)) ? "ВКЛ" : "ВЫКЛ");
                        ESP_LOGI(TAG, "  Окно 1:       %s", (action & (1 << 2)) ? "ОТКР" : "ЗАКР");
                        ESP_LOGI(TAG, "  Окно 2:       %s", (action & (1 << 1)) ? "ОТКР" : "ЗАКР");
                        ESP_LOGI(TAG, "  Вытяжка:      %s", (action & (1 << 0)) ? "ВКЛ" : "ВЫКЛ");
                    }
                    cJSON_Delete(root);
                }
            }
            break;
            
        default:
            break;
    }
}

// --- ОБРАБОТЧИК СОБЫТИЙ WI-FI ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi disconnected. Reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// --- ИНИЦИАЛИЗАЦИЯ WI-FI ---
void wifi_init_sta(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

// --- ИНИЦИАЛИЗАЦИЯ BME280 ---
void bme280_init_sensor() {
    ESP_ERROR_CHECK(i2cdev_init());

    bmp280_params_t params;
    bmp280_init_default_params(&params);

    memset(&dev, 0, sizeof(bmp280_t));
    
    // Пробуем инициализировать BME280 на стандартном I2C адресе
    ESP_ERROR_CHECK(bmp280_init_desc(&dev, BMP280_I2C_ADDRESS_0, 0, SDA_GPIO, SCL_GPIO));
    
    esp_err_t res = bmp280_init(&dev, &params);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "BME280 initialization failed at address 0x77. Trying 0x76...");
        ESP_ERROR_CHECK(bmp280_init_desc(&dev, BMP280_I2C_ADDRESS_1, 0, SDA_GPIO, SCL_GPIO));
        res = bmp280_init(&dev, &params);
    }
    
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "BME280 initialized successfully.");
        bool bme280p = dev.id == BME280_CHIP_ID;
        ESP_LOGI(TAG, "Found %s", bme280p ? "BME280" : "BMP280");
    } else {
        ESP_LOGE(TAG, "BME280 initialization completely failed!");
    }
}

// --- ЗАДАЧА ТЕЛЕМЕТРИИ ---
void telemetry_task(void *pvParameters) {
    float temp = 0, press = 0, hum = 0;
    
    while(1) {
        if (mqtt_client != NULL) {
            if (bmp280_read_float(&dev, &temp, &press, &hum) == ESP_OK) {
                cJSON *root = cJSON_CreateObject();
                cJSON_AddNumberToObject(root, "in_temp", temp);
                cJSON_AddNumberToObject(root, "in_hum", hum);
                cJSON_AddNumberToObject(root, "in_co2", 400.0); // заглушка для комнаты
                
                char *json_str = cJSON_PrintUnformatted(root);
                esp_mqtt_client_publish(mqtt_client, TOPIC_SENSORS, json_str, 0, 0, 0);
                ESP_LOGI(TAG, "Published: %s", json_str);
                
                free(json_str);
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "Failed to read BME280 sensor");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// --- MAIN ---
void app_main(void) {
    // Инициализация NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Wi-Fi
    wifi_init_sta();

    // BME280
    bme280_init_sensor();

    // MQTT
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    // Запуск задачи отправки данных
    xTaskCreate(telemetry_task, "telemetry_task", 4096, NULL, 5, NULL);
}
