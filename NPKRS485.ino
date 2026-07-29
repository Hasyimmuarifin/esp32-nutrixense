#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <LittleFS.h>

#define DEBUG_MODE 0
#if DEBUG_MODE
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTLN_EMPTY() Serial.println()
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTLN_EMPTY()
#endif

// ================= WIFI =================
const char* WIFI_MANAGER_AP_NAME = "NutriXense";
const char* WIFI_MANAGER_AP_PASSWORD = "12345678";
WiFiManager wifiManager;
unsigned long wifiDisconnectedSince = 0;

// ================= HIVEMQ =================
const char* mqtt_server = "a8805b4f45744c3f9ac83882e423e0c0.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "hasyim";
const char* mqtt_pass = "hasyimHiveMQTT@22";
const char* realtimeTopic = "nutrixense/sensor";

// ================= LITTLEFS OFFLINE LOGGER =================
const char* historyTopic = "nutrixense/history";
const char* OFFLINE_LOG_FILE = "/offline_history.jsonl";
const char* OFFLINE_TEMP_FILE = "/offline_history_tmp.jsonl";

const int MAX_SYNC_PER_LOOP = 20;
const unsigned long OFFLINE_SYNC_INTERVAL = 5000;
unsigned long lastOfflineSyncTime = 0;

unsigned long offlineSavedCount = 0;
unsigned long offlineSyncedCount = 0;

// ================= MQTT =================
WiFiClientSecure espClient;
PubSubClient client(espClient);
byte mqttFailedAttempts = 0;

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
bool littleFsReady = false;

// Control Pin MAX485
#define MAX485_DE_RE 4

// Use Serial2 UART ESP32
#define RXD2 16
#define TXD2 17

// ================= RELAY =================
// Relay module uses active HIGH: LOW = OFF, HIGH = ON
#define RELAY1 25
#define RELAY2 26
#define RELAY3 27
#define RELAY4 14
#define RELAY_OFF LOW
#define RELAY_ON HIGH
const byte RELAY_PINS[4] = { RELAY1, RELAY2, RELAY3, RELAY4 };

// ================= BUZZER =================
#define BUZZER_PIN 18

// If your buzzer still sounds when it should be OFF,
// swap these two values.
#define BUZZER_ON HIGH
#define BUZZER_OFF LOW

// ================= TIMING =================
unsigned long lastReadTime = 0;
const unsigned long READ_INTERVAL = 2000;
const unsigned long HISTORY_LOG_INTERVAL = 60000;
unsigned long lastHistoryLogTime = 0;

const unsigned long WIFI_RETRY_INTERVAL = 15000;
const unsigned long WIFI_PORTAL_OPEN_DELAY = 60000;
const unsigned long WIFI_PORTAL_MAX_DURATION = 300000;
const unsigned long WIFI_CONNECT_TIMEOUT_SECONDS = 20;
unsigned long wifiPortalStartedAt = 0;

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
bool offlineFileConflict = false;

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

// Forward declaration
String formatTimestamp(const RtcDateTime &now);

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
    DEBUG_PRINTLN("RTC DS3231 updated from Android MQTT payload.");
  } else {
    DEBUG_PRINTLN("RTC DS3231 update failed. Using software clock until RTC is available.");
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
  if (relay < 1 || relay > 4) {
    return -1;
  }

  return RELAY_PINS[relay - 1];
}

void setupRelays() {
  for (byte i = 0; i < 4; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], RELAY_OFF);
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

  DEBUG_PRINT("Manual override Relay");
  DEBUG_PRINT(relay);
  DEBUG_PRINT(": ");
  DEBUG_PRINTLN(isOn ? "ON" : "OFF");
}

