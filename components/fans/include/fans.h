// Módulo "fans": controle das ventoinhas externas de 4 fios (12 V, PWM + tacômetro).
//
// - Um único sinal PWM de 25 kHz comanda TODAS as ventoinhas ao mesmo tempo
//   (fios azuis ligados juntos): elas sempre recebem a mesma velocidade pedida.
// - Cada ventoinha tem seu próprio tacômetro (fio amarelo), lido por uma unidade PCNT.
// - Uma tarefa interna mede o RPM a cada segundo e verifica falha em cada ventoinha,
//   de forma independente.
//
// Uso típico:
//     fans_config_t cfg = FANS_CONFIG_PADRAO();
//     cfg.pwm_gpio = GPIO_NUM_5;
//     cfg.n_fans = 2;
//     cfg.tach_gpio[0] = GPIO_NUM_6;  cfg.nome[0] = "FAN1";
//     cfg.tach_gpio[1] = GPIO_NUM_4;  cfg.nome[1] = "FAN2";
//     ESP_ERROR_CHECK(fans_init(&cfg));
//     fans_set_percent(60);
//
// Ver components/fans/README.md para a documentação completa.

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FANS_MAX 4   // máximo de ventoinhas (limite das 4 unidades PCNT do ESP32-S3)

// Chamado pela tarefa interna quando uma ventoinha ENTRA ou SAI do estado de falha.
// Roda na tarefa "fans": deve ser rápido e não pode bloquear.
typedef void (*fans_evento_cb_t)(int idx, const char *nome, bool em_falha, int rpm, void *ctx);

typedef struct {
    gpio_num_t   pwm_gpio;                 // saída PWM comum (fios azuis)
    int          n_fans;                   // quantas ventoinhas (1 a FANS_MAX)
    gpio_num_t   tach_gpio[FANS_MAX];      // entrada do tacômetro de cada uma (fios amarelos)
    const char  *nome[FANS_MAX];           // nome usado nos logs (opcional)

    // Detecção de falha: com PWM >= falha_pwm_min e passados acomodacao_s segundos
    // desde a última mudança de PWM, RPM < falha_rpm_min por falha_leituras
    // medições seguidas = falha. Volta ao normal quando RPM >= falha_rpm_min.
    int          falha_pwm_min;            // %
    int          falha_rpm_min;            // RPM
    int          falha_leituras;           // medições seguidas
    int          acomodacao_s;             // segundos

    fans_evento_cb_t on_evento;            // opcional (NULL = só log)
    void        *ctx;                      // repassado ao callback
} fans_config_t;

// Valores padrão validados no teste com as AVC 120 mm (≈2800 RPM a 100%).
#define FANS_CONFIG_PADRAO() {          \
    .pwm_gpio       = GPIO_NUM_NC,      \
    .n_fans         = 0,                \
    .falha_pwm_min  = 30,               \
    .falha_rpm_min  = 300,              \
    .falha_leituras = 3,                \
    .acomodacao_s   = 3,                \
    .on_evento      = NULL,             \
    .ctx            = NULL,             \
}

typedef struct {
    const char *nome;
    int         pwm_pct;    // velocidade pedida (%)
    int         rpm;        // última medição
    bool        em_falha;
} fan_status_t;

// Configura PWM, tacômetros e inicia a tarefa de medição. Chamar uma vez.
// As ventoinhas começam com PWM em 0%.
esp_err_t fans_init(const fans_config_t *cfg);

// Define a velocidade de todas as ventoinhas, em % (valores fora de 0..100 são limitados).
// Pode ser chamada de qualquer tarefa.
esp_err_t fans_set_percent(int pct);

// Velocidade pedida atualmente (%).
int fans_get_percent(void);

// Quantas ventoinhas foram configuradas.
int fans_count(void);

// Estado de uma ventoinha (idx de 0 a fans_count()-1).
esp_err_t fans_get_status(int idx, fan_status_t *out);

// true se ao menos uma ventoinha está em falha.
bool fans_alguma_falha(void);

// ---------------------------------------------------------------- rotina de teste

#define FANS_TESTE_DEGRAUS 4   // degraus de PWM testados: 100, 75, 50 e 30%

typedef struct {
    const char *nome;
    bool        ok;
    int         rpm[FANS_TESTE_DEGRAUS];   // RPM médio em cada degrau
    const char *motivo;                    // NULL se ok; senão, o que falhou
} fan_teste_t;

typedef struct {
    int         pwm[FANS_TESTE_DEGRAUS];   // PWM de cada degrau (%)
    int         n_fans;
    fan_teste_t fan[FANS_MAX];
    bool        todas_ok;
} fans_teste_resultado_t;

// Testa todas as ventoinhas: passa por 100, 75, 50 e 30% (~28 s no total), mede o RPM
// médio de cada uma em cada degrau e verifica, para cada ventoinha:
//   1. gira em todos os degraus (RPM >= falha_rpm_min);
//   2. responde ao PWM (RPM em 100% >= 1,5x o RPM em 30%);
//   3. o RPM cai quando o PWM baixa.
// O resultado sai no log e, se res != NULL, também em *res.
// BLOQUEIA a tarefa que chamou até o fim do teste. Não chamar de dentro do callback.
// Pedidos de fans_set_percent durante o teste são guardados e aplicados no final;
// sem pedido novo, volta ao PWM que estava antes do teste.
// Retorna ESP_OK se todas passaram, ESP_FAIL se alguma reprovou.
esp_err_t rotina_teste_fan(fans_teste_resultado_t *res);

#ifdef __cplusplus
}
#endif
