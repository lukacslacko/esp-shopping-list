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
#include "lwip/sockets.h"
#include "lwip/netdb.h"

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
    ESP_LOGI(TAG, "Adding task: '%s' to project '%s'", content, TODOIST_PROJECT_ID);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "content", content);
    cJSON_AddStringToObject(root, "project_id", TODOIST_PROJECT_ID);
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post_data) return false;

    ESP_LOGI(TAG, "Add task POST body: %s", post_data);

    http_buf_t resp;
    http_buf_init(&resp);

    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", TODOIST_API_TOKEN);

    esp_http_client_config_t cfg = {
        .url = "https://api.todoist.com/api/v1/tasks",
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

    if (err == ESP_OK && (status >= 200 && status < 300)) {
        ESP_LOGI(TAG, "Task added successfully (status=%d)", status);
        if (resp.data) ESP_LOGI(TAG, "Add task response: %s", resp.data);
        http_buf_free(&resp);
        return true;
    }
    ESP_LOGE(TAG, "Add task failed: err=%d status=%d", err, status);
    if (resp.data) ESP_LOGE(TAG, "Add task response: %s", resp.data);
    http_buf_free(&resp);
    return false;
}

char **api_todoist_get_tasks(int *out_count)
{
    *out_count = 0;

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.todoist.com/api/v1/tasks?project_id=%s", TODOIST_PROJECT_ID);

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
             "https://api.todoist.com/api/v1/tasks?project_id=%s", TODOIST_PROJECT_ID);

    ESP_LOGI(TAG, "Fetching tasks from: %s", url);

    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", TODOIST_API_TOKEN);

    http_buf_t resp;
    http_buf_init(&resp);
    if (!resp.data) {
        ESP_LOGE(TAG, "Failed to allocate HTTP response buffer");
        return NULL;
    }

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

    ESP_LOGI(TAG, "Performing GET request...");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "GET tasks result: err=%d status=%d resp_len=%d", err, status, (int)resp.len);

    if (err != ESP_OK || status != 200 || !resp.data) {
        ESP_LOGE(TAG, "Get tasks failed: err=%d status=%d", err, status);
        if (resp.data && resp.len > 0) ESP_LOGE(TAG, "Response body: %s", resp.data);
        http_buf_free(&resp);
        return NULL;
    }

    ESP_LOGI(TAG, "Tasks response (first 500 chars): %.500s", resp.data);

    cJSON *root = cJSON_Parse(resp.data);
    http_buf_free(&resp);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse tasks JSON");
        return NULL;
    }

    // v1 API wraps tasks in {"results": [...]}
    cJSON *arr = cJSON_GetObjectItem(root, "results");
    if (!arr) arr = root; // fallback if top-level is array
    if (!cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "Response has no 'results' array (type=%d)", arr ? arr->type : -1);
        cJSON_Delete(root);
        return NULL;
    }

    int count = cJSON_GetArraySize(arr);
    ESP_LOGI(TAG, "Parsed %d tasks from JSON", count);
    todoist_task_t *tasks = calloc(count, sizeof(todoist_task_t));
    int valid = 0;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *content = cJSON_GetObjectItem(item, "content");
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(content) && cJSON_IsString(id)) {
            tasks[valid].content = strdup(content->valuestring);
            tasks[valid].id = strdup(id->valuestring);
            ESP_LOGI(TAG, "  Task[%d]: id=%s content='%s'", valid, tasks[valid].id, tasks[valid].content);
            valid++;
        } else {
            ESP_LOGW(TAG, "  Task[%d]: skipped (content=%d id=%d)", i,
                     cJSON_IsString(content), cJSON_IsString(id));
        }
    }
    cJSON_Delete(root);
    *out_count = valid;
    ESP_LOGI(TAG, "Returning %d valid tasks", valid);
    return tasks;
}

bool api_todoist_complete_task(const char *task_id)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.todoist.com/api/v1/tasks/%s/close", task_id);

    ESP_LOGI(TAG, "Closing task: %s url: %s", task_id, url);

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

    if (err == ESP_OK && (status >= 200 && status < 300)) {
        ESP_LOGI(TAG, "Task %s completed (status=%d)", task_id, status);
        http_buf_free(&resp);
        return true;
    }
    ESP_LOGE(TAG, "Complete task failed: err=%d status=%d", err, status);
    if (resp.data) ESP_LOGE(TAG, "Response: %s", resp.data);
    http_buf_free(&resp);
    return false;
}

// ---- Minecraft Server List Ping ----

static int mc_write_varint(uint8_t *buf, int32_t value)
{
    int i = 0;
    uint32_t uval = (uint32_t)value;
    do {
        uint8_t b = uval & 0x7F;
        uval >>= 7;
        if (uval) b |= 0x80;
        buf[i++] = b;
    } while (uval);
    return i;
}

