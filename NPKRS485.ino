#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ================= WIFI =================
const char* WIFI_MANAGER_AP_NAME = "NutriXense";
const char* WIFI_MANAGER_AP_PASSWORD = "12345678";
WiFiManager wifiManager;

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

// ================= RTC DS3231 + EEPROM AT24C32 =================
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define DS3231_ADDRESS 0x68
#define AT24C32_ADDRESS 0x57
#define AT24C32_PAGE_SIZE 32

bool ds3231Available = false;
bool at24c32Available = false;

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
#define BUZZER_PIN 18

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
const unsigned long WIFI_CONNECT_TIMEOUT_SECONDS = 5;
const unsigned long MQTT_RETRY_INTERVAL = 5000;
const unsigned long STARTUP_SCREEN_DURATION = 3000;
const unsigned long STARTUP_ANIMATION_INTERVAL = 500;
const unsigned long LCD_SENSOR_SCREEN_DURATION = 10000;
const unsigned long LCD_TIME_SCREEN_DURATION = 3000;
const unsigned long BUZZER_ALERT_INTERVAL = 30000;
const unsigned long BUZZER_BEEP_INTERVAL = 180;
const byte BUZZER_ALERT_TOGGLES = 10;

unsigned long lastWifiAttemptTime = 0;
unsigned long lastMqttAttemptTime = 0;
bool wifiStarted = false;
bool wifiManagerPortalRunning = false;
bool wifiConnectedLogged = false;

unsigned long lcdScreenStartedAt = 0;
bool showSensorScreen = true;

unsigned long lastBuzzerAlertTime = 0;
unsigned long lastBuzzerToggleTime = 0;
byte buzzerToggleCount = 0;
bool buzzerAlertActive = false;
bool buzzerOutputState = false;

bool currentNutrientAbnormal = false;
bool hasValidSensorData = false;

float lastMoisture = 0;
float lastTemperature = 0;
float lastEc = 0;
float lastPh = 0;
float lastNitrogen = 0;
float lastPhosphorus = 0;
float lastPotassium = 0;

struct RtcDateTime {
  int year;
  byte month;
  byte day;
  byte hour;
  byte minute;
  byte second;
  byte dayOfWeek;
  bool valid;
};

RtcDateTime softwareClockBase;
unsigned long softwareClockSetMillis = 0;
bool softwareClockValid = false;

struct RelaySchedule {
  bool enabled;
  byte relay;
  int startYear;
  byte startMonth;
  byte startDay;
  int endYear;
  byte endMonth;
  byte endDay;
  byte hour;
  byte minute;
  unsigned int durationSeconds;
  byte daysMask;
  bool activeToday;
};

const byte MAX_SCHEDULES = 4;
RelaySchedule relaySchedules[MAX_SCHEDULES];

bool manualOverrideActive[4] = { false, false, false, false };
bool manualOverrideState[4] = { false, false, false, false };
bool lastScheduleAutoActive[4] = { false, false, false, false };

const byte SCHEDULE_STORAGE_MAGIC_0 = 'N';
const byte SCHEDULE_STORAGE_MAGIC_1 = 'X';
const byte SCHEDULE_STORAGE_MAGIC_2 = 'S';
const byte SCHEDULE_STORAGE_MAGIC_3 = '1';
const byte SCHEDULE_STORAGE_VERSION = 1;
const byte SCHEDULE_STORAGE_HEADER_BYTES = 8;
const byte SCHEDULE_STORAGE_RECORD_BYTES = 16;
const uint16_t SCHEDULE_STORAGE_ADDRESS = 0;
const uint16_t SCHEDULE_STORAGE_TOTAL_BYTES =
  SCHEDULE_STORAGE_HEADER_BYTES + (MAX_SCHEDULES * SCHEDULE_STORAGE_RECORD_BYTES);
const byte SCHEDULE_STORAGE_CHECKSUM_INDEX = 7;

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

String formatNpkValue(float value) {
  String formatted = String((int)round(value));

  while (formatted.length() < 3) {
    formatted = " " + formatted;
  }

  if (formatted.length() > 3) {
    formatted = formatted.substring(0, 3);
  }

  return formatted;
}

String twoDigits(byte value) {
  if (value < 10) {
    return "0" + String(value);
  }

  return String(value);
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
    "N:" + formatNpkValue(nitrogen) + " mg/kg pH:" + formatLcdValue(ph, 1)
  );

  clearLcdLine(2);
  lcd.setCursor(0, 2);
  lcd.print("P:");
  lcd.print(formatNpkValue(phosphorus));
  lcd.print(" mg/kg T:");
  lcd.print(formatLcdValue(temperature, 1));
  lcd.write(byte(223));
  lcd.print("C");

  printPaddedLcdLine(
    3,
    "K:" + formatNpkValue(potassium) + " mg/kg M:" + formatLcdValue(moisture, 1) + "%"
  );
}

