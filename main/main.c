#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "api.h"

#if __has_include("secrets.h")
    #include "secrets.h"
#else
    #error "Missing 'secrets.h'! Copy 'main/secrets.h.example' to 'main/secrets.h' and fill in your credentials."
#endif

static const char *TAG = "main";

// ---------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------
#define LCD_H_RES       800
#define LCD_V_RES       800
#define SAMPLE_RATE     16000
#define REC_MAX_SEC     5
#define REC_BUFFER_SAMPLES (SAMPLE_RATE * REC_MAX_SEC)
#define MIN_REC_SAMPLES (SAMPLE_RATE / 2)  // 0.5 seconds minimum
#define TOAST_DURATION_MS 8000

// Home Assistant BMP280 sensor entity IDs (fill these in when sensor is set up)
#define HA_ENTITY_TEMPERATURE  "sensor.pico_balcony_temperature"
#define HA_ENTITY_PRESSURE     "sensor.pico_balcony_pressure"
#define HA_CHECK_INTERVAL_MS   (5 * 60 * 1000)  // 5 minutes

// ---------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------
static esp_codec_dev_handle_t mic_codec_dev = NULL;

// Recording state
static int16_t *rec_buffer = NULL;
static volatile int rec_sample_count = 0;
static volatile bool is_recording = false;

// WiFi state
static volatile bool wifi_connected = false;

// UI widgets
static lv_obj_t *clock_label = NULL;
static lv_obj_t *wifi_indicator = NULL;
static lv_obj_t *toast_label = NULL;
static lv_obj_t *toast_bar = NULL;
static lv_obj_t *record_btn = NULL;
static lv_obj_t *record_btn_label = NULL;
static lv_obj_t *mc_status_label = NULL;
static lv_obj_t *ha_sensor_label = NULL;

// Toast auto-clear
static int64_t toast_show_time = 0;

// Minecraft server check
#define MC_CHECK_INTERVAL_MS (15 * 60 * 1000)
static int64_t mc_last_check = 0;

// Home Assistant sensor check
static int64_t ha_last_check = 0;

// ---------------------------------------------------------------------
// WiFi Event Handler
// ---------------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        ESP_LOGI(TAG, "WiFi event: %ld", event_id);
        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "WiFi STA started");
        } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "WiFi STA connected to AP");
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_connected = false;
            ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            wifi_connected = true;
            ESP_LOGI(TAG, "WiFi connected, got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        }
    }
}