void clearManualOverride(byte relay) {
  if (relay < 1 || relay > 4 || !manualOverrideActive[relay - 1]) {
    return;
  }

  manualOverrideActive[relay - 1] = false;

  DEBUG_PRINT("Manual override Relay");
  DEBUG_PRINT(relay);
  DEBUG_PRINTLN(" released by schedule transition.");
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

void printRange(const char* label, float minValue, float maxValue) {
  DEBUG_PRINT(label);
  DEBUG_PRINT(" : ");
  DEBUG_PRINT(minValue);
  DEBUG_PRINT(" - ");
  DEBUG_PRINTLN(maxValue);
}

void printThresholds() {
  DEBUG_PRINTLN("===== CURRENT THRESHOLDS =====");

  printRange("Nitrogen    ", MIN_NITROGEN, MAX_NITROGEN);
  printRange("Phosphorus  ", MIN_PHOSPHORUS, MAX_PHOSPHORUS);
  printRange("Potassium   ", MIN_POTASSIUM, MAX_POTASSIUM);
  printRange("pH          ", MIN_PH, MAX_PH);
  printRange("Moisture    ", MIN_MOISTURE, MAX_MOISTURE);
  printRange("Temperature ", MIN_TEMPERATURE, MAX_TEMPERATURE);
  printRange("EC          ", MIN_EC, MAX_EC);
}

void printValueRange(const char* label, float value, float minValue, float maxValue) {
  DEBUG_PRINT(label);
  DEBUG_PRINT(" : ");
  DEBUG_PRINT(value);
  DEBUG_PRINT(" (");
  DEBUG_PRINT(minValue);
  DEBUG_PRINT(" - ");
  DEBUG_PRINT(maxValue);
  DEBUG_PRINTLN(")");
}

void printThresholdCheck(float nitrogen, float phosphorus, float potassium, float ph, float moisture, float temperature, float ec) {
  DEBUG_PRINTLN("===== DEBUG THRESHOLD CHECK =====");

  printValueRange("Nitrogen    ", nitrogen, MIN_NITROGEN, MAX_NITROGEN);
  printValueRange("Phosphorus  ", phosphorus, MIN_PHOSPHORUS, MAX_PHOSPHORUS);
  printValueRange("Potassium   ", potassium, MIN_POTASSIUM, MAX_POTASSIUM);
  printValueRange("pH          ", ph, MIN_PH, MAX_PH);
  printValueRange("Moisture    ", moisture, MIN_MOISTURE, MAX_MOISTURE);
  printValueRange("Temperature ", temperature, MIN_TEMPERATURE, MAX_TEMPERATURE);
  printValueRange("EC          ", ec, MIN_EC, MAX_EC);
}

// ================= WIFI =================
void setup_wifi() {
  DEBUG_PRINTLN("Starting WiFiManager...");

  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);

  // Hentikan koneksi STA sebelumnya tanpa menghapus SSID.
  WiFi.disconnect(false, false);
  delay(300);

  wifiManager.setDebugOutput(DEBUG_MODE == 1);
  wifiManager.setCleanConnect(true);
  wifiManager.setConnectTimeout(WIFI_CONNECT_TIMEOUT_SECONDS);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setWiFiAutoReconnect(false);
  wifiManager.setMinimumSignalQuality(0);
  wifiManager.setRemoveDuplicateAPs(false);

  // Jangan aktifkan pada program final.
  // wifiManager.resetSettings();

  bool connected = wifiManager.autoConnect(WIFI_MANAGER_AP_NAME, WIFI_MANAGER_AP_PASSWORD);

  wifiStarted = true;
  lastWifiAttemptTime = millis();

  if (connected || WiFi.status() == WL_CONNECTED) {
    wifiManagerPortalRunning = false;
    wifiConnectedLogged = true;

    WiFi.setAutoReconnect(false);

    DEBUG_PRINTLN("WiFi connected.");
    DEBUG_PRINT("IP address: ");
    DEBUG_PRINTLN(WiFi.localIP());
    return;
  }

  // autoConnect non-blocking membuka portal dan kembali ke program.
  wifiManagerPortalRunning = true;
  wifiPortalStartedAt = millis();
  wifiConnectedLogged = false;

  DEBUG_PRINTLN("WiFi not connected.");
  DEBUG_PRINT("Configuration portal active: ");
  DEBUG_PRINTLN(WIFI_MANAGER_AP_NAME);
}

void startWifiConfigPortal() {
  if (wifiManagerPortalRunning) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  DEBUG_PRINTLN("Starting NutriXense config portal...");

  // Cegah proses reconnect otomatis bertabrakan dengan scan.
  WiFi.setAutoReconnect(false);

  // Batalkan koneksi STA yang masih berjalan,
  // tanpa menghapus kredensial dari NVS.
  WiFi.disconnect(false, false);
  delay(500);

  WiFi.mode(WIFI_AP_STA);
  WiFi.scanDelete();
  delay(200);

  wifiManager.startConfigPortal(
    WIFI_MANAGER_AP_NAME,
    WIFI_MANAGER_AP_PASSWORD
  );

  wifiManagerPortalRunning = true;
  wifiPortalStartedAt = millis();
  lastWifiAttemptTime = millis();

  DEBUG_PRINT("Portal IP: ");
  DEBUG_PRINTLN(WiFi.softAPIP());
}

void tryReconnectSavedWiFi() {
  // Sangat penting:
  // jangan reconnect ketika portal sedang scan.
  if (wifiManagerPortalRunning) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  DEBUG_PRINTLN("Trying saved WiFi credentials...");

  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);

  // Membatalkan kemungkinan percobaan sebelumnya.
  WiFi.disconnect(false, false);
  delay(150);

  // begin() tanpa parameter menggunakan konfigurasi tersimpan.
  WiFi.begin();

  lastWifiAttemptTime = millis();
}

