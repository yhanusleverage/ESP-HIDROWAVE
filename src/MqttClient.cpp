#include "MqttClient.h"

#if ENABLE_MQTT

#include <ArduinoJson.h>

#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASS
#define MQTT_PASS ""
#endif

MqttClientWrapper* MqttClientWrapper::callbackInstance = nullptr;

MqttClientWrapper::MqttClientWrapper()
    : mqtt(wifiClient),
      lastReconnectAttempt(0),
      reconnectIntervalMs(5000),
      commandHandler(nullptr),
      commandHandlerUserData(nullptr) {
    mqtt.setBufferSize(512);
    lwtPayload[0] = '\0';
}

void MqttClientWrapper::setCommandHandler(MqttCommandPayloadHandler handler, void* userData) {
    commandHandler = handler;
    commandHandlerUserData = userData;
}

void MqttClientWrapper::mqttMessageCallback(char* topic, byte* payload, unsigned int length) {
    if (!callbackInstance || !callbackInstance->commandHandler || length == 0) {
        return;
    }
    Serial.printf("[MQTT] rx command topic=%s len=%u\n", topic, length);
    callbackInstance->commandHandler(reinterpret_cast<const char*>(payload), length,
                                     callbackInstance->commandHandlerUserData);
}

bool MqttClientWrapper::subscribeCommandTopic() {
    if (!mqtt.connected() || commandTopic.length() == 0) {
        return false;
    }
    bool ok = mqtt.subscribe(commandTopic.c_str(), 1);
    if (ok) {
        Serial.printf("[MQTT] subscribe command QoS1 %s\n", commandTopic.c_str());
    } else {
        Serial.println("[MQTT] subscribe command failed");
    }
    return ok;
}

bool MqttClientWrapper::begin(const String& id) {
    deviceId = id;
    telemetryTopic = String("hidrowave/") + deviceId + "/telemetry";
    heartbeatTopic = String("hidrowave/") + deviceId + "/heartbeat";
    statusTopic = String("hidrowave/") + deviceId + "/status";
    commandTopic = String("hidrowave/") + deviceId + "/command";
    ecOperationTopic = String("hidrowave/") + deviceId + "/ec_operation";
    doseTopic = String("hidrowave/") + deviceId + "/dose";
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    callbackInstance = this;
    mqtt.setCallback(mqttMessageCallback);

    if (strlen(MQTT_HOST) == 0) {
        Serial.println("[MQTT] MQTT_HOST vazio — desabilitado");
        return false;
    }

    StaticJsonDocument<128> lwtDoc;
    lwtDoc["v"] = 1;
    lwtDoc["device_id"] = deviceId;
    lwtDoc["online"] = false;
    serializeJson(lwtDoc, lwtPayload, sizeof(lwtPayload));

    Serial.printf("[MQTT] Broker %s:%d\n", MQTT_HOST, MQTT_PORT);
    Serial.printf("[MQTT] topics telemetry=%s heartbeat=%s status=%s command=%s ec_op=%s dose=%s\n",
                  telemetryTopic.c_str(), heartbeatTopic.c_str(), statusTopic.c_str(),
                  commandTopic.c_str(), ecOperationTopic.c_str(), doseTopic.c_str());
    return ensureConnected();
}

void MqttClientWrapper::loop() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    if (!mqtt.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt >= reconnectIntervalMs) {
            lastReconnectAttempt = now;
            ensureConnected();
            if (reconnectIntervalMs < 30000) {
                reconnectIntervalMs += 5000;
            }
        }
    } else {
        reconnectIntervalMs = 5000;
    }
    mqtt.loop();
}

bool MqttClientWrapper::publishOnlineStatus() {
    StaticJsonDocument<128> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["online"] = true;
    doc["fw"] = FIRMWARE_VERSION;

    char payload[128];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0) {
        return false;
    }

    return mqtt.publish(statusTopic.c_str(), payload, true);
}

bool MqttClientWrapper::ensureConnected() {
    if (mqtt.connected()) {
        return true;
    }
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    String clientId = deviceId.length() ? deviceId : "ESP32_HIDRO";
    Serial.printf("[MQTT] Connecting clientId=%s...\n", clientId.c_str());

    bool ok = mqtt.connect(
        clientId.c_str(),
        MQTT_USER,
        MQTT_PASS,
        statusTopic.c_str(),
        1,
        true,
        lwtPayload);
    if (ok) {
        Serial.println("[MQTT] Connected (LWT on status topic)");
        publishOnlineStatus();
        subscribeCommandTopic();
    } else {
        Serial.printf("[MQTT] Failed rc=%d\n", mqtt.state());
    }
    return ok;
}

