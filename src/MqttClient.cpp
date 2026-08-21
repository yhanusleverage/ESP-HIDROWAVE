#include "MqttClient.h"
#include "SensorSanitize.h"

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
      reconnectIntervalMs(30000),
      lastFailLogMs(0),
      consecutiveFailCount(0),
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
    levelsTopic = String("hidrowave/") + deviceId + "/levels";
    heartbeatTopic = String("hidrowave/") + deviceId + "/heartbeat";
    statusTopic = String("hidrowave/") + deviceId + "/status";
    commandTopic = String("hidrowave/") + deviceId + "/command";
    ecOperationTopic = String("hidrowave/") + deviceId + "/ec_operation";
    doseTopic = String("hidrowave/") + deviceId + "/dose";
    phOperationTopic = String("hidrowave/") + deviceId + "/ph_operation";
    phDoseTopic = String("hidrowave/") + deviceId + "/ph_dose";
    ecMetricTopic = String("hidrowave/") + deviceId + "/ec_metric";
    phMetricTopic = String("hidrowave/") + deviceId + "/ph_metric";
    ecDilutionTopic = String("hidrowave/") + deviceId + "/ec_dilution";
    commandAckTopic = String("hidrowave/") + deviceId + "/command_ack";
    relayStateTopic = String("hidrowave/") + deviceId + "/relay/state";
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    callbackInstance = this;
    mqtt.setCallback(mqttMessageCallback);

    mqttUsername = String("mqtt_") + deviceId;

    if (strlen(MQTT_HOST) == 0) {
        Serial.println("[MQTT] MQTT_HOST vazio — desabilitado");
        return false;
    }

    StaticJsonDocument<128> lwtDoc;
    lwtDoc["v"] = 1;
    lwtDoc["device_id"] = deviceId;
    lwtDoc["online"] = false;
    serializeJson(lwtDoc, lwtPayload, sizeof(lwtPayload));

    Serial.printf("[MQTT] Broker %s:%d user=%s\n", MQTT_HOST, MQTT_PORT, mqttUsername.c_str());
    Serial.printf("[MQTT] topics telemetry=%s levels=%s heartbeat=%s status=%s command=%s ec_op=%s dose=%s ph_op=%s ph_dose=%s ec_metric=%s ph_metric=%s\n",
                  telemetryTopic.c_str(), levelsTopic.c_str(), heartbeatTopic.c_str(), statusTopic.c_str(),
                  commandTopic.c_str(), ecOperationTopic.c_str(), doseTopic.c_str(),
                  phOperationTopic.c_str(), phDoseTopic.c_str(),
                  ecMetricTopic.c_str(), phMetricTopic.c_str());
    return ensureConnected();
}

void MqttClientWrapper::loop() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    if (!mqtt.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt >= reconnectIntervalMs) {
            ensureConnected();
        }
    } else {
        reconnectIntervalMs = 30000;
        consecutiveFailCount = 0;
    }
    mqtt.loop();
}

void MqttClientWrapper::bumpReconnectBackoff() {
    // 30s ? 60s ? 120s ? ? ? m?x 5 min (evita cascata TCP+HTTPS)
    if (reconnectIntervalMs < 300000UL) {
        unsigned long next = reconnectIntervalMs * 2UL;
        if (next < 30000UL) {
            next = 30000UL;
        }
        if (next > 300000UL) {
            next = 300000UL;
        }
        reconnectIntervalMs = next;
    }
    consecutiveFailCount = consecutiveFailCount < 255 ? consecutiveFailCount + 1 : 255;
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

    unsigned long now = millis();
    if (lastReconnectAttempt != 0 && (now - lastReconnectAttempt) < reconnectIntervalMs) {
        return false;
    }
    lastReconnectAttempt = now;

    String clientId = deviceId.length() ? deviceId : "ESP32_HIDRO";
    Serial.printf("[MQTT] Connecting clientId=%s user=%s (backoff=%lus)...\n",
                  clientId.c_str(), mqttUsername.c_str(), reconnectIntervalMs / 1000UL);

    bool ok = mqtt.connect(
        clientId.c_str(),
        mqttUsername.c_str(),
        MQTT_PASS,
        statusTopic.c_str(),
        1,
        true,
        lwtPayload);
    if (ok) {
        Serial.println("[MQTT] Connected (LWT on status topic)");
        reconnectIntervalMs = 30000;
        consecutiveFailCount = 0;
        publishOnlineStatus();
        subscribeCommandTopic();
    } else {
        bumpReconnectBackoff();
        // Rate-limit: 1? fail + depois no m?ximo a cada 60s
        if (consecutiveFailCount <= 1 || (now - lastFailLogMs) >= 60000UL) {
            lastFailLogMs = now;
            Serial.printf("[MQTT] Failed rc=%d ? pr?ximo retry em %lus\n",
                          mqtt.state(), reconnectIntervalMs / 1000UL);
        }
    }
    return ok;
}

