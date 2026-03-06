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
static lv_obj_t *time_label = NULL;
static lv_obj_t *wifi_indicator = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *record_btn = NULL;
static lv_obj_t *record_btn_label = NULL;
static lv_obj_t *list_container = NULL;

// Task list (stored with IDs for completing)
static todoist_task_t *task_list = NULL;
static int task_count = 0;

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
static void set_status(const char *text)
{
    if (status_label) {
        lv_label_set_text(status_label, text);
    }
}

static void free_task_list(void)
{
    if (task_list) {
        for (int i = 0; i < task_count; i++) {
            free(task_list[i].id);
            free(task_list[i].content);
        }
        free(task_list);
        task_list = NULL;
    }
    task_count = 0;
}

static void task_item_click_cb(lv_event_t *e);

static void refresh_list_ui(void)
{
    if (!list_container) return;

    lv_obj_clean(list_container);

    if (task_count == 0) {
        lv_obj_t *empty = lv_label_create(list_container);
        lv_label_set_text(empty, "No items. Hold button to add.");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_20, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (int i = 0; i < task_count; i++) {
        lv_obj_t *btn = lv_btn_create(list_container);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_pad_ver(btn, 12, 0);
        lv_obj_set_style_pad_hor(btn, 16, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text_fmt(lbl, LV_SYMBOL_OK "  %s", task_list[i].content);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, lv_pct(100));

        lv_obj_add_event_cb(btn, task_item_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

// ---------------------------------------------------------------------
// Complete task on tap
// ---------------------------------------------------------------------
typedef struct {
    char task_id[64];
} complete_task_arg_t;

static void complete_task_worker(void *pvParameters)
{
    complete_task_arg_t *arg = (complete_task_arg_t *)pvParameters;

    bsp_display_lock(0);
    set_status("Completing...");
    bsp_display_unlock();

    bool ok = api_todoist_complete_task(arg->task_id);
    free(arg);

    // Refresh list
    free_task_list();
    task_list = api_todoist_get_tasks_with_ids(&task_count);

    bsp_display_lock(0);
    if (ok) {
        set_status("Done!");
    } else {
        set_status("Failed to complete");
    }
    refresh_list_ui();
    bsp_display_unlock();

    vTaskDelete(NULL);
}

static void task_item_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= task_count) return;
    if (!wifi_connected) {
        set_status("No WiFi");
        return;
    }

    complete_task_arg_t *arg = malloc(sizeof(complete_task_arg_t));
    strncpy(arg->task_id, task_list[idx].id, sizeof(arg->task_id) - 1);
    arg->task_id[sizeof(arg->task_id) - 1] = '\0';

    xTaskCreate(complete_task_worker, "complete_task", 8192, arg, 5, NULL);
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
    set_status("Recognizing speech...");
    bsp_display_unlock();

    char *transcript = api_google_stt(arg->samples, arg->count);
    heap_caps_free(arg->samples);
    free(arg);

    if (!transcript) {
        bsp_display_lock(0);
        set_status("Speech not recognized");
        bsp_display_unlock();
        vTaskDelete(NULL);
        return;
    }

    bsp_display_lock(0);
    lv_label_set_text_fmt(status_label, "Adding: %s", transcript);
    bsp_display_unlock();

    bool added = api_todoist_add_task(transcript);
    free(transcript);

    // Refresh list from server
    free_task_list();
    task_list = api_todoist_get_tasks_with_ids(&task_count);

    bsp_display_lock(0);
    if (added) {
        set_status("Item added!");
    } else {
        set_status("Failed to add item");
    }
    refresh_list_ui();
    bsp_display_unlock();

    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------
// Record Button Callbacks
// ---------------------------------------------------------------------
static void record_btn_pressed_cb(lv_event_t *e)
{
    if (!wifi_connected) {
        set_status("No WiFi connection");
        return;
    }
    rec_sample_count = 0;
    is_recording = true;
    set_status("Recording...");
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
        set_status("Too short, try again");
        return;
    }

    // Copy audio to a separate buffer for the worker task
    size_t bytes = samples * sizeof(int16_t);
    int16_t *copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!copy) {
        set_status("Out of memory");
        return;
    }
    memcpy(copy, rec_buffer, bytes);

    stt_task_arg_t *arg = malloc(sizeof(stt_task_arg_t));
    arg->samples = copy;
    arg->count = samples;

    set_status("Processing...");
    xTaskCreate(stt_todoist_task, "stt_todoist", 16384, arg, 5, NULL);
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

    if (time_label) {
        if (timeinfo.tm_year > 100) {
            lv_label_set_text_fmt(time_label, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        } else {
            lv_label_set_text(time_label, "--:--");
        }
    }

    if (wifi_indicator) {
        lv_obj_set_style_bg_color(wifi_indicator,
            wifi_connected ? lv_color_hex(0x4caf50) : lv_color_hex(0xf44336), 0);
    }

    // Night mode: dim backlight between 21:00 and 07:00
    if (timeinfo.tm_year > 100) {
        bool should_dim = (timeinfo.tm_hour >= 21 || timeinfo.tm_hour < 7);
        if (should_dim != night_mode_active) {
            night_mode_active = should_dim;
            bsp_display_brightness_set(should_dim ? 10 : 100);
        }
    }
}

// ---------------------------------------------------------------------
// Initial Task List Fetch (runs once at startup)
// ---------------------------------------------------------------------
static void fetch_tasks_worker(void *pvParameters)
{
    ESP_LOGI(TAG, "fetch_tasks_worker: started, wifi_connected=%d", wifi_connected);

    // Wait for WiFi if not yet connected
    for (int i = 0; i < 100 && !wifi_connected; i++) {
        if (i % 10 == 0) ESP_LOGI(TAG, "Waiting for WiFi... (%d/100)", i);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!wifi_connected) {
        ESP_LOGW(TAG, "fetch_tasks_worker: WiFi not connected after waiting");
        bsp_display_lock(0);
        set_status("WiFi not connected");
        bsp_display_unlock();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "fetch_tasks_worker: WiFi connected, fetching tasks...");
    bsp_display_lock(0);
    set_status("Loading list...");
    bsp_display_unlock();

    free_task_list();
    task_list = api_todoist_get_tasks_with_ids(&task_count);

    ESP_LOGI(TAG, "fetch_tasks_worker: got %d tasks (task_list=%p)", task_count, (void *)task_list);

    bsp_display_lock(0);
    if (task_list) {
        lv_label_set_text_fmt(status_label, "Ready (%d items)", task_count);
    } else {
        set_status("Failed to load list");
    }
    refresh_list_ui();
    bsp_display_unlock();

    vTaskDelete(NULL);
}

static void refresh_btn_click_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Refresh button clicked, wifi=%d", wifi_connected);
    if (!wifi_connected) {
        set_status("No WiFi");
        return;
    }
    set_status("Refreshing...");
    xTaskCreate(fetch_tasks_worker, "refresh", 8192, NULL, 4, NULL);
}

// ---------------------------------------------------------------------
// Build UI
// ---------------------------------------------------------------------
static void create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a1a), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header (60px) ----
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, LCD_H_RES, 60);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0d0d0d), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // WiFi indicator (small circle)
    wifi_indicator = lv_obj_create(header);
    lv_obj_set_size(wifi_indicator, 16, 16);
    lv_obj_align(wifi_indicator, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_radius(wifi_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wifi_indicator, lv_color_hex(0xf44336), 0);
    lv_obj_set_style_border_width(wifi_indicator, 0, 0);

    // Title
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Shopping List");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 40, 0);

    // Refresh button
    lv_obj_t *refresh_btn = lv_btn_create(header);
    lv_obj_set_size(refresh_btn, 50, 40);
    lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, -80, 0);
    lv_obj_set_style_bg_color(refresh_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(refresh_btn, lv_color_hex(0x555555), LV_STATE_PRESSED);
    lv_obj_set_style_radius(refresh_btn, 6, 0);
    lv_obj_t *refresh_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_lbl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(refresh_lbl, lv_color_white(), 0);
    lv_obj_center(refresh_lbl);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_click_cb, LV_EVENT_CLICKED, NULL);

    // Time
    time_label = lv_label_create(header);
    lv_label_set_text(time_label, "--:--");
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_24, 0);
    lv_obj_align(time_label, LV_ALIGN_RIGHT_MID, -16, 0);

    // ---- Scrollable List Area (~560px) ----
    list_container = lv_obj_create(scr);
    lv_obj_set_size(list_container, LCD_H_RES - 20, 560);
    lv_obj_align(list_container, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(list_container, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(list_container, 0, 0);
    lv_obj_set_style_pad_all(list_container, 8, 0);
    lv_obj_set_style_pad_row(list_container, 8, 0);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);

    // ---- Status Bar (40px) ----
    lv_obj_t *status_bar = lv_obj_create(scr);
    lv_obj_set_size(status_bar, LCD_H_RES, 40);
    lv_obj_align(status_bar, LV_ALIGN_BOTTOM_MID, 0, -140);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x0d0d0d), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

    status_label = lv_label_create(status_bar);
    lv_label_set_text(status_label, "Starting...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_18, 0);
    lv_obj_center(status_label);

    // ---- Record Button (140px) ----
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

    // 9. Fetch initial task list
    ESP_LOGI(TAG, "Starting initial task fetch...");
    xTaskCreate(fetch_tasks_worker, "init_fetch", 8192, NULL, 4, NULL);
}
