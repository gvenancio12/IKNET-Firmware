# Componente `temperatura` – sensores DS18B20

Lê os sensores de temperatura DS18B20 do cooler (T01–T07 do Anexo B), todos ligados no
mesmo fio de dados (barramento 1-Wire), e classifica cada leitura em FRIO / NORMAL /
ATENCAO / CRITICO.

Baseado no código de teste do DS18B20 de **Raphael Rates** (repositório `temp-iknet`),
reorganizado como componente e estendido para vários sensores.

```
components/temperatura/
├── CMakeLists.txt       registro do componente
├── idf_component.yml    dependências (drivers oficiais ds18b20 0.4.0 e onewire_bus 1.1.1)
├── include/temperatura.h  API pública
├── temperatura.c        implementação
└── README.md            este arquivo
```
Os drivers são baixados automaticamente pelo ESP-IDF no primeiro build (pasta
`managed_components/`, fora do git). Isso precisa de internet no container.

## 1. Ligação

| DS18B20 (encapsulamento TO-92 ou cabo à prova d'água) | Liga em |
|---|---|
| VDD (vermelho no cabo) | 3V3 |
| GND (preto no cabo) | GND |
| DQ – dados (amarelo no cabo) | GPIO 38 (Heltec: pino 11 do J3) |
| resistor **4,7 kΩ** (2 kΩ também serve) | entre DQ e 3V3 – **um só para o barramento todo** |

- Vários sensores: todos os VDD juntos, todos os GND juntos, todos os DQ juntos no mesmo GPIO.
  Continua sendo um único resistor de pull-up.
- O pull-up interno do ESP32 também é ligado pelo código, mas sozinho é fraco demais.
- **Placa final (ESP32-S3-DevKitC-1 v1.1):** a GPIO 38 é o LED RGB da placa. Lá o
  barramento deve ir para outro pino (basta mudar `cfg.gpio`).

## 2. Como funciona

- Barramento 1-Wire pelo periférico **RMT** (driver `onewire_bus`).
- Na partida, procura todos os DS18B20 do fio. Cada sensor tem um endereço de 64 bits
  gravado de fábrica, mostrado no log:
  ```
  I (512) temperatura: S1 encontrado, endereço 3C0123456789AB28
  ```
- A tarefa `"temperatura"` (prioridade 4), em laço:
  1. se falta algum sensor, procura de novo (na partida e a cada `nova_busca_ciclos` ciclos);
  2. manda **todos** os sensores converterem ao mesmo tempo (~800 ms, resolução de 12 bits,
     0,0625 °C);
  3. lê cada um e valida;
  4. espera até completar `intervalo_ms` (padrão 2 s).
- Leitura **inválida** quando: erro de comunicação/CRC, fora da faixa do sensor
  (−55 a 125 °C) ou 85,0 °C exatos (valor que o DS18B20 devolve quando a conversão não
  aconteceu, típico de mau contato na alimentação). O sensor vai para `FALHA SENSOR`,
  é removido e procurado de novo.
- Avisos só na mudança de estado, para não encher o log:
  ```
  W (30512) temperatura: S1: FALHA SENSOR (ESP_ERR_INVALID_CRC)
  W (40512) temperatura: S1: leitura recuperada, 27.44 C
  ```

### Classificação (mesmos limites do código original)
| Nível | Faixa padrão |
|---|---|
| FRIO | < 21 °C |
| NORMAL | 21 – 40 °C |
| ATENCAO | > 40 – 50 °C |
| CRITICO | > 50 °C |
| FALHA SENSOR | sensor ausente ou leitura inválida |

Os limites ficam em `limite_frio`, `limite_atencao` e `limite_critico` da configuração.

## 3. Como usar

### Inicializar
```c
#include "temperatura.h"

temp_config_t cfg = TEMP_CONFIG_PADRAO();
cfg.gpio = GPIO_NUM_38;
ESP_ERROR_CHECK(temp_init(&cfg));   // não falha se não houver sensor: continua procurando
```

### Dar nome aos sensores (mapa de endereços)
Sem mapa, os sensores aparecem como `S1`, `S2`... na ordem em que são achados, e essa
ordem **pode mudar** ao adicionar ou trocar um sensor. Para o sistema final, cada endereço
deve ser associado ao seu ponto de medição:

1. Ligue **um sensor por vez**, anote o endereço que aparece no log e etiquete o cabo.
2. Coloque no mapa:
```c
cfg.n_mapa  = 2;
cfg.mapa[0] = (temp_mapa_t){ .endereco = 0x3C0123456789AB28, .nome = "T01" };  // ar quente
cfg.mapa[1] = (temp_mapa_t){ .endereco = 0x7A0123456789CD28, .nome = "T02" };  // ar frio
```
Sensores do mapa aparecem sempre (em `FALHA SENSOR` se não forem encontrados); um sensor
novo fora do mapa continua aparecendo como `S1`...

### API
| Função | O que faz |
|---|---|
| `temp_init(&cfg)` | configura o barramento e inicia a tarefa. Chamar uma vez. |
| `temp_count()` | quantos sensores são conhecidos (mapa + encontrados). |
| `temp_get_status(i, &st)` | `st.nome`, `st.endereco`, `st.valida`, `st.celsius`, `st.nivel`. |
| `temp_get_por_nome("T01", &st)` | o mesmo, buscando pelo nome. |
| `temp_nivel_str(nivel)` | texto do nível, para log/telemetria. |

Sempre confira `st.valida` antes de usar `st.celsius`: em falha, `celsius` guarda a última
leitura boa (ou `NAN` se nunca leu).

## 4. Integração prevista
```
 DS18B20 ──► temperatura ──► temp_get_por_nome("T01") ──► controle térmico ──► fans_set_percent()
                         └─► temp_get_status(i)       ──► telemetria / display
```
O controle térmico (temperatura → velocidade das fans) será uma tarefa separada no `main`
ou num componente próprio, ainda não implementado. Regra de segurança a adotar: sensor de
controle em `FALHA SENSOR` → fans em 100%.

## 5. Recursos de hardware usados
| Recurso | Uso |
|---|---|
| RMT (1 canal TX + 1 RX) | barramento 1-Wire |
| Tarefa `"temperatura"` | 4 KB de pilha, prioridade 4 |
| GPIO | definido em `cfg.gpio` |