bool MqttClientWrapper::publishTelemetry(const MqttTelemetryReading& reading) {
    if (!mqtt.connected()) {
        return false;
    }

    const float waterTemp = sanitizeSensorRange(reading.temperature, MIN_TEMP, MAX_TEMP);
    const float ph = sanitizeSensorRange(reading.ph, MIN_PH, MAX_PH);
    const float ecUsCm = sanitizeSensorRange(reading.ec, MIN_EC, MAX_EC);

#if !HIDRO_DEV_RELAX_SENSORS
    if (isnan(waterTemp) || isnan(ph) || isnan(ecUsCm)) {
        Serial.println("[MQTT] telemetry skipped ? lecturas hidro inv?lidas");
        return false;
    }
#endif

    StaticJsonDocument<448> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
#if HIDRO_DEV_RELAX_SENSORS
    if (reading.tempValid && isValidWaterTempReading(reading.temperature)) {
        doc["temperature"] = reading.temperature;
    } else if (!reading.tempValid) {
        Serial.println("[MQTT] temperature omitted ? invalid or stale");
    }
    if (reading.phValid && isValidPhReading(reading.ph)) {
        doc["ph"] = reading.ph;
    } else if (!reading.phValid) {
        Serial.printf("[MQTT] ph omitted ? valid=%d value=%.3f\n",
                      reading.phValid ? 1 : 0, reading.ph);
    }
    if (reading.ecValid && isValidEcMicroSiemens(reading.ec)) {
        const float ecRounded = round(reading.ec * 100.0) / 100.0;
        doc["ec"] = ecRounded;
    } else if (!reading.ecValid) {
        Serial.printf("[MQTT] ec omitted ? valid=%d value=%.0f\n",
                      reading.ecValid ? 1 : 0, reading.ec);
    }
#else
    if (reading.tempValid) {
        doc["temperature"] = waterTemp;
    }
    if (reading.phValid) {
        doc["ph"] = ph;
    }
    if (reading.ecValid) {
        const float ecRounded = round(ecUsCm * 100.0) / 100.0;
        doc["ec"] = ecRounded;
    }
#endif
    doc["water_level_ok"] = reading.waterLevelOk;
    doc["level_1"] = reading.level1Wet;
    doc["level_2"] = reading.level2Wet;
    doc["level_3"] = reading.level3Wet;
    doc["level_4"] = reading.level4Wet;
    if (reading.waterLevel && reading.waterLevel[0]) {
        doc["water_level"] = reading.waterLevel;
    }
#if HIDRO_SIMULATE_WATER_LEVELS
    doc["levels_simulated"] = true;
#else
    doc["levels_simulated"] = false;
#endif
    if (reading.interlockMode && reading.interlockMode[0]) {
        doc["interlock_mode"] = reading.interlockMode;
    }
    if (isValidEnvironmentReading(reading.airTemperature, reading.humidity)) {
        doc["air_temp"] = reading.airTemperature;
        doc["humidity"] = reading.humidity;
    }

    char payload[384];
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

bool MqttClientWrapper::publishLevels(const MqttLevelsReading& reading) {
    if (!mqtt.connected()) {
        return false;
    }

    StaticJsonDocument<320> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["water_level_ok"] = reading.waterLevelOk;
    doc["level_1"] = reading.level1Wet;
    doc["level_2"] = reading.level2Wet;
    doc["level_3"] = reading.level3Wet;
    doc["level_4"] = reading.level4Wet;
    if (reading.waterLevel && reading.waterLevel[0]) {
        doc["water_level"] = reading.waterLevel;
    }
    doc["levels_simulated"] = reading.levelsSimulated;
    if (reading.interlockMode && reading.interlockMode[0]) {
        doc["interlock_mode"] = reading.interlockMode;
    }

    char payload[320];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0) {
        return false;
    }

    bool published = mqtt.publish(levelsTopic.c_str(), payload, false);
    if (published) {
        Serial.printf("[MQTT] levels L1=%d L2=%d L3=%d L4=%d wl=%s ok=%d sim=%d ilock=%s\n",
                      reading.level1Wet ? 1 : 0,
                      reading.level2Wet ? 1 : 0,
                      reading.level3Wet ? 1 : 0,
                      reading.level4Wet ? 1 : 0,
                      reading.waterLevel ? reading.waterLevel : "-",
                      reading.waterLevelOk ? 1 : 0,
                      reading.levelsSimulated ? 1 : 0,
                      reading.interlockMode ? reading.interlockMode : "-");
    } else {
        Serial.println("[MQTT] levels publish failed");
    }
    return published;
}

