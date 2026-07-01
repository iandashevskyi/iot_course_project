
#include "mqtt_client.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmp280.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
//#include "esp_mqtt_client.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2cdev.h"
#include "nvs.h"
#include "nvs_flash.h"

// --- FALLBACK НАСТРОЙКИ ---
#define DEFAULT_DEVICE_ID       "esp32-outdoor-01"
#define DEFAULT_WIFI_SSID       "it-academy"
#define DEFAULT_WIFI_PASS       "446RKTMbb"
#define DEFAULT_MQTT_BROKER     "mqtt://172.16.22.167:1883"
#define DEFAULT_TOPIC_SENSORS   "iot_proj/sensors"
#define DEFAULT_TOPIC_ACTIONS   "iot_proj/actions"
#define DEFAULT_TRANSPORT_TYPE  "mqtt_tcp"
#define DEFAULT_INTERVAL_MS     5000

// --- ПИНЫ I2C ДЛЯ BME280/BMP280 ---
#define SDA_GPIO 14
#define SCL_GPIO 27

// --- MQ135 ---
#define MQ135_ADC_CHAN   ADC_CHANNEL_7
#define MQ135_HEATER_PIN 15
#define MQ135_R0 71.1f

// --- SERIAL ДЛЯ ПРИЕМА КОНФИГА ---
#define CONFIG_UART              UART_NUM_0
#define CONFIG_UART_BAUDRATE     115200
#define CONFIG_RX_BUFFER_SIZE    2048
#define CONFIG_LINE_BUFFER_SIZE  1024

// --- NVS ---
#define CONFIG_NAMESPACE "iot_cfg"

static const char *TAG = "OUTDOOR_NODE";

typedef struct {
    char device_id[32];
    char wifi_ssid[33];
    char wifi_password[65];
    char broker_url[192];
    char telemetry_topic[96];
    char command_topic[96];
    char transport_type[24];
    uint32_t telemetry_interval_ms;
} runtime_config_t;

static runtime_config_t g_config;
static esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool mqtt_connected = false;
static bmp280_t sensor_dev;
static adc_oneshot_unit_handle_t adc1_handle;

static void load_default_config(runtime_config_t *config) {
    memset(config, 0, sizeof(*config));
    strlcpy(config->device_id, DEFAULT_DEVICE_ID, sizeof(config->device_id));
    strlcpy(config->wifi_ssid, DEFAULT_WIFI_SSID, sizeof(config->wifi_ssid));
    strlcpy(config->wifi_password, DEFAULT_WIFI_PASS, sizeof(config->wifi_password));
    strlcpy(config->broker_url, DEFAULT_MQTT_BROKER, sizeof(config->broker_url));
    strlcpy(config->telemetry_topic, DEFAULT_TOPIC_SENSORS, sizeof(config->telemetry_topic));
    strlcpy(config->command_topic, DEFAULT_TOPIC_ACTIONS, sizeof(config->command_topic));
    strlcpy(config->transport_type, DEFAULT_TRANSPORT_TYPE, sizeof(config->transport_type));
    config->telemetry_interval_ms = DEFAULT_INTERVAL_MS;
}

static esp_err_t nvs_read_str(nvs_handle_t handle, const char *key, char *buffer, size_t buffer_size) {
    size_t required = buffer_size;
    esp_err_t err = nvs_get_str(handle, key, buffer, &required);
    if (err == ESP_OK && required > buffer_size) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    return err;
}

static esp_err_t load_config_from_nvs(runtime_config_t *config) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_read_str(handle, "device_id", config->device_id, sizeof(config->device_id));
    if (err == ESP_OK) err = nvs_read_str(handle, "wifi_ssid", config->wifi_ssid, sizeof(config->wifi_ssid));
    if (err == ESP_OK) err = nvs_read_str(handle, "wifi_pass", config->wifi_password, sizeof(config->wifi_password));
    if (err == ESP_OK) err = nvs_read_str(handle, "broker_url", config->broker_url, sizeof(config->broker_url));
    if (err == ESP_OK) err = nvs_read_str(handle, "telem_topic", config->telemetry_topic, sizeof(config->telemetry_topic));
    if (err == ESP_OK) err = nvs_read_str(handle, "cmd_topic", config->command_topic, sizeof(config->command_topic));
    if (err == ESP_OK) err = nvs_read_str(handle, "tr_type", config->transport_type, sizeof(config->transport_type));
    if (err == ESP_OK) err = nvs_get_u32(handle, "interval_ms", &config->telemetry_interval_ms);

    nvs_close(handle);
    return err;
}

