#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
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
#define LCD_H_RES       720
#define LCD_V_RES       720
#define SAMPLE_RATE     16000
#define REC_MAX_SEC     5
#define REC_BUFFER_SAMPLES (SAMPLE_RATE * REC_MAX_SEC)
#define MIN_REC_SAMPLES (SAMPLE_RATE / 2)
#define TOAST_DURATION_MS 8000

// Home Assistant BMP280 sensor entity IDs
#define HA_ENTITY_TEMPERATURE  "sensor.pico_balcony_temperature"
#define HA_ENTITY_PRESSURE     "sensor.pico_balcony_pressure"
#define HA_CHECK_INTERVAL_MS   (15 * 1000)  // 15 seconds

// 7-segment clock dimensions
#define SEG_DW      110   // digit bounding box width
#define SEG_DH      220   // digit bounding box height
#define SEG_T       22    // segment thickness
#define SEG_TIP_GAP 2     // gap between adjacent segment tips
#define CLOCK_DIGITS 4
#define DIGIT_GAP   45    // gap between digits in a pair
#define COLON_GAP   34    // gap on each side of colon
#define COLON_DOT   20    // colon dot size

// 48h chart: 576 points at 5-min intervals
#define HA_CHART_POINTS 576
#define HA_CHART_ADD_EVERY 20   // add chart point every 20 polls (20*15s = 5min)
#define MAX_GRID_LABELS 10      // max horizontal grid line labels per chart
#define CHART_PAD_V 10          // explicit vertical padding inside chart

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
static lv_obj_t *wifi_indicator = NULL;
static lv_obj_t *toast_label = NULL;
static lv_obj_t *toast_bar = NULL;
static lv_obj_t *record_btn = NULL;
static lv_obj_t *record_btn_label = NULL;
static lv_obj_t *mc_status_label = NULL;

