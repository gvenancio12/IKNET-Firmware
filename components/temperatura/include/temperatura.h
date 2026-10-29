// Módulo "temperatura": sensores DS18B20 num barramento 1-Wire.
//
// - Todos os DS18B20 ficam no MESMO fio de dados (DQ), cada um com seu endereço único.
// - Uma tarefa interna lê todos os sensores periodicamente.
// - Cada sensor pode receber um nome (T01, T02...) pelo endereço; os que não estiverem
//   no mapa aparecem como S1, S2... e o endereço é mostrado no log para ser mapeado.
// - Sensor desconectado ou com leitura inválida fica "em falha" e é procurado de novo.
//
// Uso típico:
//     temp_config_t cfg = TEMP_CONFIG_PADRAO();
//     cfg.gpio = GPIO_NUM_38;
//     ESP_ERROR_CHECK(temp_init(&cfg));
//     temp_status_t st;
//     if (temp_get_por_nome("T01", &st) == ESP_OK && st.valida) { ... st.celsius ... }
//
// Ver components/temperatura/README.md para a documentação completa.
// Baseado no código de teste do DS18B20 de Raphael Rates (repositório temp-iknet).

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEMP_MAX_SENSORES 8

typedef enum {
    TEMP_FRIO,       // abaixo de limite_frio
    TEMP_NORMAL,     // de limite_frio até limite_atencao
    TEMP_ATENCAO,    // acima de limite_atencao até limite_critico
    TEMP_CRITICO,    // acima de limite_critico
    TEMP_FALHA,      // sensor ausente ou leitura inválida
} temp_nivel_t;

typedef struct {
    uint64_t    endereco;   // endereço 1-Wire do sensor (aparece no log ao ser encontrado)
    const char *nome;       // ex.: "T01"
} temp_mapa_t;

typedef struct {
    gpio_num_t  gpio;                       // fio de dados (DQ) do barramento
    int         intervalo_ms;               // periodo desejado; conversao de 100 ms por sensor em 9 bits
    int         nova_busca_ciclos;          // a cada quantos ciclos procura sensor faltando

    float       limite_frio;                // °C
    float       limite_atencao;             // °C
    float       limite_critico;             // °C

    int         n_mapa;                     // quantos itens em mapa[] (0 = sem mapa)
    temp_mapa_t mapa[TEMP_MAX_SENSORES];
} temp_config_t;

// Limites iguais aos do código original: frio < 21, normal 21–40, atenção 40–50, crítico > 50.
#define TEMP_CONFIG_PADRAO() {          \
    .gpio              = GPIO_NUM_NC,   \
    .intervalo_ms      = 2000,          \
    .nova_busca_ciclos = 5,             \
    .limite_frio       = 21.0f,         \
    .limite_atencao    = 40.0f,         \
    .limite_critico    = 50.0f,         \
    .n_mapa            = 0,             \
}

typedef struct {
    const char   *nome;
    uint64_t      endereco;
    bool          valida;     // false = sensor ausente ou leitura inválida
    float         celsius;    // última leitura válida (NAN se nunca leu)
    temp_nivel_t  nivel;
} temp_status_t;

// Configura o barramento, procura os sensores e inicia a tarefa de leitura. Chamar uma vez.
// Não falha se nenhum sensor for encontrado: ele continua procurando.
esp_err_t temp_init(const temp_config_t *cfg);

// Quantos sensores são conhecidos (os do mapa + os encontrados fora do mapa).
int temp_count(void);

// Estado do sensor idx (0 a temp_count()-1).
esp_err_t temp_get_status(int idx, temp_status_t *out);

// Estado do sensor pelo nome ("T01"...). ESP_ERR_NOT_FOUND se o nome não existe.
esp_err_t temp_get_por_nome(const char *nome, temp_status_t *out);

// Texto do nível: "FRIO", "NORMAL", "ATENCAO", "CRITICO", "FALHA SENSOR".
const char *temp_nivel_str(temp_nivel_t nivel);

#ifdef __cplusplus
}
#endif
