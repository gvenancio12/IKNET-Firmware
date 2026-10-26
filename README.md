# Firmware – Cooler Ativo com Telemetria (IKNET)

ESP-IDF v5.5.6 em container, com a extensão oficial **ESP-IDF** da Espressif.
O Mac só precisa do VS Code, de um runtime Docker e do Chrome.

## Pré-requisitos no Mac
- VS Code + extensão **Dev Containers** (`ms-vscode-remote.remote-containers`)
- Runtime Docker: **OrbStack** ou **Docker Desktop**
- **Google Chrome** (ou Edge) para gravar a placa pelo navegador (Web Serial)

## Abrir o ambiente
1. VS Code: *File > Open Folder* → esta pasta `firmware/`.
2. Paleta de comandos → **Dev Containers: Reopen in Container**.
3. Na primeira vez baixa a imagem `espressif/idf:v5.5.6` (alguns GB). Depois fica em cache.

## Compilar
Pela barra da extensão ESP-IDF (botão *Build*) ou no terminal do container:
```
idf.py build
idf.py merge-bin
```
No primeiro build depois de adicionar um componente com dependências (ex.: `temperatura`),
o ESP-IDF baixa os drivers da internet e cria `dependencies.lock` – esse arquivo deve ir
para o git. Se o build não enxergar um componente novo, rode `idf.py reconfigure` antes.

O `merge-bin` gera `build/merged-binary.bin`: imagem completa, gravada no endereço 0x0.

## Gravar na placa (fora do container)
O Docker no macOS não enxerga portas USB, então a gravação é feita pelo Chrome:
1. Abrir https://espressif.github.io/esptool-js/
2. **Connect** → escolher a porta da placa.
3. Flash Address `0x0`, arquivo `build/merged-binary.bin` → **Program**.
4. Monitor serial: aba **Console** da mesma página (115200 baud).

Se não conectar: segurar **PRG**, apertar e soltar **RST**, soltar **PRG** e tentar de novo.

## Estrutura do código
```
firmware/
├── main/main.c          aplicação atual: ventoinha pequena (MOSFET) controlada pela temperatura
└── components/
    ├── fans/            ventoinhas externas: PWM, RPM e detecção de falha
    │                    (documentação em components/fans/README.md)
    └── temperatura/     sensores DS18B20 (1-Wire): leitura e classificação
                         (documentação em components/temperatura/README.md)
```
Cada parte do hardware vira um componente em `components/<nome>/`, com a API em
`include/<nome>.h`. O `main.c` só junta os componentes. O ESP-IDF encontra os componentes
dessa pasta sozinho, sem precisar mexer no `CMakeLists.txt` da raiz.

## Pinos em uso (Heltec WiFi LoRa 32 V3, conector J3)
| GPIO | Pino J3 | Uso |
|---|---|---|
| 5 | 16 | PWM das ventoinhas externas (azuis, via 330 Ω) |
| 6 | 17 | tacômetro FAN1 (amarelo, pull-up 2 kΩ para 3V3) |
| 4 | 15 | tacômetro FAN2 (amarelo, pull-up 2 kΩ para 3V3) |
| 7 | 18 | comando do MOSFET das ventoinhas pequenas (PWM de 50 Hz) |
| 38 | 11 | barramento 1-Wire dos DS18B20 (DQ, pull-up 4,7 kΩ ou 2 kΩ para 3V3) |
| 35 | – | LED da placa: pisca = ok, aceso fixo = alguma ventoinha em falha |