static esp_err_t save_config_to_nvs(const runtime_config_t *config) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, "device_id", config->device_id);
    if (err == ESP_OK) err = nvs_set_str(handle, "wifi_ssid", config->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "wifi_pass", config->wifi_password);
    if (err == ESP_OK) err = nvs_set_str(handle, "broker_url", config->broker_url);
    if (err == ESP_OK) err = nvs_set_str(handle, "telem_topic", config->telemetry_topic);
    if (err == ESP_OK) err = nvs_set_str(handle, "cmd_topic", config->command_topic);
    if (err == ESP_OK) err = nvs_set_str(handle, "tr_type", config->transport_type);
    if (err == ESP_OK) err = nvs_set_u32(handle, "interval_ms", config->telemetry_interval_ms);
    if (err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

static bool copy_json_string(
    const cJSON *object,
    const char *key,
    char *target,
    size_t target_size,
    bool required,
    char *error_text,
    size_t error_text_size
) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    if (item == NULL) {
        if (required) {
            snprintf(error_text, error_text_size, "Missing field: %s", key);
            return false;
        }
        return true;
    }

    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        snprintf(error_text, error_text_size, "Field %s must be a string", key);
        return false;
    }

    if (strlen(item->valuestring) >= target_size) {
        snprintf(error_text, error_text_size, "Field %s is too long", key);
        return false;
    }

    strlcpy(target, item->valuestring, target_size);
    return true;
}