bool MqttClientWrapper::publishTelemetry(const MqttTelemetryReading& reading) {
    if (!ensureConnected()) {
        return false;
    }

    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["temperature"] = reading.temperature;
    doc["ph"] = reading.ph;
    doc["tds"] = reading.tds;
    doc["water_level_ok"] = reading.waterLevelOk;
    doc["air_temp"] = reading.airTemperature;
    doc["humidity"] = reading.humidity;

    char payload[256];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0) {
        return false;
    }

    bool published = mqtt.publish(telemetryTopic.c_str(), payload, false);
    if (!published) {
        Serial.println("[MQTT] telemetry publish failed");
    }
    return published;
}

bool MqttClientWrapper::publishHeartbeat(const MqttHeartbeatReading& reading) {
    if (!ensureConnected()) {
        return false;
    }

    StaticJsonDocument<384> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["wifi_rssi"] = reading.wifiRssi;
    doc["free_heap"] = reading.freeHeap;
    doc["uptime_seconds"] = reading.uptimeSeconds;
    doc["reboot_count"] = reading.rebootCount;
    doc["firmware_version"] = reading.firmwareVersion;
    doc["ip_address"] = reading.ipAddress;

    char payload[384];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0) {
        return false;
    }

    bool published = mqtt.publish(heartbeatTopic.c_str(), payload, false);
    if (published) {
        Serial.printf("[MQTT] heartbeat heap=%u rssi=%d uptime=%lus reboot=%d\n",
                      reading.freeHeap, reading.wifiRssi,
                      reading.uptimeSeconds, reading.rebootCount);
    } else {
        Serial.println("[MQTT] heartbeat publish failed");
    }
    return published;
}

bool MqttClientWrapper::publishEcOperation(const MqttEcOperationReading& reading) {
    if (!ensureConnected()) {
        return false;
    }

    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["ec_operation_state"] = reading.state ? reading.state : "idle";
    doc["ec_operation_remaining_sec"] = reading.operationRemainingSec > 0 ? reading.operationRemainingSec : 0;
    doc["ec_next_check_in_sec"] = reading.nextCheckInSec > 0 ? reading.nextCheckInSec : 0;

    char payload[256];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0) {
        return false;
    }

    bool published = mqtt.publish(ecOperationTopic.c_str(), payload, false);
    if (published) {
        Serial.printf("[MQTT] ec_operation %s rem=%ds next=%ds\n",
                      reading.state ? reading.state : "idle",
                      reading.operationRemainingSec,
                      reading.nextCheckInSec);
    } else {
        Serial.println("[MQTT] ec_operation publish failed");
    }
    return published;
}

bool MqttClientWrapper::publishDose(const MqttDoseReading& reading) {
    if (!ensureConnected()) {
        return false;
    }

    StaticJsonDocument<512> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["sequence_id"] = reading.sequenceId ? reading.sequenceId : "";
    doc["nutrient_name"] = reading.nutrientName ? reading.nutrientName : "";
    doc["relay_number"] = reading.relayNumber;
    doc["dosage_ml"] = round(reading.dosageMl * 1000.0) / 1000.0;
    doc["dosage_time_seconds"] = round(reading.dosageTimeSeconds * 100.0) / 100.0;
    doc["ec_before"] = round(reading.ecBefore * 100.0) / 100.0;
    doc["ec_setpoint"] = round(reading.ecSetpoint * 100.0) / 100.0;
    doc["source"] = reading.source && reading.source[0] ? reading.source : "auto_ec";

    char payload[512];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0) {
        return false;
    }

    bool published = mqtt.publish(doseTopic.c_str(), payload, true);
    if (published) {
        Serial.printf("[MQTT] dose %s %.2f ml relé %d\n",
                      reading.nutrientName ? reading.nutrientName : "?",
                      reading.dosageMl,
                      reading.relayNumber + 1);
    } else {
        Serial.println("[MQTT] dose publish failed");
    }
    return published;
}

#endif