void stopWifiConfigPortalSafe() {
  if (!wifiManagerPortalRunning) {
    return;
  }

  DEBUG_PRINTLN("Stopping WiFi configuration portal...");

  wifiManager.stopConfigPortal();
  WiFi.softAPdisconnect(false);

  wifiManagerPortalRunning = false;
  wifiPortalStartedAt = 0;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
}

// ================= MQTT CONNECT =================
void reconnect() {
  if (WiFi.status() != WL_CONNECTED || client.connected()) {
    return;
  }

  lastMqttAttemptTime = millis();
  DEBUG_PRINT("Connecting MQTT...");

  if (client.connect("ESP32_Client", mqtt_user, mqtt_pass)) {
    mqttFailedAttempts = 0;
    DEBUG_PRINTLN("Connected!");

    client.subscribe("nutrixense/control");
    DEBUG_PRINTLN("Subscribed: nutrixense/control");

    client.subscribe("nutrixense/config");
    DEBUG_PRINTLN("Subscribed: nutrixense/config");

    client.subscribe("nutrixense/schedule");
    DEBUG_PRINTLN("Subscribed: nutrixense/schedule");
  } else {
    if (mqttFailedAttempts < 255) {
      mqttFailedAttempts++;
    }

    DEBUG_PRINT("MQTT Failed, rc=");
    DEBUG_PRINT(client.state());
    DEBUG_PRINT(" attempts=");
    DEBUG_PRINTLN(mqttFailedAttempts);

    DEBUG_PRINTLN("MQTT will retry later.");
  }
}

