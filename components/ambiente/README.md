# DHT11: temperatura e umidade

Componente ambiente, com o driver esp-idf-lib/dht 1.2.0.
API: ambiente_ler(&leitura), retornando ESP_OK ou um erro do ESP-IDF.
Campos: temperatura_c (graus Celsius) e umidade_pct (% de umidade relativa).
Em falhas, os valores ficam NAN; a aplicacao exibe o erro e tenta novamente no proximo ciclo.

## Ligacoes
- DHT11 VCC: 3V3; GND: GND; DATA: GPIO 35.
- Usar pull-up de 4,7 a 10 kohms entre DATA e 3V3, caso o modulo nao o tenha.
- DS18B20 permanece no GPIO 38, com seu proprio pull-up.
- Confira a identificacao dos pinos no seu modulo antes de conectar.

## Configuracao e uso
No ambiente ESP-IDF 5.x deste projeto:
    idf.py reconfigure
    idf.py menuconfig
Em Component config > IKNET - DHT11, escolha o GPIO de dados.
Use um pino livre; nao use os pinos reservados pela placa ou pelas ventoinhas.
Depois:
    idf.py build flash monitor

A aplicacao faz uma leitura a cada ciclo de aproximadamente 2 segundos.
A primeira leitura ocorre depois da espera de partida e do primeiro ciclo.
O DHT11 apenas fornece telemetria; o controle existente continua usando o DS18B20.
O componente faz leitura sincrona e deve ser chamado por uma unica tarefa.

Driver e API: https://esp-idf-lib.github.io/dht/