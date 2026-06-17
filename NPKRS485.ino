#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ================= WIFI =================
const char* ssid = "halal";
const char* password = "MAU MASUK SURGA ibadah";

// ================= HIVEMQ =================
const char* mqtt_server = "a8805b4f45744c3f9ac83882e423e0c0.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "hasyim";
const char* mqtt_pass = "hasyimHiveMQTT@22";
const char* topic = "nutrixense/sensor";

// ================= MQTT =================
WiFiClientSecure espClient;
PubSubClient client(espClient);

// Inisialisasi Modbus
ModbusMaster node;

// ================= I2C LCD =================
#define LCD_ADDRESS 0x27
#define LCD_COLUMNS 20
#define LCD_ROWS 4

LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLUMNS, LCD_ROWS);

// Control Pin MAX485
#define MAX485_DE_RE 4

// Use Serial2 UART ESP32
#define RXD2 16
#define TXD2 17

// ================= RELAY =================
// Relay module uses active LOW: LOW = ON, HIGH = OFF
#define RELAY1 25
#define RELAY2 26
#define RELAY3 27
#define RELAY4 14

#define RELAY_OFF LOW
#define RELAY_ON HIGH

// ================= BUZZER =================
#define BUZZER_PIN 12

// If your buzzer still sounds when it should be OFF,
// swap these two values.
#define BUZZER_ON HIGH
#define BUZZER_OFF LOW

// ================= TIMING =================
unsigned long lastReadTime = 0;
const unsigned long READ_INTERVAL = 2000;
const unsigned long SENSOR_DELAY = 100;
bool sensorReadSuccess = true;

const unsigned long WIFI_RETRY_INTERVAL = 15000;
const unsigned long MQTT_RETRY_INTERVAL = 5000;
const unsigned long STARTUP_SCREEN_DURATION = 10000;
const unsigned long STARTUP_ANIMATION_INTERVAL = 500;

unsigned long lastWifiAttemptTime = 0;
unsigned long lastMqttAttemptTime = 0;
bool wifiStarted = false;

// ================= NUTRITION THRESHOLD =================
float MIN_NITROGEN = 40;
float MIN_PHOSPHORUS = 20;
float MIN_POTASSIUM = 40;

float MIN_PH = 5.8;
float MIN_MOISTURE = 40.0;
float MIN_TEMPERATURE = 18.0;
float MIN_EC = 1.0;

float MAX_NITROGEN = 80;
float MAX_PHOSPHORUS = 60;
float MAX_POTASSIUM = 100;

float MAX_PH = 7.2;
float MAX_MOISTURE = 80.0;
float MAX_TEMPERATURE = 35.0;
float MAX_EC = 3.0;

// ================= BUZZER MUTE CONFIG =================
bool MUTE_NITROGEN = false;
bool MUTE_PHOSPHORUS = false;
bool MUTE_POTASSIUM = false;
bool MUTE_PH = false;
bool MUTE_MOISTURE = false;
bool MUTE_TEMPERATURE = false;
bool MUTE_EC = false;

bool isActiveAbnormal(float value, float minValue, float maxValue, bool muted) {
  return !muted && (value < minValue || value > maxValue);
}

void preTransmission() {
  digitalWrite(MAX485_DE_RE, HIGH);
}

void postTransmission() {
  digitalWrite(MAX485_DE_RE, LOW);
}

void printPaddedLcdLine(byte row, const String &text) {
  lcd.setCursor(0, row);
  lcd.print(text);

  for (int i = text.length(); i < LCD_COLUMNS; i++) {
    lcd.print(' ');
  }
}

void printCenteredLcdLine(byte row, const String &text) {
  int leftPadding = (LCD_COLUMNS - text.length()) / 2;

  if (leftPadding < 0) {
    leftPadding = 0;
  }

  String line = "";

  for (int i = 0; i < leftPadding; i++) {
    line += ' ';
  }

  line += text;
  printPaddedLcdLine(row, line);
}