void maintainNetwork() {
  if (!wifiStarted) {
    setup_wifi();
    return;
  }

  // Portal non-blocking wajib diproses secara rutin.
  if (wifiManagerPortalRunning) {
    wifiManager.process();
  }

  // =====================================================
  // WIFI CONNECTED
  // =====================================================
  if (WiFi.status() == WL_CONNECTED) {
    wifiDisconnectedSince = 0;

    if (wifiManagerPortalRunning) {
      stopWifiConfigPortalSafe();
    }

    if (!wifiConnectedLogged) {
      wifiConnectedLogged = true;

      DEBUG_PRINTLN("WiFi connected.");
      DEBUG_PRINT("SSID: ");
      DEBUG_PRINTLN(WiFi.SSID());
      DEBUG_PRINT("IP address: ");
      DEBUG_PRINTLN(WiFi.localIP());
    }

    if (
      !client.connected() &&
      millis() - lastMqttAttemptTime >= MQTT_RETRY_INTERVAL
    ) {
      reconnect();
    }

    if (client.connected()) {
      client.loop();
    }

    return;
  }

  // =====================================================
  // WIFI DISCONNECTED
  // =====================================================
  wifiConnectedLogged = false;

  if (client.connected()) {
    client.disconnect();
  }

  if (wifiDisconnectedSince == 0) {
    wifiDisconnectedSince = millis();
  }

  // Saat portal aktif, jangan memanggil begin/reconnect/mode.
  if (wifiManagerPortalRunning) {
    if (
      millis() - wifiPortalStartedAt >=
      WIFI_PORTAL_MAX_DURATION
    ) {
      DEBUG_PRINTLN("Portal timeout. Closing portal.");
      stopWifiConfigPortalSafe();

      // Mulai ulang periode pencarian WiFi tersimpan.
      wifiDisconnectedSince = millis();

      // Agar percobaan reconnect dapat dilakukan segera.
      lastWifiAttemptTime = millis() - WIFI_RETRY_INTERVAL;
    }

    return;
  }

  // Beri kesempatan reconnect selama satu menit dahulu.
  if (
    millis() - wifiDisconnectedSince >=
    WIFI_PORTAL_OPEN_DELAY
  ) {
    startWifiConfigPortal();
    return;
  }

  // Percobaan koneksi tersimpan dilakukan berkala.
  if (
    millis() - lastWifiAttemptTime >=
    WIFI_RETRY_INTERVAL
  ) {
    tryReconnectSavedWiFi();
  }
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

bool readSensorData(float &moisture, float &temperature, float &ec, float &ph, float &nitrogen, float &phosphorus, float &potassium) {
  uint8_t result = node.readHoldingRegisters(0x00, 7);

  if (result != node.ku8MBSuccess) {
    DEBUG_PRINTLN("FAILED reading sensor registers!");
    return false;
  }

  moisture = node.getResponseBuffer(0) / 10.0;

  int16_t tempRaw = (int16_t)node.getResponseBuffer(1);
  temperature = tempRaw / 10.0;

  ec = node.getResponseBuffer(2) / 1000.0;
  ph = node.getResponseBuffer(3) / 10.0;
  nitrogen = node.getResponseBuffer(4);
  phosphorus = node.getResponseBuffer(5);
  potassium = node.getResponseBuffer(6);

  return true;
}

String buildSensorPayload(float moisture, float temperature, float ec, float ph, float nitrogen, float phosphorus, float potassium, const RtcDateTime &now, const char* source) {
  StaticJsonDocument<1024> doc;

  doc["moisture"] = moisture;
  doc["temperature"] = temperature;
  doc["ec"] = ec;
  doc["ph"] = ph;
  doc["nitrogen"] = nitrogen;
  doc["phosphorus"] = phosphorus;
  doc["potassium"] = potassium;

  doc["relay1"] = digitalRead(RELAY1) == RELAY_ON ? 1 : 0;
  doc["relay2"] = digitalRead(RELAY2) == RELAY_ON ? 1 : 0;
  doc["relay3"] = digitalRead(RELAY3) == RELAY_ON ? 1 : 0;
  doc["relay4"] = digitalRead(RELAY4) == RELAY_ON ? 1 : 0;

  doc["buzzer"] = digitalRead(BUZZER_PIN) == BUZZER_ON ? 1 : 0;
  doc["rtc_valid"] = now.valid ? 1 : 0;
  doc["rtc_available"] = ds3231Available ? 1 : 0;
  doc["schedule_storage"] = at24c32Available ? 1 : 0;
  doc["source"] = source;

  if (now.valid) {
    JsonObject rtc = doc.createNestedObject("rtc");
    rtc["year"] = now.year;
    rtc["month"] = now.month;
    rtc["day"] = now.day;
    rtc["hour"] = now.hour;
    rtc["minute"] = now.minute;
    rtc["second"] = now.second;
    rtc["day_of_week"] = now.dayOfWeek;

    doc["timestamp"] = formatTimestamp(now);
  }

  String output;
  serializeJson(doc, output);
  return output;
}

String buildRelayStatusPayload(const RtcDateTime &now, const char* source) {
  StaticJsonDocument<512> doc;

  doc["relay1"] = digitalRead(RELAY1) == RELAY_ON ? 1 : 0;
  doc["relay2"] = digitalRead(RELAY2) == RELAY_ON ? 1 : 0;
  doc["relay3"] = digitalRead(RELAY3) == RELAY_ON ? 1 : 0;
  doc["relay4"] = digitalRead(RELAY4) == RELAY_ON ? 1 : 0;

  doc["buzzer"] = digitalRead(BUZZER_PIN) == BUZZER_ON ? 1 : 0;
  doc["rtc_valid"] = now.valid ? 1 : 0;
  doc["source"] = source;
  doc["event"] = "relay_status";

  if (now.valid) {
    doc["timestamp"] = formatTimestamp(now);
  }

  String output;
  serializeJson(doc, output);
  return output;
}

void publishRelayStatusNow(const char* source) {
  if (!client.connected()) {
    return;
  }

  RtcDateTime now = readCurrentDateTime();

  String payload = hasValidSensorData
      ? buildSensorPayload(
          lastMoisture,
          lastTemperature,
          lastEc,
          lastPh,
          lastNitrogen,
          lastPhosphorus,
          lastPotassium,
          now,
          source
        )
      : buildRelayStatusPayload(now, source);

  if (client.publish(realtimeTopic, payload.c_str())) {
    DEBUG_PRINTLN("Relay status MQTT Publish Success");
  } else {
    DEBUG_PRINTLN("Relay status MQTT Publish Failed");
  }
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
    DEBUG_PRINTLN("Failed saving schedules to AT24C32.");
    return false;
  }

  DEBUG_PRINTLN("Schedules saved to AT24C32 EEPROM.");
  return true;
}

bool loadSchedulesFromEeprom() {
  byte buffer[SCHEDULE_STORAGE_TOTAL_BYTES];

  if (!readAt24C32Bytes(SCHEDULE_STORAGE_ADDRESS, buffer, SCHEDULE_STORAGE_TOTAL_BYTES)) {
    DEBUG_PRINTLN("Failed reading schedules from AT24C32.");
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
    DEBUG_PRINTLN("No valid saved schedule found in AT24C32.");
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

    if (!isValidSchedule(schedule)) {
      DEBUG_PRINTLN("Saved schedule is invalid. Ignoring AT24C32 data.");
      return false;
    }

    relaySchedules[i] = schedule;
  }

  DEBUG_PRINTLN("Schedules loaded from AT24C32 EEPROM.");
  return true;
}

