#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include "Config.h"

struct MqttTelemetryReading {
    float temperature;   // água (hydro_measurements)
    float ph;
    float ec;            // µS/cm (canónico)
    bool waterLevelOk;
    bool level1Wet;
    bool level2Wet;
    bool level3Wet;
    bool level4Wet;
    const char* waterLevel;  // vazio|baixo|medio|medio_alto|alto
    const char* interlockMode;  // normal|carrera (nullable ok)
    float airTemperature;  // ambiente (environment_data)
    float humidity;
    bool phValid;      // lectura fresca y plausible (no caché stale)
    bool ecValid;
    bool tempValid;
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
    float dilutionTargetL;
    float dilutionProgressL;
    float ecOvershootUs;
    bool hasDilutionProgress;
};

struct MqttEcDilutionReading {
    const char* sequenceId;
    float ecBefore;
    float ecSetpoint;
    float volumeTargetL;
    float volumeMeasuredL;
    float drainDurationSec;
    float fillDurationSec;
    const char* source;
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

/** hidrowave/{id}/levels — evento on-change L1–L4 (PATCH device_status) */
struct MqttLevelsReading {
    bool waterLevelOk;
    bool level1Wet;
    bool level2Wet;
    bool level3Wet;
    bool level4Wet;
    const char* waterLevel;
    bool levelsSimulated;
    const char* interlockMode;  // normal|carrera
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

/** hidrowave/{id}/command_ack — bridge → complete_relay_command */
struct MqttCommandAckReading {
    int commandId;
    const char* status;
    int relayIndex;
    const char* action;
    bool currentState;
    const char* slaveMac;
    const bool* relayStates;
    uint8_t numRelayStates;
    uint32_t espnowId;
};

/** hidrowave/{id}/relay/state — doc mqtt/04 §3.4 */
struct MqttRelayStateReading {
    const bool* masterStates;
    uint8_t masterCount;
    const char* slaveMac;
    const bool* slaveStates;
    const bool* slaveHasTimers;
    const int* slaveRemainingTimes;
    uint8_t slaveCount;
    bool linkOnline;
    uint16_t linkLastSeenS;
    bool heartbeat;
    bool hasLinkMeta;
    /** Heartbeat link-only: omit relay_states[] so cloud cache is not overwritten */
    bool omitRelayStates;
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
    bool publishLevels(const MqttLevelsReading& reading);
    bool publishHeartbeat(const MqttHeartbeatReading& reading);
    bool publishEcOperation(const MqttEcOperationReading& reading);
    bool publishEcDilution(const MqttEcDilutionReading& reading);
    bool publishDose(const MqttDoseReading& reading);
    bool publishPhOperation(const MqttPhOperationReading& reading);
    bool publishPhDose(const MqttPhDoseReading& reading);
    bool publishEcMetric(const MqttEcMetricReading& reading);
    bool publishPhMetric(const MqttPhMetricReading& reading);
    bool publishCommandAck(const MqttCommandAckReading& reading);
    bool publishRelayState(const MqttRelayStateReading& reading);

    void setCommandHandler(MqttCommandPayloadHandler handler, void* userData);

private:
    WiFiClient wifiClient;
    mutable PubSubClient mqtt;
    String deviceId;
    String mqttUsername;
    String telemetryTopic;
    String levelsTopic;
    String heartbeatTopic;
    String statusTopic;
    String commandTopic;
    String ecOperationTopic;
    String doseTopic;
    String phOperationTopic;
    String phDoseTopic;
    String ecMetricTopic;
    String phMetricTopic;
    String ecDilutionTopic;
    String commandAckTopic;
    String relayStateTopic;
    char lwtPayload[128];
    unsigned long lastReconnectAttempt;
    unsigned long reconnectIntervalMs;
    unsigned long lastFailLogMs;
    uint8_t consecutiveFailCount;
    MqttCommandPayloadHandler commandHandler;
    void* commandHandlerUserData;

    static void mqttMessageCallback(char* topic, byte* payload, unsigned int length);
    static MqttClientWrapper* callbackInstance;

    /** Reconecta só se backoff permitir (não forçar em cada publish). */
    bool ensureConnected();
    /** true se já conectado — sem tentativa de TCP/MQTT. */
    bool isConnectedCached() const { return mqtt.connected(); }
    bool subscribeCommandTopic();
    bool publishOnlineStatus();
    void bumpReconnectBackoff();
};

#else

class MqttClientWrapper {
public:
    bool begin(const String&) { return false; }
    void loop() {}
    bool isConnected() const { return false; }
    bool publishTelemetry(const MqttTelemetryReading&) { return false; }
    bool publishLevels(const MqttLevelsReading&) { return false; }
    bool publishHeartbeat(const MqttHeartbeatReading&) { return false; }
    bool publishEcOperation(const MqttEcOperationReading&) { return false; }
    bool publishEcDilution(const MqttEcDilutionReading&) { return false; }
    bool publishDose(const MqttDoseReading&) { return false; }
    bool publishPhOperation(const MqttPhOperationReading&) { return false; }
    bool publishPhDose(const MqttPhDoseReading&) { return false; }
    bool publishEcMetric(const MqttEcMetricReading&) { return false; }
    bool publishPhMetric(const MqttPhMetricReading&) { return false; }
    bool publishCommandAck(const MqttCommandAckReading&) { return false; }
    bool publishRelayState(const MqttRelayStateReading&) { return false; }
    void setCommandHandler(MqttCommandPayloadHandler, void*) {}
};

#endif

#endif