bool MqttClientWrapper::publishHeartbeat(const MqttHeartbeatReading& reading) {
    if (!mqtt.connected()) {
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
    if (!mqtt.connected()) {
        return false;
    }

    StaticJsonDocument<384> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["ec_operation_state"] = reading.state ? reading.state : "idle";
    doc["ec_operation_remaining_sec"] = reading.operationRemainingSec > 0 ? reading.operationRemainingSec : 0;
    doc["ec_next_check_in_sec"] = reading.nextCheckInSec > 0 ? reading.nextCheckInSec : 0;
    if (reading.hasDilutionProgress) {
        doc["dilution_target_l"] = round(reading.dilutionTargetL * 100.0) / 100.0;
        doc["dilution_progress_l"] = round(reading.dilutionProgressL * 100.0) / 100.0;
        if (isfinite(reading.ecOvershootUs)) {
            doc["ec_overshoot_us"] = round(reading.ecOvershootUs * 100.0) / 100.0;
        }
    }

    char payload[384];
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

bool MqttClientWrapper::publishEcDilution(const MqttEcDilutionReading& reading) {
    if (!mqtt.connected()) {
        return false;
    }

    StaticJsonDocument<384> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["sequence_id"] = reading.sequenceId ? reading.sequenceId : "";
    doc["ec_before"] = round(reading.ecBefore * 100.0) / 100.0;
    doc["ec_setpoint"] = round(reading.ecSetpoint * 100.0) / 100.0;
    doc["volume_target_l"] = round(reading.volumeTargetL * 1000.0) / 1000.0;
    doc["volume_measured_l"] = round(reading.volumeMeasuredL * 1000.0) / 1000.0;
    doc["drain_duration_s"] = round(reading.drainDurationSec * 100.0) / 100.0;
    doc["fill_duration_s"] = round(reading.fillDurationSec * 100.0) / 100.0;
    doc["source"] = reading.source ? reading.source : "manual";

    char payload[384];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0) {
        return false;
    }

    bool published = mqtt.publish(ecDilutionTopic.c_str(), payload, false);
    if (!published) {
        Serial.println("[MQTT] ec_dilution publish failed");
    }
    return published;
}

bool MqttClientWrapper::publishDose(const MqttDoseReading& reading) {
    if (!mqtt.connected()) {
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

    bool published = mqtt.publish(doseTopic.c_str(), payload, false);
    if (published) {
        Serial.printf("[MQTT] dose %s %.2f ml relay_index=%d\n",
                      reading.nutrientName ? reading.nutrientName : "?",
                      reading.dosageMl,
                      reading.relayNumber);
    } else {
        Serial.println("[MQTT] dose publish failed");
    }
    return published;
}

bool MqttClientWrapper::publishPhOperation(const MqttPhOperationReading& reading) {
    if (!mqtt.connected()) {
        return false;
    }

    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["ph_operation_state"] = reading.state ? reading.state : "idle";
    doc["ph_operation_remaining_sec"] = reading.operationRemainingSec > 0 ? reading.operationRemainingSec : 0;
    doc["ph_next_check_in_sec"] = reading.nextCheckInSec > 0 ? reading.nextCheckInSec : 0;

    char payload[256];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0) {
        return false;
    }

    bool published = mqtt.publish(phOperationTopic.c_str(), payload, false);
    if (published) {
        Serial.printf("[MQTT] ph_operation %s rem=%ds next=%ds\n",
                      reading.state ? reading.state : "idle",
                      reading.operationRemainingSec,
                      reading.nextCheckInSec);
    } else {
        Serial.println("[MQTT] ph_operation publish failed");
    }
    return published;
}

bool MqttClientWrapper::publishPhDose(const MqttPhDoseReading& reading) {
    if (!mqtt.connected()) {
        return false;
    }

    StaticJsonDocument<512> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["sequence_id"] = reading.sequenceId ? reading.sequenceId : "";
    doc["direction"] = reading.direction ? reading.direction : "";
    doc["relay_number"] = reading.relayNumber;
    doc["dosage_ml"] = round(reading.dosageMl * 1000.0) / 1000.0;
    doc["dosage_time_seconds"] = round(reading.dosageTimeSeconds * 100.0) / 100.0;
    doc["ph_before"] = round(reading.phBefore * 100.0) / 100.0;
    doc["ph_setpoint"] = round(reading.phSetpoint * 100.0) / 100.0;
    doc["source"] = reading.source && reading.source[0] ? reading.source : "auto_ph";

    char payload[512];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0) {
        return false;
    }

    bool published = mqtt.publish(phDoseTopic.c_str(), payload, false);
    if (published) {
        Serial.printf("[MQTT] ph_dose %s %.2f ml relay_index=%d\n",
                      reading.direction ? reading.direction : "?",
                      reading.dosageMl,
                      reading.relayNumber);
    } else {
        Serial.println("[MQTT] ph_dose publish failed");
    }
    return published;
}