bool savePayloadToLittleFS(const String &payload) {
  if (!littleFsReady) {
    DEBUG_PRINTLN("LittleFS not ready. Cannot save offline payload.");
    return false;
  }
  File file = LittleFS.open(OFFLINE_LOG_FILE, FILE_APPEND);

  if (!file) {
    DEBUG_PRINTLN("Failed to open offline log file for append.");
    return false;
  }

  file.println(payload);
  file.close();

  offlineSavedCount++;

  DEBUG_PRINT("Offline payload saved to LittleFS. Total saved: ");
  DEBUG_PRINTLN(offlineSavedCount);

  File checkFile = LittleFS.open(OFFLINE_LOG_FILE, FILE_READ);
  if (checkFile) {
    DEBUG_PRINT("Offline log file size: ");
    DEBUG_PRINT(checkFile.size());
    DEBUG_PRINTLN(" bytes");
    checkFile.close();
  }

  return true;
}

String formatTimestamp(const RtcDateTime &now) {
  if (!now.valid) {
    return "";
  }

  String timestamp = "";
  timestamp += String(now.year);
  timestamp += "-";
  timestamp += twoDigits(now.month);
  timestamp += "-";
  timestamp += twoDigits(now.day);
  timestamp += " ";
  timestamp += twoDigits(now.hour);
  timestamp += ":";
  timestamp += twoDigits(now.minute);
  timestamp += ":";
  timestamp += twoDigits(now.second);

  return timestamp;
}

bool shouldLogHistory() {
  if (millis() - lastHistoryLogTime >= HISTORY_LOG_INTERVAL) {
    lastHistoryLogTime = millis();
    return true;
  }

  return false;
}

void setupRtcAndScheduleStorage() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  ds3231Available = isI2CDeviceAvailable(DS3231_ADDRESS);
  at24c32Available = isI2CDeviceAvailable(AT24C32_ADDRESS);

  DEBUG_PRINT("DS3231 RTC: ");
  DEBUG_PRINTLN(ds3231Available ? "detected" : "not detected");

  DEBUG_PRINT("AT24C32 EEPROM: ");
  DEBUG_PRINTLN(at24c32Available ? "detected" : "not detected");

  clearSchedules();

  if (at24c32Available) {
    loadSchedulesFromEeprom();
  }

  RtcDateTime rtcNow = readDs3231DateTime();

  if (rtcNow.valid) {
    setSoftwareClock(rtcNow);
    DEBUG_PRINTLN("System time loaded from DS3231 RTC.");
  } else if (ds3231Available) {
    DEBUG_PRINTLN("DS3231 time is not valid yet. Set time from Android.");
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
    DEBUG_PRINTLN("RTC MQTT payload ignored. Time remains locked to DS3231.");
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
    DEBUG_PRINTLN("Invalid time payload. System time not updated.");
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

  DEBUG_PRINTLN("Relay schedules updated from Android.");
  saveSchedulesToEeprom();
}

void setupLittleFS() {
  if (!LittleFS.begin(false)) {
    DEBUG_PRINTLN("LittleFS mount failed!");
    littleFsReady = false;
    return;
  }

  littleFsReady = true;

  DEBUG_PRINTLN("LittleFS mounted successfully.");

  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();

  DEBUG_PRINT("LittleFS Total: ");
  DEBUG_PRINT(totalBytes);
  DEBUG_PRINTLN(" bytes");

  DEBUG_PRINT("LittleFS Used : ");
  DEBUG_PRINT(usedBytes);
  DEBUG_PRINTLN(" bytes");
}

void inspectLittleFS() {
  Serial.println();
  Serial.println("===== LITTLEFS INSPECTION =====");

  if (!littleFsReady) {
    Serial.println("ERROR: LittleFS belum siap.");
    return;
  }

  Serial.printf("Total LittleFS : %u bytes\n", LittleFS.totalBytes());
  Serial.printf("Used LittleFS  : %u bytes\n", LittleFS.usedBytes());

  File root = LittleFS.open("/");

  if (!root || !root.isDirectory()) {
    Serial.println("Gagal membuka direktori root LittleFS.");
    return;
  }

  File file = root.openNextFile();

  while (file) {
    Serial.printf(
      "File: %s | Size: %u bytes\n",
      file.name(),
      static_cast<unsigned int>(file.size())
    );

    file.close();
    file = root.openNextFile();
  }

  root.close();
  Serial.println("===============================");
}

