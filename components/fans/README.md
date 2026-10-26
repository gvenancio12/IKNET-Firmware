# Componente `fans` – ventoinhas externas de 4 fios

Controla as ventoinhas externas do cooler (AVC 120 mm, 12 V, 4 fios): define a velocidade
por PWM, mede o RPM de cada uma pelo tacômetro e detecta quando alguma para de girar.

```
components/fans/
├── CMakeLists.txt     registro do componente no ESP-IDF
├── include/fans.h     API pública (o que os outros módulos podem usar)
├── fans.c             implementação
└── README.md          este arquivo
```

## 1. Como funciona

### Hardware
| Fio da ventoinha | Função | Ligação |
|---|---|---|
| Preto | GND | GND da fonte 12 V **e** GND do ESP32 (terra comum) |
| Vermelho | +12 V | fonte 12 V |
| Azul | entrada PWM | **todos os azuis juntos** → 330 Ω → GPIO de PWM |
| Amarelo | saída do tacômetro | **um GPIO por ventoinha** + resistor de 2 kΩ desse GPIO para 3V3 |

- O tacômetro é "coletor aberto": a ventoinha só puxa o fio para 0 V, por isso precisa do
  resistor para 3V3 (pull-up). Ele também protege o ESP32, que não aceita 5 V/12 V.
- O resistor de 330 Ω no PWM só limita corrente em caso de erro de ligação.

### PWM (velocidade)
- Periférico **LEDC**, timer 0, canal 0, **25 kHz** (padrão Intel para ventoinhas de 4 fios),
  resolução de 10 bits (0–1023).
- Um único canal para todas as ventoinhas: elas sempre recebem a mesma velocidade.
- Algumas ventoinhas continuam girando devagar com 0%; as AVC testadas param.

### Tacômetro (RPM)
- Periférico **PCNT** (contador de pulsos), uma unidade por ventoinha, contando as bordas de
  descida. Filtro de ruído de 10 µs.
- A ventoinha gera **2 pulsos por volta**. A cada 1 s:
  `RPM = pulsos × 60 / 2`. A resolução é, portanto, de 30 RPM.

### Tarefa interna
`fans_init()` cria a tarefa FreeRTOS `"fans"` (prioridade 5), que em laço:
1. zera os contadores de todas as ventoinhas;
2. espera 1 s;
3. lê os contadores, calcula o RPM e verifica falha de cada ventoinha.

Os dados ficam protegidos por uma seção crítica, então as funções da API podem ser chamadas
de qualquer tarefa.

### Detecção de falha
Cada ventoinha é verificada de forma independente. Ela entra em **falha** quando:
- o PWM está em `falha_pwm_min` (30%) ou mais, **e**
- já se passaram `acomodacao_s` (3 s) desde a última mudança de PWM (tempo de acelerar), **e**
- o RPM ficou abaixo de `falha_rpm_min` (300) por `falha_leituras` (3) medições seguidas.

Sai da falha assim que o RPM volta a `falha_rpm_min` ou mais. Com PWM abaixo de 30% a
verificação fica desligada (parar é esperado), mas uma falha já registrada continua ativa até
a ventoinha voltar a girar – o alarme não some sozinho só porque o PWM baixou.

Detecta: ventoinha travada, desconectada, sem 12 V ou fio amarelo solto.
Tempo até o alarme: ~6 s depois de parar.

Mensagens no log:
```
E (218271) fans: FAN1: FALHA! PWM 75% mas só 0 RPM
W (194261) fans: FAN1: recuperada, 480 RPM
```

### Valores medidos (AVC 120 mm, 12 V)
| PWM | RPM estável |
|---|---|
| 100% | ~2800 |
| 75% | ~2340 |
| 50% | ~1700 |
| 30% | ~1060 |
| 0% | para (~10 s até 0) |

## 2. Como usar

### Inicializar
```c
#include "fans.h"

fans_config_t cfg = FANS_CONFIG_PADRAO();   // limites de falha padrão
cfg.pwm_gpio     = GPIO_NUM_5;
cfg.n_fans       = 2;
cfg.tach_gpio[0] = GPIO_NUM_6;  cfg.nome[0] = "FAN1";
cfg.tach_gpio[1] = GPIO_NUM_4;  cfg.nome[1] = "FAN2";
ESP_ERROR_CHECK(fans_init(&cfg));           // começa com PWM em 0%
```
Para mudar um limite, altere o campo depois do `FANS_CONFIG_PADRAO()`, por exemplo
`cfg.falha_rpm_min = 500;`.

### API
| Função | O que faz |
|---|---|
| `fans_init(&cfg)` | configura PWM, tacômetros e inicia a tarefa. Chamar uma vez. |
| `fans_set_percent(pct)` | define a velocidade de todas (0–100%). |
| `fans_get_percent()` | velocidade pedida atual. |
| `fans_count()` | número de ventoinhas configuradas. |
| `fans_get_status(i, &st)` | `st.nome`, `st.pwm_pct`, `st.rpm`, `st.em_falha` da ventoinha `i`. |
| `fans_alguma_falha()` | `true` se qualquer ventoinha está em falha. |
| `rotina_teste_fan(&res)` | teste completo das ventoinhas (~28 s), ver abaixo. |

