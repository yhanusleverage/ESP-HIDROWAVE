#include "MqttCommandParser.h"

#include <cstring>
#include <math.h>

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

static void logPayloadSnippet(const char* payload, size_t length) {
    const size_t maxLen = 200;
    const size_t n = length < maxLen ? length : maxLen;
    Serial.printf("[MQTT CMD] payload (%uB): ", (unsigned)length);
    for (size_t i = 0; i < n; i++) {
        Serial.write(payload[i]);
    }
    if (length > maxLen) {
        Serial.print("...");
    }
    Serial.println();
}

static int parseJsonIntField(JsonVariantConst field) {
    if (field.isNull()) {
        return -1;
    }
    if (field.is<int>() || field.is<long>()) {
        return field.as<int>();
    }
    if (field.is<const char*>()) {
        const char* s = field.as<const char*>();
        if (s && s[0] != '\0') {
            return atoi(s);
        }
    }
    return -1;
}

static int parseRelayIndexFromDoc(JsonObjectConst doc) {
    if (doc.containsKey("relay_index")) {
        int v = parseJsonIntField(doc["relay_index"]);
        if (v >= 0) {
            return v;
        }
    }
    if (doc.containsKey("relay_number")) {
        int v = parseJsonIntField(doc["relay_number"]);
        if (v >= 0) {
            return v;
        }
    }
    if (doc.containsKey("relay_numbers") && doc["relay_numbers"].is<JsonArrayConst>()) {
        JsonArrayConst arr = doc["relay_numbers"].as<JsonArrayConst>();
        if (arr.size() > 0) {
            int v = parseJsonIntField(arr[0]);
            if (v >= 0) {
                return v;
            }
        }
    }
    return -1;
}

static String parseActionFromDoc(JsonObjectConst doc) {
    if (doc.containsKey("action") && !doc["action"].isNull()) {
        return doc["action"].as<String>();
    }
    if (doc.containsKey("actions") && doc["actions"].is<JsonArrayConst>()) {
        JsonArrayConst arr = doc["actions"].as<JsonArrayConst>();
        if (arr.size() > 0 && !arr[0].isNull()) {
            return arr[0].as<String>();
        }
    }
    return "";
}

