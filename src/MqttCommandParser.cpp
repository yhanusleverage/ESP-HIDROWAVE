#include "MqttCommandParser.h"

#include <cstring>

static bool isValidMacString(const String& mac) {
    if (mac.length() != 17) {
        return false;
    }
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (mac[i] != ':' && mac[i] != '-') {
                return false;
            }
        } else if (!isxdigit(mac[i])) {
            return false;
        }
    }
    return true;
}

bool parseMqttRelayCommand(
    const char* payload,
    size_t length,
    RelayCommand& out,
    bool& outIsSlave) {
    if (!payload || length == 0 || length > 480) {
        Serial.println("[MQTT CMD] rejeitado: payload vazio ou >480B");
        return false;
    }

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.printf("[MQTT CMD] JSON inválido: %s\n", err.c_str());
        return false;
    }

    int schemaVersion = doc["v"] | 0;
    if (schemaVersion != 1) {
        Serial.printf("[MQTT CMD] rejeitado: v=%d (suportado: 1)\n", schemaVersion);
        return false;
    }

    if (!doc.containsKey("id")) {
        Serial.println("[MQTT CMD] rejeitado: sem id");
        return false;
    }

    const char* cmdType = doc["cmd"] | "relay";
    if (strcmp(cmdType, "relay") != 0) {
        Serial.printf("[MQTT CMD] cmd não suportado: %s\n", cmdType);
        return false;
    }

    out.id = doc["id"].as<int>();
    if (out.id <= 0) {
        Serial.println("[MQTT CMD] rejeitado: id inválido");
        return false;
    }

    out.relayNumber = doc["relay_index"] | doc["relay_number"] | -1;
    if (out.relayNumber < 0) {
        Serial.println("[MQTT CMD] rejeitado: relay_index ausente");
        return false;
    }

    if (!doc.containsKey("action")) {
        Serial.println("[MQTT CMD] rejeitado: action ausente");
        return false;
    }
    out.action = doc["action"].as<String>();
    if (out.action != "on" && out.action != "off") {
        Serial.printf("[MQTT CMD] rejeitado: action inválida '%s'\n", out.action.c_str());
        return false;
    }

    out.durationSeconds = doc["duration_s"] | doc["duration_seconds"] | 0;
    if (out.durationSeconds < 0) {
        out.durationSeconds = 0;
    }

    out.command_type = doc["command_type"] | "manual";
    out.triggered_by = doc["triggered_by"] | "mqtt";
    out.priority = doc["priority"] | 50;
    if (out.priority < 0) {
        out.priority = 0;
    } else if (out.priority > 100) {
        out.priority = 100;
    }
    out.status = "pending";
    out.timestamp = millis();

    if (doc.containsKey("target_device_id") && !doc["target_device_id"].isNull()) {
        out.target_device_id = doc["target_device_id"].as<String>();
    } else if (doc.containsKey("slave_mac_address")) {
        out.target_device_id = doc["slave_mac_address"].as<String>();
    } else {
        out.target_device_id = "";
    }

    outIsSlave = out.target_device_id.length() > 0 &&
                 out.target_device_id != "local" &&
                 out.target_device_id != "master";

    if (outIsSlave) {
        if (!isValidMacString(out.target_device_id)) {
            Serial.printf("[MQTT CMD] rejeitado: MAC inválido '%s'\n", out.target_device_id.c_str());
            return false;
        }
        if (out.relayNumber > 7) {
            Serial.printf("[MQTT CMD] rejeitado: relay_index slave %d (máx 7)\n", out.relayNumber);
            return false;
        }
    } else if (out.relayNumber > 15) {
        Serial.printf("[MQTT CMD] rejeitado: relay_index master %d (máx 15)\n", out.relayNumber);
        return false;
    }

    if (doc.containsKey("rule_id")) {
        out.rule_id = doc["rule_id"].as<String>();
    }
    if (doc.containsKey("rule_name")) {
        out.rule_name = doc["rule_name"].as<String>();
    }

    return true;
}
