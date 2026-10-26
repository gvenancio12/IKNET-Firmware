// Implementação do módulo "temperatura". Ver include/temperatura.h e README.md.
// Baseado no código de teste do DS18B20 de Raphael Rates (repositório temp-iknet).

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "temperatura.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "onewire_bus.h"
#include "ds18b20.h"
#include "esp_check.h"
#include "esp_log.h"

#define TEMP_MIN_VALIDA   (-55.0f)   // faixa do DS18B20
#define TEMP_MAX_VALIDA   (125.0f)
#define TEMP_POWER_ON     (85.0f)    // valor que o DS18B20 devolve se a conversão não ocorreu
#define TAREFA_PILHA      4096
#define TAREFA_PRIORIDADE 4

static const char *TAG = "temperatura";

typedef struct {
    const char              *nome;
    char                     nome_auto[12];  // "S1", "S2"... para sensores fora do mapa
    uint64_t                 endereco;
    ds18b20_device_handle_t  handle;         // NULL = não encontrado no barramento
    bool                     valida;
    float                    celsius;
} sensor_t;

static temp_config_t        s_cfg;
static onewire_bus_handle_t s_bus;
static sensor_t             s_sens[TEMP_MAX_SENSORES];
static int                  s_n;             // sensores conhecidos
static int                  s_n_auto;        // contador para S1, S2...
static bool                 s_iniciado;
static portMUX_TYPE         s_lock = portMUX_INITIALIZER_UNLOCKED;  // protege s_sens e s_n

// ---------------------------------------------------------------- busca de sensores

static int indice_por_endereco(uint64_t end)
{
    for (int i = 0; i < s_n; i++) {
        if (s_sens[i].endereco == end) return i;
    }
    return -1;
}

// Procura todos os DS18B20 no barramento e associa cada um ao seu item da lista.
// Sensores do mapa que não aparecerem continuam na lista, sem handle (em falha).
static void procurar_sensores(void)
{
    onewire_device_iter_handle_t iter = NULL;
    if (onewire_new_device_iter(s_bus, &iter) != ESP_OK) return;

    onewire_device_t disp;
    int encontrados = 0;
    while (onewire_device_iter_get_next(iter, &disp) == ESP_OK) {
        encontrados++;
        int i = indice_por_endereco(disp.address);
        if (i >= 0 && s_sens[i].handle) continue;          // já conhecido e ativo

        if (i < 0) {                                       // sensor novo, fora do mapa
            if (s_n >= TEMP_MAX_SENSORES) {
                ESP_LOGW(TAG, "limite de %d sensores atingido", TEMP_MAX_SENSORES);
                continue;
            }
            i = s_n;
            sensor_t *s = &s_sens[i];
            memset(s, 0, sizeof(*s));
            s_n_auto++;   // nunca passa de TEMP_MAX_SENSORES; o cast deixa isso claro p/ o compilador
            snprintf(s->nome_auto, sizeof(s->nome_auto), "S%u", (unsigned)(uint8_t)s_n_auto);
            s->nome     = s->nome_auto;
            s->endereco = disp.address;
            s->celsius  = NAN;
            taskENTER_CRITICAL(&s_lock);
            s_n++;
            taskEXIT_CRITICAL(&s_lock);
        }

        const ds18b20_config_t ds_cfg = {};   // struct vazia no driver 0.4.0
        ds18b20_device_handle_t h = NULL;
        if (ds18b20_new_device_from_enumeration(&disp, &ds_cfg, &h) == ESP_OK) {
            s_sens[i].handle = h;
            ESP_LOGI(TAG, "%s encontrado, endereço %016llX",
                     s_sens[i].nome, (unsigned long long)disp.address);
        }
        // ESP_ERR_NOT_SUPPORTED = outro tipo de dispositivo 1-Wire: ignorado
    }
    onewire_del_device_iter(iter);

    if (encontrados == 0) {
        ESP_LOGW(TAG, "nenhum sensor no barramento; confira DQ, 3V3, GND e o resistor de pull-up");
    }
    for (int i = 0; i < s_n; i++) {
        if (!s_sens[i].handle) {
            ESP_LOGW(TAG, "%s (%016llX) não encontrado", s_sens[i].nome,
                     (unsigned long long)s_sens[i].endereco);
        }
    }
}

static void marcar_ausente(sensor_t *s, const char *motivo)
{
    if (s->handle) {
        ds18b20_del_device(s->handle);
        s->handle = NULL;
    }
    bool estava_ok;
    taskENTER_CRITICAL(&s_lock);
    estava_ok = s->valida;
    s->valida = false;
    taskEXIT_CRITICAL(&s_lock);
    // avisa só na transição de ok para falha, para não encher o log
    if (estava_ok) ESP_LOGW(TAG, "%s: FALHA SENSOR (%s)", s->nome, motivo);
}

// ---------------------------------------------------------------- tarefa de leitura