/** dosage_ml direto ou encoded em triggered_by/created_by: "calibragem_test#5.0" */
static void applyDosageAndTriggerMeta(RelayCommand& out, JsonObjectConst doc) {
    out.dosageMl = 0.0f;
    if (doc.containsKey("dosage_ml") && !doc["dosage_ml"].isNull()) {
        out.dosageMl = doc["dosage_ml"].as<float>();
        if (!isfinite(out.dosageMl) || out.dosageMl < 0.0f) {
            out.dosageMl = 0.0f;
        }
    }

    String meta = out.triggered_by;
    if (doc.containsKey("created_by") && !doc["created_by"].isNull()) {
        const String createdBy = doc["created_by"].as<String>();
        if (meta.length() == 0 || meta == "mqtt" || meta == "manual") {
            meta = createdBy;
        }
        if (out.dosageMl <= 0.0f) {
            const int hash = createdBy.indexOf('#');
            if (hash > 0 && hash + 1 < (int)createdBy.length()) {
                out.dosageMl = createdBy.substring(hash + 1).toFloat();
                if (!isfinite(out.dosageMl) || out.dosageMl < 0.0f) {
                    out.dosageMl = 0.0f;
                }
                if (meta == createdBy) {
                    meta = createdBy.substring(0, hash);
                }
            }
        }
    }

    const int hashTb = meta.indexOf('#');
    if (hashTb > 0) {
        if (out.dosageMl <= 0.0f) {
            out.dosageMl = meta.substring(hashTb + 1).toFloat();
            if (!isfinite(out.dosageMl) || out.dosageMl < 0.0f) {
                out.dosageMl = 0.0f;
            }
        }
        meta = meta.substring(0, hashTb);
    }
    out.triggered_by = meta;
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
        logPayloadSnippet(payload, length);
        return false;
    }

    int schemaVersion = doc["v"] | 0;
    if (schemaVersion != 1) {
        Serial.printf("[MQTT CMD] rejeitado: v=%d (suportado: 1)\n", schemaVersion);
        logPayloadSnippet(payload, length);
        return false;
    }

    if (!doc.containsKey("id")) {
        Serial.println("[MQTT CMD] rejeitado: sem id");
        logPayloadSnippet(payload, length);
        return false;
    }

    const char* cmdType = doc["cmd"] | "relay";
    if (strcmp(cmdType, "relay") != 0) {
        Serial.printf("[MQTT CMD] cmd não suportado: %s\n", cmdType);
        logPayloadSnippet(payload, length);
        return false;
    }

    out.id = doc["id"].as<int>();
    if (out.id <= 0) {
        Serial.println("[MQTT CMD] rejeitado: id inválido");
        logPayloadSnippet(payload, length);
        return false;
    }

    out.relayNumber = parseRelayIndexFromDoc(doc.as<JsonObjectConst>());
    if (out.relayNumber < 0) {
        Serial.println("[MQTT CMD] rejeitado: relay_index ausente");
        logPayloadSnippet(payload, length);
        return false;
    }

    out.action = parseActionFromDoc(doc.as<JsonObjectConst>());
    if (out.action.length() == 0) {
        Serial.println("[MQTT CMD] rejeitado: action ausente");
        logPayloadSnippet(payload, length);
        return false;
    }
    if (out.action != "on" && out.action != "off") {
        Serial.printf("[MQTT CMD] rejeitado: action inválida '%s'\n", out.action.c_str());
        logPayloadSnippet(payload, length);
        return false;
    }

    out.durationSeconds = doc["duration_s"] | doc["duration_seconds"] | 0;
    if (out.durationSeconds < 0) {
        out.durationSeconds = 0;
    }
    if (out.durationSeconds == 0 && doc.containsKey("duration_seconds") &&
        doc["duration_seconds"].is<JsonArrayConst>()) {
        JsonArrayConst darr = doc["duration_seconds"].as<JsonArrayConst>();
        if (darr.size() > 0) {
            out.durationSeconds = darr[0] | 0;
        }
    }

    if (doc.containsKey("mode") && !doc["mode"].isNull()) {
        out.commandMode = doc["mode"].as<String>();
    } else {
        out.commandMode = "instant";
    }
    out.commandMode.toLowerCase();

    out.cycleOffSeconds = doc["cycle_off_s"] | doc["cycle_off_seconds"] | 0;
    if (out.cycleOffSeconds < 0) {
        out.cycleOffSeconds = 0;
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
    applyDosageAndTriggerMeta(out, doc.as<JsonObjectConst>());

    if (doc.containsKey("target_device_id") && !doc["target_device_id"].isNull()) {
        out.target_device_id = doc["target_device_id"].as<String>();
    } else if (doc.containsKey("slave_mac_address") && !doc["slave_mac_address"].isNull()) {
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
            logPayloadSnippet(payload, length);
            return false;
        }
        if (out.relayNumber > 7) {
            Serial.printf("[MQTT CMD] rejeitado: relay_index slave %d (máx 7)\n", out.relayNumber);
            logPayloadSnippet(payload, length);
            return false;
        }
    } else if (out.relayNumber > 15) {
        Serial.printf("[MQTT CMD] rejeitado: relay_index master %d (máx 15)\n", out.relayNumber);
        logPayloadSnippet(payload, length);
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

bool parseMqttEcDilutionCommand(const char* payload, size_t length, float& outVolumeL) {
    outVolumeL = 0.0f;
    if (!payload || length == 0 || length > 480) {
        return false;
    }

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        return false;
    }

    if ((doc["v"] | 0) != 1) {
        return false;
    }

    const char* action = doc["action"] | "";
    if (strcmp(action, "ec_dilution_start") != 0) {
        return false;
    }

    outVolumeL = doc["volume_l"] | 0.0f;
    if (!isfinite(outVolumeL) || outVolumeL < 0.1f) {
        Serial.println("[MQTT CMD] ec_dilution_start volume_l inválido");
        return false;
    }

    Serial.printf("[MQTT CMD] ec_dilution_start %.2f L\n", outVolumeL);
    return true;
}

bool parseMqttLevelInterlockCommand(const char* payload, size_t length, char* outMode, size_t outModeLen) {
    if (!payload || !outMode || outModeLen < 2 || length == 0 || length > 480) {
        return false;
    }
    outMode[0] = '\0';

    StaticJsonDocument<192> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        return false;
    }

    if ((doc["v"] | 0) != 1) {
        return false;
    }

    const char* action = doc["action"] | "";
    if (strcmp(action, "set_level_interlock") != 0) {
        return false;
    }

    const char* mode = doc["mode"] | "";
    if (strcmp(mode, "normal") != 0 && strcmp(mode, "carrera") != 0) {
        Serial.println("[MQTT CMD] set_level_interlock mode inválido (normal|carrera)");
        return false;
    }

    strncpy(outMode, mode, outModeLen - 1);
    outMode[outModeLen - 1] = '\0';
    Serial.printf("[MQTT CMD] set_level_interlock mode=%s\n", outMode);
    return true;
}
