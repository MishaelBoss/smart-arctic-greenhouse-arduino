#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ============================================================
//                    НАСТРОЙКИ WI-FI
// ============================================================
const char* WIFI_SSID = "LAY";
const char* WIFI_PASS = "17922343";

// ============================================================
//                    НАСТРОЙКИ СЕРВЕРА
// ============================================================
const char* API_BASE = "http://192.168.0.115:8000";
const char* API_KEY  = "KII28tN1eh9xZkU_2jw1vlrXxHEkQ8ZL";
const int   DEVICE_ID = 1;

// ============================================================
//                    OLED
// ============================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
const int OLED_SDA = 21;
const int OLED_SCL = 22;
const int OLED_ADDRESS = 0x3C;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================================================
//                    ПИНЫ
// ============================================================
const int SOIL1_PIN      = 34;
const int SOIL2_PIN      = 35;
const int DHT1_PIN       = 4;    // внутренний
const int DHT2_PIN       = 25;   // НОВЫЙ: наружный
const int LIGHT1_ADC     = 32;   // внутренний
const int LIGHT2_ADC     = 33;   // НОВЫЙ: наружный
const int SERVO_PIN      = 13;
const int RELAY_PUMP     = 26;
const int RELAY_LIGHT    = 27;

// ============================================================
//                    ТИП РЕЛЕ
// ============================================================
const bool RELAY_ACTIVE_LOW = false;

// ============================================================
//                    КАЛИБРОВКА
// ============================================================
const int RAW1_DRY = 4095;
const int RAW1_WET = 755;
const int RAW2_DRY = 4095;
const int RAW2_WET = 755;
const int MEASUREMENTS = 20;

// ============================================================
//                    ПОРОГИ
// ============================================================
const float TEMP_OPEN_ROOF  = 28.0;
const float TEMP_CLOSE_ROOF = 22.0;
const float SOIL_TOP_DRY    = 30.0;
const float SOIL_BOTTOM_WET = 70.0;
const float SOIL_TOP_ENOUGH = 60.0;
const int   LIGHT_DARK_THRESHOLD = 1500;

// ============================================================
//                    ОБЪЕКТЫ
// ============================================================
DHT dht1(DHT1_PIN, DHT22);
DHT dht2(DHT2_PIN, DHT22);   // НОВЫЙ
Servo roofServo;

// ============================================================
//                    СОСТОЯНИЕ
// ============================================================
struct {
  // Почва
  float soil1 = 0, soil2 = 0;
  int   raw1 = 0, raw2 = 0;

  // DHT #1 (внутренний)
  float temp1 = 0, hum1 = 0;

  // DHT #2 (наружный) — НОВЫЙ
  float temp2 = 0, hum2 = 0;

  // Освещённость
  int light1 = 0;   // внутренний
  int light2 = 0;   // НОВЫЙ: наружный

  // Управление
  bool pumpOn = false;
  bool roofOpen = false;
  bool lightOn = false;
  bool wifiOk = false;
} state;

// ============================================================
//                    ПРОТОТИПЫ
// ============================================================
void printSerial();
void drawScreen();

// ============================================================
//                    УПРАВЛЕНИЕ РЕЛЕ
// ============================================================
void relayOn(int pin)  { digitalWrite(pin, RELAY_ACTIVE_LOW ? LOW : HIGH); }
void relayOff(int pin) { digitalWrite(pin, RELAY_ACTIVE_LOW ? HIGH : LOW); }

// ============================================================
//                    ЧТЕНИЕ ДАТЧИКОВ
// ============================================================
int readSoilRaw(int pin) {
  long sum = 0;
  for (int i = 0; i < MEASUREMENTS; i++) {
    sum += analogRead(pin);
    delay(5);
  }
  return sum / MEASUREMENTS;
}

float rawToPercent(int raw, int rawDry, int rawWet) {
  float p = (rawDry - raw) * 100.0 / (rawDry - rawWet);
  return constrain(p, 0.0, 100.0);
}