Todas retornam `ESP_OK` ou um código de erro (`ESP_ERR_INVALID_ARG`, `ESP_ERR_INVALID_STATE`).

### Rotina de teste (`rotina_teste_fan`)
Verifica se cada ventoinha está funcionando por completo. Pode ser chamada a qualquer
momento depois do `fans_init()` – na partida, por um comando remoto, em manutenção.

O que faz:
1. passa o PWM por **100, 75, 50 e 30%**;
2. em cada degrau espera 4 s (estabilizar) e faz a média de 3 medições de 1 s;
3. avalia cada ventoinha separadamente:

| Verificação | Reprova com | Causa provável |
|---|---|---|
| RPM ≥ 300 em todos os degraus | `não gira` | travada, sem 12 V, fio amarelo solto |
| RPM em 100% ≥ 1,5 × RPM em 30% | `velocidade não muda com o PWM` | fio azul solto (fan fica sempre no máximo) |
| RPM cai a cada degrau (folga de 5%) | `RPM não cai quando o PWM baixa` | sinal de PWM ruim, fan com defeito |

4. volta ao PWM que estava antes (ou ao último pedido feito durante o teste).

Saída no log:
```
I fans: ==== rotina de teste: início (~28 s) ====
I fans: PWM 100%:  FAN1  2800 RPM  FAN2  2830 RPM
I fans: PWM  75%:  FAN1  2340 RPM  FAN2  2340 RPM
I fans: PWM  50%:  FAN1  1710 RPM  FAN2  1700 RPM
I fans: PWM  30%:  FAN1  1070 RPM  FAN2  1060 RPM
I fans: FAN1: OK
I fans: FAN2: OK
I fans: ==== rotina de teste: todas OK ====
```

Uso no código:
```c
fans_teste_resultado_t res;
if (rotina_teste_fan(&res) != ESP_OK) {
    for (int i = 0; i < res.n_fans; i++) {
        if (!res.fan[i].ok) { /* res.fan[i].nome, res.fan[i].motivo, res.fan[i].rpm[] */ }
    }
}
```
Passe `NULL` se só o log interessa. Cuidados:
- **bloqueia** a tarefa que chamou por ~28 s: chame de uma tarefa que pode esperar
  (o `app_main` na partida, ou uma tarefa de manutenção), nunca do callback `on_evento`;
- durante o teste, `fans_set_percent()` não muda a velocidade na hora: o valor é guardado
  e aplicado ao final;
- as ventoinhas vão a 100% no início: é barulhento, e por ~28 s o controle térmico não
  manda na velocidade.

### Avisos de falha (callback)
Para reagir na hora em que uma ventoinha falha ou se recupera, registre um callback:
```c
static void ao_mudar_falha(int idx, const char *nome, bool em_falha, int rpm, void *ctx)
{
    // ex.: publicar alarme na telemetria, acender LED, acionar buzzer...
}
cfg.on_evento = ao_mudar_falha;
cfg.ctx       = NULL;   // ponteiro livre, repassado ao callback
```
O callback roda dentro da tarefa `"fans"`: deve ser rápido e não pode bloquear.
Para trabalho pesado (rede, cartão SD), envie uma mensagem para uma fila e trate em outra tarefa.

## 3. Integração com o resto do sistema

O componente só **executa** a velocidade pedida e **informa** o estado. Quem decide a
velocidade é outro módulo. Fluxo previsto:

```
 sensores (DS18B20...) ──► controle térmico ──► fans_set_percent(pct)
                                                        │
 telemetria / display  ◄── fans_get_status() ◄──────────┤
 alarmes               ◄── callback on_evento ◄─────────┘
```

**Controle por temperatura** (exemplo de como encaixar, em uma tarefa própria):
```c
static void tarefa_controle(void *arg)
{
    while (1) {
        float t = temperatura_ar_quente();          // vem do módulo de sensores
        int pct;
        if (t <= 21.0f)      pct = 30;              // piso: mínimo de circulação
        else if (t >= 40.0f) pct = 100;             // alerta: máximo
        else                 pct = 30 + (int)((t - 21.0f) * 70.0f / 19.0f);
        fans_set_percent(pct);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
```
Evite mudar o PWM a cada segundo: cada mudança reinicia o tempo de acomodação (3 s) da
detecção de falha. Atualizar a cada 2 s ou mais, ou só quando o valor mudar alguns %, é suficiente.

**Telemetria**: ler periodicamente com `fans_get_status()` para cada `i < fans_count()`
e publicar `rpm`, `pwm_pct` e `em_falha` (métricas V01/V02 do Anexo B).

## 4. Recursos de hardware usados
| Recurso | Uso |
|---|---|
| LEDC timer 0, canal 0 | PWM das ventoinhas (não usar em outro módulo) |
| PCNT unidades 0..n-1 | tacômetros (o ESP32-S3 tem 4 unidades) |
| Tarefa `"fans"` | 3 KB de pilha, prioridade 5 |
| GPIOs | definidos em `fans_config_t` |

Se outro módulo precisar de PWM (ex.: bombas), use outro canal e outro timer do LEDC
(timer 1, canal 1...), porque o timer 0 está fixo em 25 kHz.
