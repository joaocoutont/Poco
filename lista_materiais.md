# Lista de Materiais: Automação do Poço com ESP32

Esta lista detalha todos os componentes elétricos e eletrônicos necessários para realizar a montagem física completa do painel de automação do poço artesiano, incluindo especificações sugeridas e finalidade de cada item.

---

## 1. Módulos Eletrônicos e Placas

| Item | Componente | Especificações Recomendadas | Finalidade / Conexão | Status |
| :---: | :--- | :--- | :--- | :---: |
| **1.1** | **Placa ESP32 Automação** | Fabricada pela *Auto Core Robótica* | Placa de circuito impresso principal com relés e optoacopladores integrados. | Já possui |
| **1.2** | **Módulo ESP32 Devkit V1** | Versão de 30 pinos (com barramento duplo de conexões) | Cérebro da automação (Wi-Fi, MQTT, processamento). Encaixado na placa principal. | Já possui |
| **1.3** | **Conversor ADC ADS1115** | Módulo ADS1115 I2C de 16 bits (4 canais) | Permite ler com precisão o sensor analógico Danfoss de 4-20mA (via I2C). | Adquirir |

---

## 2. Sensores Industriais e de Campo

| Item | Componente | Especificações Recomendadas | Finalidade / Conexão | Status |
| :---: | :--- | :--- | :--- | :---: |
| **2.1** | **Sensor de Nível Danfoss** | Transmissor de pressão hidrostática ou nível (Saída 4 a 20 mA, 2 fios) | Medição contínua do nível do reservatório. | Já possui |
| **2.2** | **Medidor de Vazão** | Tipo turbina ou eletromagnético com saída de pulso digital (NPN ou Contato Seco) | Medição de vazão instantânea e volume acumulado. Conecta no **GPIO 15**. | Adquirir |
| **2.3** | **Sensor de Temperatura** | **DS18B20** versão sonda de aço inoxidável à prova d'água (cabo de 1m ou 3m) | Medição de temperatura no poço/reservatório. Conecta no **GPIO 14**. | Adquirir |

---

## 3. Componentes Passivos e Acessórios de Painel

| Item | Componente | Especificações Recomendadas | Finalidade / Conexão | Status |
| :---: | :--- | :--- | :--- | :---: |
| **3.1** | **Resistor Shunt** | **150 Ohms** (potência de 1/4 W ou 1/2 W) com **1% de tolerância** (precisão metálica) | Converte a corrente de 4-20mA do sensor Danfoss em tensão de 0,6V-3,0V para o ADS1115. | Adquirir |
| **3.2** | **Fonte de Alimentação** | Fonte chaveada de **12Vcc ou 24Vcc** (Mínimo de **1,5 A** ou **2 A**) com plugue macho P4 | Alimenta a placa de automação (plugue P4) e fornece energia ao loop do sensor de 4-20mA. | Adquirir |
| **3.3** | **Cabos de Conexão** | Cabos do tipo **Jumpers DuPont Fêmea-Fêmea** (para o ADS1115) | Interligar o módulo ADS1115 à barra de expansão I2C da placa principal. | Adquirir |
| **3.4** | **Cabo Blindado (Shielded)** | Cabo blindado de 2 ou 3 vias (bitola 0,5mm² ou 0,75mm²) | Recomendado para conectar o sensor Danfoss do poço até o painel (evita ruídos industriais). | Adquirir |
| **3.5** | **Painel CCM Local** | Painel elétrico físico com contatores e relé térmico/de fase | Quadro de força físico que comanda o motor da bomba localmente. | Já possui |