void recoverOfflineTempFile() {
  if (!littleFsReady) {
    return;
  }

  bool logExists = LittleFS.exists(OFFLINE_LOG_FILE);
  bool tempExists = LittleFS.exists(OFFLINE_TEMP_FILE);

  if (!logExists && tempExists) {
    Serial.println(
      "Recovering offline log from temporary file..."
    );

    if (LittleFS.rename(
          OFFLINE_TEMP_FILE,
          OFFLINE_LOG_FILE
        )) {
      Serial.println("Temporary offline log recovered.");
    } else {
      Serial.println(
        "ERROR: Failed recovering temporary offline log."
      );
    }

    return;
  }

  if (logExists && tempExists) {
    offlineFileConflict = true;
    Serial.println(
      "WARNING: Both offline and temp files exist."
    );
    Serial.println(
      "Both files are preserved to prevent data loss."
    );
  }
}

void syncOfflineDataToMqtt() {
  if (!littleFsReady) {
    return;
  }

  if (offlineFileConflict) {
    DEBUG_PRINTLN(
      "Offline sync blocked because both log files exist."
    );
    return;
  }

  if (!client.connected()) {
    return;
  }

  if (millis() - lastOfflineSyncTime < OFFLINE_SYNC_INTERVAL) {
    return;
  }

  lastOfflineSyncTime = millis();

  if (!LittleFS.exists(OFFLINE_LOG_FILE)) {
    return;
  }

  File sourceFile = LittleFS.open(OFFLINE_LOG_FILE, FILE_READ);

  if (!sourceFile) {
    DEBUG_PRINTLN("Failed to open offline log file for reading.");
    return;
  }

  File tempFile = LittleFS.open(OFFLINE_TEMP_FILE, FILE_WRITE);

  if (!tempFile) {
    DEBUG_PRINTLN("Failed to open temp offline log file.");
    sourceFile.close();
    return;
  }

  int syncedThisLoop = 0;
  bool syncLimitReached = false;

  while (sourceFile.available()) {
    String line = sourceFile.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) {
      continue;
    }

    if (syncedThisLoop < MAX_SYNC_PER_LOOP && !syncLimitReached) {
      DEBUG_PRINTLN("Syncing offline payload:");
      DEBUG_PRINTLN(line);

      bool published = client.publish(historyTopic , line.c_str());

      if (published) {
        syncedThisLoop++;
        offlineSyncedCount++;

        DEBUG_PRINT("Offline payload synced. Total synced: ");
        DEBUG_PRINTLN(offlineSyncedCount);

        client.loop();
        delay(50);
      } else {
        DEBUG_PRINTLN("Failed to publish offline payload. Keeping data.");
        tempFile.println(line);
        syncLimitReached = true;
      }
    } else {
      tempFile.println(line);
    }
  }

  sourceFile.close();
  tempFile.close();

  File checkTemp = LittleFS.open(OFFLINE_TEMP_FILE, FILE_READ);

  bool hasPendingData =
    checkTemp && checkTemp.size() > 0;

  if (checkTemp) {
    checkTemp.close();
  }

  if (!LittleFS.remove(OFFLINE_LOG_FILE)) {
    DEBUG_PRINTLN(
      "ERROR: Failed removing original offline log."
    );
    DEBUG_PRINTLN(
      "Original and temporary files are preserved."
    );

    offlineFileConflict = true;
    return;
  }

  if (hasPendingData) {
    if (!LittleFS.rename(
          OFFLINE_TEMP_FILE,
          OFFLINE_LOG_FILE
        )) {
      DEBUG_PRINTLN(
        "ERROR: Failed renaming temporary log."
      );

      DEBUG_PRINTLN(
        "Pending data remains in temporary file."
      );

      offlineFileConflict = true;
      return;
    }

    DEBUG_PRINTLN("Some offline data still pending.");
  } else {
    if (LittleFS.exists(OFFLINE_TEMP_FILE)) {
      LittleFS.remove(OFFLINE_TEMP_FILE);
    }

    DEBUG_PRINTLN("All offline data published.");
  }
}

void updateFloatIfPresent(JsonDocument &doc, const char* key, float &target) {
  if (doc.containsKey(key)) {
    target = doc[key].as<float>();
  }
}