static esp_err_t parse_config_payload(
    const char *json_line,
    runtime_config_t *out_config,
    char *error_text,
    size_t error_text_size
) {
    runtime_config_t parsed = g_config;
    cJSON *root = cJSON_Parse(json_line);
    if (root == NULL) {
        snprintf(error_text, error_text_size, "Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    const cJSON *transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
    const cJSON *sampling = cJSON_GetObjectItemCaseSensitive(root, "sampling");

    if (!copy_json_string(root, "deviceId", parsed.device_id, sizeof(parsed.device_id), true, error_text, error_text_size)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (!cJSON_IsObject(wifi)) {
        snprintf(error_text, error_text_size, "Field wifi must be an object");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (!copy_json_string(wifi, "ssid", parsed.wifi_ssid, sizeof(parsed.wifi_ssid), true, error_text, error_text_size) ||
        !copy_json_string(wifi, "password", parsed.wifi_password, sizeof(parsed.wifi_password), true, error_text, error_text_size)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (!cJSON_IsObject(transport)) {
        snprintf(error_text, error_text_size, "Field transport must be an object");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (!copy_json_string(transport, "type", parsed.transport_type, sizeof(parsed.transport_type), true, error_text, error_text_size) ||
        !copy_json_string(transport, "brokerUrl", parsed.broker_url, sizeof(parsed.broker_url), true, error_text, error_text_size) ||
        !copy_json_string(transport, "telemetryTopic", parsed.telemetry_topic, sizeof(parsed.telemetry_topic), true, error_text, error_text_size) ||
        !copy_json_string(transport, "commandTopic", parsed.command_topic, sizeof(parsed.command_topic), true, error_text, error_text_size)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (!cJSON_IsObject(sampling)) {
        snprintf(error_text, error_text_size, "Field sampling must be an object");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(sampling, "telemetryIntervalMs");
    if (!cJSON_IsNumber(interval)) {
        snprintf(error_text, error_text_size, "sampling.telemetryIntervalMs must be a number");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (interval->valuedouble < 500 || interval->valuedouble > 3600000) {
        snprintf(error_text, error_text_size, "telemetryIntervalMs must be in range 500..3600000");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    parsed.telemetry_interval_ms = (uint32_t)interval->valuedouble;
    *out_config = parsed;
    cJSON_Delete(root);
    return ESP_OK;
}

static void send_serial_status(const char *status, const char *message) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        const char *fallback = "{\"status\":\"error\",\"message\":\"no_memory\"}\n";
        uart_write_bytes(CONFIG_UART, fallback, strlen(fallback));
        uart_wait_tx_done(CONFIG_UART, pdMS_TO_TICKS(100));
        return;
    }

    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddStringToObject(root, "deviceId", g_config.device_id);

    char *json_text = cJSON_PrintUnformatted(root);
    if (json_text != NULL) {
        uart_write_bytes(CONFIG_UART, json_text, strlen(json_text));
        uart_write_bytes(CONFIG_UART, "\n", 1);
        uart_wait_tx_done(CONFIG_UART, pdMS_TO_TICKS(150));
        cJSON_free(json_text);
    }

    cJSON_Delete(root);
}

static void load_runtime_config(void) {
    load_default_config(&g_config);
    esp_err_t err = load_config_from_nvs(&g_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config loaded from NVS. deviceId=%s, broker=%s", g_config.device_id, g_config.broker_url);
        return;
    }

    ESP_LOGW(TAG, "NVS config not found or incomplete (%s). Using fallback defaults.", esp_err_to_name(err));
}

// --- MQTT ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)handler_args;
    (void)base;
    (void)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT connected: %s", g_config.broker_url);
            break;

        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT disconnected");
            break;

        default:
            break;
    }
}

static void start_mqtt(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = g_config.broker_url,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
}

// --- WI-FI ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected. Reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, g_config.wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, g_config.wifi_password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi start requested for SSID: %s", g_config.wifi_ssid);
}

// --- BME280/BMP280 ---
static void init_sensor(void) {
    ESP_ERROR_CHECK(i2cdev_init());

    bmp280_params_t params;
    bmp280_init_default_params(&params);

    memset(&sensor_dev, 0, sizeof(sensor_dev));
    ESP_ERROR_CHECK(bmp280_init_desc(&sensor_dev, BMP280_I2C_ADDRESS_0, 0, SDA_GPIO, SCL_GPIO));

    esp_err_t err = bmp280_init(&sensor_dev, &params);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sensor not found at 0x77, trying 0x76...");
        ESP_ERROR_CHECK(bmp280_init_desc(&sensor_dev, BMP280_I2C_ADDRESS_1, 0, SDA_GPIO, SCL_GPIO));
        err = bmp280_init(&sensor_dev, &params);
    }

    if (err == ESP_OK) {
        bool is_bme280 = sensor_dev.id == BME280_CHIP_ID;
        ESP_LOGI(TAG, "Sensor initialized: %s", is_bme280 ? "BME280" : "BMP280");
    } else {
        ESP_LOGE(TAG, "Sensor initialization failed: %s", esp_err_to_name(err));
    }
}

static void init_mq135(void) {
    gpio_config_t mq_conf = { 0 };
    mq_conf.intr_type = GPIO_INTR_DISABLE;
    mq_conf.mode = GPIO_MODE_OUTPUT;
    mq_conf.pin_bit_mask = (1ULL << MQ135_HEATER_PIN);
    mq_conf.pull_down_en = 0;
    mq_conf.pull_up_en = 0;
    ESP_ERROR_CHECK(gpio_config(&mq_conf));
    gpio_set_level(MQ135_HEATER_PIN, 1);

    adc_oneshot_unit_init_cfg_t adc_unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_unit_cfg, &adc1_handle));

    adc_oneshot_chan_cfg_t adc_channel_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_11,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, MQ135_ADC_CHAN, &adc_channel_cfg));
}

