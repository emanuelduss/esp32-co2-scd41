#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <SensirionI2cScd4x.h>
#include <TaskScheduler.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#include "settings-livingroom.h"

// Use proper definition of NO_ERROR
#ifdef NO_ERROR
#undef NO_ERROR
#endif
#define NO_ERROR 0

#define LED_PIN 2
#define CHECK_ERROR(result, msg)                                  \
    if (result != NO_ERROR) {                                     \
        errorToString(result, errorMessage, sizeof errorMessage); \
        Serial.print(msg);                                        \
        Serial.println(errorMessage);                             \
        return;                                                   \
    }

SensirionI2cScd4x sensor;
float temperature = 0.0;
float relativeHumidity = 0.0;
uint16_t co2Concentration = 0;

static char errorMessage[64];
static int16_t error;

void readSensorCallback();
void publishMqttValues();
Task readSensorTask(5000, TASK_FOREVER, &readSensorCallback);  // Read sensor every 5 seconds
Task sendMqttTask(MQTT_PUBLISH_INTERVAL * 1000, TASK_FOREVER,
                  &publishMqttValues);  // Publish to MQTT every 60 seconds
Scheduler scheduler;

WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// Settings from secrets.h
const char* deviceDescription = DEVICE_DESCRIPTION;
const char* deviceName = DEVICE_NAME;
const char* deviceLocation = DEVICE_LOCATION;
const char* wifiSSID = WIFI_SSID;
const char* wifiPassword = WIFI_PASSWORD;
const char* mqttServer = MQTT_SERVER;
const int mqttPort = MQTT_PORT;
const char* mqttUsername = MQTT_USERNAME;
const char* mqttPassword = MQTT_PASSWORD;
const char* mqttTopic = MQTT_TOPIC;
const char* mqttAutoDiscoverTopicPrefix = MQTT_AUTO_DISCOVER_TOPIC_PREFIX;

void PrintUint64(uint64_t& value) {
    Serial.print("0x");
    Serial.print((uint32_t)(value >> 32), HEX);
    Serial.print((uint32_t)(value & 0xFFFFFFFF), HEX);
}

void handleMetrics() {
    Serial.print("Client connected. Client IP: ");
    Serial.println(server.client().remoteIP());

    String response = "";
    response += "# HELP ambient_temperature_celsius Ambient temperature\n";
    response += "# TYPE ambient_temperature_celsius gauge\n";
    response += "ambient_temperature_celsius{location=\"" + String(deviceLocation) +
                "\" device=\"" + String(deviceName) + "\"} " + String(temperature, 2) + "\n";
    response += "# HELP ambient_humidity_percent Relative humidity\n";
    response += "# TYPE ambient_humidity_percent gauge\n";
    response += "ambient_humidity_percent{location=\"" + String(deviceLocation) + "\" device=\"" +
                String(deviceName) + "\"} " + String(relativeHumidity, 2) + "\n";
    response += "# HELP co2_ppm Indoor CO2 concentration in ppm\n";
    response += "# TYPE co2_ppm gauge\n";
    response += "co2_ppm{location=\"" + String(deviceLocation) + "\" device=\"" +
                String(deviceName) + "\"} " + String(co2Concentration) + "\n";

    server.send(200, "text/plain", response);
}

void handleNotFound() { server.send(404, "text/plain", "Not Found"); }

void setupMqtt() {
    mqttClient.setServer(mqttServer, mqttPort);
    mqttClient.setBufferSize(4096);  // Default is 256, increase for large auto-discovery payload
}

String buildDiscoveryPayload() {
    JsonDocument doc;

    // Device and Origin
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = deviceName;
    dev["name"] = deviceDescription;
    dev["mf"] = "Self-Made";
    dev["mdl"] = "ESP32 SCD41 Sensor";
    dev["sw"] = "1.0";
    dev["sn"] = deviceName + String("_") + String(WiFi.macAddress());
    dev["hw"] = "rev1";

    JsonObject o = doc["o"].to<JsonObject>();
    o["name"] = deviceName;
    o["sw"] = "1.0";
    o["url"] = "https://emanuelduss.ch/posts/co2-measurement/";

    // Components
    JsonObject cmps = doc["cmps"].to<JsonObject>();

    JsonObject co2 = cmps["co2"].to<JsonObject>();
    co2["p"] = "sensor";
    co2["device_class"] = "carbon_dioxide";
    co2["unit_of_measurement"] = "ppm";
    co2["value_template"] = "{{ value_json.co2 }}";
    co2["unique_id"] = deviceName + String("_co2");

    JsonObject temp = cmps["temperature"].to<JsonObject>();
    temp["p"] = "sensor";
    temp["device_class"] = "temperature";
    temp["unit_of_measurement"] = "°C";
    temp["value_template"] = "{{ value_json.temperature }}";
    temp["unique_id"] = deviceName + String("_temperature");

    JsonObject hum = cmps["humidity"].to<JsonObject>();
    hum["p"] = "sensor";
    hum["device_class"] = "humidity";
    hum["unit_of_measurement"] = "%";
    hum["value_template"] = "{{ value_json.humidity }}";
    hum["unique_id"] = deviceName + String("_humidity");

    // MQTT
    doc["state_topic"] = mqttTopic;
    doc["qos"] = 1;

    // Produce JSON
    String output;
    serializeJson(doc, output);
    return output;
}