void updateBoolIfPresent(JsonObject obj, const char* key, bool &target) {
  if (obj.containsKey(key)) {
    target = obj[key].as<bool>();
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);

  DEBUG_PRINT("Message arrived [");
  DEBUG_PRINT(topicStr);
  DEBUG_PRINTLN("]");

  String message;

  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  DEBUG_PRINTLN(message);

  DynamicJsonDocument doc(2048);

  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    DEBUG_PRINT("JSON Parse Failed: ");
    DEBUG_PRINTLN(error.c_str());
    return;
  }

  DEBUG_PRINTLN("=== JSON RECEIVED ===");

  #if DEBUG_MODE
    serializeJsonPretty(doc, Serial);
    DEBUG_PRINTLN_EMPTY();
  #endif

  // =====================================================
  // MQTT TOPIC : nutrixense/control
  // =====================================================
  if (topicStr == "nutrixense/control") {
    DEBUG_PRINTLN("=== RELAY CONTROL ===");
    bool relayCommandReceived = false;
    const char* commandSource = doc["source"] | "";
    bool isManualCommand =
      strcmp(commandSource, "manual") == 0 || doc.containsKey("manual_override");

    for (byte relay = 1; relay <= 4; relay++) {
      char relayKey[8];
      snprintf(relayKey, sizeof(relayKey), "relay%d", relay);

      if (!doc.containsKey(relayKey)) {
        continue;
      }

      relayCommandReceived = true;
      int state = doc[relayKey].as<int>();
      bool isOn = state == 1;

      if (isManualCommand) {
        setManualRelayOverride(relay, isOn);
      } else {
        manualOverrideActive[relay - 1] = false;
        setRelayState(relay, isOn);

        DEBUG_PRINT("Relay");
        DEBUG_PRINT(relay);
        DEBUG_PRINT(": ");
        DEBUG_PRINTLN(isOn ? "ON" : "OFF");
      }
    }

    if (relayCommandReceived) {
      publishRelayStatusNow("relay_status");
    }
  }

  // =====================================================
  // MQTT TOPIC : nutrixense/schedule
  // RTC is written only when DS3231 time is invalid, or rtc.force_update=true.
  // Schedules are saved to AT24C32.
  // =====================================================
  else if (topicStr == "nutrixense/schedule") {
    DEBUG_PRINTLN("=== SCHEDULE CONFIG ===");

    updateRtcFromJson(doc["rtc"].as<JsonObject>());

    if (doc.containsKey("schedules")) {
      updateSchedulesFromJson(doc["schedules"].as<JsonArray>());
    }
  }

  // =====================================================
  // MQTT TOPIC : nutrixense/config
  // =====================================================
  else if (topicStr == "nutrixense/config") {
    DEBUG_PRINTLN("=== THRESHOLD CONFIG ===");

    updateFloatIfPresent(doc, "min_nitrogen", MIN_NITROGEN);
    updateFloatIfPresent(doc, "min_phosphorus", MIN_PHOSPHORUS);
    updateFloatIfPresent(doc, "min_potassium", MIN_POTASSIUM);
    updateFloatIfPresent(doc, "min_ph", MIN_PH);
    updateFloatIfPresent(doc, "min_moisture", MIN_MOISTURE);
    updateFloatIfPresent(doc, "min_temperature", MIN_TEMPERATURE);
    updateFloatIfPresent(doc, "min_ec", MIN_EC);

    updateFloatIfPresent(doc, "max_nitrogen", MAX_NITROGEN);
    updateFloatIfPresent(doc, "max_phosphorus", MAX_PHOSPHORUS);
    updateFloatIfPresent(doc, "max_potassium", MAX_POTASSIUM);
    updateFloatIfPresent(doc, "max_ph", MAX_PH);
    updateFloatIfPresent(doc, "max_moisture", MAX_MOISTURE);
    updateFloatIfPresent(doc, "max_temperature", MAX_TEMPERATURE);
    updateFloatIfPresent(doc, "max_ec", MAX_EC);

    DEBUG_PRINTLN("=== THRESHOLD UPDATED ===");
    printThresholds();

    if (doc.containsKey("buzzer_muted")) {
      JsonObject muted = doc["buzzer_muted"];

      updateBoolIfPresent(muted, "nitrogen", MUTE_NITROGEN);
      updateBoolIfPresent(muted, "phosphorus", MUTE_PHOSPHORUS);
      updateBoolIfPresent(muted, "potassium", MUTE_POTASSIUM);
      updateBoolIfPresent(muted, "ph", MUTE_PH);
      updateBoolIfPresent(muted, "moisture", MUTE_MOISTURE);
      updateBoolIfPresent(muted, "temperature", MUTE_TEMPERATURE);
      updateBoolIfPresent(muted, "ec", MUTE_EC);
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
      DEBUG_PRINTLN("All minimum thresholds are 0. Buzzer forced OFF.");
    }
  }

  // =====================================================
  // UNKNOWN TOPIC
  // =====================================================
  else {
    DEBUG_PRINT("Unknown MQTT Topic: ");
    DEBUG_PRINTLN(topicStr);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  setupLittleFS();
  recoverOfflineTempFile();
  inspectLittleFS();

  setupRtcAndScheduleStorage();
  setupRelays();

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, BUZZER_OFF);
  lastBuzzerAlertTime = millis() - BUZZER_ALERT_INTERVAL;

  setupLcd();
  showStartupScreen();
  lcdScreenStartedAt = millis();

  pinMode(MAX485_DE_RE, OUTPUT);
  digitalWrite(MAX485_DE_RE, LOW);

  Serial2.begin(4800, SERIAL_8N1, RXD2, TXD2);
  node.begin(1, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  setup_wifi();

  espClient.setInsecure();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setBufferSize(2048);
  client.setSocketTimeout(2);

  DEBUG_PRINTLN("System Ready...");
}