const char* dayName(byte dayOfWeek) {
  switch (dayOfWeek) {
    case 1: return "Minggu";
    case 2: return "Senin";
    case 3: return "Selasa";
    case 4: return "Rabu";
    case 5: return "Kamis";
    case 6: return "Jumat";
    case 7: return "Sabtu";
    default: return "Hari";
  }
}

void displayTimeData(const RtcDateTime &now) {
  if (!now.valid) {
    printCenteredLcdLine(0, "NutriXense");
    printCenteredLcdLine(1, "Waktu belum set");
    printCenteredLcdLine(2, "Hubungkan MQTT");
    printCenteredLcdLine(3, "Set via Android");
    return;
  }

  printCenteredLcdLine(0, "NutriXense");
  printCenteredLcdLine(1, dayName(now.dayOfWeek));
  printCenteredLcdLine(
    2,
    twoDigits(now.day) + "/" + twoDigits(now.month) + "/" + String(now.year)
  );
  printCenteredLcdLine(
    3,
    twoDigits(now.hour) + ":" + twoDigits(now.minute) + ":" + twoDigits(now.second)
  );
}

void displaySensorError() {
  printPaddedLcdLine(0, "     NutriXense");
  printPaddedLcdLine(1, " Sensor read failed");
  printPaddedLcdLine(2, " Check RS485/NPK");
  printPaddedLcdLine(3, " Retrying...");
}

bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

byte daysInMonth(int year, byte month) {
  switch (month) {
    case 2: return isLeapYear(year) ? 29 : 28;
    case 4:
    case 6:
    case 9:
    case 11:
      return 30;
    default:
      return 31;
  }
}

bool isValidDateTime(const RtcDateTime &dateTime) {
  return dateTime.year >= 2024 && dateTime.year <= 2099 &&
         dateTime.month >= 1 && dateTime.month <= 12 &&
         dateTime.day >= 1 && dateTime.day <= daysInMonth(dateTime.year, dateTime.month) &&
         dateTime.hour <= 23 &&
         dateTime.minute <= 59 &&
         dateTime.second <= 59 &&
         dateTime.dayOfWeek >= 1 && dateTime.dayOfWeek <= 7;
}

byte decimalToBcd(byte value) {
  return ((value / 10) << 4) | (value % 10);
}

byte bcdToDecimal(byte value) {
  return ((value >> 4) * 10) + (value & 0x0F);
}

