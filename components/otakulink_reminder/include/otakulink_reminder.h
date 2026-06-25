#ifndef OTAKULINK_REMINDER_H
#define OTAKULINK_REMINDER_H

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

void otakulink_reminder_init(void);
void otakulink_reminder_tick(void);
bool otakulink_reminder_try_create_from_text(const char *text);
bool otakulink_reminder_handle_cron_task(cJSON *data);

#ifdef __cplusplus
}
#endif

#endif
