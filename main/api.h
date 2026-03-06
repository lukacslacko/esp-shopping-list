#ifndef API_H
#define API_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Send audio to Google Cloud STT. Returns transcript (caller must free), or NULL on error.
char *api_google_stt(const int16_t *audio_samples, size_t sample_count);

// Add a task to the Todoist project. Returns true on success.
bool api_todoist_add_task(const char *content);

// Fetch all tasks from the Todoist project. Returns a malloc'd array of strings (caller must free each + the array).
// Sets *out_count to the number of tasks.
char **api_todoist_get_tasks(int *out_count);

// Complete (close) a Todoist task by its ID. Returns true on success.
bool api_todoist_complete_task(const char *task_id);

// Task item with id and content for completing tasks
typedef struct {
    char *id;
    char *content;
} todoist_task_t;

// Fetch tasks with IDs. Returns malloc'd array (caller must free each id/content + the array).
todoist_task_t *api_todoist_get_tasks_with_ids(int *out_count);

#endif // API_H