static float read_mq135_co2_ppm(void) {
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, MQ135_ADC_CHAN, &raw));

    float rs = ((5.0f * 10.0f) / ((raw * 3.3f / 4095.0f) + 0.001f)) - 10.0f;
    return 110.47f * powf(rs / MQ135_R0, -2.862f);
}

// --- ТЕЛЕМЕТРИЯ ---
static void telemetry_task(void *pvParameters) {
    (void)pvParameters;
    float temperature = 0.0f;
    float pressure = 0.0f;
    float humidity = 0.0f;

    while (true) {
        if (mqtt_client != NULL && mqtt_connected) {
            if (bmp280_read_float(&sensor_dev, &temperature, &pressure, &humidity) == ESP_OK) {
                float out_co2 = read_mq135_co2_ppm();
                cJSON *root = cJSON_CreateObject();
                if (root != NULL) {
                    cJSON_AddNumberToObject(root, "out_temp", temperature);
                    cJSON_AddNumberToObject(root, "out_hum", humidity);
                    cJSON_AddNumberToObject(root, "out_co2", out_co2);

                    char *json_text = cJSON_PrintUnformatted(root);
                    if (json_text != NULL) {
                        esp_mqtt_client_publish(
                            mqtt_client,
                            g_config.telemetry_topic,
                            json_text,
                            0,
                            0,
                            0
                        );
                        ESP_LOGI(TAG, "Published to %s: %s", g_config.telemetry_topic, json_text);
                        cJSON_free(json_text);
                    }

                    cJSON_Delete(root);
                }
            } else {
                ESP_LOGE(TAG, "Failed to read sensor");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(g_config.telemetry_interval_ms));
    }
}

// --- SERIAL CONFIG ---
static void init_serial_config_port(void) {
    const uart_config_t uart_cfg = {
        .baud_rate = CONFIG_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(CONFIG_UART, CONFIG_RX_BUFFER_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_UART, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(CONFIG_UART, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_flush_input(CONFIG_UART));
}

static void serial_config_task(void *pvParameters) {
    (void)pvParameters;

    uint8_t rx_chunk[128];
    char line_buffer[CONFIG_LINE_BUFFER_SIZE];
    size_t line_length = 0;

    while (true) {
        int received = uart_read_bytes(CONFIG_UART, rx_chunk, sizeof(rx_chunk), pdMS_TO_TICKS(200));
        if (received <= 0) {
            continue;
        }

        for (int i = 0; i < received; i++) {
            char ch = (char)rx_chunk[i];

            if (ch == '\r') {
                continue;
            }

            if (ch == '\n') {
                if (line_length == 0) {
                    continue;
                }

                line_buffer[line_length] = '\0';

                runtime_config_t new_config;
                char error_text[160];
                esp_err_t err = parse_config_payload(line_buffer, &new_config, error_text, sizeof(error_text));

                if (err != ESP_OK) {
                    send_serial_status("error", error_text);
                    ESP_LOGW(TAG, "Config rejected: %s", error_text);
                } else {
                    err = save_config_to_nvs(&new_config);
                    if (err != ESP_OK) {
                        send_serial_status("error", "Failed to save config");
                        ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
                    } else {
                        g_config = new_config;
                        send_serial_status("ok", "config_saved_restart_pending");
                        ESP_LOGI(TAG, "Config saved. Restarting to apply new settings...");
                        vTaskDelay(pdMS_TO_TICKS(300));
                        esp_restart();
                    }
                }

                line_length = 0;
                continue;
            }

            if (line_length >= sizeof(line_buffer) - 1) {
                line_length = 0;
                send_serial_status("error", "Config line too long");
                ESP_LOGW(TAG, "Serial config line is too long");
                continue;
            }

            line_buffer[line_length++] = ch;
        }
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    load_runtime_config();
    init_serial_config_port();
    wifi_init_sta();
    init_sensor();
    init_mq135();
    start_mqtt();

    xTaskCreate(serial_config_task, "serial_config_task", 4096, NULL, 6, NULL);
    xTaskCreate(telemetry_task, "telemetry_task", 4096, NULL, 5, NULL);
}