void readAllSensors() {
  // Почва
  state.raw1 = readSoilRaw(SOIL1_PIN);
  state.raw2 = readSoilRaw(SOIL2_PIN);
  state.soil1 = rawToPercent(state.raw1, RAW1_DRY, RAW1_WET);
  state.soil2 = rawToPercent(state.raw2, RAW2_DRY, RAW2_WET);

  // DHT #1
  float t1 = dht1.readTemperature();
  float h1 = dht1.readHumidity();
  if (!isnan(t1)) state.temp1 = t1;
  if (!isnan(h1)) state.hum1  = h1;

  // DHT #2 (НОВЫЙ)
  float t2 = dht2.readTemperature();
  float h2 = dht2.readHumidity();
  if (!isnan(t2)) state.temp2 = t2;
  if (!isnan(h2)) state.hum2  = h2;

  // Освещённость — оба фоторезистора
  state.light1 = analogRead(LIGHT1_ADC);
  state.light2 = analogRead(LIGHT2_ADC);   // НОВЫЙ
}

// ============================================================
//                    АВТОМАТИКА
// ============================================================
void updateRoof() {
  // Управляем крышей по внутреннему DHT (temp1)
  if (isnan(state.temp1)) return;

  if (state.temp1 > TEMP_OPEN_ROOF && !state.roofOpen) {
    roofServo.write(90);
    state.roofOpen = true;
    Serial.println("[AUTO] Крыша ОТКРЫТА");
  } else if (state.temp1 < TEMP_CLOSE_ROOF && state.roofOpen) {
    roofServo.write(0);
    state.roofOpen = false;
    Serial.println("[AUTO] Крыша ЗАКРЫТА");
  }
}

void updatePump() {
  if (state.soil1 < SOIL_TOP_DRY &&
      state.soil2 < SOIL_BOTTOM_WET &&
      !state.pumpOn) {
    relayOn(RELAY_PUMP);
    state.pumpOn = true;
    Serial.println("[AUTO] Насос ВКЛ");
  }

  if ((state.soil2 >= SOIL_BOTTOM_WET ||
       state.soil1 >= SOIL_TOP_ENOUGH) && state.pumpOn) {
    relayOff(RELAY_PUMP);
    state.pumpOn = false;
    Serial.println("[AUTO] Насос ВЫКЛ");
  }
}

void updateLight() {
  // Досветка по внутреннему фоторезистору
  if (state.light1 < LIGHT_DARK_THRESHOLD && !state.lightOn) {
    relayOn(RELAY_LIGHT);
    state.lightOn = true;
    Serial.println("[AUTO] Досветка ВКЛ");
  } else if (state.light1 >= LIGHT_DARK_THRESHOLD && state.lightOn) {
    relayOff(RELAY_LIGHT);
    state.lightOn = false;
    Serial.println("[AUTO] Досветка ВЫКЛ");
  }
}

// ============================================================
//                    СЕТЬ — ОТПРАВКА
// ============================================================
void sendTelemetry() {
  if (WiFi.status() != WL_CONNECTED) {
    state.wifiOk = false;
    return;
  }
  state.wifiOk = true;

  HTTPClient http;
  http.begin(String(API_BASE) + "/api/telemetry");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);

  StaticJsonDocument<512> doc;
  doc["soil1_raw"]      = state.raw1;
  doc["soil1_moisture"] = state.soil1;
  doc["soil2_raw"]      = state.raw2;
  doc["soil2_moisture"] = state.soil2;

  // DHT #1
  if (!isnan(state.temp1)) doc["temperature"] = state.temp1;
  if (!isnan(state.hum1))  doc["humidity"]    = state.hum1;

  // DHT #2 — НОВЫЕ ПОЛЯ
  if (!isnan(state.temp2)) doc["temperature2"] = state.temp2;
  if (!isnan(state.hum2))  doc["humidity2"]    = state.hum2;

  // Освещённость — НОВОЕ ПОЛЕ
  doc["light1"] = state.light1;
  doc["light2"] = state.light2;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  Serial.printf("[NET] POST /telemetry -> %d\n", code);
  http.end();
}

void sendJsonSerial() {
  StaticJsonDocument<512> doc;
  doc["soil1_raw"]      = state.raw1;
  doc["soil1_moisture"] = state.soil1;
  doc["soil2_raw"]      = state.raw2;
  doc["soil2_moisture"] = state.soil2;
  if (!isnan(state.temp1)) doc["temperature"]  = state.temp1;
  if (!isnan(state.hum1))  doc["humidity"]     = state.hum1;
  if (!isnan(state.temp2)) doc["temperature2"] = state.temp2;
  if (!isnan(state.hum2))  doc["humidity2"]    = state.hum2;
  doc["light1"] = state.light1;
  doc["light2"] = state.light2;

  serializeJson(doc, Serial);
  Serial.println();
}