String formatLcdValue(float value, unsigned int decimals) {
  String formatted = String(value, decimals);

  if (formatted.length() > 4) {
    formatted = String(value, 0);
  }

  if (formatted.length() > 4) {
    formatted = formatted.substring(0, 4);
  }

  return formatted;
}

void clearLcdLine(byte row) {
  lcd.setCursor(0, row);

  for (int i = 0; i < LCD_COLUMNS; i++) {
    lcd.print(' ');
  }
}

void setupLcd() {
  lcd.init();
  lcd.backlight();
}

void showStartupScreen() {
  unsigned long startedAt = millis();
  byte frame = 0;

  printCenteredLcdLine(0, "NutriXense");
  printCenteredLcdLine(1, "Smart Nutrient");
  printCenteredLcdLine(2, "Monitoring");

  while (millis() - startedAt < STARTUP_SCREEN_DURATION) {
    String loadingText = "Starting";

    for (byte i = 0; i < frame; i++) {
      loadingText += " .";
    }

    printCenteredLcdLine(3, loadingText);
    frame = (frame + 1) % 4;
    delay(STARTUP_ANIMATION_INTERVAL);
  }

  lcd.clear();
}

void displaySensorData(float nitrogen, float phosphorus, float potassium, float ph, float moisture, float temperature) {
  printCenteredLcdLine(0, "NutriXense");

  printPaddedLcdLine(
    1,
    "N:" + formatLcdValue(nitrogen, 0) + " mg/kg pH:" + formatLcdValue(ph, 1)
  );

  clearLcdLine(2);
  lcd.setCursor(0, 2);
  lcd.print("P:");
  lcd.print(formatLcdValue(phosphorus, 0));
  lcd.print(" mg/kg T:");
  lcd.print(formatLcdValue(temperature, 1));
  lcd.write(byte(223));
  lcd.print("C");

  printPaddedLcdLine(
    3,
    "K:" + formatLcdValue(potassium, 0) + " mg/kg M:" + formatLcdValue(moisture, 1) + "%"
  );
}

void displaySensorError() {
  printPaddedLcdLine(0, "     NutriXense");
  printPaddedLcdLine(1, " Sensor read failed");
  printPaddedLcdLine(2, " Check RS485/NPK");
  printPaddedLcdLine(3, " Retrying...");
}

void printThresholds() {
  Serial.println("===== CURRENT THRESHOLDS =====");

  Serial.print("Nitrogen     : ");
  Serial.print(MIN_NITROGEN);
  Serial.print(" - ");
  Serial.println(MAX_NITROGEN);

  Serial.print("Phosphorus   : ");
  Serial.print(MIN_PHOSPHORUS);
  Serial.print(" - ");
  Serial.println(MAX_PHOSPHORUS);

  Serial.print("Potassium    : ");
  Serial.print(MIN_POTASSIUM);
  Serial.print(" - ");
  Serial.println(MAX_POTASSIUM);

  Serial.print("pH           : ");
  Serial.print(MIN_PH);
  Serial.print(" - ");
  Serial.println(MAX_PH);

  Serial.print("Moisture     : ");
  Serial.print(MIN_MOISTURE);
  Serial.print(" - ");
  Serial.println(MAX_MOISTURE);

  Serial.print("Temperature  : ");
  Serial.print(MIN_TEMPERATURE);
  Serial.print(" - ");
  Serial.println(MAX_TEMPERATURE);

  Serial.print("EC           : ");
  Serial.print(MIN_EC);
  Serial.print(" - ");
  Serial.println(MAX_EC);
}

