# ESP Shopping List - Build Plan

## Current Status
All source files are created and ready. The project needs to be built and any compilation errors fixed.

## Files Created

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Project name: `esp_shopping_list` |
| `partitions.csv` | 8MB app partition |
| `sdkconfig.defaults` | PSRAM, HTTPS, cert bundle, esp32p4 target |
| `.gitignore` | Excludes build/, secrets.h, sdkconfig |
| `main/CMakeLists.txt` | Registers `main.c` and `api.c` |
| `main/idf_component.yml` | LVGL, BSP, codec, wifi_remote deps (from reference repo) |
| `main/secrets.h.example` | Template with WIFI_SSID, WIFI_PASS, GOOGLE_API_KEY, GOOGLE_STT_LANG, TODOIST_API_TOKEN, TODOIST_PROJECT_ID |
| `main/secrets.h` | Placeholder copy (gitignored) - user must fill in real values |
| `main/api.h` | Declares: `api_google_stt()`, `api_todoist_add_task()`, `api_todoist_get_tasks()`, `api_todoist_complete_task()`, `api_todoist_get_tasks_with_ids()` |
| `main/api.c` | HTTP client code: Google Cloud STT, Todoist REST v2 (add/get/complete tasks), base64 encoding, JSON parsing via cJSON |
| `main/main.c` | WiFi+NTP init, LVGL UI (800x800), mic recording, touch hold-to-speak button, orchestration |

## Next Steps

1. **Set target**: `idf.py set-target esp32p4`
2. **Build**: `idf.py build`
3. **Fix any compilation errors** — likely candidates:
   - Missing includes or API mismatches with specific ESP-IDF version
   - LVGL symbol names (e.g. `LV_SYMBOL_AUDIO` may not exist in LVGL 9 — replace with `LV_SYMBOL_VOLUME_MAX` or a text string)
   - `lv_font_montserrat_22` / `lv_font_montserrat_28` may need enabling in sdkconfig or replacing with available sizes (14, 16, 18, 20, 24 are commonly enabled)
   - `esp_codec_dev_sample_info_t` struct field names may differ
   - BSP function signatures may differ
4. **Flash & test**: `idf.py flash monitor`

## Architecture Overview

### UI Layout (800x800 single screen)
```
+------------------------------------------+
|  [WiFi dot]  Shopping List     14:35     | 60px header
+------------------------------------------+
|                                          |
|  Scrollable list of items (tap to done)  | ~560px
|  [checkmark] Milk                        |
|  [checkmark] Bread                       |
|                                          |
+------------------------------------------+
|  "Ready" / "Recording..." / "Adding..."  | 40px status
+------------------------------------------+
|  [  HOLD TO SPEAK  ]  (large button)     | 140px
+------------------------------------------+
```

### Data Flow
1. Touch & hold RECORD → `LV_EVENT_PRESSED` → `is_recording = true`, audio task fills PSRAM buffer at 16kHz/16-bit/mono
2. Release → `LV_EVENT_RELEASED` → `is_recording = false`, copy buffer
3. FreeRTOS task: base64-encode audio → POST to Google Cloud STT → parse transcript
4. POST transcript to Todoist REST v2 → GET updated task list → refresh UI
5. Tap a list item → POST close task → refresh list

### Key Design Decisions
- All HTTP runs in FreeRTOS tasks, never in LVGL thread
- UI updates use `bsp_display_lock()/unlock()`
- Recording buffer allocated in PSRAM (`heap_caps_malloc` with `MALLOC_CAP_SPIRAM`)
- WiFi reconnects automatically via event handler
- Tasks fetched with IDs so they can be completed by tapping

## Reference Repo
The code was adapted from `lukacslacko/esp32` (cloned to `C:\Users\lukac\Documents\esp\esp32-ref`). Key patterns reused:
- WiFi init (SDIO to C6 chip)
- NTP/SNTP setup
- Mic init via `bsp_audio_codec_microphone_init()` + `esp_codec_dev_open()`
- Audio task reading mic in chunks of 256 samples
- LVGL init via `bsp_display_start()` / `bsp_display_lock()`
- Same component versions in `idf_component.yml`

## GitHub
- Repo created at: https://github.com/lukacslacko/esp-shopping-list
- Not yet pushed (no initial commit yet)
