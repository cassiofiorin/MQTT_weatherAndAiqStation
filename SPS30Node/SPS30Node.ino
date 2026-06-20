/**
 * @file main.cpp
 * @brief Nó de Qualidade do Ar (Sensirion SPS30)
 * @description Leitura de partículas, cálculo de AQI, limpeza automática e integração Home Assistant.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <SensirionI2cSps30.h>
#include <EEPROM.h>
#include <time.h>

// =============================================================================
// DEFINIÇÕES E CONSTANTES GLOBAIS
// =============================================================================

// Define NO_ERROR caso a biblioteca não o exponha globalmente neste contexto
#ifdef NO_ERROR
#undef NO_ERROR
#endif
#define NO_ERROR 0

struct Config {
  // Rede e MQTT
  const char* ssid = "wifiName";
  const char* password = "wifiPassword";
  const char* mqtt_server = "192.168.0.0";
  const int mqtt_port = 1883;
  const char* mqtt_user = "mqttUser";
  const char* mqtt_pass = "mtqqPassword";
  
  // NTP
  const char* ntpServer = "pool.ntp.org";
  const long gmtOffset_sec = -3 * 3600;
  const int daylightOffset_sec = 0;

  // Sensor e Limpeza
  const int eepromSize = 16;
  const int addrLastCleaning = 0;
  const unsigned long cleaningIntervalSec = 604800; // 1 semana
  
  // Timings
  const unsigned long intervalSerialStartup = 10000;
  const unsigned long intervalMqttPublish = 120000;   // 2 min
  const unsigned long timeStabilization = 60000;      // 1 min
};

const Config cfg;

// =============================================================================
// VARIÁVEIS GLOBAIS DE ESTADO
// =============================================================================

// Hardware Objects
WiFiClient espClient;
PubSubClient client(espClient);
SensirionI2cSps30 sensor;

// Estado do Sensor
uint16_t mc1p0 = 0, mc2p5 = 0, mc4p0 = 0, mc10p0 = 0;
uint16_t nc0p5 = 0, nc1p0 = 0, nc2p5 = 0, nc4p0 = 0, nc10p0 = 0;
uint16_t typicalParticleSize = 0;
bool isSensorDataValid = false;

// Estado da Qualidade do Ar
String airQualityStatus = "Calculando...";
uint16_t aqiNumeric = 0;
String lastCleaningDate = "Nunca";

// Estado do Sistema
bool isInitialPublishDone = false;
static char errorMessage[64];
static int16_t errorCode;

// Timers
unsigned long timerSerial = 0;
unsigned long timerMqtt = 0;

// =============================================================================
// MÓDULO: UTILITÁRIOS
// =============================================================================

void utilLog(const String& label, const String& msg) {
  Serial.println("[" + label + "] " + msg);
}

String utilGetDateTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "Data indisponivel";
  char buffer[32];
  sprintf(buffer, "%02d/%02d/%04d %02d:%02d", 
          timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900, 
          timeinfo.tm_hour, timeinfo.tm_min);
  return String(buffer);
}

unsigned long utilGetTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return 0;
  return (unsigned long)mktime(&timeinfo);
}

// =============================================================================
// MÓDULO: EEPROM (Persistência)
// =============================================================================

void setupEeprom() {
  EEPROM.begin(cfg.eepromSize);
  utilLog("EEPROM", "Inicializado (" + String(cfg.eepromSize) + " bytes)");
}

void eepromSaveCleaningTime(unsigned long timestamp) {
  EEPROM.put(cfg.addrLastCleaning, timestamp);
  EEPROM.commit();
  utilLog("EEPROM", "Timestamp de limpeza salvo");
}

unsigned long eepromGetLastCleaningTime() {
  unsigned long timestamp = 0;
  EEPROM.get(cfg.addrLastCleaning, timestamp);
  return timestamp;
}

// =============================================================================
// MÓDULO: SENSOR (SPS30)
// =============================================================================

void setupSensorHardware() {
  Wire.begin();
  sensor.begin(Wire, SPS30_I2C_ADDR_69);
  sensor.stopMeasurement();
  
  int8_t serialNumber[32] = {0};
  sensor.readSerialNumber(serialNumber, 32);
  utilLog("SENSOR", "SPS30 Iniciado. SN: " + String((const char*)serialNumber));
  
  sensor.startMeasurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_UINT16);
  delay(100);
  utilLog("SENSOR", "Medicao iniciada");
}

void loopSensorReadings() {
  errorCode = sensor.readMeasurementValuesUint16(mc1p0, mc2p5, mc4p0, mc10p0,
                                                 nc0p5, nc1p0, nc2p5, nc4p0,
                                                 nc10p0, typicalParticleSize);
  
  if (errorCode != NO_ERROR) {
    errorToString(errorCode, errorMessage, sizeof errorMessage);
    utilLog("ERRO", "Leitura SPS30 falhou: " + String(errorMessage));
    isSensorDataValid = false;
    return;
  }
  
  isSensorDataValid = true;
}

void loopSensorMaintenance() {
  unsigned long lastCleaning = eepromGetLastCleaningTime();
  unsigned long currentTs = utilGetTimestamp();
  
  bool forceClean = (lastCleaning == 0);
  bool intervalReached = (currentTs > 0 && (currentTs - lastCleaning) >= cfg.cleaningIntervalSec);
  
  if (forceClean || intervalReached) {
    utilLog("MANUT", forceClean ? "Primeira operacao. Limpando..." : "Intervalo semanal atingido. Limpando...");
    
    // Iniciar limpeza
    errorCode = sensor.startFanCleaning();
    if (errorCode != NO_ERROR) {
      errorToString(errorCode, errorMessage, sizeof errorMessage);
      utilLog("ERRO", "Falha ao iniciar limpeza: " + String(errorMessage));
      return;
    }
    
    utilLog("MANUT", "Limpeza em andamento (10s)...");
    delay(10000);
    
    // Salvar novo timestamp
    unsigned long newTs = utilGetTimestamp();
    if (newTs > 0) {
      eepromSaveCleaningTime(newTs);
      lastCleaningDate = utilGetDateTime();
      utilLog("MANUT", "Limpeza concluida em: " + lastCleaningDate);
      
      // Publicar atualização imediata se MQTT estiver conectado
      if (client.connected()) {
        client.publish("casa/ar/sps30_ultima_limpeza", lastCleaningDate.c_str(), true);
      }
    }
  } else {
    // Apenas atualiza string para display/logs se não limpou agora
    if (lastCleaning > 0) {
      struct tm timeinfo;
      time_t lastTime = (time_t)lastCleaning;
      localtime_r(&lastTime, &timeinfo);
      char buffer[32];
      sprintf(buffer, "%02d/%02d/%04d %02d:%02d", 
              timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900, 
              timeinfo.tm_hour, timeinfo.tm_min);
      lastCleaningDate = String(buffer);
    }
  }
}

// =============================================================================
// MÓDULO: QUALIDADE DO AR (Lógica de Negócio)
// =============================================================================

void loopAirQualityCalculation() {
  if (!isSensorDataValid) return;

  // Padrão CONAMA / AQI Simplificado
  if (mc2p5 <= 15 && mc10p0 <= 45) {
    airQualityStatus = "Boa";
    aqiNumeric = 25;      // Verde
  } else if (mc2p5 <= 45 && mc10p0 <= 120) {
    airQualityStatus = "Moderada";
    aqiNumeric = 75;      // Amarelo
  } else if (mc2p5 <= 60 || mc10p0 <= 150) {
    airQualityStatus = "Ruim (Nivel de Atencao)";
    aqiNumeric = 125;     // Laranja
  } else {
    airQualityStatus = "Pessima (Acima do Limite)";
    aqiNumeric = 200;     // Vermelho
  }
}

// =============================================================================
// MÓDULO: REDE (WiFi e NTP)
// =============================================================================

void setupNetwork() {
  utilLog("WIFI", "Conectando a " + String(cfg.ssid));
  WiFi.begin(cfg.ssid, cfg.password);
  
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    utilLog("WIFI", "Conectado! IP: " + WiFi.localIP().toString());
    configTime(cfg.gmtOffset_sec, cfg.daylightOffset_sec, cfg.ntpServer);
    delay(2000); // Aguarda sync NTP inicial
  } else {
    utilLog("ERRO", "Falha na conexao WiFi");
  }
}

void loopNetworkMaintenance() {
  if (WiFi.status() != WL_CONNECTED) {
    utilLog("WIFI", "Conexao perdida. Reconectando...");
    setupNetwork();
  }
}

// =============================================================================
// MÓDULO: MQTT (Comunicação)
// =============================================================================

void setupMqtt() {
  client.setServer(cfg.mqtt_server, cfg.mqtt_port);
  client.setKeepAlive(30);
}

void loopMqttConnection() {
  if (WiFi.status() != WL_CONNECTED || client.connected()) return;
  
  utilLog("MQTT", "Conectando ao broker...");
  String clientId = "ESP32_SPS30_Node_" + String(WiFi.macAddress());
  
  if (client.connect(clientId.c_str(), cfg.mqtt_user, cfg.mqtt_pass)) {
    utilLog("MQTT", "Conectado com sucesso");
    mqttSendDiscoveryConfig();
    // Reenvia último estado conhecido
    client.publish("casa/ar/sps30_ultima_limpeza", lastCleaningDate.c_str(), true);
  } else {
    utilLog("ERRO", "Falha MQTT rc=" + String(client.state()));
  }
}

void mqttSendDiscoveryConfig() {
  if (!client.connected()) return;

  auto sendConfig = [&](const char* topic, const char* name, const char* stateTopic, const char* unit, const char* deviceClass) {
    String payload = "{\"name\": \"" + String(name) + "\",";
    payload += "\"state_topic\": \"" + String(stateTopic) + "\",";
    if (strlen(unit) > 0) payload += "\"unit_of_measurement\": \"" + String(unit) + "\",";
    if (strlen(deviceClass) > 0) payload += "\"device_class\": \"" + String(deviceClass) + "\",";
    payload += "\"unique_id\": \"sps30_" + String(stateTopic) + "\"}";
    client.publish(topic, payload.c_str(), true);
  };

  // Massa
  sendConfig("homeassistant/sensor/sps30_pm10/config", "SPS30_PM1.0_Sala", "casa/ar/pm10", "µg/m³", "pm10");
  sendConfig("homeassistant/sensor/sps30_pm25/config", "SPS30_PM2.5_Sala", "casa/ar/pm25", "µg/m³", "pm25");
  sendConfig("homeassistant/sensor/sps30_pm40/config", "SPS30_PM4.0_Sala", "casa/ar/pm40", "µg/m³", "");
  sendConfig("homeassistant/sensor/sps30_pm100/config", "SPS30_PM10_Sala", "casa/ar/pm100", "µg/m³", "pm10");

  // Contagem
  sendConfig("homeassistant/sensor/sps30_nc05/config", "SPS30_Part_0.5um", "casa/ar/nc05", "p/cm³", "");
  sendConfig("homeassistant/sensor/sps30_nc10/config", "SPS30_Part_1.0um", "casa/ar/nc10", "p/cm³", "");
  sendConfig("homeassistant/sensor/sps30_nc25/config", "SPS30_Part_2.5um", "casa/ar/nc25", "p/cm³", "");
  sendConfig("homeassistant/sensor/sps30_nc40/config", "SPS30_Part_4.0um", "casa/ar/nc40", "p/cm³", "");
  sendConfig("homeassistant/sensor/sps30_nc100/config", "SPS30_Part_10um", "casa/ar/nc100", "p/cm³", "");

  // Diversos
  sendConfig("homeassistant/sensor/sps30_size/config", "SPS30_Tamanho_Medio", "casa/ar/tamanho_particula", "µm", "");
  
  // AQI
  client.publish("homeassistant/sensor/sps30_aqi_text/config", 
    "{\"name\": \"SPS30_Qualidade_Ar\",\"state_topic\": \"casa/ar/qualidade_ar\",\"unique_id\": \"sps30_aqi_text\"}", true);
    
  client.publish("homeassistant/sensor/sps30_aqi_num/config", 
    "{\"name\": \"SPS30_Indice_Qualidade_Ar\",\"state_topic\": \"casa/ar/indice_qualidade_ar\",\"unit_of_measurement\": \"AQI\",\"device_class\": \"aqi\",\"unique_id\": \"sps30_aqi_num\"}", true);

  // Limpeza
  client.publish("homeassistant/sensor/sps30_cleaning/config", 
    "{\"name\": \"SPS30_Sensor_limpo_em\",\"state_topic\": \"casa/ar/sps30_ultima_limpeza\",\"icon\": \"mdi:wrench-clock\",\"unique_id\": \"sps30_last_cleaning\"}", true);
}

void mqttPublishData() {
  if (!isSensorDataValid || !client.connected()) return;
  
  client.publish("casa/ar/qualidade_ar", airQualityStatus.c_str());
  client.publish("casa/ar/indice_qualidade_ar", String(aqiNumeric).c_str());
  
  client.publish("casa/ar/pm10", String(mc1p0).c_str());
  client.publish("casa/ar/pm25", String(mc2p5).c_str());
  client.publish("casa/ar/pm40", String(mc4p0).c_str());
  client.publish("casa/ar/pm100", String(mc10p0).c_str());
  
  client.publish("casa/ar/nc05", String(nc0p5).c_str());
  client.publish("casa/ar/nc10", String(nc1p0).c_str());
  client.publish("casa/ar/nc25", String(nc2p5).c_str());
  client.publish("casa/ar/nc40", String(nc4p0).c_str());
  client.publish("casa/ar/nc100", String(nc10p0).c_str());
  
  client.publish("casa/ar/tamanho_particula", String(typicalParticleSize / 1000.0, 3).c_str());
  
  utilLog("MQTT", "Dados publicados com sucesso");
}

// =============================================================================
// MÓDULO: INTERFACE (Serial Output)
// =============================================================================

void interfacePrintStatus() {
  if (!isSensorDataValid) return;
  
  Serial.println("\n=== LEITURA SPS30 ===");
  Serial.print("AQI: "); Serial.print(aqiNumeric); Serial.print(" ("); Serial.print(airQualityStatus); Serial.println(")");
  Serial.print("PM2.5: "); Serial.print(mc2p5); Serial.println(" µg/m³");
  Serial.print("PM10:  "); Serial.print(mc10p0); Serial.println(" µg/m³");
  Serial.print("Ultima Limpeza: "); Serial.println(lastCleaningDate);
  Serial.println("===================\n");
}

// =============================================================================
// MAIN: SETUP E LOOP
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(2000);
  utilLog("SYS", "=== Iniciando No SPS30 ===");

  // 1. Persistência
  setupEeprom();

  // 2. Hardware Sensor
  setupSensorHardware();

  // 3. Rede
  setupNetwork();
  
  // 4. Verifica necessidade de limpeza antes de conectar MQTT (opcional, mas recomendado)
  loopSensorMaintenance(); 

  // 5. MQTT
  setupMqtt();
  loopMqttConnection(); // Tentativa inicial

  // Inicializa timers
  unsigned long now = millis();
  timerSerial = now;
  timerMqtt = now;
  
  utilLog("SYS", "Aguardando estabilizacao (" + String(cfg.timeStabilization / 1000) + "s)...");
}

void loop() {
  unsigned long now = millis();

  // 1. Manutenção de Rede
  loopNetworkMaintenance();
  loopMqttConnection();
  client.loop();

  // 2. Leitura do Sensor (Sempre que possível)
  loopSensorReadings();
  
  // 3. Cálculo de Qualidade do Ar (Se dados válidos)
  if (isSensorDataValid) {
    loopAirQualityCalculation();
  }

  // 4. Lógica de Estabilização e Publicação
  if (now < cfg.timeStabilization) {
    // Fase de aquecimento: Log no serial a cada 10s
    if (now - timerSerial >= cfg.intervalSerialStartup) {
      interfacePrintStatus();
      timerSerial = now;
    }
  } else {
    // Fase Estável
    if (!isInitialPublishDone) {
      // Primeiro ciclo após estabilização
      utilLog("SYS", "Estabilizacao concluida. Publicacao inicial forcada.");
      interfacePrintStatus();
      mqttPublishData();
      isInitialPublishDone = true;
      timerMqtt = now;
    } 
    else if (now - timerMqtt >= cfg.intervalMqttPublish) {
      // Ciclo normal (2 min)
      interfacePrintStatus();
      mqttPublishData();
      timerMqtt = now;
    }
  }

  // 5. Manutenção Periódica do Sensor (Limpeza semanal)
  // Executa uma vez por loop, mas a ação só ocorre se a semana tiver passado
  loopSensorMaintenance();

  delay(1000); // Loop de 1 segundo conforme original
}