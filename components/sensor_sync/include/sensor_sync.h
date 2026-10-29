#pragma once
#include "esp_err.h"

// Inicializar antes de criar as tarefas dos sensores.
esp_err_t sensor_sync_init(void);
void sensor_sync_lock(void);
void sensor_sync_unlock(void);