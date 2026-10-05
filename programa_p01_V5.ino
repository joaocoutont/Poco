/**
 * ==============================================================================
 * PROJETO: Sistema de Automação e Telemetria de Poço Artesiano
 * AUTOR: João Couto (com auxílio de Antigravity AI)
 * DATA: 15 de Julho de 2026
 * VERSÃO: 10.0 (Configuração Dinâmica de Modos e Controle por Boia/Danfoss)
 * ==============================================================================
 * 
 * DESCRIÇÃO DA FUNÇÃO DO PROGRAMA:
 * Este firmware controla o acionamento automático de uma bomba de poço artesiano
 * a partir de três modos distintos selecionáveis via MQTT ou Portal WiFi:
 *   1. Modo Nível: Controle local através de sensor de nível Danfoss (4-20mA) ou
 *      através de uma Boia Digital conectada na interface de expansão.
 *   2. Modo Relógio: Controle de agendamento por janela horária via NTP.
 *   3. Modo Remoto: Acionamento direto vindo da central de controle (CCO).
 * 
 * CONFIGURAÇÃO DINÂMICA VIA CELULAR:
 * Ao conectar ao AP "AutoConnectAP" (senha: "password"), você pode configurar:
 *   - Modo Inicial de Operação (nivel, relogio, remoto).
 *   - Tipo de Controle de Nível (0 = Danfoss 4-20mA, 1 = Boia Digital).
 *   - Tags MQTT para montagem dos tópicos: Sistema, Subsistema e Unidade.
 *   - Broker MQTT: Endereço IP, Porta, Usuário e Senha.
 *   - Parâmetros da Vazão (Fator de Calibração).
 *   - Setpoints de Nível (percentual de Liga e Desliga).
 *   - Janela horária do modo relógio.
 * 
 * PROTEÇÕES FÍSICAS E DE SEGURANÇA:
 * O sistema gerencia proteções contra defeito elétrico na bomba (térmico),
 * falta de fase ou boia de segurança externa (conectados em série no pino I4),
 * rompimento de cabo do sensor (segurança a seco no modo Danfoss), e possui 
 * debounce nas entradas contra ruídos elétricos. Totaliza a vazão e salva 
 * os dados na flash NVS.
 * 
 * RECURSO DE SEGURANÇA EXTRA:
 * Permite acionar remotamente via MQTT uma sirene de segurança (conectada ao relé K3)
 * para espantar invasores. Este recurso é independente do CCM da bomba.
 * 
 * HARDWARE UTILIZADO:
 *   - Placa Principal: ESP32 Automação (Auto Core Robótica)
 *   - Microcontrolador: ESP32 Devkit V1 (Módulo de 30 pinos)
 *   - Expansor ADC: ADS1115 (I2C) - Usado para ler o sensor Danfoss de 4-20mA
 *   - Sensor de Vazão: Medidor de Vazão por Pulsos conectado ao GPIO 15 (Conector IR)
 *   - Sensor de Temperatura: Sensor DS18B20 conectado ao GPIO 14 (Conector TEMP)
 *   - Atuador de Alarme: Sirene de segurança conectada no Relé K3 (GPIO 27)
 *   - Entrada de Proteção Dupla (GPIO 36 / I4): Ligada ao sensor de falta de fase
 *     em série com o contato seco de uma boia de nível de segurança (nível crítico).
 *   - Entrada de Boia de Controle Digital: Lida via barramento I2C através do expansor
 *     PCF8574 (endereço 0x20, pino P0 / Pino 1 da interface de expansão de I/Os).
 * 
 * BIBLIOTECAS NECESSÁRIAS:
 *   - WiFiManager, PubSubClient, Adafruit_ADS1X15, OneWire, DallasTemperature, ArduinoJson
 * ==============================================================================
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <Preferences.h>  // Biblioteca Nativa do ESP32 para memoria nao-volatil
#include "time.h"
#include "sntp.h"

// ==========================================
// Mapeamento de Hardware (Pinos)
// ==========================================

// Entradas Digitais Optoacopladas (Lógica Invertida: LOW = Ativo/Tensão Presente)
#define sistema_automatico   35 // I1 (Automatico local do CCM)
#define bomba_ligada         34 // I2 (Feedback da bomba ligada)
#define bomba_defeito        39 // I3 (Rele termico/defeito da bomba)
#define falta_fase_ou_nivel  36 // I4 (Falta de fase ou Boia de seguranca do reservatorio)

// Saídas Digitais a Relé
#define ligar_bomba         13 // K1 (Comando liga bomba)
#define reset_bomba         12 // K2 (Comando reset defeito bomba)
#define acionar_sirene      27 // K3 (Relé para acionar a Sirene de Alarme)
#define K4                  26 // Saída auxiliar K4
#define ledPin              2  // LED_BUILTIN do ESP32

// Portas Auxiliares de Sensores
#define TEMP_PIN            14 // Porta de dados de temperatura (DT)
#define FLOW_PIN            15 // Porta de dados do sensor de vazão (GPIO 15 no conector IR)

// ==========================================
// Configurações Wi-Fi e MQTT Broker (Variáveis Editáveis)
// ==========================================
char mqtt_broker[40] = "187.52.106.194";
char mqtt_port_str[6] = "1883";
int mqtt_port = 1883;
char mqtt_username[40] = "sanear";
char mqtt_password[40] = "sanear#123";

// Tags de estruturação do tópico MQTT (ex: sanear/agua/p03)
char sys_name[30] = "sanear";
char sub_name[30] = "agua";
char unit_name[30] = "p03";

// ==========================================
// Variáveis Globais de Operação e Sensores
// ==========================================
enum ModoOperacao {
  MODO_NIVEL = 0,
  MODO_RELOGIO = 1,
  MODO_REMOTO = 2
};
ModoOperacao modo_atual = MODO_NIVEL;
char modo_inicial_str[10] = "nivel"; // Escolha do portal: "nivel", "relogio", "remoto"

// Variáveis de Controle de Nível
int tipo_sensor_nivel = 0;           // 0 = Sensor Danfoss 4-20mA, 1 = Boia Digital Comum
char tipo_sensor_nivel_str[3] = "0";
float nivel_liga = 30.0;             // Setpoint de nível para ligar a bomba (%)
float nivel_desliga = 95.0;          // Setpoint de nível para desligar a bomba (%)
char nivel_liga_str[10] = "30.0";
char nivel_desliga_str[10] = "95.0";

// Estado da boia digital de controle (PCF8574 pino P0 / Pino 1 da Expansão)
// True = Reservatório cheio (contato aberto), False = Reservatório baixo (contato fechado com GND)
bool boia_controle_cheia = false;

// Janela Horária (Modo Relógio)
int hora_inicio = 8;
int min_inicio = 0;
int hora_fim = 17;
int min_fim = 0;
char hora_inicio_str[5] = "8";
char min_inicio_str[5] = "0";
char hora_fim_str[5] = "17";
char min_fim_str[5] = "0";

// Variáveis Físicas
float nivel_percentual = 0.0;
float nivel_mA = 4.0;
bool falha_sensor_nivel = false; // Flag de cabo rompido/sensor com defeito (no modo Danfoss)
float temperatura_c = 0.0;

// Variáveis de Vazão
volatile unsigned long pulseCount = 0;
float vazao_l_min = 0.0;
double volume_total_litros = 0.0;
double volume_total_m3 = 0.0;
float calib_factor = 7.5;     // Fator de calibração padrão (YF-S201: 7.5 pulsos por litro por min)
char calib_factor_str[10] = "7.5";

// Variáveis de Debounce (Filtro de ruído eletromagnético do CCM)
unsigned long tempo_defeito_bomba = 0;
unsigned long tempo_falta_fase = 0;
bool defeito_bomba_confirmado = false;
bool falta_fase_confirmado = false; // Representa falha de fase ou boia de segurança ativa
const unsigned long DEBOUNCE_DELAY = 2000; // 2 segundos para confirmar a falha física

// Variável de controle do estado anterior da bomba
bool ultima_bomba_ligada = false;

// Timers não-bloqueantes
unsigned long previousMillis = 0;
const long interval = 1000;
unsigned long lastMqttRetry = 0;
const long mqttRetryInterval = 10000;
unsigned long lastTelemetryPublish = 0;
const long telemetryInterval = 5000;
unsigned long lastTempRead = 0;
const long tempReadInterval = 4000;
unsigned long lastFlashSaveTime = 0;
const long flashSaveInterval = 600000; // Salvar na Flash a cada 10 minutos (reduz ciclos de escrita)

// Estado do LED
int ledState = LOW;

// NTP Relógio
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const long  gmtOffset_sec = -14400; // GMT-4
const int   daylightOffset_sec = 0;

// Mutex para interrupção do sensor de vazão
portMUX_TYPE flowMux = spinlock_initializer_default;

// Instâncias das classes das bibliotecas
WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_ADS1115 ads;
OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);
Preferences preferences; // Namespace de memoria flash

// Flag e Callback do WiFiManager para salvar configurações
bool deve_salvar_config = false;
void saveConfigCallback() {
  deve_salvar_config = true;
}

// Protótipos de Funções
void setup_relogio();
void callback(char* topic, byte* payload, unsigned int length);
void processar_leitura_sensores();
void acionamento_bombas();
void enviar_telemetria();
bool conectar_mqtt();
void IRAM_ATTR flowPulseCounter();
void salvar_volume();
void carregar_configuracoes();
void salvar_configuracoes();
String getTopic(String subPath);
void comunicacao_wifi();
bool ler_boia_controle_pcf8574();

// ==========================================
// Interrupção do Sensor de Vazão
// ==========================================
void IRAM_ATTR flowPulseCounter() {
  portENTER_CRITICAL_ISR(&flowMux);
  pulseCount++;
  portEXIT_CRITICAL_ISR(&flowMux);
}

// ==========================================
// Auxiliar: Retorna Tópico Dinâmico
// ==========================================
String getTopic(String subPath) {
  return String(sys_name) + "/" + String(sub_name) + "/" + String(unit_name) + "/" + subPath;
}

// ==========================================
// Leitura da Boia Digital no PCF8574 (P0 / Pino 1)
// ==========================================
bool ler_boia_controle_pcf8574() {
  Wire.requestFrom(0x20, 1);
  if (Wire.available()) {
    byte data = Wire.read();
    // P0 é o bit 0. O contato fechado com GND lê '0' (Nível Baixo / liga).
    // O contato aberto (Reservatório cheio) lê '1' (Nível Cheio / desliga).
    return (data & 0x01) != 0; // Retorna true se cheio (aberto), false se vazio (fechado)
  }
  return false; // Fallback em caso de erro na leitura I2C
}

// ==========================================
// Persistência: Carregar Configurações da Flash
// ==========================================
void carregar_configuracoes() {
  preferences.begin("poco3", false);

  preferences.getString("mqtt_host", "187.52.106.194").toCharArray(mqtt_broker, sizeof(mqtt_broker));
  preferences.getString("mqtt_port", "1883").toCharArray(mqtt_port_str, sizeof(mqtt_port_str));
  preferences.getString("mqtt_user", "sanear").toCharArray(mqtt_username, sizeof(mqtt_username));
  preferences.getString("mqtt_pass", "sanear#123").toCharArray(mqtt_password, sizeof(mqtt_password));

  preferences.getString("sys_name", "sanear").toCharArray(sys_name, sizeof(sys_name));
  preferences.getString("sub_name", "agua").toCharArray(sub_name, sizeof(sub_name));
  preferences.getString("unit_name", "p03").toCharArray(unit_name, sizeof(unit_name));

  preferences.getString("calib_fac", "7.5").toCharArray(calib_factor_str, sizeof(calib_factor_str));
  preferences.getString("n_liga", "30.0").toCharArray(nivel_liga_str, sizeof(nivel_liga_str));
  preferences.getString("n_desliga", "95.0").toCharArray(nivel_desliga_str, sizeof(nivel_desliga_str));

  preferences.getString("h_ini", "8").toCharArray(hora_inicio_str, sizeof(hora_inicio_str));
  preferences.getString("m_ini", "0").toCharArray(min_inicio_str, sizeof(min_inicio_str));
  preferences.getString("h_fim", "17").toCharArray(hora_fim_str, sizeof(hora_fim_str));
  preferences.getString("m_fim", "0").toCharArray(min_fim_str, sizeof(min_fim_str));

  preferences.getString("modo_ativo", "nivel").toCharArray(modo_inicial_str, sizeof(modo_inicial_str));
  preferences.getString("ctrl_tipo", "0").toCharArray(tipo_sensor_nivel_str, sizeof(tipo_sensor_nivel_str));

  // Lê também o totalizador de vazão
  volume_total_litros = preferences.getDouble("v_litros", 0.0);
  volume_total_m3 = volume_total_litros / 1000.0;

  preferences.end();

  // Converte as variáveis numéricas correspondentes
  mqtt_port = atoi(mqtt_port_str);
  calib_factor = atof(calib_factor_str);
  nivel_liga = atof(nivel_liga_str);
  nivel_desliga = atof(nivel_desliga_str);
  hora_inicio = atoi(hora_inicio_str);
  min_inicio = atoi(min_inicio_str);
  hora_fim = atoi(hora_fim_str);
  min_fim = atoi(min_fim_str);
  tipo_sensor_nivel = atoi(tipo_sensor_nivel_str);

  // Inicializa o modo atual de operação
  if (strcmp(modo_inicial_str, "nivel") == 0) {
    modo_atual = MODO_NIVEL;
  } else if (strcmp(modo_inicial_str, "relogio") == 0) {
    modo_atual = MODO_RELOGIO;
  } else if (strcmp(modo_inicial_str, "remoto") == 0) {
    modo_atual = MODO_REMOTO;
  }
}

// ==========================================
// Persistência: Salvar Configurações na Flash
// ==========================================
void salvar_configuracoes() {
  preferences.begin("poco3", false);

  preferences.putString("mqtt_host", mqtt_broker);
  preferences.putString("mqtt_port", mqtt_port_str);
  preferences.putString("mqtt_user", mqtt_username);
  preferences.putString("mqtt_pass", mqtt_password);

  preferences.putString("sys_name", sys_name);
  preferences.putString("sub_name", sub_name);
  preferences.putString("unit_name", unit_name);

  preferences.putString("calib_fac", calib_factor_str);
  preferences.putString("n_liga", nivel_liga_str);
  preferences.putString("n_desliga", nivel_desliga_str);

  preferences.putString("h_ini", hora_inicio_str);
  preferences.putString("m_ini", min_inicio_str);
  preferences.putString("h_fim", hora_fim_str);
  preferences.putString("m_fim", min_fim_str);

  preferences.putString("modo_ativo", modo_inicial_str);
  preferences.putString("ctrl_tipo", tipo_sensor_nivel_str);

  preferences.end();
  Serial.println("Configuracoes salvas com sucesso na Flash NVS.");
}

// ==========================================
// Wi-Fi: Gerenciador de Configuração WiFiManager
// ==========================================
void comunicacao_wifi() {
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);

  // Instanciação dos parâmetros adicionais no portal web
  WiFiManagerParameter custom_modo_inicial("modo_op", "Modo Inicial (nivel, relogio, remoto)", modo_inicial_str, 10);
  WiFiManagerParameter custom_tipo_sensor("tipo_nv", "Ctrl Nivel (0=Danfoss, 1=Boia)", tipo_sensor_nivel_str, 3);
  WiFiManagerParameter custom_sys_name("sys", "Sistema (ex: sanear)", sys_name, 30);
  WiFiManagerParameter custom_sub_name("sub", "Subsistema (ex: agua)", sub_name, 30);
  WiFiManagerParameter custom_unit_name("unit", "Unidade (ex: p03)", unit_name, 30);
  WiFiManagerParameter custom_mqtt_server("server", "Broker MQTT IP", mqtt_broker, 40);
  WiFiManagerParameter custom_mqtt_port("port", "Porta MQTT", mqtt_port_str, 6);
  WiFiManagerParameter custom_mqtt_user("user", "Usuario MQTT", mqtt_username, 40);
  WiFiManagerParameter custom_mqtt_pass("pass", "Senha MQTT", mqtt_password, 40);
  WiFiManagerParameter custom_calib_factor("calib", "Calib Vazao (pulsos/L)", calib_factor_str, 10);
  WiFiManagerParameter custom_nivel_liga("nl_liga", "Nivel Liga (%)", nivel_liga_str, 10);
  WiFiManagerParameter custom_nivel_desliga("nl_desliga", "Nivel Desliga (%)", nivel_desliga_str, 10);
  WiFiManagerParameter custom_hora_ini("h_ini", "Hora Inicio Relogio", hora_inicio_str, 5);
  WiFiManagerParameter custom_min_ini("m_ini", "Min Inicio Relogio", min_inicio_str, 5);
  WiFiManagerParameter custom_hora_fim("h_fim", "Hora Fim Relogio", hora_fim_str, 5);
  WiFiManagerParameter custom_min_fim("m_fim", "Min Fim Relogio", min_fim_str, 5);

  // Adiciona os parâmetros adicionais na página web
  wm.addParameter(&custom_modo_inicial);
  wm.addParameter(&custom_tipo_sensor);
  wm.addParameter(&custom_sys_name);
  wm.addParameter(&custom_sub_name);
  wm.addParameter(&custom_unit_name);
  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_port);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);
  wm.addParameter(&custom_calib_factor);
  wm.addParameter(&custom_nivel_liga);
  wm.addParameter(&custom_nivel_desliga);
  wm.addParameter(&custom_hora_ini);
  wm.addParameter(&custom_min_ini);
  wm.addParameter(&custom_hora_fim);
  wm.addParameter(&custom_min_fim);

  Serial.println("Abrindo gerenciador de conexao WiFiManager...");
  bool res = wm.autoConnect("AutoConnectAP", "password");

  if (!res) {
    Serial.println("Falha na conexao. Continuando sem internet...");
  } else {
    Serial.println("Wi-Fi Conectado!");
    
    // Se o usuário alterou dados na página web de configuração, salva na flash
    if (deve_salvar_config) {
      strcpy(modo_inicial_str, custom_modo_inicial.getValue());
      strcpy(tipo_sensor_nivel_str, custom_tipo_sensor.getValue());
      strcpy(sys_name, custom_sys_name.getValue());
      strcpy(sub_name, custom_sub_name.getValue());
      strcpy(unit_name, custom_unit_name.getValue());
      strcpy(mqtt_broker, custom_mqtt_server.getValue());
      strcpy(mqtt_port_str, custom_mqtt_port.getValue());
      strcpy(mqtt_username, custom_mqtt_user.getValue());
      strcpy(mqtt_password, custom_mqtt_pass.getValue());
      strcpy(calib_factor_str, custom_calib_factor.getValue());
      strcpy(nivel_liga_str, custom_nivel_liga.getValue());
      strcpy(nivel_desliga_str, custom_nivel_desliga.getValue());
      strcpy(hora_inicio_str, custom_hora_ini.getValue());
      strcpy(min_inicio_str, custom_min_ini.getValue());
      strcpy(hora_fim_str, custom_hora_fim.getValue());
      strcpy(min_fim_str, custom_min_fim.getValue());

      salvar_configuracoes();
      carregar_configuracoes(); // Atualiza as variáveis de trabalho com os novos dados
    }
  }
}

// ==========================================
// Configuração Inicial (Setup)
// ==========================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- ESP32 AUTO POCO INICIANDO ---");

  // Configuração das Entradas (Optoacopladas)
  pinMode(sistema_automatico, INPUT);
  pinMode(bomba_ligada, INPUT);
  pinMode(bomba_defeito, INPUT);
  pinMode(falta_fase_ou_nivel, INPUT);

  // Configuração das Saídas
  pinMode(ligar_bomba, OUTPUT);
  pinMode(reset_bomba, OUTPUT);
  pinMode(acionar_sirene, OUTPUT);
  pinMode(K4, OUTPUT);
  pinMode(ledPin, OUTPUT);
  
  digitalWrite(ligar_bomba, LOW);
  digitalWrite(reset_bomba, LOW);
  digitalWrite(acionar_sirene, LOW);
  digitalWrite(K4, LOW);

  // Primeiro carrega as configurações da Flash (se existirem)
  carregar_configuracoes();

  // Inicialização do Barramento I2C e Conversor ADS1115
  Wire.begin(21, 22); // SDA = 21, SCL = 22
  if (!ads.begin(0x48, &Wire)) {
    Serial.println("Erro: Conversor ADS1115 nao encontrado!");
  } else {
    ads.setGain(GAIN_ONE); // 1x gain   +/- 4.096V  1 bit = 0.125mV
    Serial.println("ADS1115 inicializado com sucesso.");
  }

  // Inicialização do Sensor de Temperatura
  sensors.begin();
  Serial.println("Sensor de Temperatura DS18B20 inicializado.");

  // Configuração do Sensor de Vazão (Interrupt no GPIO 15)
  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseCounter, FALLING);
  Serial.println("Sensor de vazao configurado no GPIO 15.");

  // Configuração Wi-Fi (WiFiManager) e Carregamento de Parâmetros
  comunicacao_wifi();

  // Configuração do Cliente MQTT
  client.setServer(mqtt_broker, mqtt_port);
  client.setCallback(callback);

  // Inicializa o relógio NTP
  setup_relogio();
}

// ==========================================
// Loop Principal
// ==========================================
void loop() {
  unsigned long currentMillis = millis();

  // 1. Lógica do painel e acionamentos físicos
  acionamento_bombas();

  // 2. Reconexão MQTT Não-Bloqueante
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      if (currentMillis - lastMqttRetry >= mqttRetryInterval) {
        lastMqttRetry = currentMillis;
        if (conectar_mqtt()) {
          Serial.printf("Conectado ao broker MQTT (%s:%d)!\n", mqtt_broker, mqtt_port);
        }
      }
    } else {
      client.loop();
    }
  }

  // 3. Processamento das leituras físicas (Média temporal)
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    ledState = !ledState;
    digitalWrite(ledPin, ledState);

    // Lê sensores locais e calcula vazão/nível
    processar_leitura_sensores();

    // 4. Envio periódico da Telemetria via MQTT
    if (client.connected() && (currentMillis - lastTelemetryPublish >= telemetryInterval)) {
      lastTelemetryPublish = currentMillis;
      enviar_telemetria();
    }

    // 5. Salva na Flash periodicamente (a cada 10 min)
    if (currentMillis - lastFlashSaveTime >= flashSaveInterval) {
      lastFlashSaveTime = currentMillis;
      salvar_volume();
    }
  }
}

// ==========================================
// Leitura dos Sensores (Nível, Vazão e Temp)
// ==========================================
void processar_leitura_sensores() {
  // --- LEITURA DO NÍVEL ANALÓGICO (ADS1115 A0 - Danfoss 4-20mA) ---
  if (tipo_sensor_nivel == 0) {
    int16_t adc0 = ads.readADC_SingleEnded(0);
    float volts = ads.computeVolts(adc0);
    nivel_mA = volts * (1000.0 / 150.0);
    
    if (nivel_mA < 3.6 || nivel_mA > 21.0) {
      falha_sensor_nivel = true;
      nivel_percentual = 0.0;
      Serial.printf("ALERTA DE FALHA: Cabo rompido ou sensor de nivel em curto! (Leitura: %.2fmA)\n", nivel_mA);
    } else {
      falha_sensor_nivel = false;
      nivel_percentual = ((nivel_mA - 4.0) / 16.0) * 100.0;
      if (nivel_percentual < 0.0) nivel_percentual = 0.0;
      if (nivel_percentual > 100.0) nivel_percentual = 100.0;
    }
  } 
  // --- LEITURA DO NÍVEL DIGITAL (Boia de controle no PCF8574) ---
  else {
    falha_sensor_nivel = false; // Desativa falha analógica
    nivel_mA = 0.0;             // Inutilizado
    boia_controle_cheia = ler_boia_controle_pcf8574();
    nivel_percentual = boia_controle_cheia ? 100.0 : 0.0; // Representa 0 ou 100%
  }

  // --- LEITURA DE VAZÃO (Cálculo de Pulsos) ---
  unsigned long pulses;
  portENTER_CRITICAL(&flowMux);
  pulses = pulseCount;
  pulseCount = 0;
  portEXIT_CRITICAL(&flowMux);

  vazao_l_min = (float)pulses / calib_factor;
  volume_total_litros += (vazao_l_min / 60.0);
  volume_total_m3 = volume_total_litros / 1000.0;

  // --- LEITURA DE TEMPERATURA (DS18B20 no GPIO 14) ---
  if (millis() - lastTempRead >= tempReadInterval) {
    lastTempRead = millis();
    sensors.requestTemperatures();
    float t = sensors.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C) {
      temperatura_c = t;
    } else {
      Serial.println("Erro: Sensor de temperatura desconectado!");
    }
  }

  // Debug Serial local
  String desc_sensor = (tipo_sensor_nivel == 0) ? "Danfoss" : "Boia";
  if (falha_sensor_nivel) {
    Serial.printf("[%s/%s/%s] STATUS: [ALERTA DE NIVEL (%s)] | Vazao: %.2f L/m | Vol: %.3f m3 | Temp: %.1fC\n", 
                  sys_name, sub_name, unit_name, desc_sensor.c_str(), vazao_l_min, volume_total_m3, temperatura_c);
  } else {
    if (tipo_sensor_nivel == 0) {
      Serial.printf("[%s/%s/%s] STATUS: Nivel (%s): %.1f%% (%.2fmA) | Vazao: %.2f L/m | Vol: %.3f m3 | Temp: %.1fC\n", 
                    sys_name, sub_name, unit_name, desc_sensor.c_str(), nivel_percentual, nivel_mA, vazao_l_min, volume_total_m3, temperatura_c);
    } else {
      Serial.printf("[%s/%s/%s] STATUS: Nivel (%s): %s | Vazao: %.2f L/m | Vol: %.3f m3 | Temp: %.1fC\n", 
                    sys_name, sub_name, unit_name, desc_sensor.c_str(), boia_controle_cheia ? "CHEIO" : "BAIXO", vazao_l_min, volume_total_m3, temperatura_c);
    }
  }
}

// ==========================================
// Lógica de Acionamento da Bomba (Segurança)
// ==========================================
void acionamento_bombas() {
  bool is_ccm_automatico = (digitalRead(sistema_automatico) == LOW); // LOW = Automatico
  bool raw_defeito       = (digitalRead(bomba_defeito) == HIGH);    // HIGH = Aberto/Falha
  bool raw_falta         = (digitalRead(falta_fase_ou_nivel) == HIGH); // HIGH = Aberto/Falha (Fase ou Boia ativada)
  
  // 1. Debounce das falhas fisicas (2 segundos)
  if (raw_defeito) {
    if (tempo_defeito_bomba == 0) tempo_defeito_bomba = millis();
    else if (millis() - tempo_defeito_bomba >= DEBOUNCE_DELAY) {
      defeito_bomba_confirmado = true;
    }
  } else {
    tempo_defeito_bomba = 0;
    defeito_bomba_confirmado = false;
  }

  if (raw_falta) {
    if (tempo_falta_fase == 0) tempo_falta_fase = millis();
    else if (millis() - tempo_falta_fase >= DEBOUNCE_DELAY) {
      falta_fase_confirmado = true;
    }
  } else {
    tempo_falta_fase = 0;
    falta_fase_confirmado = false;
  }

  // 2. Seletor do painel em MANUAL
  if (!is_ccm_automatico) {
    digitalWrite(ligar_bomba, LOW);
    return;
  }

  // 3. Trava em caso de qualquer Defeito Ativo
  if (defeito_bomba_confirmado || falta_fase_confirmado || falha_sensor_nivel) {
    digitalWrite(ligar_bomba, LOW);
    return;
  }

  // Rastreia se houve alteração no estado da bomba para salvar na flash
  bool bomba_ligada_agora = (digitalRead(bomba_ligada) == LOW);
  if (ultima_bomba_ligada && !bomba_ligada_agora) {
    salvar_volume();
  }
  ultima_bomba_ligada = bomba_ligada_agora;

  // 4. MODO AUTOMÁTICO DE NÍVEL
  if (modo_atual == MODO_NIVEL) {
    if (tipo_sensor_nivel == 0) {
      // Controle via Danfoss 4-20mA
      if (nivel_percentual <= nivel_liga) {
        digitalWrite(ligar_bomba, HIGH);
      } 
      else if (nivel_percentual >= nivel_desliga) {
        digitalWrite(ligar_bomba, LOW);
      }
    } else {
      // Controle via Boia Digital Comum (pino P0 do PCF8574)
      if (!boia_controle_cheia) {
        digitalWrite(ligar_bomba, HIGH); // Reservatório vazio -> Liga
      } else {
        digitalWrite(ligar_bomba, LOW);  // Reservatório cheio -> Desliga
      }
    }
  } 
  
  // 5. MODO RELÓGIO (Com proteção de transbordo integrada)
  else if (modo_atual == MODO_RELOGIO) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      int min_atual_dia = timeinfo.tm_hour * 60 + timeinfo.tm_min;
      int min_ini_dia = hora_inicio * 60 + min_inicio;
      int min_fim_dia = hora_fim * 60 + min_fim;
      
      bool dentro_horario = false;
      if (min_ini_dia < min_fim_dia) {
        dentro_horario = (min_atual_dia >= min_ini_dia && min_atual_dia < min_fim_dia);
      } else {
        dentro_horario = (min_atual_dia >= min_ini_dia || min_atual_dia < min_fim_dia);
      }

      if (dentro_horario) {
        // Horário ativo: Liga, a menos que o reservatório já esteja cheio
        if (tipo_sensor_nivel == 0 && nivel_percentual >= nivel_desliga) {
          digitalWrite(ligar_bomba, LOW); // Trava Danfoss
        } 
        else if (tipo_sensor_nivel == 1 && boia_controle_cheia) {
          digitalWrite(ligar_bomba, LOW); // Trava Boia Digital
        } 
        else {
          digitalWrite(ligar_bomba, HIGH); // Nível ok: aciona bomba
        }
      } else {
        digitalWrite(ligar_bomba, LOW); // Fora do horário configurado
      }
    } else {
      digitalWrite(ligar_bomba, LOW); // Sem sincronização NTP
    }
  } 
  
  // 6. MODO REMOTO (Controle MQTT direto)
  else if (modo_atual == MODO_REMOTO) {
    if (rem_ligar_bomba == 1) {
      digitalWrite(ligar_bomba, HIGH);
    } else {
      digitalWrite(ligar_bomba, LOW);
    }
  }
}

// ==========================================
// Gravação na Flash (NVS) do Totalizador
// ==========================================
void salvar_volume() {
  preferences.begin("poco3", false);
  preferences.putDouble("v_litros", volume_total_litros);
  preferences.end();
  Serial.printf(">>> [NVS FLASH] Volume total salvo na Flash: %.2f L (%.3f m3)\n", volume_total_litros, volume_total_m3);
}

// ==========================================
// Configuração do Horário (SNTP / NTP)
// ==========================================
void timeavailable(struct timeval *t) {
  Serial.println("Relogio atualizado com sucesso via NTP!");
}

void setup_relogio() {
  sntp_set_time_sync_notification_cb(timeavailable);
  sntp_servermode_dhcp(1);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
}

// ==========================================
// Reconexão MQTT Não-Bloqueante
// ==========================================
bool conectar_mqtt() {
  String client_id = "esp32-";
  client_id += String(sys_name) + "-" + String(unit_name) + "-";
  client_id += String(WiFi.macAddress());
  
  if (client.connect(client_id.c_str(), mqtt_username, mqtt_password)) {
    client.subscribe(getTopic("cmd/modo").c_str());
    client.subscribe(getTopic("cmd/bomba").c_str());
    client.subscribe(getTopic("cmd/config/horario").c_str());
    client.subscribe(getTopic("cmd/config/setpoints").c_str());
    client.subscribe(getTopic("cmd/reset").c_str());
    client.subscribe(getTopic("cmd/sirene").c_str());
    return true;
  }
  return false;
}

// ==========================================
// Recebimento de Comandos MQTT (Callback)
// ==========================================
void callback(char* topic, byte* payload, unsigned int length) {
  String messageTemp = "";
  for (int i = 0; i < length; i++) {
    messageTemp += (char)payload[i];
  }
  
  Serial.printf("Mensagem MQTT recebida no topico: %s | Conteudo: %s\n", topic, messageTemp.c_str());
  String topicStr = String(topic);

  // 1. Comando de Modo de Operação (Sincroniza com o boot/NVS)
  if (topicStr == getTopic("cmd/modo")) {
    if (messageTemp == "nivel") {
      modo_atual = MODO_NIVEL;
      strcpy(modo_inicial_str, "nivel");
      salvar_configuracoes();
      Serial.println("Modo alterado para: NIVEL (Salvo como padrão)");
    } else if (messageTemp == "relogio") {
      modo_atual = MODO_RELOGIO;
      strcpy(modo_inicial_str, "relogio");
      salvar_configuracoes();
      Serial.println("Modo alterado para: RELOGIO (Salvo como padrão)");
    } else if (messageTemp == "remoto") {
      modo_atual = MODO_REMOTO;
      strcpy(modo_inicial_str, "remoto");
      salvar_configuracoes();
      Serial.println("Modo alterado para: REMOTO (Salvo como padrão)");
    }
  } 
  
  // 2. Comando de Acionamento Remoto (Bomba)
  else if (topicStr == getTopic("cmd/bomba")) {
    if (messageTemp == "1") {
      rem_ligar_bomba = 1;
      Serial.println("Comando remoto recebido: LIGAR BOMBA");
    } else if (messageTemp == "0") {
      rem_ligar_bomba = 0;
      Serial.println("Comando remoto recebido: DESLIGAR BOMBA");
    }
  } 
  
  // 3. Configuração de Janela Horária do Modo Relógio (Salva em Flash)
  else if (topicStr == getTopic("cmd/config/horario")) {
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, messageTemp);
    if (!error) {
      if (doc.containsKey("h_ini")) hora_inicio = doc["h_ini"];
      if (doc.containsKey("m_ini")) min_inicio = doc["m_ini"];
      if (doc.containsKey("h_fim")) hora_fim = doc["h_fim"];
      if (doc.containsKey("m_fim")) min_fim = doc["m_fim"];
      
      sprintf(hora_inicio_str, "%d", hora_inicio);
      sprintf(min_inicio_str, "%d", min_inicio);
      sprintf(hora_fim_str, "%d", hora_fim);
      sprintf(min_fim_str, "%d", min_fim);
      
      salvar_configuracoes();
      Serial.printf("Config Horaria: das %02d:%02d as %02d:%02d (Salvo na Flash)\n", 
                    hora_inicio, min_inicio, hora_fim, min_fim);
    } else {
      Serial.println("Falha ao analisar JSON de configuracao horaria.");
    }
  } 
  
  // 4. Configuração de Setpoints de Nível (Salva em Flash)
  else if (topicStr == getTopic("cmd/config/setpoints")) {
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, messageTemp);
    if (!error) {
      if (doc.containsKey("nivel_liga")) nivel_liga = doc["nivel_liga"];
      if (doc.containsKey("nivel_desliga")) nivel_desliga = doc["nivel_desliga"];
      
      dtostrf(nivel_liga, 4, 1, nivel_liga_str);
      dtostrf(nivel_desliga, 4, 1, nivel_desliga_str);
      
      salvar_configuracoes();
      Serial.printf("Setpoints de Nivel: Liga com %.1f%% | Desliga com %.1f%% (Salvo na Flash)\n", 
                    nivel_liga, nivel_desliga);
    } else {
      Serial.println("Falha ao analisar JSON de setpoints de nivel.");
    }
  } 
  
  // 5. Comando de Reset do Rele Térmico (Pulso em K2)
  else if (topicStr == getTopic("cmd/reset")) {
    if (messageTemp == "1") {
      Serial.println("Enviando pulso de Reset na Bomba (K2)...");
      digitalWrite(reset_bomba, HIGH);
      delay(1000); // Pulso de 1 segundo
      digitalWrite(reset_bomba, LOW);
      Serial.println("Pulso de reset concluido.");
    }
  }

  // 6. Comando para acionar a Sirene de Alarme (Relé K3)
  else if (topicStr == getTopic("cmd/sirene")) {
    if (messageTemp == "1") {
      digitalWrite(acionar_sirene, HIGH);
      Serial.println("SIRENE ATIVADA VIA MQTT!");
    } else if (messageTemp == "0") {
      digitalWrite(acionar_sirene, LOW);
      Serial.println("Sirene desligada via MQTT.");
    }
  }
}

// ==========================================
// Envio de Telemetria via MQTT (Publicação)
// ==========================================
void enviar_telemetria() {
  StaticJsonDocument<500> doc;
  
  String str_modo = "nivel";
  if (modo_atual == MODO_RELOGIO) str_modo = "relogio";
  else if (modo_atual == MODO_REMOTO) str_modo = "remoto";

  bool is_ccm_automatico = (digitalRead(sistema_automatico) == LOW);
  String str_painel = is_ccm_automatico ? "automatico" : "manual";

  // Preenche o documento JSON
  JsonObject bomba = doc.createNestedObject("bomba");
  bomba["ligada"] = (digitalRead(bomba_ligada) == LOW);
  bomba["defeito"] = defeito_bomba_confirmado;
  bomba["falta_fase_ou_nivel"] = falta_fase_confirmado;
  bomba["modo_painel"] = str_painel;
  bomba["sirene"] = (digitalRead(acionar_sirene) == HIGH);

  JsonObject controle = doc.createNestedObject("controle");
  controle["modo_operacao"] = str_modo;
  controle["tipo_sensor_nivel"] = tipo_sensor_nivel; // 0 = Danfoss, 1 = Boia Digital
  controle["nivel_percentual"] = round(nivel_percentual * 10.0) / 10.0;
  controle["nivel_mA"] = round(nivel_mA * 100.0) / 100.0;
  controle["nivel_liga"] = nivel_liga;
  controle["nivel_desliga"] = nivel_desliga;
  controle["nivel_falha"] = falha_sensor_nivel;
  controle["boia_controle_cheia"] = boia_controle_cheia; // Status da boia de controle digital

  JsonObject sensores = doc.createNestedObject("sensores");
  sensores["vazao_l_min"] = round(vazao_l_min * 10.0) / 10.0;
  sensores["volume_m3"] = round(volume_total_m3 * 1000.0) / 1000.0;
  sensores["temperatura_c"] = round(temperatura_c * 10.0) / 10.0;

  doc["uptime_s"] = millis() / 1000;

  // Serializa e envia
  char buffer[512];
  serializeJson(doc, buffer);
  
  String t_topic = getTopic("telemetria");
  if (client.publish(t_topic.c_str(), buffer)) {
    Serial.printf("Telemetria enviada via MQTT no topico: %s\n", t_topic.c_str());
  } else {
    Serial.println("Erro ao publicar telemetria no broker.");
  }
}