void printThresholdCheck(float nitrogen, float phosphorus, float potassium, float ph, float moisture, float temperature, float ec) {
  Serial.println("===== DEBUG THRESHOLD CHECK =====");

  Serial.print("Nitrogen     : ");
  Serial.print(nitrogen);
  Serial.print(" (");
  Serial.print(MIN_NITROGEN);
  Serial.print(" - ");
  Serial.print(MAX_NITROGEN);
  Serial.println(")");

  Serial.print("Phosphorus   : ");
  Serial.print(phosphorus);
  Serial.print(" (");
  Serial.print(MIN_PHOSPHORUS);
  Serial.print(" - ");
  Serial.print(MAX_PHOSPHORUS);
  Serial.println(")");

  Serial.print("Potassium    : ");
  Serial.print(potassium);
  Serial.print(" (");
  Serial.print(MIN_POTASSIUM);
  Serial.print(" - ");
  Serial.print(MAX_POTASSIUM);
  Serial.println(")");

  Serial.print("pH           : ");
  Serial.print(ph);
  Serial.print(" (");
  Serial.print(MIN_PH);
  Serial.print(" - ");
  Serial.print(MAX_PH);
  Serial.println(")");

  Serial.print("Moisture     : ");
  Serial.print(moisture);
  Serial.print(" (");
  Serial.print(MIN_MOISTURE);
  Serial.print(" - ");
  Serial.print(MAX_MOISTURE);
  Serial.println(")");

  Serial.print("Temperature  : ");
  Serial.print(temperature);
  Serial.print(" (");
  Serial.print(MIN_TEMPERATURE);
  Serial.print(" - ");
  Serial.print(MAX_TEMPERATURE);
  Serial.println(")");

  Serial.print("EC           : ");
  Serial.print(ec);
  Serial.print(" (");
  Serial.print(MIN_EC);
  Serial.print(" - ");
  Serial.print(MAX_EC);
  Serial.println(")");
}

// ================= WIFI =================
void setup_wifi() {
  Serial.println("Starting WiFi connection attempt...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  wifiStarted = true;
  lastWifiAttemptTime = millis();
}

// ================= MQTT CONNECT =================
void reconnect() {
  if (WiFi.status() != WL_CONNECTED || client.connected()) {
    return;
  }

  lastMqttAttemptTime = millis();
  Serial.print("Connecting MQTT...");

  if (client.connect("ESP32_Client", mqtt_user, mqtt_pass)) {
    Serial.println("Connected!");

    client.subscribe("nutrixense/control");
    Serial.println("Subscribed: nutrixense/control");

    client.subscribe("nutrixense/config");
    Serial.println("Subscribed: nutrixense/config");
  } else {
    Serial.print("Failed, rc=");
    Serial.print(client.state());
    Serial.println(" will retry later.");
  }
}

void maintainNetwork() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiStarted || millis() - lastWifiAttemptTime >= WIFI_RETRY_INTERVAL) {
      setup_wifi();
    }

    return;
  }

  if (!client.connected() && millis() - lastMqttAttemptTime >= MQTT_RETRY_INTERVAL) {
    reconnect();
  }

  if (client.connected()) {
    client.loop();
  }
}

// ================= READ REGISTER =================
uint16_t readRegister(uint16_t reg, bool &success) {
  uint8_t result = node.readHoldingRegisters(reg, 1);

  if (result == node.ku8MBSuccess) {
    success = true;
    return node.getResponseBuffer(0);
  }

  success = false;

  Serial.print("Failed reading register: 0x");
  Serial.println(reg, HEX);

  return 0;
}