static void tarefa_temperatura(void *arg)
{
    (void)arg;
    int ciclos = 0;

    while (1) {
        // procura sensores que faltam: na partida e a cada nova_busca_ciclos
        bool falta = (s_n == 0);
        for (int i = 0; i < s_n; i++) falta |= (s_sens[i].handle == NULL);
        if (falta && (ciclos % s_cfg.nova_busca_ciclos) == 0) {
            procurar_sensores();
        }
        ciclos++;

        // uma conversão para todos ao mesmo tempo (~800 ms em 12 bits)
        bool algum = false;
        for (int i = 0; i < s_n; i++) algum |= (s_sens[i].handle != NULL);
        esp_err_t erro = algum ? ds18b20_trigger_temperature_conversion_for_all(s_bus)
                               : ESP_ERR_NOT_FOUND;

        for (int i = 0; i < s_n; i++) {
            sensor_t *s = &s_sens[i];
            if (!s->handle) {
                marcar_ausente(s, "não encontrado no barramento");
                continue;
            }
            float t = NAN;
            esp_err_t e = (erro == ESP_OK) ? ds18b20_get_temperature(s->handle, &t) : erro;

            bool ok = e == ESP_OK && isfinite(t) && t >= TEMP_MIN_VALIDA && t <= TEMP_MAX_VALIDA
                      && t != TEMP_POWER_ON;
            if (!ok) {
                marcar_ausente(s, e != ESP_OK ? esp_err_to_name(e) : "fora da faixa");
                continue;
            }

            bool voltou;
            taskENTER_CRITICAL(&s_lock);
            voltou     = !s->valida && !isnan(s->celsius);
            s->celsius = t;
            s->valida  = true;
            taskEXIT_CRITICAL(&s_lock);
            if (voltou) ESP_LOGW(TAG, "%s: leitura recuperada, %.2f C", s->nome, (double)t);
        }

        vTaskDelay(pdMS_TO_TICKS(s_cfg.intervalo_ms > 800 ? s_cfg.intervalo_ms - 800 : 0));
    }
}

// ---------------------------------------------------------------- API pública

esp_err_t temp_init(const temp_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(!s_iniciado, ESP_ERR_INVALID_STATE, TAG, "temp_init já foi chamado");
    ESP_RETURN_ON_FALSE(cfg && cfg->gpio != GPIO_NUM_NC, ESP_ERR_INVALID_ARG, TAG, "gpio");
    ESP_RETURN_ON_FALSE(cfg->n_mapa >= 0 && cfg->n_mapa <= TEMP_MAX_SENSORES,
                        ESP_ERR_INVALID_ARG, TAG, "n_mapa");
    s_cfg = *cfg;
    if (s_cfg.nova_busca_ciclos < 1) s_cfg.nova_busca_ciclos = 1;

    // sensores do mapa entram na lista desde já (em falha até serem encontrados)
    for (int i = 0; i < s_cfg.n_mapa; i++) {
        s_sens[i] = (sensor_t){ .nome = s_cfg.mapa[i].nome,
                                .endereco = s_cfg.mapa[i].endereco, .celsius = NAN };
    }
    s_n = s_cfg.n_mapa;

    const onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = cfg->gpio,
        .flags = { .en_pull_up = 1 },   // reforço; o resistor externo continua necessário
    };
    const onewire_bus_rmt_config_t rmt_cfg = { .max_rx_bytes = 10 };
    ESP_RETURN_ON_ERROR(onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_bus), TAG, "barramento 1-Wire");

    BaseType_t ok = xTaskCreate(tarefa_temperatura, "temperatura", TAREFA_PILHA, NULL,
                                TAREFA_PRIORIDADE, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "tarefa");

    s_iniciado = true;
    ESP_LOGI(TAG, "barramento 1-Wire na GPIO %d, %d sensor(es) no mapa", cfg->gpio, cfg->n_mapa);
    return ESP_OK;
}

int temp_count(void)
{
    taskENTER_CRITICAL(&s_lock);
    int n = s_n;
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

static temp_nivel_t classificar(float t)
{
    if (t < s_cfg.limite_frio)     return TEMP_FRIO;
    if (t <= s_cfg.limite_atencao) return TEMP_NORMAL;
    if (t <= s_cfg.limite_critico) return TEMP_ATENCAO;
    return TEMP_CRITICO;
}

esp_err_t temp_get_status(int idx, temp_status_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out");
    taskENTER_CRITICAL(&s_lock);
    bool existe = idx >= 0 && idx < s_n;
    if (existe) {
        out->nome     = s_sens[idx].nome;
        out->endereco = s_sens[idx].endereco;
        out->valida   = s_sens[idx].valida;
        out->celsius  = s_sens[idx].celsius;
    }
    taskEXIT_CRITICAL(&s_lock);
    ESP_RETURN_ON_FALSE(existe, ESP_ERR_INVALID_ARG, TAG, "idx");
    out->nivel = out->valida ? classificar(out->celsius) : TEMP_FALHA;
    return ESP_OK;
}

esp_err_t temp_get_por_nome(const char *nome, temp_status_t *out)
{
    ESP_RETURN_ON_FALSE(nome && out, ESP_ERR_INVALID_ARG, TAG, "nome");
    int n = temp_count();
    for (int i = 0; i < n; i++) {
        if (s_sens[i].nome && strcmp(s_sens[i].nome, nome) == 0) {
            return temp_get_status(i, out);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

const char *temp_nivel_str(temp_nivel_t nivel)
{
    switch (nivel) {
    case TEMP_FRIO:    return "FRIO";
    case TEMP_NORMAL:  return "NORMAL";
    case TEMP_ATENCAO: return "ATENCAO";
    case TEMP_CRITICO: return "CRITICO";
    default:           return "FALHA SENSOR";
    }
}
