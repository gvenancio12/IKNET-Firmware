// Implementação do módulo "fans". Ver include/fans.h e README.md.

#include <stdio.h>
#include <string.h>
#include "fans.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_log.h"

#define PWM_FREQ_HZ       25000             // 25 kHz: padrão Intel para fans de 4 fios
#define PWM_RESOLUCAO     LEDC_TIMER_10_BIT // 0 a 1023
#define PWM_MAX           ((1 << 10) - 1)
#define PWM_MODO          LEDC_LOW_SPEED_MODE
#define PWM_TIMER         LEDC_TIMER_0
#define PWM_CANAL         LEDC_CHANNEL_0
#define PULSOS_POR_VOLTA  2                 // fans de PC geram 2 pulsos por volta
#define JANELA_MS         1000              // tempo de contagem de cada medição
#define FILTRO_RUIDO_NS   10000             // ignora pulsos < 10 µs no tacômetro

// Rotina de teste (rotina_teste_fan)
#define TESTE_ACOMODACAO_MS 4000            // espera a fan estabilizar em cada degrau
#define TESTE_LEITURAS      3               // medições (1 s cada) para a média do degrau
#define TESTE_RAZAO_MIN     1.5f            // RPM em 100% deve ser >= 1,5x o RPM em 30%
#define TESTE_TOLERANCIA    0.05f           // folga de 5% na checagem "RPM cai com o PWM"

#define TAREFA_PILHA      3072
#define TAREFA_PRIORIDADE 5

static const char *TAG = "fans";

typedef struct {
    const char         *nome;
    pcnt_unit_handle_t  tach;
    int                 rpm;
    int                 s_desde_mudanca;
    int                 leituras_ruins;
    bool                em_falha;
} fan_t;

static fans_config_t s_cfg;
static fan_t         s_fans[FANS_MAX];
static int           s_pwm_pct;     // PWM aplicado agora
static int           s_pct_pedido;  // último valor pedido por fans_set_percent
static bool          s_em_teste;    // rotina_teste_fan em andamento
static bool          s_iniciado;
static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;  // protege s_fans e s_pwm_pct

static const char *NOMES_PADRAO[FANS_MAX] = { "FAN1", "FAN2", "FAN3", "FAN4" };

// ---------------------------------------------------------------- inicialização

static esp_err_t pwm_init(gpio_num_t gpio)
{
    ledc_timer_config_t timer = {
        .speed_mode      = PWM_MODO,
        .duty_resolution = PWM_RESOLUCAO,
        .timer_num       = PWM_TIMER,
        .freq_hz         = PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "timer PWM");

    ledc_channel_config_t canal = {
        .gpio_num   = gpio,
        .speed_mode = PWM_MODO,
        .channel    = PWM_CANAL,
        .timer_sel  = PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    return ledc_channel_config(&canal);
}

static esp_err_t tach_init(fan_t *f, gpio_num_t gpio)
{
    pcnt_unit_config_t unit_cfg = { .low_limit = -32768, .high_limit = 32767 };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, &f->tach), TAG, "PCNT de %s", f->nome);

    pcnt_glitch_filter_config_t filtro = { .max_glitch_ns = FILTRO_RUIDO_NS };
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(f->tach, &filtro), TAG, "filtro");

    pcnt_chan_config_t chan_cfg = { .edge_gpio_num = gpio, .level_gpio_num = -1 };
    pcnt_channel_handle_t ch;
    ESP_RETURN_ON_ERROR(pcnt_new_channel(f->tach, &chan_cfg, &ch), TAG, "canal PCNT");
    // conta cada borda de descida (o tacômetro puxa o sinal para 0 V)
    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(ch,
                        PCNT_CHANNEL_EDGE_ACTION_HOLD, PCNT_CHANNEL_EDGE_ACTION_INCREASE),
                        TAG, "borda");

    ESP_RETURN_ON_ERROR(pcnt_unit_enable(f->tach), TAG, "enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(f->tach), TAG, "clear");
    return pcnt_unit_start(f->tach);
}

// ---------------------------------------------------------------- medição e falha

// Atualiza o estado de falha. Retorna +1 se entrou em falha, -1 se saiu, 0 se nada mudou.
// Chamar com s_lock travado.
static int verificar_falha(fan_t *f, int pwm_pct)
{
    bool deveria_girar = pwm_pct >= s_cfg.falha_pwm_min &&
                         f->s_desde_mudanca > s_cfg.acomodacao_s;

    if (deveria_girar && f->rpm < s_cfg.falha_rpm_min) {
        if (f->leituras_ruins < s_cfg.falha_leituras) f->leituras_ruins++;
        if (f->leituras_ruins >= s_cfg.falha_leituras && !f->em_falha) {
            f->em_falha = true;
            return +1;
        }
    } else if (f->rpm >= s_cfg.falha_rpm_min) {
        f->leituras_ruins = 0;
        if (f->em_falha) {
            f->em_falha = false;
            return -1;
        }
    }
    return 0;
}

