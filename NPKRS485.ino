#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>

// ================= WIFI =================
const char* ssid = "Hasyim_M";
const char* password = "Koseka123";

// ================= HIVEMQ =================
const char* mqtt_server = "a8805b4f45744c3f9ac83882e423e0c0.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "hasyim";
const char* mqtt_pass = "hasyimHiveMQTT#22";
const char* topic = "nutrixense/sensor";

// ================= MQTT =================
WiFiClientSecure espClient;
PubSubClient client(espClient);

// Inisialisasi Modbus
ModbusMaster node;

// Control Pin MAX485
#define MAX485_DE_RE 4

// Use Serial2 UART ESP32
#define RXD2 16
#define TXD2 17

// ================= RELAY =================
#define RELAY1 25
#define RELAY2 26
#define RELAY3 27
#define RELAY4 14

// ================= TIMING =================
unsigned long lastReadTime = 0;
const unsigned long READ_INTERVAL = 2000;
const unsigned long SENSOR_DELAY = 100;

void preTransmission() {
  digitalWrite(MAX485_DE_RE, HIGH); // Send Mode
}

void postTransmission() {
  digitalWrite(MAX485_DE_RE, LOW); // Receive Mode
}

// ================= WIFI =================
void setup_wifi() {
  Serial.print("Connecting WiFi...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected!");
}

// ================= MQTT CONNECT =================
void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting MQTT...");

    if (client.connect("ESP32_Client", mqtt_user, mqtt_pass)) {
      Serial.println("Connected!");
    } else {
      Serial.print("Failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying...");
      delay(2000);
    }
  }
}

// ================= READ REGISTER =================
uint16_t readRegister(uint16_t reg) {

  uint8_t result = node.readHoldingRegisters(reg, 1);

  if (result == node.ku8MBSuccess) {
    return node.getResponseBuffer(0);
  } else {
    Serial.print("Failed reading register: 0x");
    Serial.println(reg, HEX);
    return 0;
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
  digitalWrite(RELAY1, LOW);
  digitalWrite(RELAY2, LOW);
  digitalWrite(RELAY3, LOW);
  digitalWrite(RELAY4, LOW);

  // Setup pin MAX485
  pinMode(MAX485_DE_RE, OUTPUT);
  digitalWrite(MAX485_DE_RE, LOW);

  // Serial RS485 Communication
  Serial2.begin(4800, SERIAL_8N1, RXD2, TXD2);

  // Slave ID sensor (default = 1)
  node.begin(1, Serial2);


  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  setup_wifi();

  // ⚠️ SSL (for HiveMQ Cloud)
  espClient.setInsecure(); 

  client.setServer(mqtt_server, mqtt_port);

  Serial.println("System Ready...");
}

void loop() {

  if (!client.connected()) {
    reconnect();
  }

  client.loop();

  // Interval Read
  if (millis() - lastReadTime >= READ_INTERVAL) {

    Serial.println("\n===== READING SENSOR =====");

    // ================= READ SENSOR =================

    // 0x00 = Moisture
    float moisture = readRegister(0x00) / 10.0;
    delay(SENSOR_DELAY);

    // 0x01 = Temperature
    float temperature = readRegister(0x01) / 10.0;
    delay(SENSOR_DELAY);

    // 0x02 = EC
    uint16_t ec = readRegister(0x02);
    delay(SENSOR_DELAY);

    // 0x03 = pH
    float ph = readRegister(0x03) / 10.0;
    delay(SENSOR_DELAY);

    // 0x04 = Nitrogen
    uint16_t nitrogen = readRegister(0x04);
    delay(SENSOR_DELAY);

    // 0x05 = Phosphorus
    uint16_t phosphorus = readRegister(0x05);
    delay(SENSOR_DELAY);

    // 0x06 = Potassium
    uint16_t potassium = readRegister(0x06);
    delay(SENSOR_DELAY);

    // ================= SERIAL OUTPUT =================

    Serial.println("===== HASIL SENSOR =====");

    Serial.print("Soil Moisture : ");
    Serial.print(moisture);
    Serial.println(" %");

    Serial.print("Temperature   : ");
    Serial.print(temperature);
    Serial.println(" C");

    Serial.print("EC            : ");
    Serial.print(ec);
    Serial.println(" us/cm");

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

    // ================= JSON MQTT =================

    String payload = "{";

    payload += "\"moisture\":" + String(moisture, 1) + ",";
    payload += "\"temperature\":" + String(temperature, 1) + ",";
    payload += "\"ec\":" + String(ec) + ",";
    payload += "\"ph\":" + String(ph, 1) + ",";
    payload += "\"nitrogen\":" + String(nitrogen) + ",";
    payload += "\"phosphorus\":" + String(phosphorus) + ",";
    payload += "\"potassium\":" + String(potassium);
    payload += ",\"relay1\":" + String(digitalRead(RELAY1) == LOW ? 1 : 0);
    payload += ",\"relay2\":" + String(digitalRead(RELAY2) == LOW ? 1 : 0);
    payload += ",\"relay3\":" + String(digitalRead(RELAY3) == LOW ? 1 : 0);
    payload += ",\"relay4\":" + String(digitalRead(RELAY4) == LOW ? 1 : 0);

    payload += "}";

    Serial.println("Sending MQTT:");
    Serial.println(payload);

    client.publish(topic, payload.c_str());

    lastReadTime = millis();
  }
}