void callback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);

  Serial.print("Message arrived [");
  Serial.print(topicStr);
  Serial.println("]");

  String message;

  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.println(message);

  DynamicJsonDocument doc(2048);

  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    Serial.print("JSON Parse Failed: ");
    Serial.println(error.c_str());
    return;
  }

  Serial.println("=== JSON RECEIVED ===");
  serializeJsonPretty(doc, Serial);
  Serial.println();

  // =====================================================
  // MQTT TOPIC : nutrixense/control
  // =====================================================
  if (topicStr == "nutrixense/control") {
    Serial.println("=== RELAY CONTROL ===");

    if (doc.containsKey("relay1")) {
      int state = doc["relay1"].as<int>();
      digitalWrite(RELAY1, state ? RELAY_ON : RELAY_OFF);

      Serial.print("Relay1: ");
      Serial.println(state ? "ON" : "OFF");
    }

    if (doc.containsKey("relay2")) {
      int state = doc["relay2"].as<int>();
      digitalWrite(RELAY2, state ? RELAY_ON : RELAY_OFF);

      Serial.print("Relay2: ");
      Serial.println(state ? "ON" : "OFF");
    }

    if (doc.containsKey("relay3")) {
      int state = doc["relay3"].as<int>();
      digitalWrite(RELAY3, state ? RELAY_ON : RELAY_OFF);

      Serial.print("Relay3: ");
      Serial.println(state ? "ON" : "OFF");
    }

    if (doc.containsKey("relay4")) {
      int state = doc["relay4"].as<int>();
      digitalWrite(RELAY4, state ? RELAY_ON : RELAY_OFF);

      Serial.print("Relay4: ");
      Serial.println(state ? "ON" : "OFF");
    }
  }

  // =====================================================
  // MQTT TOPIC : nutrixense/config
  // =====================================================
  else if (topicStr == "nutrixense/config") {
    Serial.println("=== THRESHOLD CONFIG ===");

    if (doc.containsKey("min_nitrogen")) {
      MIN_NITROGEN = doc["min_nitrogen"].as<float>();
    }

    if (doc.containsKey("min_phosphorus")) {
      MIN_PHOSPHORUS = doc["min_phosphorus"].as<float>();
    }

    if (doc.containsKey("min_potassium")) {
      MIN_POTASSIUM = doc["min_potassium"].as<float>();
    }

    if (doc.containsKey("min_ph")) {
      MIN_PH = doc["min_ph"].as<float>();
    }

    if (doc.containsKey("min_moisture")) {
      MIN_MOISTURE = doc["min_moisture"].as<float>();
    }

    if (doc.containsKey("min_temperature")) {
      MIN_TEMPERATURE = doc["min_temperature"].as<float>();
    }

    if (doc.containsKey("min_ec")) {
      MIN_EC = doc["min_ec"].as<float>();
    }

    if (doc.containsKey("max_nitrogen")) {
      MAX_NITROGEN = doc["max_nitrogen"].as<float>();
    }

    if (doc.containsKey("max_phosphorus")) {
      MAX_PHOSPHORUS = doc["max_phosphorus"].as<float>();
    }

    if (doc.containsKey("max_potassium")) {
      MAX_POTASSIUM = doc["max_potassium"].as<float>();
    }

    if (doc.containsKey("max_ph")) {
      MAX_PH = doc["max_ph"].as<float>();
    }

    if (doc.containsKey("max_moisture")) {
      MAX_MOISTURE = doc["max_moisture"].as<float>();
    }

    if (doc.containsKey("max_temperature")) {
      MAX_TEMPERATURE = doc["max_temperature"].as<float>();
    }

    if (doc.containsKey("max_ec")) {
      MAX_EC = doc["max_ec"].as<float>();
    }

    Serial.println("=== THRESHOLD UPDATED ===");
    printThresholds();

    if (doc.containsKey("buzzer_muted")) {
      JsonObject muted = doc["buzzer_muted"];

      if (muted.containsKey("nitrogen")) MUTE_NITROGEN = muted["nitrogen"].as<bool>();
      if (muted.containsKey("phosphorus")) MUTE_PHOSPHORUS = muted["phosphorus"].as<bool>();
      if (muted.containsKey("potassium")) MUTE_POTASSIUM = muted["potassium"].as<bool>();
      if (muted.containsKey("ph")) MUTE_PH = muted["ph"].as<bool>();
      if (muted.containsKey("moisture")) MUTE_MOISTURE = muted["moisture"].as<bool>();
      if (muted.containsKey("temperature")) MUTE_TEMPERATURE = muted["temperature"].as<bool>();
      if (muted.containsKey("ec")) MUTE_EC = muted["ec"].as<bool>();
    }

    if (
      MIN_NITROGEN <= 0 &&
      MIN_PHOSPHORUS <= 0 &&
      MIN_POTASSIUM <= 0 &&
      MIN_PH <= 0 &&
      MIN_MOISTURE <= 0 &&
      MIN_TEMPERATURE <= 0 &&
      MIN_EC <= 0
    ) {
      digitalWrite(BUZZER_PIN, BUZZER_OFF);
      Serial.println("All minimum thresholds are 0. Buzzer forced OFF.");
    }
  }

  // =====================================================
  // UNKNOWN TOPIC
  // =====================================================
  else {
    Serial.print("Unknown MQTT Topic: ");
    Serial.println(topicStr);
  }
}