bool MqttClientWrapper::publishEcMetric(const MqttEcMetricReading& reading) {
    if (!mqtt.connected()) {
        return false;
    }

    StaticJsonDocument<768> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["ec_setpoint"] = round(reading.ecSetpoint * 100.0) / 100.0;
    doc["ec_actual"] = round(reading.ecActual * 100.0) / 100.0;
    doc["ec_error"] = round(reading.ecError * 100.0) / 100.0;
    doc["k_value"] = reading.kValue;
    doc["dosage_ml"] = round(reading.dosageMl * 1000.0) / 1000.0;
    doc["dosage_time_seconds"] = round(reading.dosageTimeSeconds * 100.0) / 100.0;
    doc["base_dose"] = reading.baseDose;
    doc["flow_rate"] = reading.flowRate;
    doc["volume"] = reading.volume;
    doc["total_ml"] = reading.totalMl;
    doc["kp"] = reading.kp;
    doc["auto_enabled"] = reading.autoEnabled;
    doc["adjustment_needed"] = reading.adjustmentNeeded;
    doc["adjustment_applied"] = reading.adjustmentApplied;
    if (reading.sequenceId && reading.sequenceId[0]) {
        doc["sequence_id"] = reading.sequenceId;
    }

    char payload[768];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0) {
        return false;
    }

    bool published = mqtt.publish(ecMetricTopic.c_str(), payload, false);
    if (published) {
        Serial.printf("[MQTT] ec_metric err=%.0f u(t)=%.2fml adj=%d\n",
                      reading.ecError, reading.dosageMl, reading.adjustmentApplied ? 1 : 0);
    } else {
        Serial.println("[MQTT] ec_metric publish failed");
    }
    return published;
}

bool MqttClientWrapper::publishPhMetric(const MqttPhMetricReading& reading) {
    if (!mqtt.connected()) {
        return false;
    }

    StaticJsonDocument<768> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["ph_setpoint"] = round(reading.phSetpoint * 1000.0) / 1000.0;
    float phBeforeMetric = reading.phBefore;
#if HIDRO_DEV_RELAX_SENSORS
    if (isfinite(phBeforeMetric)) {
        if (phBeforeMetric > 99.999f) phBeforeMetric = 99.999f;
        else if (phBeforeMetric < -99.999f) phBeforeMetric = -99.999f;
    }
#endif
    doc["ph_before"] = round(phBeforeMetric * 1000.0) / 1000.0;
    float errorH = reading.errorH;
    if (!isfinite(errorH)) {
        const float linearErr = reading.phSetpoint - reading.phBefore;
        errorH = isfinite(linearErr) ? linearErr * 1e-6f : 0.0f;
    }
    doc["error_h"] = errorH;
    if (reading.direction && reading.direction[0]) {
        doc["direction"] = reading.direction;
    }
    doc["k_acid"] = reading.kAcid;
    doc["k_base"] = reading.kBase;
    doc["k_used"] = reading.kUsed;
    doc["dose_ideal_ml"] = round(reading.doseIdealMl * 1000.0) / 1000.0;
    doc["dose_real_ml"] = round(reading.doseRealMl * 1000.0) / 1000.0;
    doc["dosage_time_seconds"] = round(reading.dosageTimeSeconds * 100.0) / 100.0;
    doc["aggressiveness"] = reading.aggressiveness;
    doc["auto_enabled"] = reading.autoEnabled;
    doc["adjustment_needed"] = reading.adjustmentNeeded;
    doc["adjustment_applied"] = reading.adjustmentApplied;
    if (reading.sequenceId && reading.sequenceId[0]) {
        doc["sequence_id"] = reading.sequenceId;
    }

    char payload[768];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0) {
        return false;
    }

    bool published = mqtt.publish(phMetricTopic.c_str(), payload, false);
    if (published) {
        Serial.printf("[MQTT] ph_metric errH=%.3e u(t)=%.2fml adj=%d\n",
                      reading.errorH, reading.doseRealMl, reading.adjustmentApplied ? 1 : 0);
    } else {
        Serial.println("[MQTT] ph_metric publish failed");
    }
    return published;
}

