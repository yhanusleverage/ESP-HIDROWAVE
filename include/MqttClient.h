#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include "Config.h"

struct MqttTelemetryReading {
    float temperature;   // água (hydro_measurements)
    float ph;
    float tds;
    bool waterLevelOk;
    bool level1Wet;
    bool level2Wet;
    bool level3Wet;
    bool level4Wet;
    const char* waterLevel;  // vazio|baixo|medio|alto
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

struct MqttPhOperationReading {
    const char* state;
    int operationRemainingSec;
    int nextCheckInSec;
};

struct MqttPhDoseReading {
    const char* sequenceId;
    const char* direction;
    int relayNumber;
    float dosageMl;
    float dosageTimeSeconds;
    float phBefore;
    float phSetpoint;
    const char* source;
};

struct MqttEcMetricReading {
    float ecSetpoint;
    float ecActual;
    float ecError;
    float kValue;
    float dosageMl;
    float dosageTimeSeconds;
    float baseDose;
    float flowRate;
    float volume;
    float totalMl;
    float kp;
    bool autoEnabled;
    bool adjustmentNeeded;
    bool adjustmentApplied;
    const char* sequenceId;
};

struct MqttPhMetricReading {
    float phSetpoint;
    float phBefore;
    float errorH;
    const char* direction;
    float kAcid;
    float kBase;
    float kUsed;
    float doseIdealMl;
    float doseRealMl;
    float dosageTimeSeconds;
    float aggressiveness;
    bool autoEnabled;
    bool adjustmentNeeded;
    bool adjustmentApplied;
    const char* sequenceId;
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
    bool publishPhOperation(const MqttPhOperationReading& reading);
    bool publishPhDose(const MqttPhDoseReading& reading);
    bool publishEcMetric(const MqttEcMetricReading& reading);
    bool publishPhMetric(const MqttPhMetricReading& reading);

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
    String phOperationTopic;
    String phDoseTopic;
    String ecMetricTopic;
    String phMetricTopic;
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
    bool publishPhOperation(const MqttPhOperationReading&) { return false; }
    bool publishPhDose(const MqttPhDoseReading&) { return false; }
    bool publishEcMetric(const MqttEcMetricReading&) { return false; }
    bool publishPhMetric(const MqttPhMetricReading&) { return false; }
    void setCommandHandler(MqttCommandPayloadHandler, void*) {}
};

#endif

#endif