void sendMqttAutoDiscovery() {
    String discoveryPayload = buildDiscoveryPayload();
    Serial.println("Publishing MQTT auto-discovery message: ");
    Serial.println(discoveryPayload.c_str());
    bool status = mqttClient.publish(
        (String(mqttAutoDiscoverTopicPrefix) + "/device/" + String(deviceName) + "/config").c_str(),
        discoveryPayload.c_str(), true);
    if (!status) {
        Serial.println("Failed to publish MQTT auto-discovery message");
        return;
    }
}

bool reconnectMqtt() {
    if (mqttClient.connected()) {
        return true;
    }

    Serial.print("Connecting to MQTT broker...");
    String clientId = "esp32-" + String(WiFi.macAddress());
    if (mqttClient.connect(clientId.c_str(), mqttUsername, mqttPassword)) {
        Serial.println(" connected");
        return true;
    }

    Serial.print(" failed, rc=");
    Serial.println(mqttClient.state());
    return false;
}

void publishMqttValues() {
    if (!mqttClient.connected()) {
        if (!reconnectMqtt()) {
            return;
        }
    }

    char jsonPayload[256];
    snprintf(jsonPayload, sizeof(jsonPayload),
             "{\"temperature\":%.2f,\"humidity\":%.2f,\"co2\":%u}", temperature, relativeHumidity,
             co2Concentration);

    bool status = mqttClient.publish(mqttTopic, jsonPayload, true);
    if (!status) {
        Serial.println("Failed to publish MQTT values");
        return;
    }
    Serial.print("Published to MQTT topic " + String(mqttTopic) + " on " + String(mqttServer) +
                 ": ");
    Serial.println(jsonPayload);
}

void readSensorCallback() {
    bool dataReady = false;
    error = sensor.getDataReadyStatus(dataReady);
    CHECK_ERROR(error, "getDataReadyStatus error: ")

    uint8_t attempts = 0;
    while (!dataReady && attempts < 50) {
        delay(10);
        error = sensor.getDataReadyStatus(dataReady);
        CHECK_ERROR(error, "getDataReadyStatus error: ")
        attempts++;
    }

    error = sensor.readMeasurement(co2Concentration, temperature, relativeHumidity);
    CHECK_ERROR(error, "readMeasurement error: ")

    Serial.print((String) "Co2: " + co2Concentration);
    Serial.print((String) " | Temperature: " + temperature);
    Serial.println((String) " | Humidity: " + relativeHumidity);
}

void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    // ESP32 setup
    Serial.begin(115200);
    while (!Serial) {
        delay(100);
    }
    Wire.begin();

    Serial.println("Device starting up...");
    Serial.print("Device description: ");
    Serial.println(deviceDescription);
    Serial.print("Device name: ");
    Serial.println(deviceName);
    Serial.println();

    // Sensor Setup
    sensor.begin(Wire, SCD41_I2C_ADDR_62);
    delay(30);
    error = sensor.wakeUp();
    if (error != NO_ERROR) errorToString(error, errorMessage, sizeof errorMessage);
    error = sensor.stopPeriodicMeasurement();
    CHECK_ERROR(error, "")
    error = sensor.reinit();
    CHECK_ERROR(error, "")

    uint64_t serialNumber = 0;
    error = sensor.getSerialNumber(serialNumber);
    if (error != NO_ERROR) {
        errorToString(error, errorMessage, sizeof errorMessage);
        Serial.println(errorMessage);
        return;
    }
    Serial.print("Serial number: ");
    PrintUint64(serialNumber);
    Serial.println();

    error = sensor.startPeriodicMeasurement();
    CHECK_ERROR(error, "")

    // Wi-Fi setup
    IPAddress ip(IPADDRESS[0], IPADDRESS[1], IPADDRESS[2], IPADDRESS[3]);
    Serial.print("Connecting to Wi-Fi ");
    Serial.print(wifiSSID);
    WiFi.begin(wifiSSID, wifiPassword);

    uint8_t WiFiAttempts = 0;
    while (WiFi.status() != WL_CONNECTED && WiFiAttempts < 20) {
        delay(500);
        Serial.write('.');
        WiFiAttempts++;
    }
    Serial.print(" connected. ");
    WiFi.config(ip, IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // MQTT setup
    setupMqtt();
    reconnectMqtt();
    sendMqttAutoDiscovery();

    // Webserver setup
    Serial.print("Starting webserver...");
    server.on("/", handleMetrics);
    server.on("/metrics", handleMetrics);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println(" started.");

    delay(2000);  // Wait for first measurement to be ready

    // Scheduler
    scheduler.init();
    scheduler.addTask(readSensorTask);
    scheduler.addTask(sendMqttTask);
    readSensorTask.enable();
    sendMqttTask.enable();
    Serial.println("Setup complete. Sensor is ready.");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
}

void loop() {
    scheduler.execute();

    if (!mqttClient.connected()) {
        reconnectMqtt();
    }
    mqttClient.loop();

    server.handleClient();
}