// 7-segment clock (canvas-based with hexagonal segments)
static lv_obj_t *clock_canvas = NULL;
static int clock_digit_vals[CLOCK_DIGITS] = {-2, -2, -2, -2};
// Segment patterns: bits 0-6 = segments a-g
// a=top, b=top-right, c=bot-right, d=bottom, e=bot-left, f=top-left, g=middle
static const uint8_t seg_patterns[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

// Sensor display
static lv_obj_t *temp_value_label = NULL;
static lv_obj_t *pres_value_label = NULL;
static lv_obj_t *temp_chart = NULL;
static lv_obj_t *pres_chart = NULL;
static lv_chart_series_t *temp_series = NULL;
static lv_chart_series_t *pres_series = NULL;
static lv_obj_t *temp_grid_labels[MAX_GRID_LABELS];
static lv_obj_t *pres_grid_labels[MAX_GRID_LABELS];
static int chart_y_pos, chart_h_size;  // stored for label positioning
static int ha_chart_add_counter = 0;

// Toast auto-clear
static int64_t toast_show_time = 0;

// Minecraft server check
#define MC_CHECK_INTERVAL_MS (1 * 60 * 1000)
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
// 7-Segment Display Helpers (canvas-based hexagonal segments)
// ---------------------------------------------------------------------
// Draw a horizontal hexagonal segment on a canvas layer
static void draw_hseg(lv_layer_t *layer, int x, int y, int w, int t, lv_color_t color)
{
    int ht = t / 2;
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = LV_OPA_COVER;

    // Left point
    tri.p[0].x = x;        tri.p[0].y = y + ht;
    tri.p[1].x = x + ht;   tri.p[1].y = y;
    tri.p[2].x = x + ht;   tri.p[2].y = y + t;
    lv_draw_triangle(layer, &tri);

    // Center rectangle
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = color;
    rect.bg_opa = LV_OPA_COVER;
    rect.radius = 0;
    rect.border_width = 0;
    lv_area_t area = {x + ht, y, x + w - ht - 1, y + t - 1};
    lv_draw_rect(layer, &rect, &area);

    // Right point
    tri.p[0].x = x + w;        tri.p[0].y = y + ht;
    tri.p[1].x = x + w - ht;   tri.p[1].y = y;
    tri.p[2].x = x + w - ht;   tri.p[2].y = y + t;
    lv_draw_triangle(layer, &tri);
}

// Draw a vertical hexagonal segment on a canvas layer
static void draw_vseg(lv_layer_t *layer, int x, int y, int w, int h, lv_color_t color)
{
    int hw = w / 2;
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = LV_OPA_COVER;

    // Top point
    tri.p[0].x = x + hw;   tri.p[0].y = y;
    tri.p[1].x = x;        tri.p[1].y = y + hw;
    tri.p[2].x = x + w;    tri.p[2].y = y + hw;
    lv_draw_triangle(layer, &tri);

    // Center rectangle
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = color;
    rect.bg_opa = LV_OPA_COVER;
    rect.radius = 0;
    rect.border_width = 0;
    lv_area_t area = {x, y + hw, x + w - 1, y + h - hw - 1};
    lv_draw_rect(layer, &rect, &area);

    // Bottom point
    tri.p[0].x = x + hw;   tri.p[0].y = y + h;
    tri.p[1].x = x;        tri.p[1].y = y + h - hw;
    tri.p[2].x = x + w;    tri.p[2].y = y + h - hw;
    lv_draw_triangle(layer, &tri);
}

static void draw_clock(void)
{
    if (!clock_canvas) return;

    lv_canvas_fill_bg(clock_canvas, lv_color_hex(0x1a1a1a), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(clock_canvas, &layer);

    // Compute digit x positions (with padding for outward-shifted vertical bars)
    int ht = SEG_T / 2;
    int pad = ht;  // left/right padding for outward-shifted verticals
    int d_x[4];
    d_x[0] = pad;
    d_x[1] = pad + SEG_DW + DIGIT_GAP;
    int colon_x = d_x[1] + SEG_DW + COLON_GAP;
    d_x[2] = colon_x + COLON_DOT + COLON_GAP;
    d_x[3] = d_x[2] + SEG_DW + DIGIT_GAP;

    // Segment geometry: vertical bars span from midpoint of one
    // horizontal bar to midpoint of the next, shifted outward by ht
    int vsec = (SEG_DH - 3 * SEG_T) / 2;  // vertical section between bars
    int mid_y = SEG_T + vsec;               // middle horizontal bar y
    int bot_y = SEG_DH - SEG_T;             // bottom horizontal bar y
    int vbar_h = vsec + SEG_T - 2 * SEG_TIP_GAP;  // midpoint-to-midpoint minus tip gaps
    int top_vy = ht + SEG_TIP_GAP;          // top verticals start just below top bar midpoint
    int bot_vy = mid_y + ht + SEG_TIP_GAP;  // bottom verticals start just below mid bar midpoint

    for (int d = 0; d < CLOCK_DIGITS; d++) {
        int dx = d_x[d];
        int value = clock_digit_vals[d];
        uint8_t pat = (value >= 0 && value <= 9) ? seg_patterns[value] : 0x40;

        struct { int x, y, w, h; bool horiz; } segs[7] = {
            {dx,                 0,      SEG_DW, SEG_T,  true},    // a: top
            {dx + SEG_DW - ht,  top_vy, SEG_T,  vbar_h, false},   // b: top-right
            {dx + SEG_DW - ht,  bot_vy, SEG_T,  vbar_h, false},   // c: bot-right
            {dx,                 bot_y,  SEG_DW, SEG_T,  true},    // d: bottom
            {dx - ht,           bot_vy, SEG_T,  vbar_h, false},   // e: bot-left
            {dx - ht,           top_vy, SEG_T,  vbar_h, false},   // f: top-left
            {dx,                 mid_y,  SEG_DW, SEG_T,  true},    // g: middle
        };

        for (int s = 0; s < 7; s++) {
            bool on = (pat >> s) & 1;
            lv_color_t color = lv_color_hex(on ? 0x00e676 : 0x1a2a1a);
            if (segs[s].horiz) {
                draw_hseg(&layer, segs[s].x, segs[s].y, segs[s].w, segs[s].h, color);
            } else {
                draw_vseg(&layer, segs[s].x, segs[s].y, segs[s].w, segs[s].h, color);
            }
        }
    }

    // Draw colon dots
    lv_draw_rect_dsc_t dot_dsc;
    lv_draw_rect_dsc_init(&dot_dsc);
    dot_dsc.bg_color = lv_color_hex(0x00e676);
    dot_dsc.bg_opa = LV_OPA_COVER;
    dot_dsc.radius = LV_RADIUS_CIRCLE;
    dot_dsc.border_width = 0;

    int dot_y1 = SEG_DH / 3 - COLON_DOT / 2;
    int dot_y2 = 2 * SEG_DH / 3 - COLON_DOT / 2;
    lv_area_t dot1 = {colon_x, dot_y1, colon_x + COLON_DOT - 1, dot_y1 + COLON_DOT - 1};
    lv_area_t dot2 = {colon_x, dot_y2, colon_x + COLON_DOT - 1, dot_y2 + COLON_DOT - 1};
    lv_draw_rect(&layer, &dot_dsc, &dot1);
    lv_draw_rect(&layer, &dot_dsc, &dot2);

    lv_canvas_finish_layer(clock_canvas, &layer);
}

static void set_digit(int digit_idx, int value)
{
    clock_digit_vals[digit_idx] = value;
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
// Chart grid helpers
// ---------------------------------------------------------------------
static int32_t floor_to(int32_t val, int32_t step) {
    if (val >= 0) return (val / step) * step;
    return ((val - step + 1) / step) * step;
}

static int32_t ceil_to(int32_t val, int32_t step) {
    if (val >= 0) return ((val + step - 1) / step) * step;
    return (val / step) * step;
}

static int32_t pick_nice_step(int32_t range, int32_t scale) {
    static const int mults[] = {1, 2, 5, 10, 20, 50, 100};
    for (int i = 0; i < 7; i++) {
        int32_t step = (int32_t)mults[i] * scale;
        int n = range / step;
        if (n >= 2 && n <= 8) return step;
    }
    return scale;
}

// Scan chart buffer for current min/max (ignores LV_CHART_POINT_NONE)
static void chart_min_max(lv_obj_t *chart, lv_chart_series_t *ser,
                          int32_t *out_min, int32_t *out_max) {
    int32_t mn = INT32_MAX, mx = INT32_MIN;
    int32_t *arr = lv_chart_get_y_array(chart, ser);
    uint32_t cnt = lv_chart_get_point_count(chart);
    for (uint32_t i = 0; i < cnt; i++) {
        if (arr[i] == LV_CHART_POINT_NONE) continue;
        if (arr[i] < mn) mn = arr[i];
        if (arr[i] > mx) mx = arr[i];
    }
    *out_min = mn;
    *out_max = mx;
}

// Update chart range to snap to round values, position grid labels
static void update_chart_grid(lv_obj_t *chart, lv_chart_series_t *ser,
                              int32_t scale, lv_obj_t *grid_labels[],
                              int chart_x) {
    int32_t data_min, data_max;
    chart_min_max(chart, ser, &data_min, &data_max);
    if (data_min > data_max) return;  // no valid data

    int32_t raw_range = data_max - data_min;
    if (raw_range < scale * 2) raw_range = scale * 2;

    int32_t step = pick_nice_step(raw_range, scale);
    int32_t lo = floor_to(data_min - step / 2, step);
    int32_t hi = ceil_to(data_max + step / 2, step);
    while ((hi - lo) / step < 3) hi += step;

    int num_steps = (hi - lo) / step;
    int num_divs = num_steps - 1;
    if (num_divs > MAX_GRID_LABELS) num_divs = MAX_GRID_LABELS;

    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
    lv_chart_set_div_line_count(chart, num_divs, 3);

    // Position labels at each horizontal grid line
    int border = 1;
    int content_top = border + CHART_PAD_V;
    int content_h = chart_h_size - 2 * (border + CHART_PAD_V);

    for (int i = 0; i < MAX_GRID_LABELS; i++) {
        if (i < num_divs) {
            int k = i + 1;
            int y = content_top + content_h * k / (num_divs + 1) - 7;
            int32_t val = hi - (int32_t)((int64_t)(hi - lo) * k / (num_divs + 1));

            if (scale == 10)
                lv_label_set_text_fmt(grid_labels[i], "%"PRId32, val / 10);
            else
                lv_label_set_text_fmt(grid_labels[i], "%"PRId32, val / 100);

            lv_obj_set_pos(grid_labels[i], chart_x + 4, chart_y_pos + y);
            lv_obj_remove_flag(grid_labels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(grid_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------------
// Home Assistant Sensor Check
// ---------------------------------------------------------------------
static void ha_check_worker(void *pvParameters)
{
    ha_sensor_reading_t temp = api_ha_get_sensor(HA_ENTITY_TEMPERATURE);
    ha_sensor_reading_t pres = api_ha_get_sensor(HA_ENTITY_PRESSURE);

    bsp_display_lock(0);

    // Update temperature label
    if (temp_value_label) {
        if (temp.ok) {
            int t_int = (int)temp.value;
            int t_dec = ((int)(temp.value * 10)) % 10;
            if (t_dec < 0) t_dec = -t_dec;
            lv_label_set_text_fmt(temp_value_label, "%d.%dC", t_int, t_dec);
            lv_obj_set_style_text_color(temp_value_label, lv_color_hex(0xff6d00), 0);
        } else {
            lv_label_set_text(temp_value_label, "--.-C");
            lv_obj_set_style_text_color(temp_value_label, lv_color_hex(0x555555), 0);
        }
    }

    // Update pressure label
    if (pres_value_label) {
        if (pres.ok) {
            int p_int = (int)pres.value;
            int p_dec = ((int)(pres.value * 100)) % 100;
            if (p_dec < 0) p_dec = -p_dec;
            lv_label_set_text_fmt(pres_value_label, "%d.%02d hPa", p_int, p_dec);
            lv_obj_set_style_text_color(pres_value_label, lv_color_hex(0x42a5f5), 0);
        } else {
            lv_label_set_text(pres_value_label, "---.-- hPa");
            lv_obj_set_style_text_color(pres_value_label, lv_color_hex(0x555555), 0);
        }
    }

    // Throttle chart point addition: every HA_CHART_ADD_EVERY polls (5 min)
    bool add_point = (++ha_chart_add_counter >= HA_CHART_ADD_EVERY);
    if (add_point) ha_chart_add_counter = 0;

    if (temp_chart && temp_series && temp.ok) {
        int32_t tv = (int32_t)(temp.value * 10);
        if (add_point) lv_chart_set_next_value(temp_chart, temp_series, tv);
        update_chart_grid(temp_chart, temp_series, 10, temp_grid_labels, 20);
    }
    if (pres_chart && pres_series && pres.ok) {
        int32_t pv = (int32_t)(pres.value * 100);
        if (add_point) lv_chart_set_next_value(pres_chart, pres_series, pv);
        update_chart_grid(pres_chart, pres_series, 100, pres_grid_labels, 20 + 330 + 20);
    }

    bsp_display_unlock();

    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------
// Time Update Timer
// ---------------------------------------------------------------------
static bool night_mode_active = false;
static int last_displayed_minute = -1;

static void update_time_cb(lv_timer_t *timer)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Update 7-segment clock
    if (timeinfo.tm_year > 100) {
        int cur_min = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        if (cur_min != last_displayed_minute) {
            last_displayed_minute = cur_min;
            set_digit(0, timeinfo.tm_hour / 10);
            set_digit(1, timeinfo.tm_hour % 10);
            set_digit(2, timeinfo.tm_min / 10);
            set_digit(3, timeinfo.tm_min % 10);
            draw_clock();
        }
    } else if (last_displayed_minute != -2) {
        // Show dashes when time not synced
        last_displayed_minute = -2;
        for (int d = 0; d < CLOCK_DIGITS; d++) set_digit(d, -1);
        draw_clock();
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
            bsp_display_brightness_set(should_dim ? 0 : 100);
        }
    }

    // Periodic checks
    if (wifi_connected) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (mc_last_check == 0 || (now_ms - mc_last_check > MC_CHECK_INTERVAL_MS)) {
            mc_last_check = now_ms;
            xTaskCreate(mc_check_worker, "mc_check", 8192, NULL, 3, NULL);
        }
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
    lv_obj_set_style_text_font(mc_status_label, &lv_font_montserrat_28, 0);
    lv_obj_align(mc_status_label, LV_ALIGN_TOP_RIGHT, -16, 10);

    // ---- 7-Segment Clock (canvas) ----
    int ht = SEG_T / 2;
    int pad = ht;  // padding for outward-shifted vertical bars
    int clock_w = pad + SEG_DW + DIGIT_GAP + SEG_DW + COLON_GAP
                + COLON_DOT + COLON_GAP + SEG_DW + DIGIT_GAP + SEG_DW + pad;
    int clock_x = (LCD_H_RES - clock_w) / 2;
    int clock_y = 40;

    clock_canvas = lv_canvas_create(scr);
    uint32_t canvas_buf_size = LV_CANVAS_BUF_SIZE(clock_w, SEG_DH, 32, LV_DRAW_BUF_STRIDE_ALIGN);
    void *canvas_buf = heap_caps_malloc(canvas_buf_size, MALLOC_CAP_SPIRAM);
    lv_canvas_set_buffer(clock_canvas, canvas_buf, clock_w, SEG_DH, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(clock_canvas, clock_x, clock_y);

    // Initialize digits to dashes and draw
    for (int d = 0; d < CLOCK_DIGITS; d++) set_digit(d, -1);
    draw_clock();

    // ---- Sensor Values (below clock) ----
    int values_y = clock_y + SEG_DH + 15;

    // Temperature value (left)
    temp_value_label = lv_label_create(scr);
    lv_label_set_text(temp_value_label, "--.-C");
    lv_obj_set_style_text_color(temp_value_label, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(temp_value_label, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(temp_value_label, 40, values_y);

    // Pressure value (right)
    pres_value_label = lv_label_create(scr);
    lv_label_set_text(pres_value_label, "--- hPa");
    lv_obj_set_style_text_color(pres_value_label, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(pres_value_label, &lv_font_montserrat_48, 0);
    lv_obj_align(pres_value_label, LV_ALIGN_TOP_RIGHT, -40, values_y);

    // ---- 48h Charts ----
    int chart_y = values_y + 60;
    int chart_h = 240;
    int chart_w = 330;
    chart_y_pos = chart_y;
    chart_h_size = chart_h;

    // Temperature chart (left)
    temp_chart = lv_chart_create(scr);
    lv_obj_set_pos(temp_chart, 20, chart_y);
    lv_obj_set_size(temp_chart, chart_w, chart_h);
    lv_chart_set_type(temp_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(temp_chart, HA_CHART_POINTS);
    lv_chart_set_range(temp_chart, LV_CHART_AXIS_PRIMARY_Y, 100, 400);
    lv_chart_set_div_line_count(temp_chart, 5, 3);  // 3 vertical = -12h, -24h, -36h
    lv_obj_set_style_pad_top(temp_chart, CHART_PAD_V, 0);
    lv_obj_set_style_pad_bottom(temp_chart, CHART_PAD_V, 0);
    lv_obj_set_style_pad_left(temp_chart, 2, 0);
    lv_obj_set_style_pad_right(temp_chart, 2, 0);
    lv_obj_set_style_bg_color(temp_chart, lv_color_hex(0x262626), 0);
    lv_obj_set_style_border_color(temp_chart, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(temp_chart, 1, 0);
    lv_obj_set_style_radius(temp_chart, 8, 0);
    lv_obj_set_style_line_color(temp_chart, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_line_width(temp_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(temp_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(temp_chart, 0, LV_PART_INDICATOR);
    temp_series = lv_chart_add_series(temp_chart, lv_color_hex(0xff6d00), LV_CHART_AXIS_PRIMARY_Y);

    // Temperature grid labels
    for (int i = 0; i < MAX_GRID_LABELS; i++) {
        temp_grid_labels[i] = lv_label_create(scr);
        lv_label_set_text(temp_grid_labels[i], "");
        lv_obj_set_style_text_color(temp_grid_labels[i], lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(temp_grid_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_add_flag(temp_grid_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Pressure chart (right)
    int pres_chart_x = 20 + chart_w + 20;
    pres_chart = lv_chart_create(scr);
    lv_obj_set_pos(pres_chart, pres_chart_x, chart_y);
    lv_obj_set_size(pres_chart, chart_w, chart_h);
    lv_chart_set_type(pres_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(pres_chart, HA_CHART_POINTS);
    lv_chart_set_range(pres_chart, LV_CHART_AXIS_PRIMARY_Y, 94000, 106000);
    lv_chart_set_div_line_count(pres_chart, 5, 3);  // 3 vertical = -12h, -24h, -36h
    lv_obj_set_style_pad_top(pres_chart, CHART_PAD_V, 0);
    lv_obj_set_style_pad_bottom(pres_chart, CHART_PAD_V, 0);
    lv_obj_set_style_pad_left(pres_chart, 2, 0);
    lv_obj_set_style_pad_right(pres_chart, 2, 0);
    lv_obj_set_style_bg_color(pres_chart, lv_color_hex(0x262626), 0);
    lv_obj_set_style_border_color(pres_chart, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(pres_chart, 1, 0);
    lv_obj_set_style_radius(pres_chart, 8, 0);
    lv_obj_set_style_line_color(pres_chart, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_line_width(pres_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(pres_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(pres_chart, 0, LV_PART_INDICATOR);
    pres_series = lv_chart_add_series(pres_chart, lv_color_hex(0x42a5f5), LV_CHART_AXIS_PRIMARY_Y);

    // Pressure grid labels
    for (int i = 0; i < MAX_GRID_LABELS; i++) {
        pres_grid_labels[i] = lv_label_create(scr);
        lv_label_set_text(pres_grid_labels[i], "");
        lv_obj_set_style_text_color(pres_grid_labels[i], lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(pres_grid_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_add_flag(pres_grid_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    // ---- Toast Bar (overlay, hidden by default) ----
    toast_bar = lv_obj_create(scr);
    lv_obj_set_size(toast_bar, LCD_H_RES - 40, 70);
    lv_obj_align(toast_bar, LV_ALIGN_BOTTOM_MID, 0, -120);
    lv_obj_set_style_bg_color(toast_bar, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(toast_bar, LV_OPA_90, 0);
    lv_obj_set_style_border_width(toast_bar, 0, 0);
    lv_obj_set_style_radius(toast_bar, 12, 0);
    lv_obj_remove_flag(toast_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toast_bar, LV_OBJ_FLAG_HIDDEN);

    toast_label = lv_label_create(toast_bar);
    lv_label_set_text(toast_label, "");
    lv_obj_set_style_text_color(toast_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(toast_label, &lv_font_montserrat_22, 0);
    lv_label_set_long_mode(toast_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(toast_label, LCD_H_RES - 80);
    lv_obj_center(toast_label);

    // ---- Record Button (bottom) ----
    record_btn = lv_btn_create(scr);
    lv_obj_set_size(record_btn, LCD_H_RES - 40, 100);
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

    // Rotate 90 CW to compensate for physically rotated unit
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