void loop() {
  maintainNetwork();

  if (client.connected()) {
    syncOfflineDataToMqtt();
  }

  RtcDateTime now = readCurrentDateTime();
  applyRelaySchedules(now);
  maintainLcdDisplay();
  maintainBuzzer(currentNutrientAbnormal);

  if (millis() - lastReadTime >= READ_INTERVAL) {
    DEBUG_PRINTLN("\n===== READING SENSOR =====");

    float moisture = 0;
    float temperature = 0;
    float ec = 0;
    float ph = 0;
    float nitrogen = 0;
    float phosphorus = 0;
    float potassium = 0;

    bool sensorReadSuccess = readSensorData(
      moisture,
      temperature,
      ec,
      ph,
      nitrogen,
      phosphorus,
      potassium
    );

    DEBUG_PRINTLN("===== HASIL SENSOR =====");

    DEBUG_PRINT("Soil Moisture : ");
    DEBUG_PRINT(moisture);
    DEBUG_PRINTLN(" %");

    DEBUG_PRINT("Temperature   : ");
    DEBUG_PRINT(temperature);
    DEBUG_PRINTLN(" C");

    DEBUG_PRINT("EC            : ");
    DEBUG_PRINT(ec);
    DEBUG_PRINTLN(" mS/cm");

    DEBUG_PRINT("pH            : ");
    DEBUG_PRINTLN(ph);

    DEBUG_PRINT("Nitrogen      : ");
    DEBUG_PRINT(nitrogen);
    DEBUG_PRINTLN(" mg/kg");

    DEBUG_PRINT("Phosphorus    : ");
    DEBUG_PRINT(phosphorus);
    DEBUG_PRINTLN(" mg/kg");

    DEBUG_PRINT("Potassium     : ");
    DEBUG_PRINT(potassium);
    DEBUG_PRINTLN(" mg/kg");

    DEBUG_PRINTLN("==========================");

    if (!sensorReadSuccess) {
      DEBUG_PRINTLN("Sensor read failed!");
      DEBUG_PRINTLN("Skipping threshold check...");

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

    DEBUG_PRINT("nutrientAbnormal = ");
    DEBUG_PRINTLN(nutrientAbnormal ? "TRUE" : "FALSE");
    currentNutrientAbnormal = nutrientAbnormal;

    if (nutrientAbnormal) {
      DEBUG_PRINTLN("WARNING: Nutrisi di bawah ambang normal!");
      DEBUG_PRINTLN("Buzzer beep pattern armed");
    } else {
      DEBUG_PRINTLN("Nutrisi Normal");
      DEBUG_PRINTLN("Buzzer OFF");
    }

    String payload = buildSensorPayload(
      moisture, temperature, ec, ph,
      nitrogen, phosphorus, potassium,
      now,
      "realtime"
    );

    // ================= REALTIME MQTT SETIAP 2 DETIK =================
    if (client.connected()) {
      DEBUG_PRINTLN("Sending realtime MQTT:");
      DEBUG_PRINTLN(payload);

      if (client.publish(realtimeTopic, payload.c_str())) {
        DEBUG_PRINTLN("Realtime MQTT Publish Success");
      } else {
        DEBUG_PRINTLN("Realtime MQTT Publish Failed");
      }
    } else {
      DEBUG_PRINTLN("MQTT offline. Realtime data only shown on LCD.");
    }

    // ================= HISTORICAL LOG SETIAP 1 MENIT =================
    if (shouldLogHistory()) {
      String historyPayload = payload;

      if (client.connected()) {
        historyPayload.replace("\"source\":\"realtime\"", "\"source\":\"history\"");

        DEBUG_PRINTLN("Sending history MQTT:");
        DEBUG_PRINTLN(historyPayload);

        if (client.publish(historyTopic, historyPayload.c_str())) {
          DEBUG_PRINTLN("History MQTT Publish Success");
        } else {
          DEBUG_PRINTLN("History MQTT Publish Failed. Saving to LittleFS.");

          historyPayload.replace("\"source\":\"history\"", "\"source\":\"offline_cache\"");
          savePayloadToLittleFS(historyPayload);
        }

      } else {
        historyPayload.replace("\"source\":\"realtime\"", "\"source\":\"offline_cache\"");

        DEBUG_PRINTLN("MQTT offline. Saving history data to LittleFS:");
        DEBUG_PRINTLN(historyPayload);

        savePayloadToLittleFS(historyPayload);
      }
    }

    lastReadTime = millis();
  }
}