void setup() {
  Serial.begin(115200);

  // ================= RELAY SETUP =================
  pinMode(RELAY1, OUTPUT);
  pinMode(RELAY2, OUTPUT);
  pinMode(RELAY3, OUTPUT);
  pinMode(RELAY4, OUTPUT);

  // Relay OFF awal
  digitalWrite(RELAY1, RELAY_OFF);
  digitalWrite(RELAY2, RELAY_OFF);
  digitalWrite(RELAY3, RELAY_OFF);
  digitalWrite(RELAY4, RELAY_OFF);

  // ================= BUZZER SETUP =================
  pinMode(BUZZER_PIN, OUTPUT);

  // Buzzer awal mati
  digitalWrite(BUZZER_PIN, BUZZER_OFF);

  // ================= LCD SETUP =================
  setupLcd();
  showStartupScreen();

  // Setup pin MAX485
  pinMode(MAX485_DE_RE, OUTPUT);
  digitalWrite(MAX485_DE_RE, LOW);

  // Serial RS485 Communication
  Serial2.begin(4800, SERIAL_8N1, RXD2, TXD2);

  // Slave ID sensor default = 1
  node.begin(1, Serial2);

  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  setup_wifi();

  // SSL for HiveMQ Cloud
  espClient.setInsecure();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setBufferSize(2048);

  Serial.println("System Ready...");
}