static void tarefa_fans(void *arg)
{
    (void)arg;
    const int n = s_cfg.n_fans;
    while (1) {
        for (int i = 0; i < n; i++) {
            pcnt_unit_clear_count(s_fans[i].tach);
        }
        vTaskDelay(pdMS_TO_TICKS(JANELA_MS));

        int pulsos[FANS_MAX] = { 0 };
        for (int i = 0; i < n; i++) {
            pcnt_unit_get_count(s_fans[i].tach, &pulsos[i]);
        }

        int mudou[FANS_MAX] = { 0 };
        int rpm[FANS_MAX];
        int pwm;
        taskENTER_CRITICAL(&s_lock);
        pwm = s_pwm_pct;
        for (int i = 0; i < n; i++) {
            fan_t *f = &s_fans[i];
            f->rpm = pulsos[i] * 60000 / (PULSOS_POR_VOLTA * JANELA_MS);
            f->s_desde_mudanca++;
            mudou[i] = verificar_falha(f, pwm);
            rpm[i] = f->rpm;
        }
        taskEXIT_CRITICAL(&s_lock);

        // logs e callback fora da seção crítica
        for (int i = 0; i < n; i++) {
            if (mudou[i] > 0) {
                ESP_LOGE(TAG, "%s: FALHA! PWM %d%% mas só %d RPM", s_fans[i].nome, pwm, rpm[i]);
            } else if (mudou[i] < 0) {
                ESP_LOGW(TAG, "%s: recuperada, %d RPM", s_fans[i].nome, rpm[i]);
            }
            if (mudou[i] != 0 && s_cfg.on_evento) {
                s_cfg.on_evento(i, s_fans[i].nome, mudou[i] > 0, rpm[i], s_cfg.ctx);
            }
        }
    }
}

// ---------------------------------------------------------------- API pública

esp_err_t fans_init(const fans_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(!s_iniciado, ESP_ERR_INVALID_STATE, TAG, "fans_init já foi chamado");
    ESP_RETURN_ON_FALSE(cfg && cfg->n_fans >= 1 && cfg->n_fans <= FANS_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "n_fans deve ser de 1 a %d", FANS_MAX);
    ESP_RETURN_ON_FALSE(cfg->pwm_gpio != GPIO_NUM_NC, ESP_ERR_INVALID_ARG, TAG, "pwm_gpio");

    s_cfg = *cfg;
    ESP_RETURN_ON_ERROR(pwm_init(cfg->pwm_gpio), TAG, "PWM");
    s_pwm_pct = 0;
    s_pct_pedido = 0;

    for (int i = 0; i < cfg->n_fans; i++) {
        s_fans[i] = (fan_t){ .nome = cfg->nome[i] ? cfg->nome[i] : NOMES_PADRAO[i] };
        ESP_RETURN_ON_ERROR(tach_init(&s_fans[i], cfg->tach_gpio[i]), TAG, "tacômetro");
    }

    BaseType_t ok = xTaskCreate(tarefa_fans, "fans", TAREFA_PILHA, NULL, TAREFA_PRIORIDADE, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "tarefa");

    s_iniciado = true;
    ESP_LOGI(TAG, "%d ventoinha(s), PWM na GPIO %d", cfg->n_fans, cfg->pwm_gpio);
    return ESP_OK;
}

