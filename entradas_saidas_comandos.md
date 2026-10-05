# Guia Rápido: Entradas, Saídas e Comandos MQTT

Este guia serve como referência rápida para a pinagem das entradas e saídas físicas do ESP32 e os comandos remotos aceitos via MQTT.

---

## 1. Entradas Físicas (Sensores e Sinais)

Todas as entradas digitais optoacopladas possuem **lógica invertida** (nível lógico `LOW` indica sinal presente/tensão de 5-25V).

| Entrada Física | Pino no ESP32 | Função do Programa | Lógica de Leitura |
| :---: | :---: | :--- | :--- |
| **I1** | **GPIO 35** | Seletor Automático/Manual do Painel CCM | `LOW` = Automático (Placa controla)<br>`HIGH` = Manual (Placa em monitoramento) |
| **I2** | **GPIO 34** | Feedback de Status da Bomba | `LOW` = Bomba Ligada<br>`HIGH` = Bomba Desligada |
| **I3** | **GPIO 39** | Defeito Térmico/Sobrecarga do Motor | `HIGH` = Bomba com Defeito (Confirma após 2s)<br>`LOW` = Bomba OK |
| **I4** | **GPIO 36** | Detector de Falta de Fase / Boia de Segurança | `HIGH` = Falta de Fase ou Boia de Segurança Ativa (Confirma após 2s)<br>`LOW` = Rede Elétrica e Nível de Proteção OK |
| **DT (Temp)** | **GPIO 14** | Sensor de Temperatura DS18B20 | Barramento OneWire digital |
| **IR (Vazão)** | **GPIO 15** | Sensor de Vazão (Contagem de Pulsos) | Interrupção de hardware por borda de descida |
| **ADS1115 (A0)**| **I2C (21/22)**| Sensor de Nível Danfoss (4-20mA) | Analógico (Tensão de 0,6V a 3,0V no shunt de 150 $\Omega$) |

---

## 2. Saídas Físicas (Atuadores e Relés)

Saídas digitais de acionamento dos relés integrados (nível lógico `HIGH` ativa o relé correspondente).

| Saída Física | Pino no ESP32 | Dispositivo Controlado | Comportamento |
| :---: | :---: | :--- | :--- |
| **K1** | **GPIO 13** | Comando para Ligar a Bomba | `HIGH` = Liga Contator / `LOW` = Desliga Contator |
| **K2** | **GPIO 12** | Comando de Reset de Falhas do Painel | Envia pulso de `HIGH` por 1 segundo e volta a `LOW` |
| **K3** | **GPIO 27** | Acionamento da Sirene (Alarme de Invasão)| `HIGH` = Liga Sirene / `LOW` = Desliga Sirene |
| **K4** | **GPIO 26** | Relé Auxiliar K4 | Disponível para expansão futura |
| **LED** | **GPIO 2** | LED Built-in da placa | Pisca a cada 1 segundo (indica funcionamento da placa) |

---

## 3. Comandos Remotos (Assinatura MQTT)

Os tópicos de comando são construídos dinamicamente de acordo com a parametrização do portal Wi-Fi:  
`Tópico Base = <Sistema>/<Subsistema>/<Unidade>/`  
*(Valores padrão: `sanear/agua/p03/`)*

### 3.1. Troca de Modo de Operação
* **Tópico:** `<Tópico Base>cmd/modo`
* **Mensagens Aceitas:**
  * `"nivel"` -> Seleciona controle automático por nível de reservatório.
  * `"relogio"` -> Seleciona controle de agendamento horário por relógio NTP.
  * `"remoto"` -> Seleciona controle direto por comando de botão via MQTT.

### 3.2. Comando de Acionamento da Bomba (Modo Remoto)
* **Tópico:** `<Tópico Base>cmd/bomba`
* **Mensagens Aceitas:**
  * `"1"` -> Liga a bomba (saída K1).
  * `"0"` -> Desliga a bomba (saída K1).

### 3.3. Acionamento da Sirene (Alarme contra Intruso)
* **Tópico:** `<Tópico Base>cmd/sirene`
* **Mensagens Aceitas:**
  * `"1"` -> Ativa a sirene (liga o Relé K3). Funciona de forma independente do modo da bomba.
  * `"0"` -> Desliga a sirene (desliga o Relé K3).

### 3.4. Configuração de Janela do Modo Relógio
* **Tópico:** `<Tópico Base>cmd/config/horario` (Atualiza e salva na Flash)
* **Formato do Payload (JSON):**
  ```json
  {
    "h_ini": 8,   // Hora de início (0 a 23)
    "m_ini": 0,   // Minuto de início (0 a 59)
    "h_fim": 17,  // Hora de fim (0 a 23)
    "m_fim": 30   // Minuto de fim (0 a 59)
  }
  ```

### 3.5. Configuração de Setpoints de Nível
* **Tópico:** `<Tópico Base>cmd/config/setpoints` (Atualiza e salva na Flash)
* **Formato do Payload (JSON):**
  ```json
  {
    "nivel_liga": 30.0,   // Liga a bomba quando o nível cai abaixo desse valor (%)
    "nivel_desliga": 95.0 // Desliga a bomba quando o nível atinge esse valor (%)
  }
  ```

### 3.6. Comando de Reset do Relé Térmico da Bomba
* **Tópico:** `<Tópico Base>cmd/reset`
* **Mensagens Aceitas:**
  * `"1"` -> Envia um pulso temporizado de 1 segundo na saída do Relé K2.
