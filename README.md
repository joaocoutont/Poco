# Automação e Telemetria de Poço - ESP32

Este projeto consiste em um firmware para **ESP32** (desenvolvido na plataforma Arduino) projetado para automatizar e monitorar remotamente uma bomba de poço artesiano através do protocolo **MQTT**. 

A automação baseia-se na placa **ESP32 Automação** (fabricada pela *Auto Core Robótica*), utilizando o módulo **ESP32 Devkit V1** (30 pinos).

---

## 📋 Sumário
1. [Recursos Principais](#-recursos-principais)
2. [Pinagem e Hardware](#-pinagem-e-hardware)
3. [Modos de Operação](#-modos-de-operação)
4. [Estrutura do Broker MQTT](#-estrutura-do-broker-mqtt)
5. [Requisitos e Compilação](#-requisitos-e-compilação)
6. [Resumo da Placa](#-resumo-da-placa)

---

## 🚀 Recursos Principais
* **Controle de Bomba Seguro:** Lógica integrada de intertravamento para liberar o controle à placa quando em modo automático físico no CCM, desligando o relé e passando apenas a monitorar caso o CCM seja colocado em Manual.
* **Máquina de Estados de Modos:** Permite selecionar o comportamento do acionamento de forma flexível (Nível, Relógio ou Remoto).
* **Leitura Analógica Industrial (4-20mA):** Suporte a sensores de nível de corrente (como Danfoss) usando um expansor ADC externo **ADS1115** via I2C, garantindo precisão e evitando limitações de Wi-Fi no ADC nativo.
* **Medição de Vazão por Pulsos:** Processamento com interrupção de hardware (ISR) para cálculo de vazão instantânea ($L/min$) e volume acumulado ($m^3$).
* **Conectividade MQTT Robusta:** Algoritmo de reconexão não-bloqueante para manter a placa executando o loop de segurança e monitoramento local mesmo em caso de perda de conexão de internet.
* **Sincronização de Relógio (NTP):** Ajuste automático de data e hora para o modo relógio (agendamento).
* **Alarme de Intrusos (Sirene):** Possibilidade de disparar uma sirene remotamente via MQTT (Relé K3) para espantar invasores vistos pelas câmeras de segurança, funcionando de forma independente das lógicas de controle da bomba.
* **Proteção Dupla por Hardware (Entrada I4):** O pino I4 (GPIO 36) atua monitorando conjuntamente a falta de fase elétrica e uma boia física de segurança externa (nível crítico do poço/reservatório). Se qualquer um dos dois for acionado (contato aberto), a bomba desliga imediatamente.

---

## 🔌 Pinagem e Hardware

| Componente | Tipo | Pino no ESP32 (GPIO) | Detalhe Físico / Comportamento |
| :--- | :---: | :---: | :--- |
| **sistema_automatico** | Entrada | **GPIO 35** | Entrada optoacoplada (I1) - Detecta modo automático local do painel (LOW = Ativo) |
| **bomba_ligada** | Entrada | **GPIO 34** | Entrada optoacoplada (I2) - Feedback de bomba fisicamente ligada (LOW = Ligada) |
| **bomba_defeito** | Entrada | **GPIO 39** | Entrada optoacoplada (I3) - Relé térmico / Defeito da bomba (HIGH = Falha / Aberto) |
| **falta_fase_ou_nivel**| Entrada | **GPIO 36** | Entrada optoacoplada (I4) - Detector de Falta de Fase e Boia de Segurança (HIGH = Falha) |
| **ligar_bomba** | Saída | **GPIO 13** | Relé K1 - Acionamento do contator da bomba (HIGH = Ligar) |
| **reset_bomba** | Saída | **GPIO 12** | Relé K2 - Comando para resetar falhas no painel (HIGH = Pulso ativo) |
| **acionar_sirene** | Saída | **GPIO 27** | Relé K3 - Acionamento de sirene de alarme contra intruso (HIGH = Ligar) |
| **Sensor de Vazão** | Entrada | **GPIO 15** | Conector IR - Medição de vazão por pulsos de interrupção (com pull-up) |
| **Sensor de Temp.** | E/S | **GPIO 14** | Conector Temp - Barramento OneWire para sensor de temperatura DS18B20 |
| **Barramento I2C** | E/S | **GPIO 21 (SDA) / 22 (SCL)** | Conexão para o Conversor ADS1115 (Sensor de Nível) e displays opcionais |

---

## ⚙️ Modos de Operação

O comportamento do acionamento automático da bomba (quando o CCM físico está em automático) é definido pelo modo atual selecionado:

1. **Modo Nível (`nivel`):**
   * Controlado pelo sensor de 4-20mA via ADS1115.
   * A bomba liga se o nível for inferior ao setpoint mínimo (`nivel_liga`, padrão: 30.0%) e desliga se for superior ao setpoint máximo (`nivel_desliga`, padrão: 95.0%).
2. **Modo Relógio (`relogio`):**
   * Controlado por horários baseados em sincronização NTP.
   * Funciona dentro da janela horária configurada (padrão: 08:00 às 17:00).
3. **Modo Remoto (`remoto`):**
   * Controlado diretamente pela Central de Controle Operacional (CCO) via comandos MQTT de ligar/desligar.

---

## 🌐 Estrutura do Broker MQTT

### Telemetria (Publicação a cada 5 segundos)
* **Tópico:** `<Sistema>/<Subsistema>/<Unidade>/telemetria`
* **Exemplo de Payload (JSON):**
```json
{
  "bomba": {
    "ligada": true,
    "defeito": false,
    "falta_fase_ou_nivel": false,
    "modo_painel": "automatico",
    "sirene": false
  },
  "controle": {
    "modo_operacao": "nivel",
    "nivel_percentual": 78.5,
    "nivel_mA": 16.56,
    "nivel_liga": 30.0,
    "nivel_desliga": 95.0,
    "nivel_falha": false
  },
  "sensores": {
    "vazao_l_min": 45.2,
    "volume_m3": 124.52,
    "temperatura_c": 24.8
  },
  "uptime_s": 3600
}
```

### Comandos (Assinatura)
Os tópicos de comando começam com o Tópico Base configurado no portal Wi-Fi (`<Sistema>/<Subsistema>/<Unidade>/`):
* **Alterar Modo:** `<Tópico Base>cmd/modo` -> Enviar `"nivel"`, `"relogio"` ou `"remoto"`.
* **Acionamento Remoto:** `<Tópico Base>cmd/bomba` -> Enviar `"1"` para ligar ou `"0"` para desligar (apenas se o modo remoto estiver ativo).
* **Acionamento da Sirene:** `<Tópico Base>cmd/sirene` -> Enviar `"1"` para ligar ou `"0"` para desligar a sirene contra intrusos.
* **Configuração de Horário:** `<Tópico Base>cmd/config/horario` -> Enviar JSON contendo a nova janela: `{"h_ini":8,"m_ini":0,"h_fim":17,"m_fim":0}`.
* **Configuração de Setpoints:** `<Tópico Base>cmd/config/setpoints` -> Enviar JSON: `{"nivel_liga":30,"nivel_desliga":95}`.
* **Reset de Falha:** `<Tópico Base>cmd/reset` -> Enviar `"1"` para emitir um pulso de 1 segundo no relé de reset da bomba (K2).

---

## 🛠️ Requisitos e Compilação

Para compilar este firmware no Arduino IDE ou VS Code (PlatformIO), instale as seguintes bibliotecas através do gerenciador de bibliotecas:

1. **WiFiManager** (por tzapu)
2. **PubSubClient** (por Nick O'Leary)
3. **Adafruit ADS1X15** (por Adafruit)
4. **OneWire** (por Paul Stoffregen)
5. **DallasTemperature** (por Miles Burton)
6. **ArduinoJson** (por Benoit Blanchon)

---

## 📖 Resumo da Placa
Para detalhes físicos de conexões elétricas, alimentação de 9-25Vcc e expansões, consulte o arquivo de resumo:
👉 **[resumo_placa_esp32_automacao.md](file:///C:/Users/joao.couto/.gemini/antigravity/brain/336b7f17-e70d-45a6-84ee-ba3b546a3aa3/resumo_placa_esp32_automacao.md)**