bool MqttClientWrapper::publishCommandAck(const MqttCommandAckReading& reading) {
    if (!mqtt.connected() || reading.commandId <= 0) {
        return false;
    }

    StaticJsonDocument<512> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["ts"] = (uint32_t)(millis() / 1000UL);
    doc["id"] = reading.commandId;
    doc["status"] = reading.status ? reading.status : "completed";
    doc["relay_index"] = reading.relayIndex;
    if (reading.action && reading.action[0]) {
        doc["action"] = reading.action;
    }
    doc["current_state"] = reading.currentState;
    if (reading.espnowId > 0) {
        doc["espnow_id"] = reading.espnowId;
    }
    if (reading.slaveMac && reading.slaveMac[0]) {
        doc["slave_mac_address"] = reading.slaveMac;
        if (reading.relayStates && reading.numRelayStates > 0) {
            JsonArray arr = doc.createNestedArray("relay_states");
            uint8_t n = reading.numRelayStates > 8 ? 8 : reading.numRelayStates;
            for (uint8_t i = 0; i < n; i++) {
                arr.add(reading.relayStates[i]);
            }
        }
    }

    char payload[512];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0 || len >= sizeof(payload)) {
        return false;
    }

    bool published = mqtt.publish(commandAckTopic.c_str(), payload, false);
    if (published) {
        Serial.printf("[MQTT] command_ack id=%d relay=%d state=%d status=%s\n",
                      reading.commandId, reading.relayIndex, reading.currentState ? 1 : 0,
                      reading.status ? reading.status : "completed");
    } else {
        Serial.println("[MQTT] command_ack publish failed");
    }
    return published;
}

bool MqttClientWrapper::publishRelayState(const MqttRelayStateReading& reading) {
    if (!mqtt.connected()) {
        return false;
    }

    StaticJsonDocument<512> doc;
    doc["v"] = 1;
    doc["device_id"] = deviceId;
    doc["ts"] = (uint32_t)(millis() / 1000UL);

    if (reading.masterStates && reading.masterCount > 0) {
        JsonArray master = doc.createNestedArray("master");
        uint8_t n = reading.masterCount > 16 ? 16 : reading.masterCount;
        for (uint8_t i = 0; i < n; i++) {
            master.add(reading.masterStates[i] ? 1 : 0);
        }
    }

    if (reading.slaveMac && reading.slaveMac[0]) {
        doc["slave_mac_address"] = reading.slaveMac;
        if (!reading.omitRelayStates && reading.slaveStates && reading.slaveCount > 0) {
            JsonArray states = doc.createNestedArray("relay_states");
            uint8_t n = reading.slaveCount > 8 ? 8 : reading.slaveCount;
            for (uint8_t i = 0; i < n; i++) {
                states.add(reading.slaveStates[i]);
            }
            if (reading.slaveHasTimers) {
                JsonArray timers = doc.createNestedArray("relay_has_timers");
                for (uint8_t i = 0; i < n; i++) {
                    timers.add(reading.slaveHasTimers[i]);
                }
            }
            if (reading.slaveRemainingTimes) {
                JsonArray rem = doc.createNestedArray("relay_remaining_times");
                for (uint8_t i = 0; i < n; i++) {
                    rem.add(reading.slaveRemainingTimes[i]);
                }
            }
        }
        if (reading.hasLinkMeta) {
            doc["link_online"] = reading.linkOnline;
            doc["link_last_seen_s"] = reading.linkLastSeenS;
        }
        if (reading.heartbeat) {
            doc["heartbeat"] = true;
        }
    }

    char payload[512];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0 || len >= sizeof(payload)) {
        return false;
    }

    bool published = mqtt.publish(relayStateTopic.c_str(), payload, false);
    if (published) {
        Serial.println("[MQTT] relay/state published");
    } else {
        Serial.println("[MQTT] relay/state publish failed");
    }
    return published;
}

#endif
