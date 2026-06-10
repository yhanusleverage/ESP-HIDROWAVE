#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include "Config.h"

struct MqttTelemetryReading {
    float temperature;   // água (hydro_measurements)
    float ph;
    float tds;
    bool waterLevelOk;
    float airTemperature;  // ambiente (environment_data)
    float humidity;
};

struct MqttHeartbeatReading {
    int wifiRssi;
    uint32_t freeHeap;
    unsigned long uptimeSeconds;
    int rebootCount;
    String firmwareVersion;
    String ipAddress;
};

struct MqttEcOperationReading {
    const char* state;
    int operationRemainingSec;
    int nextCheckInSec;
};

struct MqttDoseReading {
    const char* sequenceId;
    const char* nutrientName;
    int relayNumber;
    float dosageMl;
    float dosageTimeSeconds;
    float ecBefore;
    float ecSetpoint;
    const char* source;
};

typedef void (*MqttCommandPayloadHandler)(const char* payload, size_t length, void* userData);

#if ENABLE_MQTT

#include <WiFi.h>
#include <PubSubClient.h>

class MqttClientWrapper {
public:
    MqttClientWrapper();

    bool begin(const String& deviceId);
    void loop();

    bool isConnected() const { return mqtt.connected(); }
    bool publishTelemetry(const MqttTelemetryReading& reading);
    bool publishHeartbeat(const MqttHeartbeatReading& reading);
    bool publishEcOperation(const MqttEcOperationReading& reading);
    bool publishDose(const MqttDoseReading& reading);

    void setCommandHandler(MqttCommandPayloadHandler handler, void* userData);

private:
    WiFiClient wifiClient;
    mutable PubSubClient mqtt;
    String deviceId;
    String telemetryTopic;
    String heartbeatTopic;
    String statusTopic;
    String commandTopic;
    String ecOperationTopic;
    String doseTopic;
    char lwtPayload[128];
    unsigned long lastReconnectAttempt;
    unsigned long reconnectIntervalMs;
    MqttCommandPayloadHandler commandHandler;
    void* commandHandlerUserData;

    static void mqttMessageCallback(char* topic, byte* payload, unsigned int length);
    static MqttClientWrapper* callbackInstance;

    bool ensureConnected();
    bool subscribeCommandTopic();
    bool publishOnlineStatus();
};

#else

class MqttClientWrapper {
public:
    bool begin(const String&) { return false; }
    void loop() {}
    bool isConnected() const { return false; }
    bool publishTelemetry(const MqttTelemetryReading&) { return false; }
    bool publishHeartbeat(const MqttHeartbeatReading&) { return false; }
    bool publishEcOperation(const MqttEcOperationReading&) { return false; }
    bool publishDose(const MqttDoseReading&) { return false; }
    void setCommandHandler(MqttCommandPayloadHandler, void*) {}
};

#endif

#endif
