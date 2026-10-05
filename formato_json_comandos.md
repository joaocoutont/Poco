# Formato do JSON de Telemetria e Lista de Comandos MQTT

Este documento serve como referência técnica exclusiva para a comunicação MQTT do sistema de automação do poço (Poço 3).

---

## 1. Formato do JSON de Telemetria (Mensagem Publicada pelo ESP32)

A telemetria é enviada periodicamente (a cada 5 segundos) no formato JSON estruturado.

* **Tópico de Publicação Dinâmico:** `<Sistema>/<Subsistema>/<Unidade>/telemetria`  
  *(Exemplo padrão: `sanear/agua/p03/telemetria`)*
* **Estrutura do Payload (JSON):**

```json
{
  "bomba": {
    "ligada": true,         // true = Bomba em funcionamento / false = Desligada
    "defeito": false,       // true = Disparo de rele termico/falha de motor confirmada (2s)
    "falta_fase_ou_nivel": false, // true = Falta de fase ou Boia de seguranca ativa confirmada (2s)
    "modo_painel": "automatico", // "automatico" = Chave CCM em auto / "manual" = Chave CCM em manual
    "sirene": false         // true = Relé K3 da sirene de seguranca ativo
  },
  "controle": {
    "modo_operacao": "nivel", // Modo ativo da placa: "nivel", "relogio" ou "remoto"
    "nivel_percentual": 78.5,  // Nivel atual do reservatorio (%)
    "nivel_mA": 16.56,         // Corrente medida do sensor Danfoss (mA)
    "nivel_liga": 30.0,        // Nivel configurado para acionar a bomba (%)
    "nivel_desliga": 95.0,     // Nivel configurado para desligar a bomba (%)
    "nivel_falha": false       // true = Alerta de cabo de sensor Danfoss rompido ou curto (<3.6mA ou >21mA)
  },
  "sensores": {
    "vazao_l_min": 45.2,       // Vazao de agua instantanea medida no GPIO 15 (Litros por minuto)
    "volume_m3": 124.522,      // Volume total acumulado recuperado e somado na flash (Metros cubicos)
    "temperatura_c": 24.8      // Temperatura medida pelo sensor DS18B20 no GPIO 14 (graus Celsius)
  },
  "uptime_s": 3600             // Tempo de funcionamento ininterrupto da placa (segundos)
}
```

---

## 2. Lista de Comandos Remotos (Mensagens Recebidas pelo ESP32)

Os tópicos de comando são construídos dinamicamente de acordo com a parametrização do portal Wi-Fi:  
`Tópico Base = <Sistema>/<Subsistema>/<Unidade>/`  
*(Valores padrão: `sanear/agua/p03/`)*

### 2.1. Alteração do Modo de Operação
* **Tópico:** `<Tópico Base>cmd/modo`
* **Payloads aceitos (Texto Plano):**
  * `"nivel"` -> Passa o controle automático para o sensor de nível Danfoss.
  * `"relogio"` -> Passa o controle automático para o agendamento de relógio NTP.
  * `"remoto"` -> Passa o controle para acionamento direto via MQTT.

### 2.2. Liga/Desliga Bomba (Apenas no Modo Remoto)
* **Tópico:** `<Tópico Base>cmd/bomba`
* **Payloads aceitos (Texto Plano):**
  * `"1"` -> Liga a bomba (saída K1).
  * `"0"` -> Desliga a bomba (saída K1).

### 2.3. Acionamento da Sirene (Alarme contra Intrusos)
* **Tópico:** `<Tópico Base>cmd/sirene`
* **Payloads aceitos (Texto Plano):**
  * `"1"` -> Liga a sirene de alarme (saída K3). Funciona de forma independente do estado do poço/CCM.
  * `"0"` -> Desliga a sirene de alarme (saída K3).

### 2.4. Configuração de Janela do Modo Relógio
* **Tópico:** `<Tópico Base>cmd/config/horario` (Atualiza e salva na Flash)
* **Payload aceito (JSON):**
  ```json
  {
    "h_ini": 8,   // Hora de inicio (0 a 23)
    "m_ini": 0,   // Minuto de inicio (0 a 59)
    "h_fim": 17,  // Hora de fim (0 a 23)
    "m_fim": 30   // Minuto de fim (0 a 59)
  }
  ```

### 2.5. Configuração de Setpoints de Nível
* **Tópico:** `<Tópico Base>cmd/config/setpoints` (Atualiza e salva na Flash)
* **Payload aceito (JSON):**
  ```json
  {
    "nivel_liga": 30.0,   // Liga a bomba abaixo dessa porcentagem (%)
    "nivel_desliga": 95.0 // Desliga a bomba ao atingir essa porcentagem (%)
  }
  ```

### 2.6. Reset de Falhas da Bomba (Painel Físico)
* **Tópico:** `<Tópico Base>cmd/reset`
* **Payload aceito (Texto Plano):**
  * `"1"` -> Emite um pulso de acionamento de 1 segundo no Relé K2.