static int mc_read_varint(const uint8_t *buf, int len, int32_t *out)
{
    int32_t result = 0;
    int shift = 0;
    int i = 0;
    do {
        if (i >= len) return -1;
        uint8_t b = buf[i];
        result |= (int32_t)(b & 0x7F) << shift;
        shift += 7;
        i++;
        if (!(b & 0x80)) break;
    } while (shift < 35);
    *out = result;
    return i;
}

mc_server_status_t api_mc_server_status(const char *host, uint16_t port)
{
    mc_server_status_t status = {0};

    ESP_LOGI(TAG, "MC ping: %s:%d", host, port);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "MC: socket create failed");
        return status;
    }

    // Set timeout
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_aton(host, &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "MC: connect failed");
        close(sock);
        return status;
    }

    // Build handshake packet: [packet_id=0x00] [protocol=-1] [host string] [port] [next_state=1]
    uint8_t pkt[256];
    int host_len = strlen(host);
    int di = 0;
    // Packet ID
    di += mc_write_varint(pkt + di, 0x00);
    // Protocol version (-1 = unspecified)
    di += mc_write_varint(pkt + di, -1);
    // Server address (varint length + string)
    di += mc_write_varint(pkt + di, host_len);
    memcpy(pkt + di, host, host_len);
    di += host_len;
    // Port (big-endian unsigned short)
    pkt[di++] = (port >> 8) & 0xFF;
    pkt[di++] = port & 0xFF;
    // Next state (1 = status)
    di += mc_write_varint(pkt + di, 1);

    // Prepend length
    uint8_t frame[300];
    int li = mc_write_varint(frame, di);
    memcpy(frame + li, pkt, di);

    // Send handshake
    if (send(sock, frame, li + di, 0) < 0) {
        ESP_LOGE(TAG, "MC: send handshake failed");
        close(sock);
        return status;
    }

    // Send status request: length(1) + packet_id(0x00)
    uint8_t status_req[] = {0x01, 0x00};
    if (send(sock, status_req, 2, 0) < 0) {
        ESP_LOGE(TAG, "MC: send status request failed");
        close(sock);
        return status;
    }

    // Read response
    uint8_t resp[4096];
    int total = 0;
    while (total < (int)sizeof(resp) - 1) {
        int n = recv(sock, resp + total, sizeof(resp) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        // Try to parse to see if we have a complete packet
        if (total > 5) break; // We'll get more below if needed
    }

    close(sock);

    if (total < 5) {
        ESP_LOGW(TAG, "MC: short response (%d bytes)", total);
        return status;
    }

    // Parse: varint packet_length, varint packet_id, varint string_length, json string
    int off = 0;
    int32_t pkt_len;
    int r = mc_read_varint(resp, total, &pkt_len);
    if (r < 0) return status;
    off += r;

    int32_t pkt_id;
    r = mc_read_varint(resp + off, total - off, &pkt_id);
    if (r < 0) return status;
    off += r;

    int32_t str_len;
    r = mc_read_varint(resp + off, total - off, &str_len);
    if (r < 0) return status;
    off += r;

    // We may need to read more data for the full JSON string
    int needed = off + str_len;
    if (needed > (int)sizeof(resp) - 1) needed = sizeof(resp) - 1;

    // Re-open isn't needed; we may already have enough, but let's check
    // Actually the socket is closed. If we don't have enough data, truncate.
    if (total < needed) {
        ESP_LOGW(TAG, "MC: truncated response (have %d, need %d)", total, needed);
        str_len = total - off;
    }

    if (str_len <= 0) return status;

    // Null-terminate the JSON
    char *json_str = malloc(str_len + 1);
    if (!json_str) return status;
    memcpy(json_str, resp + off, str_len);
    json_str[str_len] = '\0';

    ESP_LOGI(TAG, "MC status JSON (first 300): %.300s", json_str);

    cJSON *json = cJSON_Parse(json_str);
    free(json_str);
    if (!json) {
        ESP_LOGE(TAG, "MC: JSON parse failed");
        return status;
    }

    status.online = true;

    cJSON *players = cJSON_GetObjectItem(json, "players");
    if (players) {
        cJSON *online = cJSON_GetObjectItem(players, "online");
        cJSON *max = cJSON_GetObjectItem(players, "max");
        if (cJSON_IsNumber(online)) status.players_online = online->valueint;
        if (cJSON_IsNumber(max)) status.players_max = max->valueint;
    }

    cJSON *desc = cJSON_GetObjectItem(json, "description");
    if (cJSON_IsString(desc)) {
        strncpy(status.motd, desc->valuestring, sizeof(status.motd) - 1);
    } else if (cJSON_IsObject(desc)) {
        cJSON *text = cJSON_GetObjectItem(desc, "text");
        if (cJSON_IsString(text)) {
            strncpy(status.motd, text->valuestring, sizeof(status.motd) - 1);
        }
    }

    ESP_LOGI(TAG, "MC: online=%d players=%d/%d motd='%s'",
             status.online, status.players_online, status.players_max, status.motd);

    cJSON_Delete(json);
    return status;
}