// Aplica o PWM de fato (usado por fans_set_percent e pela rotina de teste)
static esp_err_t aplicar_pwm(int pct)
{
    uint32_t duty = (uint32_t)pct * PWM_MAX / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(PWM_MODO, PWM_CANAL, duty), TAG, "duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(PWM_MODO, PWM_CANAL), TAG, "duty");

    taskENTER_CRITICAL(&s_lock);
    if (pct != s_pwm_pct) {
        for (int i = 0; i < s_cfg.n_fans; i++) {
            s_fans[i].s_desde_mudanca = 0;   // dá tempo da fan acelerar/frear
            s_fans[i].leituras_ruins  = 0;
        }
    }
    s_pwm_pct = pct;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t fans_set_percent(int pct)
{
    ESP_RETURN_ON_FALSE(s_iniciado, ESP_ERR_INVALID_STATE, TAG, "chame fans_init antes");
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    taskENTER_CRITICAL(&s_lock);
    s_pct_pedido = pct;
    bool em_teste = s_em_teste;
    taskEXIT_CRITICAL(&s_lock);

    // durante a rotina de teste o valor só é guardado e aplicado quando o teste acabar
    return em_teste ? ESP_OK : aplicar_pwm(pct);
}

int fans_get_percent(void)
{
    taskENTER_CRITICAL(&s_lock);
    int pct = s_pwm_pct;
    taskEXIT_CRITICAL(&s_lock);
    return pct;
}

int fans_count(void)
{
    return s_iniciado ? s_cfg.n_fans : 0;
}

esp_err_t fans_get_status(int idx, fan_status_t *out)
{
    ESP_RETURN_ON_FALSE(out && idx >= 0 && idx < fans_count(), ESP_ERR_INVALID_ARG, TAG, "idx");
    taskENTER_CRITICAL(&s_lock);
    out->nome     = s_fans[idx].nome;
    out->pwm_pct  = s_pwm_pct;
    out->rpm      = s_fans[idx].rpm;
    out->em_falha = s_fans[idx].em_falha;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

bool fans_alguma_falha(void)
{
    bool falha = false;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_cfg.n_fans; i++) {
        falha |= s_fans[i].em_falha;
    }
    taskEXIT_CRITICAL(&s_lock);
    return falha;
}

// ---------------------------------------------------------------- rotina de teste

static const int TESTE_PWM[FANS_TESTE_DEGRAUS] = { 100, 75, 50, 30 };

static int rpm_atual(int i)
{
    taskENTER_CRITICAL(&s_lock);
    int rpm = s_fans[i].rpm;
    taskEXIT_CRITICAL(&s_lock);
    return rpm;
}

// Avalia uma fan a partir das médias de cada degrau. Retorna NULL se ok, ou o motivo.
static const char *avaliar(const int rpm[FANS_TESTE_DEGRAUS])
{
    for (int d = 0; d < FANS_TESTE_DEGRAUS; d++) {
        if (rpm[d] < s_cfg.falha_rpm_min) return "não gira (travada, sem 12 V ou tacômetro solto)";
    }
    if (rpm[0] < rpm[FANS_TESTE_DEGRAUS - 1] * TESTE_RAZAO_MIN) {
        return "velocidade não muda com o PWM (fio azul solto?)";
    }
    for (int d = 1; d < FANS_TESTE_DEGRAUS; d++) {
        if (rpm[d] > rpm[d - 1] * (1.0f + TESTE_TOLERANCIA)) return "RPM não cai quando o PWM baixa";
    }
    return NULL;
}

esp_err_t rotina_teste_fan(fans_teste_resultado_t *res)
{
    ESP_RETURN_ON_FALSE(s_iniciado, ESP_ERR_INVALID_STATE, TAG, "chame fans_init antes");

    taskENTER_CRITICAL(&s_lock);
    bool ja_em_teste = s_em_teste;
    s_em_teste = true;
    taskEXIT_CRITICAL(&s_lock);
    ESP_RETURN_ON_FALSE(!ja_em_teste, ESP_ERR_INVALID_STATE, TAG, "teste já em andamento");

    fans_teste_resultado_t local;
    fans_teste_resultado_t *r = res ? res : &local;
    memset(r, 0, sizeof(*r));
    const int n = s_cfg.n_fans;
    r->n_fans = n;

    const int duracao_s = FANS_TESTE_DEGRAUS * (TESTE_ACOMODACAO_MS / 1000 + TESTE_LEITURAS);
    ESP_LOGI(TAG, "==== rotina de teste: início (~%d s) ====", duracao_s);

    for (int d = 0; d < FANS_TESTE_DEGRAUS; d++) {
        r->pwm[d] = TESTE_PWM[d];
        aplicar_pwm(TESTE_PWM[d]);
        vTaskDelay(pdMS_TO_TICKS(TESTE_ACOMODACAO_MS));

        int soma[FANS_MAX] = { 0 };
        for (int k = 0; k < TESTE_LEITURAS; k++) {
            vTaskDelay(pdMS_TO_TICKS(JANELA_MS));
            for (int i = 0; i < n; i++) soma[i] += rpm_atual(i);
        }

        char linha[160];
        int pos = snprintf(linha, sizeof(linha), "PWM %3d%%:", TESTE_PWM[d]);
        for (int i = 0; i < n; i++) {
            r->fan[i].rpm[d] = soma[i] / TESTE_LEITURAS;
            if (pos < (int)sizeof(linha)) {
                pos += snprintf(linha + pos, sizeof(linha) - pos, "  %s %5d RPM",
                                s_fans[i].nome, r->fan[i].rpm[d]);
            }
        }
        ESP_LOGI(TAG, "%s", linha);
    }

    r->todas_ok = true;
    for (int i = 0; i < n; i++) {
        r->fan[i].nome   = s_fans[i].nome;
        r->fan[i].motivo = avaliar(r->fan[i].rpm);
        r->fan[i].ok     = (r->fan[i].motivo == NULL);
        r->todas_ok     &= r->fan[i].ok;
        if (r->fan[i].ok) {
            ESP_LOGI(TAG, "%s: OK", s_fans[i].nome);
        } else {
            ESP_LOGE(TAG, "%s: REPROVADA - %s", s_fans[i].nome, r->fan[i].motivo);
        }
    }
    ESP_LOGI(TAG, "==== rotina de teste: %s ====", r->todas_ok ? "todas OK" : "HÁ FALHAS");

    // volta para o valor que estava pedido antes (ou que foi pedido durante o teste)
    taskENTER_CRITICAL(&s_lock);
    s_em_teste = false;
    int pedido = s_pct_pedido;
    taskEXIT_CRITICAL(&s_lock);
    aplicar_pwm(pedido);

    return r->todas_ok ? ESP_OK : ESP_FAIL;
}