void loop() {
  maintainNetwork();

  if (millis() - lastReadTime >= READ_INTERVAL) {
    Serial.println("\n===== READING SENSOR =====");

    sensorReadSuccess = true;

    uint8_t result = node.readHoldingRegisters(0x00, 7);

    float moisture = 0;
    float temperature = 0;
    float ec = 0;
    float ph = 0;
    float nitrogen = 0;
    float phosphorus = 0;
    float potassium = 0;

    if (result == node.ku8MBSuccess) {
      moisture =
        node.getResponseBuffer(0) / 10.0;

      int16_t tempRaw =
        (int16_t)node.getResponseBuffer(1);

      temperature =
        tempRaw / 10.0;

      ec =
        node.getResponseBuffer(2) / 1000.0;

      ph =
        node.getResponseBuffer(3) / 10.0;

      nitrogen =
        node.getResponseBuffer(4);

      phosphorus =
        node.getResponseBuffer(5);

      potassium =
        node.getResponseBuffer(6);

    } else {
      sensorReadSuccess = false;
      Serial.println("FAILED reading sensor registers!");
    }

    Serial.println("===== HASIL SENSOR =====");

    Serial.print("Soil Moisture : ");
    Serial.print(moisture);
    Serial.println(" %");

    Serial.print("Temperature   : ");
    Serial.print(temperature);
    Serial.println(" C");

    Serial.print("EC            : ");
    Serial.print(ec);
    Serial.println(" mS/cm");

    Serial.print("pH            : ");
    Serial.println(ph);

    Serial.print("Nitrogen      : ");
    Serial.print(nitrogen);
    Serial.println(" mg/kg");

    Serial.print("Phosphorus    : ");
    Serial.print(phosphorus);
    Serial.println(" mg/kg");

    Serial.print("Potassium     : ");
    Serial.print(potassium);
    Serial.println(" mg/kg");

    Serial.println("==========================");

    if (!sensorReadSuccess) {
      Serial.println("Sensor read failed!");
      Serial.println("Skipping threshold check...");

      digitalWrite(BUZZER_PIN, BUZZER_OFF);
      displaySensorError();

      lastReadTime = millis();
      return;
    }

    displaySensorData(
      nitrogen,
      phosphorus,
      potassium,
      ph,
      moisture,
      temperature
    );

    printThresholdCheck(
      nitrogen,
      phosphorus,
      potassium,
      ph,
      moisture,
      temperature,
      ec
    );

    bool nutrientAbnormal =
     isActiveAbnormal(nitrogen, MIN_NITROGEN, MAX_NITROGEN, MUTE_NITROGEN) ||
     isActiveAbnormal(phosphorus, MIN_PHOSPHORUS, MAX_PHOSPHORUS, MUTE_PHOSPHORUS) ||
     isActiveAbnormal(potassium, MIN_POTASSIUM, MAX_POTASSIUM, MUTE_POTASSIUM) ||
     isActiveAbnormal(ph, MIN_PH, MAX_PH, MUTE_PH) ||
     isActiveAbnormal(moisture, MIN_MOISTURE, MAX_MOISTURE, MUTE_MOISTURE) ||
     isActiveAbnormal(temperature, MIN_TEMPERATURE, MAX_TEMPERATURE, MUTE_TEMPERATURE) ||
     isActiveAbnormal(ec, MIN_EC, MAX_EC, MUTE_EC);

    Serial.print("nutrientAbnormal = ");
    Serial.println(nutrientAbnormal ? "TRUE" : "FALSE");

    if (nutrientAbnormal) {
      digitalWrite(BUZZER_PIN, BUZZER_ON);

      Serial.println("WARNING: Nutrisi di bawah ambang normal!");
      Serial.println("Buzzer ON");
    } else {
      digitalWrite(BUZZER_PIN, BUZZER_OFF);

      Serial.println("Nutrisi Normal");
      Serial.println("Buzzer OFF");
    }

    String payload = "{";

    payload += "\"moisture\":" + String(moisture, 1) + ",";
    payload += "\"temperature\":" + String(temperature, 1) + ",";
    payload += "\"ec\":" + String(ec, 2) + ",";
    payload += "\"ph\":" + String(ph, 1) + ",";
    payload += "\"nitrogen\":" + String(nitrogen, 1) + ",";
    payload += "\"phosphorus\":" + String(phosphorus, 1) + ",";
    payload += "\"potassium\":" + String(potassium, 1);
    payload += ",\"relay1\":" + String(digitalRead(RELAY1) == RELAY_ON ? 1 : 0);
    payload += ",\"relay2\":" + String(digitalRead(RELAY2) == RELAY_ON ? 1 : 0);
    payload += ",\"relay3\":" + String(digitalRead(RELAY3) == RELAY_ON ? 1 : 0);
    payload += ",\"relay4\":" + String(digitalRead(RELAY4) == RELAY_ON ? 1 : 0);
    payload += ",\"buzzer\":" + String(digitalRead(BUZZER_PIN) == BUZZER_ON ? 1 : 0);

    payload += "}";

    if (client.connected()) {
      Serial.println("Sending MQTT:");
      Serial.println(payload);

      if (client.publish(topic, payload.c_str())) {
        Serial.println("MQTT Publish Success");
      } else {
        Serial.println("MQTT Publish Failed");
      }
    } else {
      Serial.println("MQTT offline. Sensor data shown on LCD only.");
    }

    lastReadTime = millis();
  }
}
