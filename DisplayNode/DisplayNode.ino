/**
 * @file main.cpp
 * @brief Estação Barométrica Modular ESP32-S3
 * @description Arquitetura baseada em funções independentes para fácil manutenção.
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RTClib.h>
#include <time.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =============================================================================
// CONFIGURAÇÕES GERAIS (Constants & Structs)
// =============================================================================

struct Config {
  const char* ssid = "wifiNetwork";
  const char* password = "wifiPassword";
  const char* mqtt_server = "192.168.0.0";
  const int mqtt_port = 1883;
  const char* mqtt_user = "mqttUser";
  const char* mqtt_pass = "mqttPassword";
  
  const int pinI2C_SDA = 8;
  const int pinI2C_SCL = 9;
  const int pinButton = 4;
  
  const int screenW = 128;
  const int screenH = 64;
  const int oledReset = -1;
  const int displayAddr = 0x3C;
  
  const unsigned long intervalBarometer = 900000;   
  const unsigned long intervalHourlyPublish = 3600000; 
  const unsigned long intervalWifiCheck = 30000;    
  const unsigned long durationAlertInit = 120000;   
  const unsigned long durationAlertDisplay = 1800000; 
  const unsigned long durationButtonWake = 30000;   
  const unsigned long debounceDelay = 50;
};

const Config cfg;

struct AlertThresholds {
  const float diff15 = 3.0;
  const float diff60 = 4.0;
  const float diff120 = 2.0;
};

const AlertThresholds thresholds;

// =============================================================================
// VARIÁVEIS GLOBAIS DE ESTADO (Compartilhadas entre módulos)
// =============================================================================

// Hardware Objects
WiFiClient espClient;
PubSubClient client(espClient);
RTC_DS3231 rtc;
Adafruit_BMP280 bmp;
Adafruit_SSD1306 display(cfg.screenW, cfg.screenH, &Wire, cfg.oledReset);

// Estados dos Módulos
bool isRtcFound = false;
bool isBmpFound = false;
bool isWifiConnected = false;
bool isDisplayOn = true;

// Estado da Lógica de Negócio
bool initPhaseSent = false;
bool initPhaseCleared = false;
float pressureHistory[9] = {0};
float currentTemp = 0.0;
float currentPressure = 0.0;
String currentAlertMsg = "Clima estavel";

// Timers Globais
unsigned long timerBarometer = 0;
unsigned long timerHourly = 0;
unsigned long timerWifi = 0;
unsigned long timerAlertInit = 0;
unsigned long timerAlertStart = 0;
unsigned long timerButtonWake = 0;
unsigned long timerDebounce = 0;

// Estado do Botão
int btnState = HIGH;
int lastBtnState = HIGH;

// =============================================================================
// MÓDULO: UTILITÁRIOS
// =============================================================================

void utilLog(const String& label, const String& msg) {
  Serial.println("[" + label + "] " + msg);
}

bool utilIsValidTime(DateTime t) {
  return (t.hour() <= 23 && t.minute() <= 59 && t.second() <= 59 &&
          t.day() >= 1 && t.day() <= 31 &&
          t.month() >= 1 && t.month() <= 12 &&
          t.year() >= 2020 && t.year() <= 2100);
}

void utilSetDisplayBrightness(uint8_t brightness) {
  Wire.beginTransmission(cfg.displayAddr);
  Wire.write(0x80); Wire.write(0x81);
  Wire.write(0x80); Wire.write(brightness);
  Wire.endTransmission();
}

// =============================================================================
// MÓDULO: DISPLAY (OLED)
// =============================================================================

void setupDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, cfg.displayAddr)) {
    utilLog("ERROR", "Display SSD1306 nao encontrado");
    return;
  }
  utilSetDisplayBrightness(191);
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println("Iniciando...");
  display.display();
  utilLog("INFO", "Display inicializado");
}

void loopDisplayPowerManagement(unsigned long now) {
  if (!isRtcFound) return;

  DateTime t = rtc.now();
  bool isNight = (t.hour() >= 20 || t.hour() < 8);
  
  int reading = digitalRead(cfg.pinButton);
  if (reading != lastBtnState) {
    timerDebounce = now;
  }

  if ((now - timerDebounce) > cfg.debounceDelay) {
    if (reading != btnState) {
      btnState = reading;
      if (btnState == LOW) {
        timerButtonWake = now;
        utilLog("BTN", "Pressionado");
        if (!isDisplayOn) {
          display.ssd1306_command(SSD1306_DISPLAYON);
          isDisplayOn = true;
        }
      }
    }
  }
  lastBtnState = reading;

  if (isNight) {
    if (isDisplayOn && (now - timerButtonWake > cfg.durationButtonWake)) {
      display.ssd1306_command(SSD1306_DISPLAYOFF);
      isDisplayOn = false;
      utilLog("DISPLAY", "Desligado (Modo noturno)");
    }
  } else {
    if (!isDisplayOn) {
      display.ssd1306_command(SSD1306_DISPLAYON);
      isDisplayOn = true;
      utilLog("DISPLAY", "Ligado (Modo diurno)");
    }
  }
}

void loopDisplayRender() {
  if (!isDisplayOn) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Data/Hora
  if (isRtcFound) {
    DateTime now = rtc.now();
    if (utilIsValidTime(now)) {
      display.setTextSize(1);
      display.setCursor(34, 0);
      display.printf("%02d/%02d/%04d", now.day(), now.month(), now.year());
      display.setTextSize(2);
      display.setCursor(16, 10);
      display.printf("%02d:%02d:%02d", now.hour(), now.minute(), now.second());
    } else {
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.println("RTC dados invalidos");
    }
  } else {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("RTC nao encontrado");
  }
  
  // Sensores
  if (isBmpFound) {
    display.setTextSize(1);
    display.setCursor(28, 30);
    display.printf("Temp: %.1f C", currentTemp);
    display.setCursor(19, 42);
    display.printf("Press: %.0f hPa", currentPressure);
  }
  
  // Alerta
  display.setTextSize(1);
  display.setCursor(25, 56);
  String displayMsg = currentAlertMsg.length() > 18 ? currentAlertMsg.substring(0, 17) + "." : currentAlertMsg;
  display.print(displayMsg);
  
  display.display();
}

// =============================================================================
// MÓDULO: SENSORES (RTC e BMP280)
// =============================================================================

void setupSensors() {
  // RTC
  if (rtc.begin()) {
    isRtcFound = true;
    utilLog("INFO", "RTC DS3231 encontrado");
    if (!utilIsValidTime(rtc.now())) {
      utilLog("WARN", "RTC com data invalida");
    }
  } else {
    utilLog("ERROR", "RTC DS3231 nao encontrado");
  }

  // BMP280
  if (bmp.begin(0x76)) {
    isBmpFound = true;
    utilLog("INFO", "BMP280 encontrado");
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
  } else {
    utilLog("ERROR", "BMP280 nao encontrado");
  }
}

void loopSensorReadings(unsigned long now) {
  if (!isBmpFound || (now - timerBarometer < cfg.intervalBarometer)) return;
  
  timerBarometer = now;
  
  // Leitura direta
  float newTemp = bmp.readTemperature();
  float newPressure = bmp.readPressure() / 100.0F;
  
  utilLog("SENSOR", "Temp: " + String(newTemp, 1) + " C | Pressao: " + String(newPressure, 1) + " hPa");
  
  // Atualiza histórico (desloca valores antigos)
  for (int i = 8; i > 0; i--) {
    pressureHistory[i] = pressureHistory[i-1];
  }
  pressureHistory[0] = newPressure;
  
  // Atualiza variáveis globais APÓS salvar no histórico
  currentTemp = newTemp;
  currentPressure = newPressure;
}

void syncRTCIfNeeded() {
  if (!isRtcFound || !isWifiConnected) return;

  DateTime now = rtc.now();
  bool needsSync = (now.year() < 2026 || (now.year() == 2026 && now.month() < 6)) || !utilIsValidTime(now);
  
  if (needsSync) {
    utilLog("INFO", "Sincronizando RTC com NTP...");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, 
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
      utilLog("INFO", "RTC atualizado via NTP");
    }
  }
}

// =============================================================================
// MÓDULO: REDE (WiFi e MQTT)
// =============================================================================

void setupNetwork() {
  utilLog("WIFI", "Conectando...");
  WiFi.disconnect();
  WiFi.begin(cfg.ssid, cfg.password);
  
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 60) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    isWifiConnected = true;
    utilLog("WIFI", "Conectado! IP: " + WiFi.localIP().toString());
    configTime(-10800, 0, "time.cloudflare.com");
    
    client.setServer(cfg.mqtt_server, cfg.mqtt_port);
    client.setKeepAlive(30);
    networkSendDiscoveryConfig();
  } else {
    isWifiConnected = false;
    utilLog("ERROR", "Falha na conexao WiFi");
  }
}

void loopNetworkMaintenance(unsigned long now) {
  if (now - timerWifi > cfg.intervalWifiCheck) {
    timerWifi = now;
    if (WiFi.status() != WL_CONNECTED) {
      utilLog("WARN", "WiFi desconectado. Reconectando...");
      isWifiConnected = false;
      setupNetwork(); // Reutiliza a lógica de conexão
      if (isWifiConnected && !client.connected()) {
        networkReconnectMQTT();
      }
    }
  }
  
  if (isWifiConnected && !client.connected()) {
    networkReconnectMQTT();
  }
  
  if (isWifiConnected) {
    client.loop();
  }
}

void networkReconnectMQTT() {
  String clientId = "ESP32_S3_Sensor_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (client.connect(clientId.c_str(), cfg.mqtt_user, cfg.mqtt_pass)) {
    utilLog("MQTT", "Conectado");
    networkSendDiscoveryConfig();
  } else {
    utilLog("WARN", "Falha MQTT rc=" + String(client.state()));
    delay(5000);
  }
}

void networkSendDiscoveryConfig() {
  if (!isWifiConnected || !client.connected()) return;
  
  client.publish("homeassistant/sensor/esp32_ar_temp/config", 
    "{\"name\":\"Temperatura ESP\",\"state_topic\":\"casa/temperatura\",\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\",\"unique_id\":\"esp32_ar_temp\"}", true);
  client.publish("homeassistant/sensor/esp32_ar_pressao/config", 
    "{\"name\":\"Pressao ESP\",\"state_topic\":\"casa/pressao\",\"unit_of_measurement\":\"hPa\",\"device_class\":\"pressure\",\"unique_id\":\"esp32_ar_pressao\"}", true);
  client.publish("homeassistant/sensor/esp32_ar_alerta/config", 
    "{\"name\":\"Alerta Clima ESP\",\"state_topic\":\"casa/alerta_clima\",\"unique_id\":\"esp32_ar_alerta\"}", true);
}

void networkPublish(const char* topic, const char* payload) {
  if (isWifiConnected && client.connected()) {
    client.publish(topic, payload);
  }
}

// =============================================================================
// MÓDULO: LÓGICA DE ALERTAS E NEGÓCIO
// =============================================================================

void loopAlertLogicInitialization(unsigned long now) {
  // Fase Inicial (Iniciando -> Monitorando)
  if (isBmpFound && !initPhaseSent && isWifiConnected) {
    networkPublish("casa/alerta_clima", "Iniciando");
    delay(200);
    networkPublish("casa/alerta_clima", "Iniciando");
    utilLog("ALERT", "Fase inicial: Iniciando");
    
    initPhaseSent = true;
    timerAlertInit = now;
  }

  if (initPhaseSent && !initPhaseCleared && isWifiConnected && (now - timerAlertInit > cfg.durationAlertInit)) {
    networkPublish("casa/alerta_clima", "Monitorando");
    utilLog("ALERT", "Fase inicial: Monitorando");
    initPhaseCleared = true;
  }
}

void loopAlertLogicEvaluation() {
  // Só avalia se temos leituras válidas no histórico
  bool triggered = false;
  String msg = "";

  if (pressureHistory[0] > 0) {
    float diff = currentPressure - pressureHistory[0];
    if (diff <= -thresholds.diff15) {
      msg = "PIORA RAPIDA: Queda (" + String(diff, 1) + " hPa/15min)";
      triggered = true;
    } else if (diff >= thresholds.diff15) {
      msg = "MELHORA RAPIDA: Alta (" + String(diff, 1) + " hPa/15min)";
      triggered = true;
    }
  }

  if (!triggered && pressureHistory[3] > 0) {
    float diff = currentPressure - pressureHistory[3];
    if (diff <= -thresholds.diff60) {
      msg = "TENDENCIA FORTE PIORA: (" + String(diff, 1) + " hPa/1h)";
      triggered = true;
    } else if (diff >= thresholds.diff60) {
      msg = "TENDENCIA FORTE MELHORA: (" + String(diff, 1) + " hPa/1h)";
      triggered = true;
    }
  }

  if (!triggered && pressureHistory[7] > 0) {
    float diff = currentPressure - pressureHistory[7];
    if (diff <= -thresholds.diff120) {
      msg = "TENDENCIA MODERADA PIORA: (" + String(diff, 1) + " hPa/2h)";
      triggered = true;
    } else if (diff >= thresholds.diff120) {
      msg = "TENDENCIA MODERADA MELHORA: (" + String(diff, 1) + " hPa/2h)";
      triggered = true;
    }
  }

  if (triggered) {
    currentAlertMsg = msg;
    timerAlertStart = millis();
    utilLog("ALERT", msg);
    networkPublish("casa/alerta_clima", msg.c_str());
  }
}

void loopAlertLogicHourlyStatus(unsigned long now) {
  if (!isWifiConnected || !initPhaseCleared || (now - timerHourly < cfg.intervalHourlyPublish)) return;
  
  timerHourly = now;
  
  if (currentAlertMsg == "Clima estavel" || (now - timerAlertStart > cfg.durationAlertDisplay)) {
    networkPublish("casa/alerta_clima", "Sem alerta");
    utilLog("ALERT", "Status horario: Sem alerta");
    currentAlertMsg = "Sem alerta";
  }
}

void loopAlertLogicExpiration(unsigned long now) {
  if (currentAlertMsg != "Clima estavel" && currentAlertMsg != "Sem alerta") {
    if (now - timerAlertStart > cfg.durationAlertDisplay) {
      currentAlertMsg = "Clima estavel";
      utilLog("ALERT", "Alerta expirou no display");
    }
  }
}

// =============================================================================
// MAIN: SETUP E LOOP
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(2000);
  utilLog("SYS", "=== Inicializando Estacao Barometrica ===");

  Wire.begin(cfg.pinI2C_SDA, cfg.pinI2C_SCL, 400000);
  pinMode(cfg.pinButton, INPUT_PULLUP);

  // 1. Inicializa Display
  setupDisplay();

  // 2. Inicializa Sensores
  setupSensors();

  // 3. Inicializa Rede
  setupNetwork();

  // 4. Sync RTC (depende de Rede e Sensor)
  syncRTCIfNeeded();

  // Inicializa timers
  timerBarometer = millis() - cfg.intervalBarometer; // Permite leitura imediata
  timerHourly = millis();
  timerWifi = millis();
  
  utilLog("SYS", "=== Setup Concluido ===");
}

void loop() {
  unsigned long now = millis();

  // 1. Gerenciamento de Energia do Display (Botão/Horário)
  loopDisplayPowerManagement(now);

  // 2. Manutenção de Rede (Reconexão WiFi/MQTT)
  loopNetworkMaintenance(now);

  // 3. Lógica de Inicialização do Sistema (Mensagem "Iniciando")
  loopAlertLogicInitialization(now);

  // 4. Leitura de Sensores (A cada 15 min)
  bool sensorRead = false;
  if (isBmpFound && (now - timerBarometer >= cfg.intervalBarometer)) {
    loopSensorReadings(now);
    sensorRead = true;
  }

  // 5. Avaliação de Alertas (apenas quando há nova leitura de sensores)
  if (sensorRead) {
     loopAlertLogicEvaluation();
  }

  // 6. Publicação Horária de Status
  loopAlertLogicHourlyStatus(now);

  // 7. Expiração Visual de Alertas
  loopAlertLogicExpiration(now);

  // 8. Renderização do Display (Se ligado)
  loopDisplayRender();

  delay(10);
}