// ============================================================
//                    СЕТЬ — ПРИЁМ КОМАНД
// ============================================================
void checkCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(API_BASE) + "/api/devices/" + String(DEVICE_ID) + "/command";
  http.begin(url);
  http.addHeader("X-API-Key", API_KEY);

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, payload)) {
      for (JsonVariant cmd : doc["commands"].as<JsonArray>()) {
        String c = cmd.as<String>();
        Serial.printf("[NET] CMD: %s\n", c.c_str());

        if (c == "pump_on")    { relayOn(RELAY_PUMP);   state.pumpOn = true; }
        if (c == "pump_off")   { relayOff(RELAY_PUMP);  state.pumpOn = false; }
        if (c == "roof_open")  { roofServo.write(90);   state.roofOpen = true; }
        if (c == "roof_close") { roofServo.write(0);    state.roofOpen = false; }
        if (c == "light_on")   { relayOn(RELAY_LIGHT);  state.lightOn = true; }
        if (c == "light_off")  { relayOff(RELAY_LIGHT); state.lightOn = false; }
      }
    }
  }
  http.end();
}

// ============================================================
//                    РУЧНОЕ УПРАВЛЕНИЕ (SERIAL)
// ============================================================
void handleSerialCommands() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.isEmpty()) return;

  if (line == "ping")   { Serial.println("pong"); return; }
  if (line == "whoami") { Serial.println("SMART_GREENHOUSE_ARCTICA_2035"); return; }

  Serial.printf("[SERIAL CMD] %s\n", line.c_str());

  if (line == "pump_on")   { relayOn(RELAY_PUMP);   state.pumpOn = true;   Serial.println("OK"); return; }
  if (line == "pump_off")  { relayOff(RELAY_PUMP);  state.pumpOn = false;  Serial.println("OK"); return; }
  if (line == "roof_open") { roofServo.write(90);   state.roofOpen = true;  Serial.println("OK"); return; }
  if (line == "roof_close"){ roofServo.write(0);    state.roofOpen = false; Serial.println("OK"); return; }
  if (line == "light_on")  { relayOn(RELAY_LIGHT);  state.lightOn = true;  Serial.println("OK"); return; }
  if (line == "light_off") { relayOff(RELAY_LIGHT); state.lightOn = false; Serial.println("OK"); return; }

  if (line == "p") { state.pumpOn = !state.pumpOn; state.pumpOn ? relayOn(RELAY_PUMP) : relayOff(RELAY_PUMP); Serial.printf("[MANUAL] Насос %s\n", state.pumpOn ? "ON" : "OFF"); }
  if (line == "l") { state.lightOn = !state.lightOn; state.lightOn ? relayOn(RELAY_LIGHT) : relayOff(RELAY_LIGHT); Serial.printf("[MANUAL] Досветка %s\n", state.lightOn ? "ON" : "OFF"); }
  if (line == "r") { state.roofOpen = !state.roofOpen; roofServo.write(state.roofOpen ? 90 : 0); Serial.printf("[MANUAL] Крыша %s\n", state.roofOpen ? "OPEN" : "CLOSED"); }
  if (line == "s") printSerial();
  if (line == "h") Serial.println("Команды: pump_on/off, roof_open/close, light_on/off, p, l, r, s, h");
}

// ============================================================
//                    ВЫВОД НА OLED
// ============================================================
void drawScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("SMART GREENHOUSE");
  display.setCursor(108, 0);
  display.print(state.wifiOk ? "W" : "-");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  // Датчики 1 и 2 (почва)
  display.setCursor(0, 13);  display.print("D1:");
  display.setCursor(64, 13); display.print("D2:");
  display.setTextSize(2);
  display.setCursor(0, 23);
  display.print(state.soil1, 1); display.print("%");
  display.setCursor(64, 23);
  display.print(state.soil2, 1); display.print("%");

  // DHT22
  display.setTextSize(1);
  display.setCursor(0, 41);
  display.print("T1:"); display.print(state.temp1, 1);
  display.setCursor(64, 41);
  display.print("T2:"); display.print(state.temp2, 1);

  display.setCursor(0, 51);
  display.print("H1:"); display.print(state.hum1, 0); display.print("%");
  display.setCursor(64, 51);
  display.print("H2:"); display.print(state.hum2, 0); display.print("%");

  display.setCursor(0, 58);
  display.print(state.pumpOn ? "PUMP" : "----");
  display.setCursor(44, 58);
  display.print(state.roofOpen ? "ROOF" : "----");
  display.setCursor(88, 58);
  display.print(state.lightOn ? "LED " : "----");

  display.display();
}

