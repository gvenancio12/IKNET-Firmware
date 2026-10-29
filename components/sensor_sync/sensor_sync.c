#include "sensor_sync.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t mutex;

esp_err_t sensor_sync_init(void)
{
    if (!mutex) mutex = xSemaphoreCreateMutex();
    return mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

void sensor_sync_lock(void)
{
    configASSERT(mutex);
    xSemaphoreTake(mutex, portMAX_DELAY);
}

void sensor_sync_unlock(void)
{
    xSemaphoreGive(mutex);
}