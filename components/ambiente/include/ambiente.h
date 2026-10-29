#pragma once

#include "esp_err.h"

// Leitura independente do DHT11. Chamar com intervalo de pelo menos 2 segundos.
typedef struct {
    float temperatura_c;
    float umidade_pct;
} ambiente_leitura_t;

// Chamar sensor_sync_init() antes do primeiro uso (temp_init ja o faz).
// Em caso de erro, os valores retornados sao NAN.
esp_err_t ambiente_ler(ambiente_leitura_t *leitura);