// Controle simples da ventoinha pequena (2 fios, via MOSFET) pela temperatura
// (Heltec WiFi LoRa 32 V3).
//   temperatura > 24 °C  -> potência 100%
//   temperatura <= 24 °C -> potência 75%
//   sensor em falha      -> potência 100% (segurança)
// Indicacao do sensor: no console (sensor OK / FALHA SENSOR).
//
// Ligações:
//   MOSFET: comando na GPIO 7 (pino 18 do J3)
//   DS18B20: VDD -> 3V3 | GND -> GND | DQ -> GPIO 38 + resistor (4,7 kΩ ou 2 kΩ) para 3V3
//
// O componente components/fans/ (ventoinhas grandes) continua no projeto, só não é usado aqui.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "temperatura.h"
#include "ambiente.h"
#include "sdkconfig.h"

#define MOSFET_GPIO       GPIO_NUM_7          // pino 18 do J3 da Heltec
#define LOGICA_INVERTIDA  1                  // 1 se no seu circuito GPIO em 0 = fan ligada
#define SENSOR_GPIO       GPIO_NUM_38         // DQ do DS18B20 (pino 11 do J3)

#define LIMITE_C          24.0f               // acima disso: potência alta
#define HISTERESE_C       0.0f                // ex.: 0.5 -> só volta para 75% abaixo de 23,5 °C
#define POTENCIA_ALTA     100                 // %
#define POTENCIA_NORMAL   75                  // %

#define PWM_FREQ_HZ       50                  // baixa frequência: própria para fan de 2 fios
#define PWM_RESOLUCAO     LEDC_TIMER_14_BIT   // 14 bits: necessário para chegar a 50 Hz
#define PWM_TOTAL         (1 << 14)           // duty de 100%
#define PWM_TIMER         LEDC_TIMER_1        // timer 0 fica reservado para as fans grandes
#define PWM_CANAL         LEDC_CHANNEL_1

#define PARTIDA_MS        1000                // 100% na partida, para a fan vencer a inércia
#define CICLO_MS          100
#define DHT_INTERVALO_MS  2100

static const char *TAG = "app";

static void pwm_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = PWM_RESOLUCAO,
        .timer_num       = PWM_TIMER,
        .freq_hz         = PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t canal = {
        .gpio_num   = MOSFET_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = PWM_CANAL,
        .timer_sel  = PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .flags.output_invert = LOGICA_INVERTIDA,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&canal));
}

// Potência da ventoinha em % (0 a 100)
static void fan_potencia(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    uint32_t duty = (uint32_t)pct * PWM_TOTAL / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, PWM_CANAL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, PWM_CANAL));
}

void app_main(void)
{
    pwm_init();
    // Evita que o DHT11 reconfigure pinos utilizados pela aplicacao.
    ESP_ERROR_CHECK((CONFIG_IKNET_DHT11_GPIO == SENSOR_GPIO ||
                     CONFIG_IKNET_DHT11_GPIO == MOSFET_GPIO)
                    ? ESP_ERR_INVALID_ARG : ESP_OK);
    ESP_LOGI(TAG, "DS18B20: GPIO %d | DHT11: GPIO %d",
             SENSOR_GPIO, CONFIG_IKNET_DHT11_GPIO);

    temp_config_t tcfg = TEMP_CONFIG_PADRAO();
    tcfg.gpio = SENSOR_GPIO;
    tcfg.intervalo_ms = 100;
    tcfg.nova_busca_ciclos = 20;
    ESP_ERROR_CHECK(temp_init(&tcfg));

    // partida em 100% enquanto o sensor faz a primeira leitura
    fan_potencia(100);
    vTaskDelay(pdMS_TO_TICKS(PARTIDA_MS));

    TickType_t ultima_dht = xTaskGetTickCount();
    int potencia = POTENCIA_ALTA;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CICLO_MS));

        if (xTaskGetTickCount() - ultima_dht >= pdMS_TO_TICKS(DHT_INTERVALO_MS)) {
            ambiente_leitura_t ambiente;
            esp_err_t erro_dht = ambiente_ler(&ambiente);
            // Conta a partir do fim para nunca ler antes do intervalo minimo.
            ultima_dht = xTaskGetTickCount();
            if (erro_dht == ESP_OK) {
                ESP_LOGI(TAG, "DHT11: %.1f C | umidade: %.1f %%",
                         (double)ambiente.temperatura_c, (double)ambiente.umidade_pct);
            } else {
                ESP_LOGW(TAG, "DHT11: FALHA DE LEITURA (%s)", esp_err_to_name(erro_dht));
            }
        }
        temp_status_t t = { 0 };
        bool valida = temp_count() > 0 && temp_get_status(0, &t) == ESP_OK && t.valida;

        if (!valida) {
            potencia = POTENCIA_ALTA;                  // sem leitura: ventila no máximo
        } else if (t.celsius > LIMITE_C) {
            potencia = POTENCIA_ALTA;
        } else if (t.celsius <= LIMITE_C - HISTERESE_C) {
            potencia = POTENCIA_NORMAL;
        }                                              // dentro da histerese: mantém

        fan_potencia(potencia);


        if (valida) {
            ESP_LOGI(TAG, "%s: %6.2f C | sensor OK    | potência %3d%%",
                     t.nome, (double)t.celsius, potencia);
        } else if (temp_count() > 0) {
            ESP_LOGW(TAG, "%s:   ----   | FALHA SENSOR | potência %3d%% (segurança)",
                     t.nome ? t.nome : "S1", potencia);
        } else {
            ESP_LOGW(TAG, "nenhum sensor encontrado | FALHA SENSOR | potência %3d%% (segurança)",
                     potencia);
        }
    }
}
