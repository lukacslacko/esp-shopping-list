#include "api.h"
#include "secrets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"

static const char *TAG = "api";

// HTTP response buffer (dynamic)
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buf_t *buf = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (buf->len + evt->data_len >= buf->cap) {
            size_t new_cap = buf->cap + evt->data_len + 1024;
            char *new_data = heap_caps_realloc(buf->data, new_cap, MALLOC_CAP_SPIRAM);
            if (!new_data) {
                ESP_LOGE(TAG, "Failed to realloc HTTP buffer");
                return ESP_FAIL;
            }
            buf->data = new_data;
            buf->cap = new_cap;
        }
        memcpy(buf->data + buf->len, evt->data, evt->data_len);
        buf->len += evt->data_len;
        buf->data[buf->len] = '\0';
    }
    return ESP_OK;
}

static void http_buf_init(http_buf_t *buf)
{
    buf->cap = 4096;
    buf->data = heap_caps_malloc(buf->cap, MALLOC_CAP_SPIRAM);
    buf->len = 0;
    if (buf->data) buf->data[0] = '\0';
}

static void http_buf_free(http_buf_t *buf)
{
    if (buf->data) heap_caps_free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

// ---- Google Cloud STT ----

char *api_google_stt(const int16_t *audio_samples, size_t sample_count)
{
    if (!audio_samples || sample_count == 0) return NULL;

    size_t raw_len = sample_count * sizeof(int16_t);

    // Base64 encode
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, (const unsigned char *)audio_samples, raw_len);
    char *b64_buf = heap_caps_malloc(b64_len + 1, MALLOC_CAP_SPIRAM);
    if (!b64_buf) {
        ESP_LOGE(TAG, "Failed to alloc base64 buffer (%d bytes)", (int)b64_len);
        return NULL;
    }
    mbedtls_base64_encode((unsigned char *)b64_buf, b64_len + 1, &b64_len,
                          (const unsigned char *)audio_samples, raw_len);
    b64_buf[b64_len] = '\0';

    // Build JSON request
    cJSON *root = cJSON_CreateObject();
    cJSON *config = cJSON_AddObjectToObject(root, "config");
    cJSON_AddStringToObject(config, "encoding", "LINEAR16");
    cJSON_AddNumberToObject(config, "sampleRateHertz", 16000);
    cJSON_AddStringToObject(config, "languageCode", GOOGLE_STT_LANG);
    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    // Use raw reference to avoid cJSON duplicating the huge base64 string
    cJSON *b64_item = cJSON_CreateStringReference(b64_buf);
    cJSON_AddItemToObject(audio, "content", b64_item);

    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    heap_caps_free(b64_buf);

    if (!post_data) {
        ESP_LOGE(TAG, "Failed to serialize STT JSON");
        return NULL;
    }

    ESP_LOGI(TAG, "STT request size: %d bytes", (int)strlen(post_data));

    // Build URL
    char url[256];
    snprintf(url, sizeof(url),
             "https://speech.googleapis.com/v1/speech:recognize?key=%s", GOOGLE_API_KEY);

    http_buf_t resp;
    http_buf_init(&resp);
    if (!resp.data) {
        free(post_data);
        return NULL;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    char *transcript = NULL;
    if (err == ESP_OK && status == 200 && resp.data) {
        ESP_LOGI(TAG, "STT response: %s", resp.data);
        cJSON *json = cJSON_Parse(resp.data);
        if (json) {
            cJSON *results = cJSON_GetObjectItem(json, "results");
            if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
                cJSON *first = cJSON_GetArrayItem(results, 0);
                cJSON *alternatives = cJSON_GetObjectItem(first, "alternatives");
                if (cJSON_IsArray(alternatives) && cJSON_GetArraySize(alternatives) > 0) {
                    cJSON *alt = cJSON_GetArrayItem(alternatives, 0);
                    cJSON *t = cJSON_GetObjectItem(alt, "transcript");
                    if (cJSON_IsString(t)) {
                        transcript = strdup(t->valuestring);
                        ESP_LOGI(TAG, "Transcript: %s", transcript);
                    }
                }
            }
            cJSON_Delete(json);
        }
    } else {
        ESP_LOGE(TAG, "STT HTTP error: err=%d status=%d", err, status);
        if (resp.data) ESP_LOGE(TAG, "Response: %s", resp.data);
    }

    http_buf_free(&resp);
    return transcript;
}

// ---- Todoist API ----

bool api_todoist_add_task(const char *content)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "content", content);
    cJSON_AddStringToObject(root, "project_id", TODOIST_PROJECT_ID);
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post_data) return false;

    http_buf_t resp;
    http_buf_init(&resp);

    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", TODOIST_API_TOKEN);

    esp_http_client_config_t cfg = {
        .url = "https://api.todoist.com/rest/v2/tasks",
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);
    http_buf_free(&resp);

    if (err == ESP_OK && (status == 200 || status == 204)) {
        ESP_LOGI(TAG, "Task added successfully");
        return true;
    }
    ESP_LOGE(TAG, "Add task failed: err=%d status=%d", err, status);
    return false;
}

char **api_todoist_get_tasks(int *out_count)
{
    *out_count = 0;

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.todoist.com/rest/v2/tasks?project_id=%s", TODOIST_PROJECT_ID);

    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", TODOIST_API_TOKEN);

    http_buf_t resp;
    http_buf_init(&resp);
    if (!resp.data) return NULL;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || !resp.data) {
        ESP_LOGE(TAG, "Get tasks failed: err=%d status=%d", err, status);
        http_buf_free(&resp);
        return NULL;
    }

    cJSON *arr = cJSON_Parse(resp.data);
    http_buf_free(&resp);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }

    int count = cJSON_GetArraySize(arr);
    char **tasks = calloc(count, sizeof(char *));
    int valid = 0;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *content = cJSON_GetObjectItem(item, "content");
        if (cJSON_IsString(content)) {
            tasks[valid++] = strdup(content->valuestring);
        }
    }
    cJSON_Delete(arr);
    *out_count = valid;
    return tasks;
}

todoist_task_t *api_todoist_get_tasks_with_ids(int *out_count)
{
    *out_count = 0;

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.todoist.com/rest/v2/tasks?project_id=%s", TODOIST_PROJECT_ID);

    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", TODOIST_API_TOKEN);

    http_buf_t resp;
    http_buf_init(&resp);
    if (!resp.data) return NULL;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || !resp.data) {
        ESP_LOGE(TAG, "Get tasks failed: err=%d status=%d", err, status);
        http_buf_free(&resp);
        return NULL;
    }

    cJSON *arr = cJSON_Parse(resp.data);
    http_buf_free(&resp);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }

    int count = cJSON_GetArraySize(arr);
    todoist_task_t *tasks = calloc(count, sizeof(todoist_task_t));
    int valid = 0;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *content = cJSON_GetObjectItem(item, "content");
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(content) && cJSON_IsString(id)) {
            tasks[valid].content = strdup(content->valuestring);
            tasks[valid].id = strdup(id->valuestring);
            valid++;
        }
    }
    cJSON_Delete(arr);
    *out_count = valid;
    return tasks;
}

bool api_todoist_complete_task(const char *task_id)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.todoist.com/rest/v2/tasks/%s/close", task_id);

    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", TODOIST_API_TOKEN);

    http_buf_t resp;
    http_buf_init(&resp);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    http_buf_free(&resp);

    if (err == ESP_OK && (status == 200 || status == 204)) {
        ESP_LOGI(TAG, "Task %s completed", task_id);
        return true;
    }
    ESP_LOGE(TAG, "Complete task failed: err=%d status=%d", err, status);
    return false;
}