// ---------------------------------------------------------------------
// Audio Recording Task
// ---------------------------------------------------------------------
static void audio_task(void *pvParameters)
{
    size_t chunk = 256;
    int16_t *audio_buf = malloc(chunk * sizeof(int16_t));

    while (1) {
        if (is_recording && mic_codec_dev && rec_buffer) {
            esp_codec_dev_read(mic_codec_dev, audio_buf, chunk * sizeof(int16_t));
            for (int i = 0; i < chunk; i++) {
                if (rec_sample_count < REC_BUFFER_SAMPLES) {
                    rec_buffer[rec_sample_count++] = audio_buf[i];
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// ---------------------------------------------------------------------
// UI Helpers
// ---------------------------------------------------------------------
static void show_toast(const char *text)
{
    if (toast_label) {
        lv_label_set_text(toast_label, text);
    }
    if (toast_bar) {
        lv_obj_remove_flag(toast_bar, LV_OBJ_FLAG_HIDDEN);
    }
    toast_show_time = esp_timer_get_time() / 1000; // ms
}

// ---------------------------------------------------------------------
// STT + Todoist Worker Task
// ---------------------------------------------------------------------
typedef struct {
    int16_t *samples;
    int count;
} stt_task_arg_t;

static void stt_todoist_task(void *pvParameters)
{
    stt_task_arg_t *arg = (stt_task_arg_t *)pvParameters;

    bsp_display_lock(0);
    show_toast("Recognizing speech...");
    bsp_display_unlock();

    char *transcript = api_google_stt(arg->samples, arg->count);
    heap_caps_free(arg->samples);
    free(arg);

    if (!transcript) {
        bsp_display_lock(0);
        show_toast("Speech not recognized");
        bsp_display_unlock();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Transcript: '%s'", transcript);

    bsp_display_lock(0);
    lv_label_set_text_fmt(toast_label, "Heard: \"%s\"", transcript);
    bsp_display_unlock();

    bool added = api_todoist_add_task(transcript);
    free(transcript);

    bsp_display_lock(0);
    if (added) {
        show_toast("Item added to shopping list!");
    } else {
        show_toast("Failed to add item");
    }
    bsp_display_unlock();

    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------
// Record Button Callbacks
// ---------------------------------------------------------------------
static void record_btn_pressed_cb(lv_event_t *e)
{
    if (!wifi_connected) {
        show_toast("No WiFi connection");
        return;
    }
    rec_sample_count = 0;
    is_recording = true;
    show_toast("Recording...");
    lv_obj_set_style_bg_color(record_btn, lv_color_hex(0xcc0000), 0);
    lv_label_set_text(record_btn_label, LV_SYMBOL_AUDIO "  RECORDING...");
}

static void record_btn_released_cb(lv_event_t *e)
{
    if (!is_recording) return;
    is_recording = false;

    lv_obj_set_style_bg_color(record_btn, lv_color_hex(0x1565c0), 0);
    lv_label_set_text(record_btn_label, LV_SYMBOL_AUDIO "  HOLD TO SPEAK");

    int samples = rec_sample_count;
    ESP_LOGI(TAG, "Recorded %d samples (%.1f sec)", samples, (float)samples / SAMPLE_RATE);

    if (samples < MIN_REC_SAMPLES) {
        show_toast("Too short, try again");
        return;
    }

    // Copy audio to a separate buffer for the worker task
    size_t bytes = samples * sizeof(int16_t);
    int16_t *copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!copy) {
        show_toast("Out of memory");
        return;
    }
    memcpy(copy, rec_buffer, bytes);

    stt_task_arg_t *arg = malloc(sizeof(stt_task_arg_t));
    arg->samples = copy;
    arg->count = samples;

    show_toast("Processing...");
    xTaskCreate(stt_todoist_task, "stt_todoist", 16384, arg, 5, NULL);
}

// ---------------------------------------------------------------------
// Minecraft Server Status Check
// ---------------------------------------------------------------------
static void mc_check_worker(void *pvParameters)
{
    mc_server_status_t st = api_mc_server_status("192.168.1.38", 25555);

    bsp_display_lock(0);
    if (mc_status_label) {
        if (st.online) {
            lv_label_set_text_fmt(mc_status_label, "MC: %d/%d", st.players_online, st.players_max);
            lv_obj_set_style_text_color(mc_status_label, lv_color_hex(0x4caf50), 0);
        } else {
            lv_label_set_text(mc_status_label, "MC: off");
            lv_obj_set_style_text_color(mc_status_label, lv_color_hex(0x888888), 0);
        }
    }
    bsp_display_unlock();

    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------
// Home Assistant Sensor Check
// ---------------------------------------------------------------------
static void ha_check_worker(void *pvParameters)
{
    ha_sensor_reading_t temp = api_ha_get_sensor(HA_ENTITY_TEMPERATURE);
    ha_sensor_reading_t pres = api_ha_get_sensor(HA_ENTITY_PRESSURE);

    bsp_display_lock(0);
    if (ha_sensor_label) {
        if (temp.ok && pres.ok) {
            int t_int = (int)temp.value;
            int t_dec = ((int)(temp.value * 10)) % 10;
            if (t_dec < 0) t_dec = -t_dec;
            lv_label_set_text_fmt(ha_sensor_label, "%d.%dC  %d hPa",
                                  t_int, t_dec, (int)pres.value);
            lv_obj_set_style_text_color(ha_sensor_label, lv_color_hex(0x4caf50), 0);
        } else if (temp.ok) {
            int t_int = (int)temp.value;
            int t_dec = ((int)(temp.value * 10)) % 10;
            if (t_dec < 0) t_dec = -t_dec;
            lv_label_set_text_fmt(ha_sensor_label, "%d.%dC", t_int, t_dec);
            lv_obj_set_style_text_color(ha_sensor_label, lv_color_hex(0x4caf50), 0);
        } else if (pres.ok) {
            lv_label_set_text_fmt(ha_sensor_label, "%d hPa", (int)pres.value);
            lv_obj_set_style_text_color(ha_sensor_label, lv_color_hex(0x4caf50), 0);
        } else {
            lv_label_set_text(ha_sensor_label, "Sensor: n/a");
            lv_obj_set_style_text_color(ha_sensor_label, lv_color_hex(0x888888), 0);
        }
    }
    bsp_display_unlock();

    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------
// Time Update Timer
// ---------------------------------------------------------------------
static bool night_mode_active = false;

static void update_time_cb(lv_timer_t *timer)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (clock_label) {
        if (timeinfo.tm_year > 100) {
            lv_label_set_text_fmt(clock_label, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        } else {
            lv_label_set_text(clock_label, "--:--");
        }
    }

    if (wifi_indicator) {
        lv_obj_set_style_bg_color(wifi_indicator,
            wifi_connected ? lv_color_hex(0x4caf50) : lv_color_hex(0xf44336), 0);
    }

    // Auto-hide toast after TOAST_DURATION_MS
    if (toast_bar && toast_show_time > 0) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - toast_show_time > TOAST_DURATION_MS) {
            lv_obj_add_flag(toast_bar, LV_OBJ_FLAG_HIDDEN);
            toast_show_time = 0;
        }
    }

    // Night mode: dim backlight between 21:00 and 07:00
    if (timeinfo.tm_year > 100) {
        bool should_dim = (timeinfo.tm_hour >= 21 || timeinfo.tm_hour < 7);
        if (should_dim != night_mode_active) {
            night_mode_active = should_dim;
            bsp_display_brightness_set(should_dim ? 10 : 100);
        }
    }

    // Periodic Minecraft server check
    if (wifi_connected) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (mc_last_check == 0 || (now_ms - mc_last_check > MC_CHECK_INTERVAL_MS)) {
            mc_last_check = now_ms;
            xTaskCreate(mc_check_worker, "mc_check", 8192, NULL, 3, NULL);
        }
        // Periodic Home Assistant sensor check
        if (ha_last_check == 0 || (now_ms - ha_last_check > HA_CHECK_INTERVAL_MS)) {
            ha_last_check = now_ms;
            xTaskCreate(ha_check_worker, "ha_check", 8192, NULL, 3, NULL);
        }
    }
}

// ---------------------------------------------------------------------
// Build UI
// ---------------------------------------------------------------------
static void create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a1a), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- WiFi indicator (top-left corner) ----
    wifi_indicator = lv_obj_create(scr);
    lv_obj_set_size(wifi_indicator, 16, 16);
    lv_obj_align(wifi_indicator, LV_ALIGN_TOP_LEFT, 16, 16);
    lv_obj_set_style_radius(wifi_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wifi_indicator, lv_color_hex(0xf44336), 0);
    lv_obj_set_style_border_width(wifi_indicator, 0, 0);

    // ---- Minecraft server status (top-right) ----
    mc_status_label = lv_label_create(scr);
    lv_label_set_text(mc_status_label, "MC: ...");
    lv_obj_set_style_text_color(mc_status_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(mc_status_label, &lv_font_montserrat_18, 0);
    lv_obj_align(mc_status_label, LV_ALIGN_TOP_RIGHT, -16, 12);

    // ---- Home Assistant sensor (below MC status, top-right) ----
    ha_sensor_label = lv_label_create(scr);
    lv_label_set_text(ha_sensor_label, "Sensor: ...");
    lv_obj_set_style_text_color(ha_sensor_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(ha_sensor_label, &lv_font_montserrat_18, 0);
    lv_obj_align(ha_sensor_label, LV_ALIGN_TOP_RIGHT, -16, 36);

    // ---- Large Clock (center of screen) ----
    clock_label = lv_label_create(scr);
    lv_label_set_text(clock_label, "--:--");
    lv_obj_set_style_text_color(clock_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_48, 0);
    lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, -80);

    // ---- Toast Bar (below clock, hidden by default) ----
    toast_bar = lv_obj_create(scr);
    lv_obj_set_size(toast_bar, LCD_H_RES - 40, 80);
    lv_obj_align(toast_bar, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(toast_bar, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(toast_bar, 0, 0);
    lv_obj_set_style_radius(toast_bar, 12, 0);
    lv_obj_remove_flag(toast_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toast_bar, LV_OBJ_FLAG_HIDDEN);

    toast_label = lv_label_create(toast_bar);
    lv_label_set_text(toast_label, "");
    lv_obj_set_style_text_color(toast_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(toast_label, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(toast_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(toast_label, LCD_H_RES - 80);
    lv_obj_center(toast_label);

    // ---- Record Button (bottom) ----
    record_btn = lv_btn_create(scr);
    lv_obj_set_size(record_btn, LCD_H_RES - 40, 120);
    lv_obj_align(record_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(record_btn, lv_color_hex(0x1565c0), 0);
    lv_obj_set_style_bg_color(record_btn, lv_color_hex(0x0d47a1), LV_STATE_PRESSED);
    lv_obj_set_style_radius(record_btn, 16, 0);

    record_btn_label = lv_label_create(record_btn);
    lv_label_set_text(record_btn_label, LV_SYMBOL_AUDIO "  HOLD TO SPEAK");
    lv_obj_set_style_text_color(record_btn_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(record_btn_label, &lv_font_montserrat_28, 0);
    lv_obj_center(record_btn_label);

    lv_obj_add_event_cb(record_btn, record_btn_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(record_btn, record_btn_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(record_btn, record_btn_released_cb, LV_EVENT_PRESS_LOST, NULL);

    // Start time update timer
    lv_timer_create(update_time_cb, 1000, NULL);
}

// ---------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-P4 Shopping List App...");

    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Display
    ESP_LOGI(TAG, "Initializing display...");
    bsp_display_start();
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "Display initialized");

    // Rotate 90° CW to compensate for physically rotated unit
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    }

    // 3. Audio (mic only, no speaker needed)
    ESP_LOGI(TAG, "Initializing audio...");
    if (bsp_audio_init(NULL) == ESP_OK) {
        mic_codec_dev = bsp_audio_codec_microphone_init();
        if (mic_codec_dev) {
            esp_codec_dev_sample_info_t fs_mic = {
                .sample_rate = SAMPLE_RATE,
                .channel = 1,
                .bits_per_sample = 16,
            };
            esp_codec_dev_open(mic_codec_dev, &fs_mic);
            ESP_LOGI(TAG, "Microphone initialized");
        }
    }

    // 4. Recording buffer in PSRAM
    rec_buffer = heap_caps_malloc(REC_BUFFER_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!rec_buffer) {
        ESP_LOGE(TAG, "Failed to allocate recording buffer!");
    }

    // 5. WiFi
    ESP_LOGI(TAG, "Initializing WiFi (SSID: %s)...", WIFI_SSID);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "WiFi init done, connecting...");

    // 6. NTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    // 7. Start audio recording task
    xTaskCreate(audio_task, "audio_task", 4096, NULL, 5, NULL);

    // 8. Build UI
    bsp_display_lock(0);
    create_ui();
    bsp_display_unlock();
}