bool isI2CDeviceAvailable(byte address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool writeDs3231Register(byte reg, byte value) {
  if (!ds3231Available) {
    return false;
  }

  Wire.beginTransmission(DS3231_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readDs3231Registers(byte startReg, byte *buffer, byte length) {
  if (!ds3231Available) {
    return false;
  }

  Wire.beginTransmission(DS3231_ADDRESS);
  Wire.write(startReg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  byte readCount = Wire.requestFrom(DS3231_ADDRESS, length);

  if (readCount != length) {
    return false;
  }

  for (byte i = 0; i < length; i++) {
    buffer[i] = Wire.read();
  }

  return true;
}

bool writeDs3231DateTime(const RtcDateTime &dateTime) {
  if (!isValidDateTime(dateTime)) {
    return false;
  }

  Wire.beginTransmission(DS3231_ADDRESS);
  Wire.write(0x00);
  Wire.write(decimalToBcd(dateTime.second));
  Wire.write(decimalToBcd(dateTime.minute));
  Wire.write(decimalToBcd(dateTime.hour));
  Wire.write(decimalToBcd(dateTime.dayOfWeek));
  Wire.write(decimalToBcd(dateTime.day));
  Wire.write(decimalToBcd(dateTime.month));
  Wire.write(decimalToBcd(dateTime.year - 2000));

  if (Wire.endTransmission() != 0) {
    return false;
  }

  byte statusRegister = 0;

  if (readDs3231Registers(0x0F, &statusRegister, 1)) {
    writeDs3231Register(0x0F, statusRegister & ~0x80);
  }

  return true;
}

RtcDateTime readDs3231DateTime() {
  RtcDateTime dateTime;
  dateTime.valid = false;

  byte statusRegister = 0;

  if (!readDs3231Registers(0x0F, &statusRegister, 1) || (statusRegister & 0x80)) {
    return dateTime;
  }

  byte buffer[7];

  if (!readDs3231Registers(0x00, buffer, 7)) {
    return dateTime;
  }

  dateTime.second = bcdToDecimal(buffer[0] & 0x7F);
  dateTime.minute = bcdToDecimal(buffer[1] & 0x7F);
  dateTime.hour = bcdToDecimal(buffer[2] & 0x3F);
  dateTime.dayOfWeek = bcdToDecimal(buffer[3] & 0x07);
  dateTime.day = bcdToDecimal(buffer[4] & 0x3F);
  dateTime.month = bcdToDecimal(buffer[5] & 0x1F);
  dateTime.year = 2000 + bcdToDecimal(buffer[6]);
  dateTime.valid = isValidDateTime(dateTime);

  return dateTime;
}

void incrementDate(RtcDateTime &dateTime) {
  dateTime.day++;
  dateTime.dayOfWeek++;

  if (dateTime.dayOfWeek > 7) {
    dateTime.dayOfWeek = 1;
  }

  if (dateTime.day > daysInMonth(dateTime.year, dateTime.month)) {
    dateTime.day = 1;
    dateTime.month++;

    if (dateTime.month > 12) {
      dateTime.month = 1;
      dateTime.year++;
    }
  }
}

RtcDateTime advanceDateTime(RtcDateTime dateTime, unsigned long elapsedSeconds) {
  unsigned long secondsOfDay =
    (dateTime.hour * 3600UL) + (dateTime.minute * 60UL) + dateTime.second;
  unsigned long totalSeconds = secondsOfDay + elapsedSeconds;

  while (totalSeconds >= 86400UL) {
    totalSeconds -= 86400UL;
    incrementDate(dateTime);
  }

  dateTime.hour = totalSeconds / 3600UL;
  totalSeconds %= 3600UL;
  dateTime.minute = totalSeconds / 60UL;
  dateTime.second = totalSeconds % 60UL;
  dateTime.valid = true;

  return dateTime;
}

void setSoftwareClock(const RtcDateTime &dateTime) {
  softwareClockBase = dateTime;
  softwareClockBase.valid = true;
  softwareClockSetMillis = millis();
  softwareClockValid = true;
}

bool setCurrentDateTime(const RtcDateTime &dateTime) {
  if (!isValidDateTime(dateTime)) {
    return false;
  }

  bool rtcUpdated = writeDs3231DateTime(dateTime);
  setSoftwareClock(dateTime);

  if (rtcUpdated) {
    Serial.println("RTC DS3231 updated from Android MQTT payload.");
  } else {
    Serial.println("RTC DS3231 update failed. Using software clock until RTC is available.");
  }

  return rtcUpdated;
}

RtcDateTime readSoftwareClock() {
  RtcDateTime now = softwareClockBase;

  if (!softwareClockValid) {
    now.valid = false;
    return now;
  }

  return advanceDateTime(now, (millis() - softwareClockSetMillis) / 1000UL);
}

RtcDateTime readCurrentDateTime() {
  RtcDateTime rtcNow = readDs3231DateTime();

  if (rtcNow.valid) {
    setSoftwareClock(rtcNow);
    return rtcNow;
  }

  return readSoftwareClock();
}

int dateKey(int year, byte month, byte day) {
  return (year * 10000) + (month * 100) + day;
}

bool isScheduleDayActive(const RelaySchedule &schedule, byte dayOfWeek) {
  if (schedule.daysMask == 0) {
    return true;
  }

  return schedule.daysMask & (1 << (dayOfWeek - 1));
}

bool isScheduleActiveNow(const RelaySchedule &schedule, const RtcDateTime &now) {
  if (!schedule.enabled || !now.valid || schedule.relay < 1 || schedule.relay > 4) {
    return false;
  }

  int today = dateKey(now.year, now.month, now.day);
  int startDate = dateKey(schedule.startYear, schedule.startMonth, schedule.startDay);
  int endDate = dateKey(schedule.endYear, schedule.endMonth, schedule.endDay);

  if (today < startDate || today > endDate || !isScheduleDayActive(schedule, now.dayOfWeek)) {
    return false;
  }

  unsigned long nowSeconds = (now.hour * 3600UL) + (now.minute * 60UL) + now.second;
  unsigned long startSeconds = (schedule.hour * 3600UL) + (schedule.minute * 60UL);
  unsigned long endSeconds = startSeconds + schedule.durationSeconds;

  return nowSeconds >= startSeconds && nowSeconds < endSeconds;
}

int relayPin(byte relay) {
  switch (relay) {
    case 1: return RELAY1;
    case 2: return RELAY2;
    case 3: return RELAY3;
    case 4: return RELAY4;
    default: return -1;
  }
}

void setRelayState(byte relay, bool isOn) {
  int pin = relayPin(relay);

  if (pin < 0) {
    return;
  }

  digitalWrite(pin, isOn ? RELAY_ON : RELAY_OFF);
}

void setManualRelayOverride(byte relay, bool isOn) {
  if (relay < 1 || relay > 4) {
    return;
  }

  manualOverrideActive[relay - 1] = true;
  manualOverrideState[relay - 1] = isOn;
  setRelayState(relay, isOn);

  Serial.print("Manual override Relay");
  Serial.print(relay);
  Serial.print(": ");
  Serial.println(isOn ? "ON" : "OFF");
}

void clearManualOverride(byte relay) {
  if (relay < 1 || relay > 4 || !manualOverrideActive[relay - 1]) {
    return;
  }

  manualOverrideActive[relay - 1] = false;

  Serial.print("Manual override Relay");
  Serial.print(relay);
  Serial.println(" released by schedule transition.");
}

void applyRelaySchedules(const RtcDateTime &now) {
  bool relayAutoActive[4] = { false, false, false, false };
  bool relayConfigured[4] = { false, false, false, false };

  for (byte i = 0; i < MAX_SCHEDULES; i++) {
    RelaySchedule &schedule = relaySchedules[i];

    if (schedule.enabled && schedule.relay >= 1 && schedule.relay <= 4) {
      relayConfigured[schedule.relay - 1] = true;

      if (isScheduleActiveNow(schedule, now)) {
        relayAutoActive[schedule.relay - 1] = true;
      }
    }
  }

  for (byte i = 0; i < 4; i++) {
    if (relayConfigured[i]) {
      if (relayAutoActive[i] != lastScheduleAutoActive[i]) {
        clearManualOverride(i + 1);
      }

      lastScheduleAutoActive[i] = relayAutoActive[i];

      if (manualOverrideActive[i]) {
        setRelayState(i + 1, manualOverrideState[i]);
        continue;
      }

      setRelayState(i + 1, relayAutoActive[i]);
    }
  }
}

void maintainLcdDisplay() {
  RtcDateTime now = readCurrentDateTime();

  if (!now.valid) {
    showSensorScreen = true;
  }

  unsigned long duration = showSensorScreen ? LCD_SENSOR_SCREEN_DURATION : LCD_TIME_SCREEN_DURATION;

  if (now.valid && millis() - lcdScreenStartedAt >= duration) {
    showSensorScreen = !showSensorScreen;
    lcdScreenStartedAt = millis();
  }

  if (showSensorScreen) {
    if (hasValidSensorData) {
      displaySensorData(lastNitrogen, lastPhosphorus, lastPotassium, lastPh, lastMoisture, lastTemperature);
    } else {
      displaySensorError();
    }
  } else {
    displayTimeData(now);
  }
}

void maintainBuzzer(bool nutrientAbnormal) {
  if (!nutrientAbnormal) {
    buzzerAlertActive = false;
    buzzerToggleCount = 0;
    buzzerOutputState = false;
    digitalWrite(BUZZER_PIN, BUZZER_OFF);
    return;
  }

  if (!buzzerAlertActive && millis() - lastBuzzerAlertTime >= BUZZER_ALERT_INTERVAL) {
    buzzerAlertActive = true;
    buzzerToggleCount = 0;
    buzzerOutputState = false;
    lastBuzzerToggleTime = millis();
    lastBuzzerAlertTime = millis();
  }

  if (buzzerAlertActive && millis() - lastBuzzerToggleTime >= BUZZER_BEEP_INTERVAL) {
    buzzerOutputState = !buzzerOutputState;
    digitalWrite(BUZZER_PIN, buzzerOutputState ? BUZZER_ON : BUZZER_OFF);
    buzzerToggleCount++;
    lastBuzzerToggleTime = millis();

    if (buzzerToggleCount >= BUZZER_ALERT_TOGGLES) {
      buzzerAlertActive = false;
      buzzerOutputState = false;
      digitalWrite(BUZZER_PIN, BUZZER_OFF);
    }
  }
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
  Serial.println("Starting WiFiManager...");
  WiFi.mode(WIFI_STA);

  wifiManager.setConnectTimeout(WIFI_CONNECT_TIMEOUT_SECONDS);
  wifiManager.setConfigPortalBlocking(false);

  // Uncomment this line only when you want to erase saved WiFi credentials.
  // wifiManager.resetSettings();

  bool connected = wifiManager.autoConnect(WIFI_MANAGER_AP_NAME, WIFI_MANAGER_AP_PASSWORD);

  wifiStarted = true;
  lastWifiAttemptTime = millis();

  if (!connected) {
    wifiManagerPortalRunning = true;
    wifiConnectedLogged = false;
    Serial.println("WiFi not connected yet.");
    Serial.print("Config portal active. Connect your phone to AP: ");
    Serial.println(WIFI_MANAGER_AP_NAME);
    return;
  }

  wifiManagerPortalRunning = false;
  wifiConnectedLogged = true;
  Serial.println("WiFi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void tryReconnectSavedWiFi() {
  Serial.println("Trying saved WiFi credentials in background...");

  if (wifiManagerPortalRunning) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }

  bool reconnectStarted = WiFi.reconnect();
  if (!reconnectStarted) {
    WiFi.begin();
  }

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

    client.subscribe("nutrixense/schedule");
    Serial.println("Subscribed: nutrixense/schedule");
  } else {
    Serial.print("Failed, rc=");
    Serial.print(client.state());
    Serial.println(" will retry later.");
  }
}

void maintainNetwork() {
  if (wifiStarted) {
    wifiManager.process();
  }

  if (WiFi.status() != WL_CONNECTED) {
    wifiConnectedLogged = false;

    if (!wifiStarted) {
      setup_wifi();
      return;
    }

    if (millis() - lastWifiAttemptTime >= WIFI_RETRY_INTERVAL) {
      tryReconnectSavedWiFi();
    }

    return;
  }

  wifiManagerPortalRunning = false;

  if (!wifiConnectedLogged) {
    wifiConnectedLogged = true;
    Serial.println("WiFi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
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

byte calculateDayOfWeek(int year, byte month, byte day) {
  if (month < 3) {
    month += 12;
    year--;
  }

  int k = year % 100;
  int j = year / 100;
  int h = (day + ((13 * (month + 1)) / 5) + k + (k / 4) + (j / 4) + (5 * j)) % 7;

  return ((h + 6) % 7) + 1;
}

bool parseDateString(const char* dateText, int &year, byte &month, byte &day) {
  if (dateText == nullptr || strlen(dateText) < 10) {
    return false;
  }

  String value = String(dateText);
  year = value.substring(0, 4).toInt();
  month = value.substring(5, 7).toInt();
  day = value.substring(8, 10).toInt();

  return year >= 2024 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

bool parseTimeString(const char* timeText, byte &hour, byte &minute) {
  if (timeText == nullptr || strlen(timeText) < 5) {
    return false;
  }

  String value = String(timeText);
  hour = value.substring(0, 2).toInt();
  minute = value.substring(3, 5).toInt();

  return hour <= 23 && minute <= 59;
}

byte parseDaysMask(JsonVariant days) {
  if (!days.is<JsonArray>()) {
    return 0;
  }

  byte mask = 0;

  for (JsonVariant day : days.as<JsonArray>()) {
    byte dayOfWeek = day.as<byte>();

    if (dayOfWeek >= 1 && dayOfWeek <= 7) {
      mask |= (1 << (dayOfWeek - 1));
    }
  }

  return mask;
}

void writeUint16(byte *buffer, uint16_t index, uint16_t value) {
  buffer[index] = (value >> 8) & 0xFF;
  buffer[index + 1] = value & 0xFF;
}

uint16_t readUint16(const byte *buffer, uint16_t index) {
  return (uint16_t(buffer[index]) << 8) | buffer[index + 1];
}

byte calculateScheduleStorageChecksum(byte *buffer) {
  byte checksum = 0;
  byte originalChecksum = buffer[SCHEDULE_STORAGE_CHECKSUM_INDEX];
  buffer[SCHEDULE_STORAGE_CHECKSUM_INDEX] = 0;

  for (uint16_t i = 0; i < SCHEDULE_STORAGE_TOTAL_BYTES; i++) {
    checksum += buffer[i];
  }

  buffer[SCHEDULE_STORAGE_CHECKSUM_INDEX] = originalChecksum;
  return checksum;
}

bool readAt24C32Bytes(uint16_t address, byte *buffer, uint16_t length) {
  if (!at24c32Available) {
    return false;
  }

  uint16_t offset = 0;

  while (offset < length) {
    byte chunk = length - offset;

    if (chunk > AT24C32_PAGE_SIZE) {
      chunk = AT24C32_PAGE_SIZE;
    }

    Wire.beginTransmission(AT24C32_ADDRESS);
    Wire.write((address + offset) >> 8);
    Wire.write((address + offset) & 0xFF);

    if (Wire.endTransmission(false) != 0) {
      return false;
    }

    byte readCount = Wire.requestFrom(AT24C32_ADDRESS, chunk);

    if (readCount != chunk) {
      return false;
    }

    for (byte i = 0; i < chunk; i++) {
      buffer[offset + i] = Wire.read();
    }

    offset += chunk;
  }

  return true;
}

bool writeAt24C32Bytes(uint16_t address, const byte *buffer, uint16_t length) {
  if (!at24c32Available) {
    return false;
  }

  uint16_t offset = 0;

  while (offset < length) {
    byte pageRemaining = AT24C32_PAGE_SIZE - ((address + offset) % AT24C32_PAGE_SIZE);
    byte chunk = length - offset;

    if (chunk > pageRemaining) {
      chunk = pageRemaining;
    }

    Wire.beginTransmission(AT24C32_ADDRESS);
    Wire.write((address + offset) >> 8);
    Wire.write((address + offset) & 0xFF);

    for (byte i = 0; i < chunk; i++) {
      Wire.write(buffer[offset + i]);
    }

    if (Wire.endTransmission() != 0) {
      return false;
    }

    delay(6);
    offset += chunk;
  }

  return true;
}

bool isValidSchedule(const RelaySchedule &schedule) {
  if (!schedule.enabled) {
    return true;
  }

  return schedule.relay >= 1 && schedule.relay <= 4 &&
         schedule.startYear >= 2024 && schedule.startYear <= 2099 &&
         schedule.startMonth >= 1 && schedule.startMonth <= 12 &&
         schedule.startDay >= 1 && schedule.startDay <= daysInMonth(schedule.startYear, schedule.startMonth) &&
         schedule.endYear >= 2024 && schedule.endYear <= 2099 &&
         schedule.endMonth >= 1 && schedule.endMonth <= 12 &&
         schedule.endDay >= 1 && schedule.endDay <= daysInMonth(schedule.endYear, schedule.endMonth) &&
         dateKey(schedule.startYear, schedule.startMonth, schedule.startDay) <=
           dateKey(schedule.endYear, schedule.endMonth, schedule.endDay) &&
         schedule.hour <= 23 &&
         schedule.minute <= 59 &&
         schedule.durationSeconds > 0 && schedule.durationSeconds <= 65535 &&
         schedule.daysMask <= 0x7F;
}

void clearSchedules() {
  for (byte i = 0; i < MAX_SCHEDULES; i++) {
    relaySchedules[i].enabled = false;
    relaySchedules[i].relay = i + 1;
    relaySchedules[i].startYear = 2024;
    relaySchedules[i].startMonth = 1;
    relaySchedules[i].startDay = 1;
    relaySchedules[i].endYear = 2099;
    relaySchedules[i].endMonth = 12;
    relaySchedules[i].endDay = 31;
    relaySchedules[i].hour = 6;
    relaySchedules[i].minute = 0;
    relaySchedules[i].durationSeconds = 5;
    relaySchedules[i].daysMask = 0;
    relaySchedules[i].activeToday = false;
  }
}

bool saveSchedulesToEeprom() {
  byte buffer[SCHEDULE_STORAGE_TOTAL_BYTES];

  for (uint16_t i = 0; i < SCHEDULE_STORAGE_TOTAL_BYTES; i++) {
    buffer[i] = 0;
  }

  buffer[0] = SCHEDULE_STORAGE_MAGIC_0;
  buffer[1] = SCHEDULE_STORAGE_MAGIC_1;
  buffer[2] = SCHEDULE_STORAGE_MAGIC_2;
  buffer[3] = SCHEDULE_STORAGE_MAGIC_3;
  buffer[4] = SCHEDULE_STORAGE_VERSION;
  buffer[5] = MAX_SCHEDULES;
  buffer[6] = SCHEDULE_STORAGE_RECORD_BYTES;

  for (byte i = 0; i < MAX_SCHEDULES; i++) {
    const RelaySchedule &schedule = relaySchedules[i];
    uint16_t index = SCHEDULE_STORAGE_HEADER_BYTES + (i * SCHEDULE_STORAGE_RECORD_BYTES);

    buffer[index] = schedule.enabled ? 1 : 0;
    buffer[index + 1] = schedule.relay;
    writeUint16(buffer, index + 2, schedule.startYear);
    buffer[index + 4] = schedule.startMonth;
    buffer[index + 5] = schedule.startDay;
    writeUint16(buffer, index + 6, schedule.endYear);
    buffer[index + 8] = schedule.endMonth;
    buffer[index + 9] = schedule.endDay;
    buffer[index + 10] = schedule.hour;
    buffer[index + 11] = schedule.minute;
    writeUint16(buffer, index + 12, schedule.durationSeconds);
    buffer[index + 14] = schedule.daysMask;
  }

  buffer[SCHEDULE_STORAGE_CHECKSUM_INDEX] = calculateScheduleStorageChecksum(buffer);

  if (!writeAt24C32Bytes(SCHEDULE_STORAGE_ADDRESS, buffer, SCHEDULE_STORAGE_TOTAL_BYTES)) {
    Serial.println("Failed saving schedules to AT24C32.");
    return false;
  }

  Serial.println("Schedules saved to AT24C32 EEPROM.");
  return true;
}

bool loadSchedulesFromEeprom() {
  byte buffer[SCHEDULE_STORAGE_TOTAL_BYTES];

  if (!readAt24C32Bytes(SCHEDULE_STORAGE_ADDRESS, buffer, SCHEDULE_STORAGE_TOTAL_BYTES)) {
    Serial.println("Failed reading schedules from AT24C32.");
    return false;
  }

  byte storedChecksum = buffer[SCHEDULE_STORAGE_CHECKSUM_INDEX];

  if (buffer[0] != SCHEDULE_STORAGE_MAGIC_0 ||
      buffer[1] != SCHEDULE_STORAGE_MAGIC_1 ||
      buffer[2] != SCHEDULE_STORAGE_MAGIC_2 ||
      buffer[3] != SCHEDULE_STORAGE_MAGIC_3 ||
      buffer[4] != SCHEDULE_STORAGE_VERSION ||
      buffer[5] != MAX_SCHEDULES ||
      buffer[6] != SCHEDULE_STORAGE_RECORD_BYTES ||
      storedChecksum != calculateScheduleStorageChecksum(buffer)) {
    Serial.println("No valid saved schedule found in AT24C32.");
    return false;
  }

  for (byte i = 0; i < MAX_SCHEDULES; i++) {
    RelaySchedule schedule;
    uint16_t index = SCHEDULE_STORAGE_HEADER_BYTES + (i * SCHEDULE_STORAGE_RECORD_BYTES);

    schedule.enabled = buffer[index] == 1;
    schedule.relay = buffer[index + 1];
    schedule.startYear = readUint16(buffer, index + 2);
    schedule.startMonth = buffer[index + 4];
    schedule.startDay = buffer[index + 5];
    schedule.endYear = readUint16(buffer, index + 6);
    schedule.endMonth = buffer[index + 8];
    schedule.endDay = buffer[index + 9];
    schedule.hour = buffer[index + 10];
    schedule.minute = buffer[index + 11];
    schedule.durationSeconds = readUint16(buffer, index + 12);
    schedule.daysMask = buffer[index + 14];
    schedule.activeToday = false;

    if (!isValidSchedule(schedule)) {
      Serial.println("Saved schedule is invalid. Ignoring AT24C32 data.");
      return false;
    }

    relaySchedules[i] = schedule;
  }

  Serial.println("Schedules loaded from AT24C32 EEPROM.");
  return true;
}

void setupRtcAndScheduleStorage() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  ds3231Available = isI2CDeviceAvailable(DS3231_ADDRESS);
  at24c32Available = isI2CDeviceAvailable(AT24C32_ADDRESS);

  Serial.print("DS3231 RTC: ");
  Serial.println(ds3231Available ? "detected" : "not detected");

  Serial.print("AT24C32 EEPROM: ");
  Serial.println(at24c32Available ? "detected" : "not detected");

  clearSchedules();

  if (at24c32Available) {
    loadSchedulesFromEeprom();
  }

  RtcDateTime rtcNow = readDs3231DateTime();

  if (rtcNow.valid) {
    setSoftwareClock(rtcNow);
    Serial.println("System time loaded from DS3231 RTC.");
  } else if (ds3231Available) {
    Serial.println("DS3231 time is not valid yet. Set time from Android.");
  }
}

void updateRtcFromJson(JsonObject rtcConfig) {
  if (rtcConfig.isNull()) {
    return;
  }

  bool forceUpdate = (rtcConfig["force_update"] | false) || (rtcConfig["force"] | false);
  RtcDateTime currentRtc = readDs3231DateTime();

  if (currentRtc.valid && !forceUpdate) {
    setSoftwareClock(currentRtc);
    Serial.println("RTC MQTT payload ignored. Time remains locked to DS3231.");
    return;
  }

  RtcDateTime dateTime;
  dateTime.year = rtcConfig["year"] | 0;
  dateTime.month = rtcConfig["month"] | 0;
  dateTime.day = rtcConfig["day"] | 0;
  dateTime.hour = rtcConfig["hour"] | 0;
  dateTime.minute = rtcConfig["minute"] | 0;
  dateTime.second = rtcConfig["second"] | 0;
  dateTime.dayOfWeek = rtcConfig["day_of_week"] | 0;

  if (dateTime.dayOfWeek < 1 || dateTime.dayOfWeek > 7) {
    dateTime.dayOfWeek = calculateDayOfWeek(dateTime.year, dateTime.month, dateTime.day);
  }

  dateTime.valid = isValidDateTime(dateTime);

  if (dateTime.valid) {
    setCurrentDateTime(dateTime);
  } else {
    Serial.println("Invalid time payload. System time not updated.");
  }
}

void updateSchedulesFromJson(JsonArray schedules) {
  if (schedules.isNull()) {
    return;
  }

  clearSchedules();
  byte index = 0;

  for (JsonObject item : schedules) {
    if (index >= MAX_SCHEDULES) {
      break;
    }

    RelaySchedule &schedule = relaySchedules[index];
    schedule.enabled = item["enabled"] | true;
    schedule.relay = item["relay"] | 1;
    schedule.durationSeconds = item["duration_seconds"] | 5;
    schedule.daysMask = parseDaysMask(item["days"]);

    const char* startDateText = item["start_date"] | "2024-01-01";
    const char* endDateText = item["end_date"] | "2099-12-31";
    const char* timeText = item["time"] | "06:00";

    bool validStartDate = parseDateString(startDateText, schedule.startYear, schedule.startMonth, schedule.startDay);
    bool validEndDate = parseDateString(endDateText, schedule.endYear, schedule.endMonth, schedule.endDay);
    bool validTime = parseTimeString(timeText, schedule.hour, schedule.minute);

    if (!validStartDate || !validEndDate || !validTime || !isValidSchedule(schedule)) {
      schedule.enabled = false;
    }

    index++;
  }

  Serial.println("Relay schedules updated from Android.");
  saveSchedulesToEeprom();
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

    const char* commandSource = doc["source"] | "";
    bool isManualCommand =
      strcmp(commandSource, "manual") == 0 || doc.containsKey("manual_override");

    for (byte relay = 1; relay <= 4; relay++) {
      char relayKey[8];
      snprintf(relayKey, sizeof(relayKey), "relay%d", relay);

      if (!doc.containsKey(relayKey)) {
        continue;
      }

      int state = doc[relayKey].as<int>();
      bool isOn = state == 1;

      if (isManualCommand) {
        setManualRelayOverride(relay, isOn);
      } else {
        manualOverrideActive[relay - 1] = false;
        setRelayState(relay, isOn);

        Serial.print("Relay");
        Serial.print(relay);
        Serial.print(": ");
        Serial.println(isOn ? "ON" : "OFF");
      }
    }
  }

  // =====================================================
  // MQTT TOPIC : nutrixense/schedule
  // Payload example:
  // {
  //   "rtc": {"year":2026,"month":7,"day":2,"hour":6,"minute":30,"second":0,"day_of_week":5},
  //   "schedules": [
  //     {"enabled":true,"relay":1,"start_date":"2026-07-02","end_date":"2026-12-31","time":"06:30","duration_seconds":5,"days":[2,3,4,5,6]}
  //   ]
  // }
  // RTC is written only when DS3231 time is invalid, or rtc.force_update=true.
  // Schedules are saved to AT24C32.
  // =====================================================
  else if (topicStr == "nutrixense/schedule") {
    Serial.println("=== SCHEDULE CONFIG ===");

    updateRtcFromJson(doc["rtc"].as<JsonObject>());

    if (doc.containsKey("schedules")) {
      updateSchedulesFromJson(doc["schedules"].as<JsonArray>());
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
  setupRtcAndScheduleStorage();

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
  lastBuzzerAlertTime = millis() - BUZZER_ALERT_INTERVAL;

  // ================= LCD SETUP =================
  setupLcd();
  showStartupScreen();
  lcdScreenStartedAt = millis();

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
  client.setSocketTimeout(2);

  Serial.println("System Ready...");
}

void loop() {
  maintainNetwork();
  RtcDateTime now = readCurrentDateTime();
  applyRelaySchedules(now);
  maintainLcdDisplay();
  maintainBuzzer(currentNutrientAbnormal);

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

      currentNutrientAbnormal = false;
      hasValidSensorData = false;

      lastReadTime = millis();
      return;
    }

    lastMoisture = moisture;
    lastTemperature = temperature;
    lastEc = ec;
    lastPh = ph;
    lastNitrogen = nitrogen;
    lastPhosphorus = phosphorus;
    lastPotassium = potassium;
    hasValidSensorData = true;

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
    currentNutrientAbnormal = nutrientAbnormal;

    if (nutrientAbnormal) {
      Serial.println("WARNING: Nutrisi di bawah ambang normal!");
      Serial.println("Buzzer beep pattern armed");
    } else {
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
    payload += ",\"rtc_valid\":" + String(now.valid ? 1 : 0);
    payload += ",\"rtc_available\":" + String(ds3231Available ? 1 : 0);
    payload += ",\"schedule_storage\":" + String(at24c32Available ? 1 : 0);

    if (now.valid) {
      payload += ",\"rtc\":{";
      payload += "\"year\":" + String(now.year) + ",";
      payload += "\"month\":" + String(now.month) + ",";
      payload += "\"day\":" + String(now.day) + ",";
      payload += "\"hour\":" + String(now.hour) + ",";
      payload += "\"minute\":" + String(now.minute) + ",";
      payload += "\"second\":" + String(now.second) + ",";
      payload += "\"day_of_week\":" + String(now.dayOfWeek);
      payload += "}";
    }

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