void printSerial() {
  Serial.printf("Soil1: %.1f%% (RAW=%d) | Soil2: %.1f%% (RAW=%d)\n",
                state.soil1, state.raw1, state.soil2, state.raw2);
  Serial.printf("T1: %.1fC | H1: %.0f%% | Light1: %d\n",
                state.temp1, state.hum1, state.light1);
  Serial.printf("T2: %.1fC | H2: %.0f%% | Light2: %d\n",
                state.temp2, state.hum2, state.light2);
  Serial.printf("Pump:%s Roof:%s Light:%s | WiFi:%s\n",
                state.pumpOn ? "ON" : "OFF",
                state.roofOpen ? "OPEN" : "CLOSED",
                state.lightOn ? "ON" : "OFF",
                WiFi.status() == WL_CONNECTED ? "OK" : "FAIL");
  Serial.println("-----------------------------");
}

// ============================================================
//                    SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SMART GREENHOUSE (2x DHT + 2x Light) ===");

  pinMode(RELAY_PUMP, OUTPUT);
  pinMode(RELAY_LIGHT, OUTPUT);
  relayOff(RELAY_PUMP);
  relayOff(RELAY_LIGHT);

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL1_PIN, ADC_11db);
  analogSetPinAttenuation(SOIL2_PIN, ADC_11db);
  analogSetPinAttenuation(LIGHT1_ADC, ADC_11db);
  analogSetPinAttenuation(LIGHT2_ADC, ADC_11db);   // НОВЫЙ

  dht1.begin();
  dht2.begin();   // НОВЫЙ

  roofServo.setPeriodHertz(50);
  roofServo.attach(SERVO_PIN, 500, 2400);
  roofServo.write(0);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED not found");
    while (true) delay(1000);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 15);
  display.println("  Smart Greenhouse");
  display.setCursor(0, 30);
  display.println("  Connecting WiFi...");
  display.display();

  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting to: ");
  Serial.println(WIFI_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  display.clearDisplay();
  display.setCursor(0, 15);
  if (WiFi.status() == WL_CONNECTED) {
    state.wifiOk = true;
    Serial.println();
    Serial.print("WiFi OK. IP: ");
    Serial.println(WiFi.localIP());
    display.println("  WiFi OK");
    display.setCursor(0, 30);
    display.print("  IP: ");
    display.println(WiFi.localIP().toString());
  } else {
    state.wifiOk = false;
    Serial.printf("\nWiFi FAILED (Status: %d)\n", WiFi.status());
    display.println("  WiFi FAILED");
    display.setCursor(0, 30);
    display.println("  Autonomous mode");
  }
  display.display();

  Serial.println("Готово.");
  delay(2000);
}

// ============================================================
//                    LOOP
// ============================================================
unsigned long tSensor = 0, tAuto = 0, tOLED = 0, tSerial = 0;
unsigned long tNet = 0, tCmd = 0, tJson = 0;

void loop() {
  unsigned long now = millis();

  handleSerialCommands();

  if (now - tSensor >= 2000) { tSensor = now; readAllSensors(); }
  if (now - tAuto   >= 2000) { tAuto   = now; updateRoof(); updatePump(); updateLight(); }
  if (now - tOLED   >= 500)  { tOLED   = now; drawScreen(); }
  if (now - tSerial >= 3000) { tSerial = now; printSerial(); }
  if (now - tNet    >= 5000) { tNet    = now; sendTelemetry(); }
  if (now - tCmd    >= 2000) { tCmd    = now; checkCommands(); }
  if (now - tJson   >= 2000) { tJson   = now; sendJsonSerial(); }
}