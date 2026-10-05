/**
 * ==============================================================================
 * PROJETO: Sistema de Automação e Telemetria de Poço Artesiano
 * AUTOR: João Couto (com auxílio de Antigravity AI)
 * DATA: 15 de Julho de 2026 (Revisado em 05 de Outubro de 2026)
 * VERSÃO: 11.0 (Grau Industrial - 100% Robusto e Resiliente a Falhas)
 * ==============================================================================
 * 
 * MELHORIAS DE ROBUSTEZ IMPLEMENTADAS NA V11.0:
 *   1. Hardware Watchdog Timer (WDT): Reinicialização automática em caso de travamento.
 *   2. PubSubClient Buffer Expandido (1024 bytes): Previne perda silenciosa da telemetria.
 *   3. WiFiManager com Timeout (180s): Impede travamento em blecautes/reinicializações do roteador.
 *   4. Reconexão Wi-Fi Ativa no Loop: Recupera conexão mesmo se o roteador cair após o boot.
 *   5. Leitura Não-Bloqueante do DS18B20: Elimina congelamento de 750ms a cada leitura de temperatura.
 *   6. Reset da Bomba Não-Bloqueante: Elimina delay(1000) dentro da rotina de callback do MQTT.
 *   7. Timeout de Segurança da Sirene (5 min): Impede queima da sirene se a rede cair com ela ligada.
 *   8. Fail-Safe no Expansor PCF8574: Falha no I2C desliga a bomba por segurança contra transbordo.
 *   9. Proteção contra Estouro de Buffer: Uso de strncpy seguro em todos os campos do portal web.
 *  10. Antirrepique na Boia Digital: Previne oscilações rápidas no contator causadas por ondulação na água.
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
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "time.h"
#include "sntp.h"

// ==========================================
// Mapeamento de Hardware (Pinos)
// ==========================================

// Entradas Digitais Optoacopladas (Lógica Invertida: LOW = Ativo/Tensão Presente)
#define sistema_automatico   35 // I1 (Automático local do CCM)
#define bomba_ligada         34 // I2 (Feedback da bomba ligada)
#define bomba_defeito        39 // I3 (Relé térmico/defeito da bomba)
#define falta_fase_ou_nivel  36 // I4 (Falta de fase ou Boia de segurança do reservatório)

// Saídas Digitais a Relé
#define ligar_bomba         13 // K1 (Comando liga bomba)
#define reset_bomba         12 // K2 (Comando reset defeito bomba)
#define acionar_sirene      27 // K3 (Relé para acionar a Sirene de Alarme)
#define K4                  26 // Saída auxiliar K4
#define ledPin              2  // LED_BUILTIN do ESP32

// Portas Auxiliares de Sensores
#define TEMP_PIN            14 // Porta de dados de temperatura (DT)
#define FLOW_PIN            15 // Porta de dados do sensor de vazão (GPIO 15 no conector IR)

// Constante de Watchdog
#define WDT_TIMEOUT_SECONDS 10

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
char modo_inicial_str[10] = "nivel";

// Variáveis de Controle de Nível
int tipo_sensor_nivel = 0;           // 0 = Sensor Danfoss 4-20mA, 1 = Boia Digital Comum
char tipo_sensor_nivel_str[3] = "0";
float nivel_liga = 30.0;             // Setpoint de nível para ligar a bomba (%)
float nivel_desliga = 95.0;          // Setpoint de nível para desligar a bomba (%)
char nivel_liga_str[10] = "30.0";
char nivel_desliga_str[10] = "95.0";

// Estado da boia digital de controle (PCF8574 pino P0 / Pino 1 da Expansão)
bool boia_controle_cheia = false;
bool falha_i2c_boia = false;
unsigned long tempo_filtro_boia = 0;
bool boia_estado_bruto_anterior = false;

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
double last_saved_volume_litros = 0.0;
float calib_factor = 7.5;     // Fator de calibração padrão (YF-S201: 7.5 pulsos por litro por min)
char calib_factor_str[10] = "7.5";

// Variáveis de Debounce (Filtro de ruído eletromagnético do CCM)
unsigned long tempo_defeito_bomba = 0;
unsigned long tempo_falta_fase = 0;
bool defeito_bomba_confirmado = false;
bool falta_fase_confirmado = false; // Falha de fase ou boia de segurança ativa
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
const long flashSaveInterval = 600000; // Salvar na Flash a cada 10 minutos (se houver alteração)
unsigned long lastWiFiCheck = 0;
const long wifiCheckInterval = 30000; // Verificar conexão Wi-Fi a cada 30 segundos

// Controle Não-Bloqueante do Reset da Bomba
bool reset_em_andamento = false;
unsigned long tempo_inicio_reset = 0;

// Controle de Timeout de Segurança da Sirene
bool sirene_ativa = false;
unsigned long tempo_inicio_sirene = 0;
const unsigned long SIRENE_TIMEOUT_MAX = 300000; // 5 minutos máximo contínuo

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
Preferences preferences; // Namespace de memória flash

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
void gerenciar_temporizadores_seguranca();
void enviar_telemetria();
bool conectar_mqtt();
void IRAM_ATTR flowPulseCounter();
void salvar_volume();
void carregar_configuracoes();
void salvar_configuracoes();
String getTopic(String subPath);
void comunicacao_wifi();
bool ler_boia_controle_pcf8574();
void inicializar_watchdog();

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
// Leitura da Boia Digital no PCF8574 (Fail-Safe)
// ==========================================
bool ler_boia_controle_pcf8574() {
  Wire.requestFrom(0x20, 1);
  if (Wire.available()) {
    byte data = Wire.read();
    falha_i2c_boia = false;
    // P0 é o bit 0. 
    // Aberto (Nível Cheio) = 1. Fechado com GND (Nível Baixo) = 0.
    bool estado_bruto = ((data & 0x01) != 0);

    // Antirrepique de 3 segundos para ondulações no reservatório
    if (estado_bruto != boia_estado_bruto_anterior) {
      boia_estado_bruto_anterior = estado_bruto;
      tempo_filtro_boia = millis();
    } else if (millis() - tempo_filtro_boia >= 3000) {
      boia_controle_cheia = estado_bruto;
    }
    return boia_controle_cheia;
  }
  
  // FAIL-SAFE: Se o I2C falhar, assume reservatório cheio para não transbordar
  falha_i2c_boia = true;
  return true;
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

  // Totalizador de vazão persistente
  volume_total_litros = preferences.getDouble("v_litros", 0.0);
  volume_total_m3 = volume_total_litros / 1000.0;
  last_saved_volume_litros = volume_total_litros;

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
  Serial.println("Configurações salvas com sucesso na Flash NVS.");
}

// ==========================================
// Wi-Fi: Gerenciador com Timeout Resiliente
// ==========================================
void comunicacao_wifi() {
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);

  // Timeout de 180 segundos no portal. Se ninguém configurar, segue a execução local!
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(20);

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

  Serial.println("Iniciando conexão Wi-Fi (Timeout de portal: 180s)...");
  bool res = wm.autoConnect("AutoConnectAP", "password");

  if (!res) {
    Serial.println("Aviso: Timeout no portal de configuração Wi-Fi. Continuando operação local...");
  } else {
    Serial.println("Wi-Fi Conectado com sucesso!");
    
    if (deve_salvar_config) {
      strncpy(modo_inicial_str, custom_modo_inicial.getValue(), sizeof(modo_inicial_str) - 1);
      strncpy(tipo_sensor_nivel_str, custom_tipo_sensor.getValue(), sizeof(tipo_sensor_nivel_str) - 1);
      strncpy(sys_name, custom_sys_name.getValue(), sizeof(sys_name) - 1);
      strncpy(sub_name, custom_sub_name.getValue(), sizeof(sub_name) - 1);
      strncpy(unit_name, custom_unit_name.getValue(), sizeof(unit_name) - 1);
      strncpy(mqtt_broker, custom_mqtt_server.getValue(), sizeof(mqtt_broker) - 1);
      strncpy(mqtt_port_str, custom_mqtt_port.getValue(), sizeof(mqtt_port_str) - 1);
      strncpy(mqtt_username, custom_mqtt_user.getValue(), sizeof(mqtt_username) - 1);
      strncpy(mqtt_password, custom_mqtt_pass.getValue(), sizeof(mqtt_password) - 1);
      strncpy(calib_factor_str, custom_calib_factor.getValue(), sizeof(calib_factor_str) - 1);
      strncpy(nivel_liga_str, custom_nivel_liga.getValue(), sizeof(nivel_liga_str) - 1);
      strncpy(nivel_desliga_str, custom_nivel_desliga.getValue(), sizeof(nivel_desliga_str) - 1);
      strncpy(hora_inicio_str, custom_hora_ini.getValue(), sizeof(hora_inicio_str) - 1);
      strncpy(min_inicio_str, custom_min_ini.getValue(), sizeof(min_inicio_str) - 1);
      strncpy(hora_fim_str, custom_hora_fim.getValue(), sizeof(hora_fim_str) - 1);
      strncpy(min_fim_str, custom_min_fim.getValue(), sizeof(min_fim_str) - 1);

      salvar_configuracoes();
      carregar_configuracoes();
    }
  }
}

// ==========================================
// Watchdog Timer de Hardware
// ==========================================
void inicializar_watchdog() {
#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&twdt_config);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
  esp_task_wdt_add(NULL);
#endif
  Serial.printf("Watchdog Timer ativado (%d segundos).\n", WDT_TIMEOUT_SECONDS);
}

// ==========================================
// Configuração Inicial (Setup)
// ==========================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- ESP32 AUTO POCO INICIANDO (V11.0 ROBUSTO) ---");

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

  // Carrega configurações da Flash
  carregar_configuracoes();

  // Inicialização do Barramento I2C e Conversor ADS1115
  Wire.begin(21, 22);
  Wire.setTimeOut(50); // Timeout de 50ms para evitar bloqueios na I2C
  if (!ads.begin(0x48, &Wire)) {
    Serial.println("Alerta: Conversor ADS1115 não detectado no I2C!");
  } else {
    ads.setGain(GAIN_ONE);
    Serial.println("ADS1115 inicializado com sucesso.");
  }

  // Inicialização Assíncrona do Sensor DS18B20 (Sem bloqueio de 750ms!)
  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();
  Serial.println("Sensor de Temperatura DS18B20 configurado (Modo Assíncrono).");

  // Configuração do Sensor de Vazão (Interrupt no GPIO 15)
  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseCounter, FALLING);
  Serial.println("Sensor de vazão configurado no GPIO 15.");

  // Configuração Wi-Fi (WiFiManager)
  comunicacao_wifi();

  // Configuração do Cliente MQTT com Buffer Expandido
  client.setServer(mqtt_broker, mqtt_port);
  client.setCallback(callback);
  client.setBufferSize(1024); // CRÍTICO: Permite envio completo do payload JSON de 400+ bytes!

  // Inicializa o relógio NTP
  setup_relogio();

  // Ativa o Watchdog de Hardware
  inicializar_watchdog();
}

// ==========================================
// Loop Principal
// ==========================================
void loop() {
  // Alimenta o Watchdog Timer
  esp_task_wdt_reset();

  unsigned long currentMillis = millis();

  // 1. Gerenciamento de segurança física e acionamento da bomba
  acionamento_bombas();
  gerenciar_temporizadores_seguranca();

  // 2. Verificação e Reconexão Wi-Fi Ativa
  if (currentMillis - lastWiFiCheck >= wifiCheckInterval) {
    lastWiFiCheck = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Aviso: Conexão Wi-Fi perdida. Tentando reconectar...");
      WiFi.reconnect();
    }
  }

  // 3. Reconexão MQTT Não-Bloqueante
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

  // 4. Processamento das leituras físicas (Média temporal)
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    ledState = !ledState;
    digitalWrite(ledPin, ledState);

    // Lê sensores locais e calcula vazão/nível
    processar_leitura_sensores();

    // 5. Envio periódico da Telemetria via MQTT
    if (client.connected() && (currentMillis - lastTelemetryPublish >= telemetryInterval)) {
      lastTelemetryPublish = currentMillis;
      enviar_telemetria();
    }

    // 6. Salva na Flash periodicamente (se houver alteração acumulada)
    if (currentMillis - lastFlashSaveTime >= flashSaveInterval) {
      lastFlashSaveTime = currentMillis;
      if (volume_total_litros != last_saved_volume_litros) {
        salvar_volume();
      }
    }
  }
}

// ==========================================
// Temporizadores Não-Bloqueantes de Segurança
// ==========================================
void gerenciar_temporizadores_seguranca() {
  unsigned long agora = millis();

  // Pulso do Relé de Reset da Bomba (1 segundo)
  if (reset_em_andamento) {
    if (agora - tempo_inicio_reset >= 1000) {
      digitalWrite(reset_bomba, LOW);
      reset_em_andamento = false;
      Serial.println("Pulso de reset finalizado com sucesso.");
    }
  }

  // Auto-Desligamento da Sirene de Alarme (Máximo 5 minutos)
  if (sirene_ativa) {
    if (agora - tempo_inicio_sirene >= SIRENE_TIMEOUT_MAX) {
      digitalWrite(acionar_sirene, LOW);
      sirene_ativa = false;
      Serial.println("Segurança: Timeout atingido. Sirene desligada automaticamente!");
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
      Serial.printf("ALERTA: Cabo rompido ou sensor de nível em curto! (Leitura: %.2fmA)\n", nivel_mA);
    } else {
      falha_sensor_nivel = false;
      nivel_percentual = ((nivel_mA - 4.0) / 16.0) * 100.0;
      if (nivel_percentual < 0.0) nivel_percentual = 0.0;
      if (nivel_percentual > 100.0) nivel_percentual = 100.0;
    }
  } 
  // --- LEITURA DO NÍVEL DIGITAL (Boia de controle no PCF8574) ---
  else {
    falha_sensor_nivel = falha_i2c_boia;
    nivel_mA = 0.0;
    boia_controle_cheia = ler_boia_controle_pcf8574();
    nivel_percentual = boia_controle_cheia ? 100.0 : 0.0;
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

  // --- LEITURA NÃO-BLOQUEANTE DE TEMPERATURA ---
  if (millis() - lastTempRead >= tempReadInterval) {
    lastTempRead = millis();
    float t = sensors.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C && t > -50.0 && t < 125.0) {
      temperatura_c = t;
    }
    // Dispara nova leitura assíncrona para a próxima amostragem
    sensors.requestTemperatures();
  }

  // Debug Serial local
  String desc_sensor = (tipo_sensor_nivel == 0) ? "Danfoss" : "Boia";
  if (falha_sensor_nivel) {
    Serial.printf("[%s/%s/%s] STATUS: [ALERTA DE FALHA (%s)] | Vazão: %.2f L/m | Vol: %.3f m3 | Temp: %.1fC\n", 
                  sys_name, sub_name, unit_name, desc_sensor.c_str(), vazao_l_min, volume_total_m3, temperatura_c);
  } else {
    if (tipo_sensor_nivel == 0) {
      Serial.printf("[%s/%s/%s] STATUS: Nível (%s): %.1f%% (%.2fmA) | Vazão: %.2f L/m | Vol: %.3f m3 | Temp: %.1fC\n", 
                    sys_name, sub_name, unit_name, desc_sensor.c_str(), nivel_percentual, nivel_mA, vazao_l_min, volume_total_m3, temperatura_c);
    } else {
      Serial.printf("[%s/%s/%s] STATUS: Nível (%s): %s | Vazão: %.2f L/m | Vol: %.3f m3 | Temp: %.1fC\n", 
                    sys_name, sub_name, unit_name, desc_sensor.c_str(), boia_controle_cheia ? "CHEIO" : "BAIXO", vazao_l_min, volume_total_m3, temperatura_c);
    }
  }
}

// ==========================================
// Lógica de Acionamento da Bomba (Segurança)
// ==========================================
void acionamento_bombas() {
  bool is_ccm_automatico = (digitalRead(sistema_automatico) == LOW); // LOW = Automático
  bool raw_defeito       = (digitalRead(bomba_defeito) == HIGH);    // HIGH = Aberto/Falha
  bool raw_falta         = (digitalRead(falta_fase_ou_nivel) == HIGH); // HIGH = Aberto/Falha
  
  // 1. Debounce das falhas físicas (2 segundos)
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

  // 2. Seletor do painel em MANUAL -> Libera contator imediatamente
  if (!is_ccm_automatico) {
    digitalWrite(ligar_bomba, LOW);
    return;
  }

  // 3. Trava em caso de qualquer Defeito Ativo
  if (defeito_bomba_confirmado || falta_fase_confirmado || falha_sensor_nivel) {
    digitalWrite(ligar_bomba, LOW);
    return;
  }

  // Rastreia se a bomba desligou para persistir volume na Flash
  bool bomba_ligada_agora = (digitalRead(bomba_ligada) == LOW);
  if (ultima_bomba_ligada && !bomba_ligada_agora) {
    salvar_volume();
  }
  ultima_bomba_ligada = bomba_ligada_agora;

  // 4. MODO AUTOMÁTICO DE NÍVEL
  if (modo_atual == MODO_NIVEL) {
    if (tipo_sensor_nivel == 0) {
      // Controle Danfoss 4-20mA com Histerese
      if (nivel_percentual <= nivel_liga) {
        digitalWrite(ligar_bomba, HIGH);
      } 
      else if (nivel_percentual >= nivel_desliga) {
        digitalWrite(ligar_bomba, LOW);
      }
    } else {
      // Controle via Boia Digital Comum
      if (!boia_controle_cheia) {
        digitalWrite(ligar_bomba, HIGH); // Vazio -> Liga
      } else {
        digitalWrite(ligar_bomba, LOW);  // Cheio -> Desliga
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
        // Horário ativo: Liga, exceto se o reservatório já estiver cheio
        if (tipo_sensor_nivel == 0 && nivel_percentual >= nivel_desliga) {
          digitalWrite(ligar_bomba, LOW); // Trava Danfoss
        } 
        else if (tipo_sensor_nivel == 1 && boia_controle_cheia) {
          digitalWrite(ligar_bomba, LOW); // Trava Boia Digital
        } 
        else {
          digitalWrite(ligar_bomba, HIGH);
        }
      } else {
        digitalWrite(ligar_bomba, LOW);
      }
    } else {
      digitalWrite(ligar_bomba, LOW); // Sem horário NTP válido
    }
  } 
  
  // 6. MODO REMOTO (Comando direto via MQTT)
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
  last_saved_volume_litros = volume_total_litros;
  Serial.printf(">>> [NVS FLASH] Volume total salvo: %.2f L (%.3f m3)\n", volume_total_litros, volume_total_m3);
}

// ==========================================
// Configuração do Horário (SNTP / NTP)
// ==========================================
void timeavailable(struct timeval *t) {
  Serial.println("Relógio atualizado com sucesso via NTP!");
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
  
  Serial.printf("Mensagem MQTT recebida no tópico: %s | Conteúdo: %s\n", topic, messageTemp.c_str());
  String topicStr = String(topic);

  // 1. Comando de Modo de Operação
  if (topicStr == getTopic("cmd/modo")) {
    if (messageTemp == "nivel") {
      modo_atual = MODO_NIVEL;
      strncpy(modo_inicial_str, "nivel", sizeof(modo_inicial_str) - 1);
      salvar_configuracoes();
      Serial.println("Modo alterado para: NIVEL");
    } else if (messageTemp == "relogio") {
      modo_atual = MODO_RELOGIO;
      strncpy(modo_inicial_str, "relogio", sizeof(modo_inicial_str) - 1);
      salvar_configuracoes();
      Serial.println("Modo alterado para: RELOGIO");
    } else if (messageTemp == "remoto") {
      modo_atual = MODO_REMOTO;
      strncpy(modo_inicial_str, "remoto", sizeof(modo_inicial_str) - 1);
      salvar_configuracoes();
      Serial.println("Modo alterado para: REMOTO");
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
  
  // 3. Configuração de Janela Horária do Modo Relógio
  else if (topicStr == getTopic("cmd/config/horario")) {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, messageTemp);
    if (!error) {
      if (doc.containsKey("h_ini")) hora_inicio = doc["h_ini"];
      if (doc.containsKey("m_ini")) min_inicio = doc["m_ini"];
      if (doc.containsKey("h_fim")) hora_fim = doc["h_fim"];
      if (doc.containsKey("m_fim")) min_fim = doc["m_fim"];
      
      snprintf(hora_inicio_str, sizeof(hora_inicio_str), "%d", hora_inicio);
      snprintf(min_inicio_str, sizeof(min_inicio_str), "%d", min_inicio);
      snprintf(hora_fim_str, sizeof(hora_fim_str), "%d", hora_fim);
      snprintf(min_fim_str, sizeof(min_fim_str), "%d", min_fim);
      
      salvar_configuracoes();
      Serial.printf("Config Horária: das %02d:%02d às %02d:%02d\n", 
                    hora_inicio, min_inicio, hora_fim, min_fim);
    }
  } 
  
  // 4. Configuração de Setpoints de Nível
  else if (topicStr == getTopic("cmd/config/setpoints")) {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, messageTemp);
    if (!error) {
      if (doc.containsKey("nivel_liga")) nivel_liga = doc["nivel_liga"];
      if (doc.containsKey("nivel_desliga")) nivel_desliga = doc["nivel_desliga"];
      
      dtostrf(nivel_liga, 4, 1, nivel_liga_str);
      dtostrf(nivel_desliga, 4, 1, nivel_desliga_str);
      
      salvar_configuracoes();
      Serial.printf("Setpoints de Nível: Liga com %.1f%% | Desliga com %.1f%%\n", 
                    nivel_liga, nivel_desliga);
    }
  } 
  
  // 5. Comando de Reset do Relé Térmico (Pulso NÃO-BLOQUEANTE de 1s)
  else if (topicStr == getTopic("cmd/reset")) {
    if (messageTemp == "1") {
      Serial.println("Iniciando pulso de reset na Bomba (K2)...");
      digitalWrite(reset_bomba, HIGH);
      reset_em_andamento = true;
      tempo_inicio_reset = millis();
    }
  }

  // 6. Comando para acionar a Sirene de Alarme (Relé K3 com Auto-Timeout)
  else if (topicStr == getTopic("cmd/sirene")) {
    if (messageTemp == "1") {
      digitalWrite(acionar_sirene, HIGH);
      sirene_ativa = true;
      tempo_inicio_sirene = millis();
      Serial.println("SIRENE ATIVADA VIA MQTT! (Auto-timeout: 5 min)");
    } else if (messageTemp == "0") {
      digitalWrite(acionar_sirene, LOW);
      sirene_ativa = false;
      Serial.println("Sirene desligada via MQTT.");
    }
  }
}

// ==========================================
// Envio de Telemetria via MQTT (Publicação)
// ==========================================
void enviar_telemetria() {
  StaticJsonDocument<768> doc;
  
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
  controle["boia_controle_cheia"] = boia_controle_cheia;

  JsonObject sensores = doc.createNestedObject("sensores");
  sensores["vazao_l_min"] = round(vazao_l_min * 10.0) / 10.0;
  sensores["volume_m3"] = round(volume_total_m3 * 1000.0) / 1000.0;
  sensores["temperatura_c"] = round(temperatura_c * 10.0) / 10.0;

  doc["uptime_s"] = millis() / 1000;

  // Serializa e envia
  char buffer[768];
  serializeJson(doc, buffer);
  
  String t_topic = getTopic("telemetria");
  if (client.publish(t_topic.c_str(), buffer)) {
    Serial.printf("Telemetria enviada via MQTT no tópico: %s\n", t_topic.c_str());
  } else {
    Serial.println("Erro ao publicar telemetria no broker.");
  }
}
