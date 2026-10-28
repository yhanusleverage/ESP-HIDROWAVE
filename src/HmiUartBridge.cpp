#include "HmiUartBridge.h"

#if ENABLE_HMI_UART

#include "HybridStateManager.h"
extern HydroStateManager stateManager;

#include "HydroControl.h"
#include "RelayCoordinator.h"
#include "MasterSlaveManager.h"
#include "SupabaseClient.h"
#include "ESPNowController.h"
#include "Controller.h"
#include "DecisionEngine.h"
#include "MqttClient.h"
#include "RelayNameCatalog.h"
#include "WiFiCredentialsManager.h"
#include <WiFi.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <cstring>
#include <climits>
#include <cstdio>
#include <math.h>

static HardwareSerial HmiSerial(1);

HmiUartBridge* HmiUartBridge::s_active_ = nullptr;

HmiUartBridge* HmiUartBridge::activeInstance() {
    return s_active_;
}

#if UART_LINK_DEBUG
static uint32_t hmiRxByteCount = 0;
static uint32_t hmiRxLineCount = 0;
static unsigned long hmiLastDbgMs = 0;

static void logInvalidLine(const char* line, const char* errMsg) {
    char preview[97];
    size_t n = strlen(line);
    if (n > 96) {
        n = 96;
    }
    memcpy(preview, line, n);
    preview[n] = '\0';
    Serial.printf("[HMI UART] JSON invalido: %s | raw (len=%u): %s%s\n", errMsg,
                  static_cast<unsigned>(strlen(line)), preview, strlen(line) > 96 ? "..." : "");
}

static void maybeLogHmiUartDebug(unsigned long nowMs) {
    if (hmiLastDbgMs != 0 && (nowMs - hmiLastDbgMs) < UART_LINK_DEBUG_INTERVAL_MS) {
        return;
    }
    hmiLastDbgMs = nowMs;
    Serial.printf("[HMI UART DBG] rx_bytes=%lu lines=%lu avail=%d RX=%d TX=%d\n",
                  static_cast<unsigned long>(hmiRxByteCount),
                  static_cast<unsigned long>(hmiRxLineCount), HmiSerial.available(),
                  HMI_UART_RX_PIN, HMI_UART_TX_PIN);
    if (hmiRxByteCount == 0) {
        Serial.println("[HMI UART DBG] sin bytes RX — HMI TX(17)->Master RX(17) + GND?");
    }
}
#endif

void HmiUartBridge::dumpLinkStatus(Stream& out) const {
    out.printf("[HMI UART STATUS] ready=%s RX=%d TX=%d baud=%d\n",
               ready_ ? "yes" : "no", HMI_UART_RX_PIN, HMI_UART_TX_PIN, HMI_UART_BAUD);
#if UART_LINK_DEBUG
    out.printf("[HMI UART STATUS] rx_bytes=%lu rx_lines=%lu avail=%d\n",
               static_cast<unsigned long>(hmiRxByteCount),
               static_cast<unsigned long>(hmiRxLineCount), HmiSerial.available());
#endif
    out.println("[HMI UART STATUS] cable: HMI TX(17)->Master RX(17), Master TX(18)->HMI RX(18), GND");
    out.println("[HMI UART STATUS] NOTA: IO17=RX firmware (no UART2 TX del pinout ESP32U)");
#if UART_LINK_DEBUG
    if (hmiRxByteCount == 0) {
        out.println("[HMI UART STATUS] sin bytes RX — revisar cable cruzado + GND comun");
    }
#endif
}

void HmiUartBridge::dumpLastTelemetry(Stream& out) const {
    out.println("[HMI UART LAST] --- careo Serial vs HMI ---");
    if (lastTelemetryValid_) {
        out.printf("[HMI UART LAST] tx_json=%s\n", lastTelemetryJson_);
    } else {
        out.println("[HMI UART LAST] tx_json=(ninguno aún)");
    }
    if (!ctx_.hydro) {
        out.println("[HMI UART LAST] hydro=null");
        return;
    }
    HydroControl& hydro = *ctx_.hydro;
    const unsigned long ecAge = hydro.getEcValidAgeMs();
    const unsigned long phAge = hydro.getPhValidAgeMs();
    const unsigned long tempAge = hydro.getTempValidAgeMs();
    out.printf("[HMI UART LAST] ec=%.1f valid=%d age_ms=%lu stale_limit=%lu\n",
               hydro.getEC(), hydro.isEcValidForTelemetry() ? 1 : 0,
               ecAge == ULONG_MAX ? 0UL : ecAge, SENSOR_READING_STALE_MS);
    out.printf("[HMI UART LAST] ph=%.2f valid=%d age_ms=%lu\n",
               hydro.getpH(), hydro.isPhValidForTelemetry() ? 1 : 0,
               phAge == ULONG_MAX ? 0UL : phAge);
    out.printf("[HMI UART LAST] temp=%.1f valid=%d age_ms=%lu\n",
               hydro.getTemperature(), hydro.isTempValidForTelemetry() ? 1 : 0,
               tempAge == ULONG_MAX ? 0UL : tempAge);
    if (ecAge == ULONG_MAX) {
        out.println("[HMI UART LAST] EC: nunca hubo lectura válida");
    } else if (!hydro.isEcValidForTelemetry()) {
        out.println("[HMI UART LAST] EC: STALE — telemetry omite campo ec; HMI puede congelar valor viejo");
    }
}

void HmiUartBridge::attach(const Context& ctx) {
    ctx_ = ctx;
}

void HmiUartBridge::bindMasterManager(MasterSlaveManager* masterManager) {
    ctx_.masterManager = masterManager;
}

void HmiUartBridge::begin() {
    HmiSerial.begin(HMI_UART_BAUD, SERIAL_8N1, HMI_UART_RX_PIN, HMI_UART_TX_PIN);
    lineLen_ = 0;
    ready_ = true;
    plantCfgPushed_ = false;
    lastTelemetryMs_ = 0;
    lastTelemetryValid_ = false;
    lastTelemetryJson_[0] = '\0';
    pendingRestart_ = false;
    s_active_ = this;
    Serial.printf("[HMI UART] RX=%d TX=%d baud=%d\n",
                  HMI_UART_RX_PIN, HMI_UART_TX_PIN, HMI_UART_BAUD);
#if UART_LINK_DEBUG
    Serial.println("[HMI UART DBG] cable: HMI TX(17)->Master RX(17), Master TX(18)->HMI RX(18), GND");
    Serial.println("[HMI UART DBG] NOTA: IO17=RX firmware (no UART2 TX del pinout)");
#endif
    if (!ctx_.hydro) {
        /* SoftAP: avisar ya has_wifi / perfil para wizard HMI. */
        sendSysInfo();
    }
}

void HmiUartBridge::end() {
    if (s_active_ == this) {
        s_active_ = nullptr;
    }
    ready_ = false;
    lineLen_ = 0;
    pendingRestart_ = false;
}

void HmiUartBridge::emitJson(const JsonDocument& doc) {
    serializeJson(doc, HmiSerial);
    HmiSerial.print('\n');
    HmiSerial.flush();
    Serial.print("[HMI UART TX] ");
    serializeJson(doc, Serial);
    Serial.println();
}

void HmiUartBridge::sendCmdAck(const char* action, bool ok) {
    StaticJsonDocument<kJsonCapacity> doc;
    doc["t"] = "cmd_ack";
    doc["action"] = action ? action : "";
    doc["ok"] = ok;
    doc["commandId"] = ++commandId_;
    emitJson(doc);
}

void HmiUartBridge::sendSysInfo() {
    /* Snapshot de provisión SoftAP/NVS — HMI precarga wizard si Master ya tiene WiFi/perfil. */
    String ssid;
    String password;
    String email;
    String deviceName;
    String location;
    bool hasWifi = false;
    Preferences prefs;
    if (prefs.begin("hydro_system", true)) {
        ssid = prefs.getString("ssid", "");
        password = prefs.getString("password", "");
        email = prefs.getString("user_email", "");
        deviceName = prefs.getString("device_name", "");
        location = prefs.getString("location", "");
        prefs.end();
        hasWifi = ssid.length() > 0;
    }

    StaticJsonDocument<kJsonCapacity> doc;
    doc["t"] = "sys_info";
    if (ctx_.deviceIdFn) {
        doc["device_id"] = ctx_.deviceIdFn();
    } else {
        doc["device_id"] = "";
    }
    const bool cloudOk = ctx_.cloudOkFn ? ctx_.cloudOkFn() : false;
    doc["cloud_ok"] = cloudOk;
    doc["process_bridge"] = true;
    doc["has_wifi"] = hasWifi;
    doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
    if (hasWifi) {
        doc["ssid"] = ssid;
        doc["password"] = password;
    }
    if (email.length() > 0) {
        doc["email"] = email;
    }
    if (deviceName.length() > 0) {
        doc["device_name"] = deviceName;
    }
    if (location.length() > 0) {
        doc["location"] = location;
    }

    serializeJson(doc, HmiSerial);
    HmiSerial.print('\n');
    /* USB: no volcar password. */
    Serial.print("[HMI UART TX] ");
    Serial.printf(
        "{\"t\":\"sys_info\",\"device_id\":\"%s\",\"cloud_ok\":%s,\"has_wifi\":%s,"
        "\"wifi_connected\":%s,\"ssid\":\"%s\",\"password\":\"%s\"",
        ctx_.deviceIdFn ? ctx_.deviceIdFn().c_str() : "", cloudOk ? "true" : "false",
        hasWifi ? "true" : "false", (WiFi.status() == WL_CONNECTED) ? "true" : "false",
        ssid.c_str(), hasWifi ? "***" : "");
    if (email.length() > 0) {
        Serial.printf(",\"email\":\"%s\"", email.c_str());
    }
    if (deviceName.length() > 0) {
        Serial.printf(",\"device_name\":\"%s\"", deviceName.c_str());
    }
    if (location.length() > 0) {
        Serial.printf(",\"location\":\"%s\"", location.c_str());
    }
    Serial.println(",\"process_bridge\":true}");
}

void HmiUartBridge::notifyRelayState(const uint8_t mac[6], int relay, bool on) {
    if (!ready_ || !mac || relay < 0 || relay > 7) {
        return;
    }
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    StaticJsonDocument<96> doc;
    doc["t"] = "relay";
    doc["mac"] = macStr;
    doc["relay"] = relay;
    doc["on"] = on ? 1 : 0;
    emitJson(doc);
}

void HmiUartBridge::sendSlavesList() {
    /* Snapshot primero (mutex corto): JSON + rule-lock fuera del lock ESP-NOW.
     * Capacidad dedicada: Master+8 + varios Atlas+8 no caben fiable en kJsonCapacity=1536. */
    static const size_t kSlavesJsonCapacity = 4096;
    static const size_t kMaxSnap = 6;
    struct SlaveSnap {
        char mac[18];
        char name[32];
        bool online;
        uint8_t numRelays;
        bool relayOn[8];
    };
    SlaveSnap snaps[kMaxSnap];
    size_t snapCount = 0;
    bool mutexOk = false;

    if (ctx_.masterManager) {
        mutexOk = ctx_.masterManager->forEachTrustedSlave([&](const TrustedSlave& slave) {
            if (snapCount >= kMaxSnap) {
                return;
            }
            SlaveSnap& s = snaps[snapCount];
            memset(&s, 0, sizeof(s));
            snprintf(s.mac, sizeof(s.mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     slave.macAddress[0], slave.macAddress[1], slave.macAddress[2],
                     slave.macAddress[3], slave.macAddress[4], slave.macAddress[5]);
            strncpy(s.name, slave.deviceName.c_str(), sizeof(s.name) - 1);
            /* Ping/pong (status ONLINE). lastSeen de 120 s marcaba Atlas apagados. */
            s.online = slave.isOnline();
            const int nr = slave.numRelays > 0 ? slave.numRelays : 8;
            s.numRelays = static_cast<uint8_t>(nr > 8 ? 8 : nr);
            for (uint8_t i = 0; i < s.numRelays; ++i) {
                s.relayOn[i] = slave.relayStates[i].state;
            }
            ++snapCount;
        });
        if (!mutexOk) {
            Serial.println("[HMI UART] slaves_req — mutex trustedSlaves timeout (lista incompleta)");
        } else if (snapCount == 0) {
            const int trusted = ctx_.masterManager->getTrustedSlaveCount();
            if (trusted > 0) {
                Serial.printf("[HMI UART] slaves_req — trusted=%d pero snapshot vacío\n", trusted);
            }
        }
    } else {
        Serial.println("[HMI UART] slaves_req SIN gestor — late-bind no aplicado");
    }

    bool localOn[8] = {};
    if (ctx_.hydro) {
        const bool* rs = ctx_.hydro->getRelayStates();
        if (rs) {
            for (int i = 0; i < 8; ++i) {
                localOn[i] = rs[i];
            }
        }
    }

    /* BSS, no pila de loopTask: 4096 B en stack reventó el canary al armar t:slaves. */
    static StaticJsonDocument<kSlavesJsonCapacity> doc;
    doc.clear();
    doc["t"] = "slaves";
    JsonArray arr = doc.createNestedArray("slaves");

    auto fillRelays = [&](JsonObject o, const char* macStr, int numRelays, bool isLocal,
                          const bool* onStates) {
        JsonArray relays = o.createNestedArray("relays");
        for (int i = 0; i < numRelays; ++i) {
            JsonObject r = relays.createNestedObject();
            if (r.isNull()) {
                Serial.printf("[HMI UART] slaves_req JSON overflow mid-relays mac=%s i=%d\n",
                              macStr ? macStr : "?", i);
                return;
            }
            r["on"] = (onStates && onStates[i]) ? 1 : 0;
            char reason[16] = {};
            char label[24] = {};
            const bool locked =
                !isLocal && isSlaveRelayAutomationLocked(macStr, i, reason, sizeof(reason),
                                                         label, sizeof(label));
            r["locked"] = locked;
            if (locked) {
                if (reason[0]) {
                    r["lock_reason"] = reason;
                }
                if (label[0]) {
                    r["lock_label"] = label;
                }
            }
        }
    };

    JsonObject local = arr.createNestedObject();
    local["mac"] = "local";
    local["name"] = "Master";
    local["local"] = true;
    local["online"] = true;
    local["numRelays"] = 8;
    fillRelays(local, "local", 8, true, localOn);

    for (size_t i = 0; i < snapCount; ++i) {
        JsonObject o = arr.createNestedObject();
        if (o.isNull()) {
            Serial.printf("[HMI UART] slaves_req JSON overflow at Atlas[%u] mac=%s\n",
                          static_cast<unsigned>(i), snaps[i].mac);
            break;
        }
        o["mac"] = snaps[i].mac;
        o["name"] = snaps[i].name;
        o["local"] = false;
        o["online"] = snaps[i].online;
        o["numRelays"] = snaps[i].numRelays;
        fillRelays(o, snaps[i].mac, snaps[i].numRelays, false, snaps[i].relayOn);
    }

    if (doc.overflowed()) {
        Serial.println("[HMI UART] slaves_req — JsonDocument overflowed");
    }
    const int trusted = ctx_.masterManager ? ctx_.masterManager->getTrustedSlaveCount() : -1;
    Serial.printf("[HMI UART] slaves_req atlas=%u trusted=%d mutex=%d\n",
                  static_cast<unsigned>(snapCount), trusted, mutexOk ? 1 : 0);
    emitJson(doc);
}

bool HmiUartBridge::isSlaveRelayAutomationLocked(const char* macStr, int relay,
                                                 char* reasonOut, size_t reasonLen,
                                                 char* labelOut, size_t labelLen) const {
    if (reasonOut && reasonLen) {
        reasonOut[0] = '\0';
    }
    if (labelOut && labelLen) {
        labelOut[0] = '\0';
    }
    if (!macStr || !macStr[0] || relay < 0 || relay >= 8) {
        return false;
    }
    if (strcmp(macStr, "local") == 0) {
        return false;
    }

    auto setOut = [&](const char* reason, const char* label) {
        if (reasonOut && reasonLen && reason) {
            strncpy(reasonOut, reason, reasonLen - 1);
            reasonOut[reasonLen - 1] = '\0';
        }
        if (labelOut && labelLen && label) {
            strncpy(labelOut, label, labelLen - 1);
            labelOut[labelLen - 1] = '\0';
        }
    };

    auto macMatch = [&](const String& deviceId) -> bool {
        if (deviceId.length() == 0) {
            return false;
        }
        String a = deviceId;
        a.toUpperCase();
        String b = String(macStr);
        b.toUpperCase();
        a.replace("-", ":");
        b.replace("-", ":");
        return a == b || a.indexOf(b) >= 0 || b.indexOf(a) >= 0;
    };

    if (ctx_.decisionEngine) {
        for (const DecisionRule& rule : ctx_.decisionEngine->getAllRules()) {
            if (!rule.enabled) {
                continue;
            }
            for (const RuleAction& act : rule.actions) {
                if (act.target_relay != relay) {
                    continue;
                }
                if (!macMatch(act.target_device_id)) {
                    continue;
                }
                setOut("rule", rule.name.length() ? rule.name.c_str() : rule.id.c_str());
                return true;
            }
        }
    }

    /* Ciclo Auto EC/pH activo: bloquear tipagem circ si la regra fn_* apunta a este MAC+relay. */
    if (ctx_.hydro && ctx_.decisionEngine) {
        const char* ecSt = ctx_.hydro->getEcOperationStateName();
        const char* phSt = ctx_.hydro->getPhOperationStateName();
        const bool ecBusy = ecSt && (strcmp(ecSt, "dosing") == 0 || strcmp(ecSt, "recirculating") == 0 ||
                                     strncmp(ecSt, "diluting", 8) == 0);
        const bool phBusy = phSt && (strcmp(phSt, "dosing") == 0 || strcmp(phSt, "recirculating") == 0 ||
                                     strncmp(phSt, "diluting", 8) == 0);
        if (ecBusy || phBusy) {
            for (const DecisionRule& rule : ctx_.decisionEngine->getAllRules()) {
                if (!rule.enabled) {
                    continue;
                }
                if (rule.id.indexOf("recirc") < 0 && rule.id.indexOf("fn_") < 0 &&
                    rule.id.indexOf("circ") < 0) {
                    continue;
                }
                for (const RuleAction& act : rule.actions) {
                    if (act.target_relay == relay && macMatch(act.target_device_id)) {
                        setOut(ecBusy ? "auto_ec" : "auto_ph",
                               ecBusy ? "Auto EC" : "Auto pH");
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void HmiUartBridge::publishTelemetryNow() {
    if (!ctx_.hydro) {
        return;
    }
    HydroControl& hydro = *ctx_.hydro;
    StaticJsonDocument<kTelemetryJsonCapacity> doc;
    doc["t"] = "telemetry";
    const bool phOk = hydro.isPhValidForTelemetry();
    const bool ecOk = hydro.isEcValidForTelemetry();
    const bool tempOk = hydro.isTempValidForTelemetry();
    doc["ph_valid"] = phOk;
    doc["ec_valid"] = ecOk;
    doc["temp_valid"] = tempOk;
    if (phOk) {
        doc["ph"] = hydro.getpH();
    }
    if (ecOk) {
        doc["ec"] = hydro.getEC();
    }
    const float temp = hydro.getWaterTemp();
    if (isfinite(temp)) {
        doc["temp_agua"] = temp;
    } else     if (tempOk) {
        doc["temp_agua"] = hydro.getTemperature();
    }
    if (hydro.consumoEc24hOpen() && isfinite(hydro.consumoEc24hRef())) {
        doc["ec_t0"] = hydro.consumoEc24hRef();
        doc["ec_el"] = hydro.consumoEc24hElapsedSec();
    }
    if (hydro.hasConsumoEc24hDelta()) {
        doc["deriv_ec"] = hydro.consumoEc24hDelta();
    }
    if (hydro.consumoPh24hOpen() && isfinite(hydro.consumoPh24hRef())) {
        doc["ph_t0"] = hydro.consumoPh24hRef();
        doc["ph_el"] = hydro.consumoPh24hElapsedSec();
    }
    if (hydro.hasConsumoPh24hDelta()) {
        doc["deriv_ph"] = hydro.consumoPh24hDelta();
    }
    doc["ilock"] = hydro.getLevelInterlockModeName();
    const char* ecOp = hydro.getEcOperationStateName();
    const char* phOp = hydro.getPhOperationStateName();
    const bool dosingNow =
        (ecOp && strcmp(ecOp, "dosing") == 0) || (phOp && strcmp(phOp, "dosing") == 0);
    doc["dosing"] = dosingNow ? 1 : 0;
    doc["autoEc"] = hydro.isAutoECEnabled();
    doc["autoPh"] = hydro.isAutoPHEnabled();
    doc["wifi_off"] = stateManager.wifiUserOffline() ? 1 : 0;
    const size_t n = serializeJson(doc, lastTelemetryJson_, sizeof(lastTelemetryJson_));
    lastTelemetryValid_ = (n > 0 && n < sizeof(lastTelemetryJson_));
    if (!lastTelemetryValid_) {
        lastTelemetryJson_[0] = '\0';
    }
    emitJson(doc);
}

void HmiUartBridge::publishPlantCfg() {
    if (!ready_ || !ctx_.hydro) {
        return;
    }
    HydroControl& hydro = *ctx_.hydro;
    String nuts;
    if (!hydro.buildNutrientsJsonForCloud(nuts)) {
        nuts = "[]";
    }
    DynamicJsonDocument src(1536);
    if (deserializeJson(src, nuts)) {
        return;
    }
    DynamicJsonDocument doc(1536);
    doc["t"] = "plant_cfg";
    JsonArray pumps = doc.createNestedArray("pumps");
    int n = 0;
    bool seen[6] = {false, false, false, false, false, false};
    for (JsonObject o : src.as<JsonArray>()) {
        const int relay0 = o["relay"] | -1;
        if (relay0 < 0 || relay0 >= 6) {
            continue;
        }
        seen[relay0] = true;
        JsonObject p = pumps.createNestedObject();
        p["relay"] = relay0 + 1;
        p["name"] = o["name"] | "";
        p["mlPerLiter"] = o["mlPerLiter"] | 0.0f;
        const float q = o["flowRate"] | 0.0f;
        if (q > 0.01f) {
            p["flowMlPerMin"] = q * 60.0f;
        }
        ++n;
    }
    const int up = hydro.getRelayPhUp();
    const int down = hydro.getRelayPhDown();
    if (up >= 0 && up < 6 && !seen[up] && hydro.getFlowRatePhUp() > 0.01f) {
        JsonObject p = pumps.createNestedObject();
        p["relay"] = up + 1;
        p["name"] = "pH+";
        p["mlPerLiter"] = 0.0f;
        p["flowMlPerMin"] = hydro.getFlowRatePhUp() * 60.0f;
        ++n;
    }
    if (down >= 0 && down < 6 && !seen[down] && hydro.getFlowRatePhDown() > 0.01f) {
        JsonObject p = pumps.createNestedObject();
        p["relay"] = down + 1;
        p["name"] = "pH-";
        p["mlPerLiter"] = 0.0f;
        p["flowMlPerMin"] = hydro.getFlowRatePhDown() * 60.0f;
        ++n;
    }
    emitJson(doc);
    Serial.printf("[HMI PLANT] pumps=%d\n", n);
}

void HmiUartBridge::publishConfigHeartbeat() {
    StaticJsonDocument<kTelemetryJsonCapacity> doc;
    doc["t"] = "telemetry";
    doc["ph_valid"] = false;
    doc["ec_valid"] = false;
    doc["temp_valid"] = false;
    const size_t n = serializeJson(doc, lastTelemetryJson_, sizeof(lastTelemetryJson_));
    lastTelemetryValid_ = (n > 0 && n < sizeof(lastTelemetryJson_));
    if (!lastTelemetryValid_) {
        lastTelemetryJson_[0] = '\0';
        return;
    }
    /* Solo HMI UART — no spamear USB Serial cada 2s en SoftAP. */
    HmiSerial.print(lastTelemetryJson_);
    HmiSerial.print('\n');
}

void HmiUartBridge::maybePublishTelemetry(unsigned long nowMs) {
    if (!ready_) {
        return;
    }
    if (nowMs - lastTelemetryMs_ >= HMI_TELEMETRY_INTERVAL_MS) {
        if (ctx_.hydro) {
            publishTelemetryNow();
        } else {
            /* WIFI_CONFIG_MODE: sin sensores — heartbeat para MasterLink. */
            publishConfigHeartbeat();
        }
        lastTelemetryMs_ = nowMs;
    }
}

int HmiUartBridge::parseRelayChannel(const char* channel) {
    if (!channel || channel[0] == '\0') {
        return -1;
    }
    if ((channel[0] == 'R' || channel[0] == 'r') && channel[1] >= '1' && channel[1] <= '8') {
        if (channel[2] == '\0') {
            return channel[1] - '1';
        }
    }
    if (strncmp(channel, "bomba", 5) == 0) {
        const char* n = channel + 5;
        if (*n >= '1' && *n <= '8' && n[1] == '\0') {
            return *n - '1';
        }
    }
    return -1;
}

bool HmiUartBridge::parseMacString(const char* macStr, uint8_t macOut[6]) {
    if (!macStr || !macOut) {
        return false;
    }
    if (strcmp(macStr, "local") == 0 || strcmp(macStr, "atlas") == 0) {
        return false;
    }
    unsigned int b[6];
    if (sscanf(macStr, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        macOut[i] = static_cast<uint8_t>(b[i]);
    }
    return true;
}

void HmiUartBridge::applyRecipeGain(HydroControl& hydro, float baseDose, float totalMl) {
    ECController& ec = hydro.getECController();
    if (baseDose > 0.0f) {
        ec.setBaseDose(baseDose);
    }
    if (totalMl > 0.0f) {
        ec.setTotalMl(totalMl);
    }
    if (baseDose > 0.0f && totalMl > 0.0f) {
        ec.setLearnedK(0.0f);
        hydro.saveECControllerConfig();
        Serial.printf("[HMI UART] recipe k = %.4f (base=%.0f totalMl=%.2f)\n",
                      baseDose / totalMl, baseDose, totalMl);
    }
}

bool HmiUartBridge::applyDeadbandFromLimits(HydroControl& hydro, float lo, float hi, bool isEc) {
    if (!(hi > lo)) {
        Serial.printf("[HMI UART] deadband inválida %s lo=%.2f hi=%.2f\n",
                      isEc ? "EC" : "pH", lo, hi);
        return false;
    }
    if (isEc) {
        const float tol = max(1.0f, (hi - lo) / 2.0f);
        hydro.setECTolerance(tol, false);
    } else {
        const float tol = max(0.01f, (hi - lo) / 2.0f);
        hydro.setPHTolerance(tol);
    }
    return true;
}

bool HmiUartBridge::handleDose(JsonDocument& doc, const char* action) {
    if (!ctx_.hydro || !ctx_.coordinator) {
        return false;
    }
    const char* channel = doc["channel"] | "";
    const int relay = parseRelayChannel(channel);
    if (relay < 0 || relay >= 8) {
        Serial.printf("[HMI UART] dose canal inválido: %s\n", channel);
        return false;
    }

    if (strcmp(action, "dose_stop") == 0) {
        return ctx_.coordinator->actuateLocal(RelayOwner::Manual, relay, "off", 0);
    }
    if (strcmp(action, "dose_hold") == 0) {
        const bool on = (doc["on"] | 0) != 0;
        return ctx_.hydro->setRelay(relay, on, 0);
    }

    const float ml = doc["ml"] | 0.0f;
    if (!(ml > 0.0f) || !isfinite(ml)) {
        Serial.println("[HMI UART] dose ml inválido");
        return false;
    }
    float flow = ctx_.hydro->getFlowRateMlPerSecForRelay(relay);
    if (flow <= 0.01f) {
        flow = ctx_.hydro->getECController().getFlowRate();
    }
    if (flow <= 0.01f) {
        Serial.printf("[HMI UART] dose R%d sem flowRate calibrado\n", relay + 1);
        return false;
    }
    const int durationSec = max(1, (int)ceilf(ml / flow));
    Serial.printf("[HMI UART] dose %s %.2f ml q=%.3f ml/s → %ds\n",
                  channel, ml, flow, durationSec);
    return ctx_.coordinator->actuateLocal(RelayOwner::Manual, relay, "on", durationSec);
}

bool HmiUartBridge::handleNutrientProportions(JsonDocument& doc) {
    if (!ctx_.hydro) {
        return false;
    }
    JsonArray nutrients = doc["nutrients"].as<JsonArray>();
    if (!nutrients.isNull()) {
        ctx_.hydro->updateNutrientProportions(nutrients);
    }
    const float totalMl = doc["totalMlPerLiter"] | 0.0f;
    float baseDose = doc["baseDose"] | 0.0f;
    if (baseDose <= 0.0f) {
        baseDose = doc["recipeEcUs"] | 0.0f;
    }
    applyRecipeGain(*ctx_.hydro, baseDose, totalMl);
    syncPumpFlowToCloud(1);
    publishPlantCfg();
    return true;
}

bool HmiUartBridge::handleLoopControl(JsonDocument& doc) {
    if (!ctx_.hydro) {
        return false;
    }
    HydroControl& hydro = *ctx_.hydro;
    bool ecPersist = false;

    if (doc.containsKey("volumeL")) {
        const float vol = doc["volumeL"];
        if (vol > 0.0f) {
            hydro.getECController().setVolume(vol);
            ecPersist = true;
        }
    }
    if (doc.containsKey("homoSec")) {
        hydro.setTempoRecirculacaoSeconds((unsigned long)(doc["homoSec"] | 60));
    }
    if (doc.containsKey("pulseMl") || doc.containsKey("pulseGapSec")) {
        hydro.setEcPulseDosing(doc["pulseMl"] | 2.0f, doc["pulseGapSec"] | 2.0f);
    }
    if (doc.containsKey("autoEcIntervalSec")) {
        hydro.setAutoECInterval((int)(doc["autoEcIntervalSec"] | 30), false);
        ecPersist = true;
    }
    if (doc.containsKey("autoPhIntervalSec")) {
        hydro.setAutoPHInterval((int)(doc["autoPhIntervalSec"] | 300), true);
    }
    const bool pushedEc = doc.containsKey("autoEc");
    const bool pushedPh = doc.containsKey("autoPh");
    if (pushedEc) {
        hydro.setAutoECEnabled(doc["autoEc"] | false, false);
        ecPersist = true;
    }
    if (pushedPh) {
        hydro.setAutoPHEnabled(doc["autoPh"] | false, true);
    }
    if ((pushedEc || pushedPh) && ctx_.mqtt) {
        ctx_.mqtt->publishAutoSwitch(
            pushedEc, hydro.isAutoECEnabled(), pushedPh, hydro.isAutoPHEnabled());
    }
    if (doc.containsKey("maxStepEc")) {
        hydro.setMaxStepEcFraction(doc["maxStepEc"] | 0.5f);
    }
    if (doc.containsKey("maxStepPh")) {
        hydro.setPhAdaptiveConfig(doc["maxStepPh"] | 0.5f, 0.1f);
    }
    if (doc.containsKey("consumoDiario")) {
        hydro.setConsumoEc24hEnabled(doc["consumoDiario"] | false);
    }
    if (doc.containsKey("consumoPh24h")) {
        hydro.setConsumoPh24hEnabled(doc["consumoPh24h"] | false);
    }
    bool ecBandOk = false;
    bool phBandOk = false;
    if (doc.containsKey("ecLo") && doc.containsKey("ecHi")) {
        ecBandOk = applyDeadbandFromLimits(hydro, doc["ecLo"], doc["ecHi"], true);
        if (ecBandOk) {
            ecPersist = true;
        }
    }
    if (doc.containsKey("phLo") && doc.containsKey("phHi")) {
        phBandOk = applyDeadbandFromLimits(hydro, doc["phLo"], doc["phHi"], false);
    }
    if (ecPersist) {
        hydro.saveECControllerConfig();
    }
    if (doc.containsKey("phUpRelay") || doc.containsKey("phDownRelay")) {
        const int prevUp = hydro.getRelayPhUp();
        const int prevDown = hydro.getRelayPhDown();
        auto fromHmi = [](int raw) { return (raw > 0 && raw <= 8) ? (raw - 1) : -1; };
        int up = prevUp;
        int down = prevDown;
        if (doc.containsKey("phUpRelay")) {
            up = fromHmi(doc["phUpRelay"] | 0);
        }
        if (doc.containsKey("phDownRelay")) {
            down = fromHmi(doc["phDownRelay"] | 0);
        }
        if (up >= 0 && up == down) {
            Serial.println("[HMI LOOP] pH relays iguales — sin cambio");
        } else {
            hydro.setPhPumpRelays(up, down);
            int release0 = -1;
            int release1 = -1;
            auto noteRelease = [&](int prev) {
                if (prev < 0 || prev > 7 || prev == up || prev == down) {
                    return;
                }
                if (release0 < 0) {
                    release0 = prev;
                } else if (prev != release0) {
                    release1 = prev;
                }
            };
            noteRelease(prevUp);
            noteRelease(prevDown);
            if (ctx_.mqtt) {
                ctx_.mqtt->publishPhFlow(
                    hydro.getFlowRatePhUp(), hydro.getFlowRatePhDown(), up, down, release0, release1);
            }
        }
    }

    char ignored[80];
    size_t ignoredLen = 0;
    ignored[0] = '\0';
    auto appendIgnored = [&](const char* token) {
        if (ignoredLen >= sizeof(ignored)) {
            return;
        }
        const int wrote = snprintf(ignored + ignoredLen, sizeof(ignored) - ignoredLen, "%s", token);
        if (wrote > 0) {
            ignoredLen += static_cast<size_t>(wrote);
        }
    };
    if (doc.containsKey("dosingArmed")) {
        appendIgnored(" armed=ignorado");
    }
    if (doc.containsKey("dosingDelaySec") || doc.containsKey("dosingMode")) {
        appendIgnored(" delay/mode=ignorado");
    }
    if (doc.containsKey("nutrientGapSec")) {
        appendIgnored(" nutGap=no-uart");
    }

    char ecBand[24];
    char phBand[24];
    if (ecBandOk) {
        snprintf(ecBand, sizeof(ecBand), "%.0f-%.0f", (float)doc["ecLo"], (float)doc["ecHi"]);
    } else {
        snprintf(ecBand, sizeof(ecBand), "-");
    }
    if (phBandOk) {
        snprintf(phBand, sizeof(phBand), "%.2f-%.2f", (float)doc["phLo"], (float)doc["phHi"]);
    } else {
        snprintf(phBand, sizeof(phBand), "-");
    }

    Serial.printf(
        "[HMI LOOP] autoEc=%d iv=%d autoPh=%d iv=%d vol=%.0f pulse=%.1f/%.1fs "
        "maxStepEc=%.2f maxStepPh=%.2f ecBand=%s phBand=%s%s\n",
        hydro.isAutoECEnabled() ? 1 : 0,
        hydro.getAutoECInterval(),
        hydro.isAutoPHEnabled() ? 1 : 0,
        hydro.getAutoPHInterval(),
        hydro.getECController().getVolume(),
        hydro.getEcPulseMl(),
        hydro.getEcPulseGapSec(),
        hydro.getMaxStepEcFraction(),
        hydro.getPhAggressiveness(),
        ecBand,
        phBand,
        ignored);
    return true;
}

bool HmiUartBridge::handleSetpoint(JsonDocument& doc) {
    if (!ctx_.hydro) {
        return false;
    }
    bool ok = false;
    if (doc.containsKey("ec")) {
        ctx_.hydro->setECSetpoint(doc["ec"], true);
        ok = true;
    }
    if (doc.containsKey("ph")) {
        ctx_.hydro->setPHSetpoint(doc["ph"], true);
        ok = true;
    }
    return ok;
}

bool HmiUartBridge::handleRelayLocal(JsonDocument& doc) {
    if (!ctx_.coordinator) {
        return false;
    }
    const int relay = doc["relay"] | -1;
    const char* state = doc["state"] | "off";
    const int duration = doc["duration"] | 0;
    if (relay < 0 || relay >= 8) {
        return false;
    }
    return ctx_.coordinator->actuateLocal(RelayOwner::Manual, relay, state, duration);
}

bool HmiUartBridge::handleRelaySlave(JsonDocument& doc) {
    if (!ctx_.coordinator) {
        return false;
    }
    const char* macStr = doc["mac"] | "";
    uint8_t mac[6];
    if (!parseMacString(macStr, mac)) {
        Serial.printf("[HMI UART] relay_slave MAC inválido: %s\n", macStr);
        return false;
    }
    const int relay = doc["relay"] | -1;
    const char* state = doc["state"] | "off";
    const int duration = doc["duration"] | 0;
    const int cycleOff = doc["cycleOff"] | 0;
    const char* mode = doc["mode"] | "";
    if (mode[0] == '\0' &&
        (strcmp(state, "cycle") == 0 || strcmp(state, "cycle_stop") == 0)) {
        mode = state;
    }
    if (relay < 0 || relay >= 8) {
        return false;
    }
    char reason[16] = {};
    char label[24] = {};
    if (isSlaveRelayAutomationLocked(macStr, relay, reason, sizeof(reason), label, sizeof(label))) {
        Serial.printf("[HMI UART] relay_slave DENY locked mac=%s R%d reason=%s label=%s\n",
                      macStr, relay, reason, label);
        return false;
    }
    const uint32_t cmdId = ctx_.coordinator->actuateSlave(
        RelayOwner::Manual, mac, relay, state, duration, 0, cycleOff, mode);
    if (cmdId != 0 && ctx_.masterManager) {
        if (strcmp(mode, "cycle") == 0 && duration > 0 && cycleOff > 0) {
            ctx_.masterManager->rememberSlaveCycle(mac, relay, duration, cycleOff);
        } else if (strcmp(mode, "cycle_stop") == 0) {
            ctx_.masterManager->forgetSlaveCycle(mac, relay);
        }
    }
    return cmdId != 0;
}

bool HmiUartBridge::handleCalib(JsonDocument& doc) {
    const char* param = doc["param"] | "";
    const float point = doc["point"] | NAN;
    Serial.printf("[HMI UART] calib stub param=%s point=%.3f\n", param, point);
    return true;
}

bool HmiUartBridge::handlePumpFlowCalib(JsonDocument& doc) {
    if (!ctx_.hydro) {
        return false;
    }
    const char* channel = doc["channel"] | "";
    const int relay = parseRelayChannel(channel);
    float flowMlPerMin = doc["flowMlPerMin"] | 0.0f;
    if (flowMlPerMin <= 0.01f && doc.containsKey("measuredMl") && doc.containsKey("durationSec")) {
        const float measured = doc["measuredMl"] | 0.0f;
        const float dur = doc["durationSec"] | 0.0f;
        if (measured > 0.01f && dur > 0.01f) {
            flowMlPerMin = measured * (60.0f / dur);
        }
    }
    if (relay < 0 || relay >= 6 || flowMlPerMin <= 0.01f) {
        Serial.println("[HMI UART] pump_flow_calib inválido");
        return false;
    }
    const float flowMlPerS = flowMlPerMin / 60.0f;
    const int target = ctx_.hydro->applyPumpFlowCalib(relay, flowMlPerS);
    if (target == 0) {
        return false;
    }
    syncPumpFlowToCloud(target);
    publishPlantCfg();
    return true;
}

static String activeRecipeJson(HydroControl& hydro) {
    String raw;
    if (!hydro.buildNutrientsJsonForCloud(raw)) {
        return "[]";
    }
    DynamicJsonDocument src(1536);
    if (deserializeJson(src, raw) || !src.is<JsonArray>()) {
        return "[]";
    }
    DynamicJsonDocument out(1536);
    JsonArray arr = out.to<JsonArray>();
    for (JsonObject o : src.as<JsonArray>()) {
        const char* name = o["name"] | "";
        if (!name[0] || strncmp(name, "pump_r", 6) == 0) {
            continue;
        }
        const float ml = o["mlPerLiter"] | 0.0f;
        const bool active = o["active"] | false;
        const float q = o["flowRate"] | 0.0f;
        if (!active || ml <= 0.05f) {
            JsonObject p = arr.createNestedObject();
            p["name"] = name;
            p["relay"] = o["relay"] | 0;
            p["mlPerLiter"] = 0;
            p["active"] = false;
            if (q > 0.01f) {
                p["flowRate"] = q;
            }
            continue;
        }
        JsonObject p = arr.createNestedObject();
        p["name"] = name;
        p["relay"] = o["relay"] | 0;
        p["mlPerLiter"] = ml;
        p["active"] = true;
        if (q > 0.01f) {
            p["flowRate"] = q;
        }
    }
    String result;
    serializeJson(arr, result);
    if (result.length() == 0) {
        return "[]";
    }
    return result;
}

void HmiUartBridge::syncPumpFlowToCloud(int target) {
    if (!ctx_.mqtt || !ctx_.hydro) {
        Serial.println("[MQTT] ph_flow publish failed");
        return;
    }
    if (target != 2) {
        const String nutrientsJson = activeRecipeJson(*ctx_.hydro);
        ctx_.mqtt->publishEcNutrients(nutrientsJson.c_str());
    }
    ctx_.mqtt->publishPhFlow(
        ctx_.hydro->getFlowRatePhUp(),
        ctx_.hydro->getFlowRatePhDown(),
        ctx_.hydro->getRelayPhUp(),
        ctx_.hydro->getRelayPhDown());
}

bool HmiUartBridge::handlePhDeltaGain(JsonDocument& doc) {
    if (!ctx_.hydro) {
        return false;
    }
    const char* side = doc["side"] | "";
    const float ml = doc["mlPerPh"] | 0.0f;
    const bool acid = strcmp(side, "down") == 0;
    const bool base = strcmp(side, "up") == 0;
    if ((!acid && !base) || !ctx_.hydro->applyPhMlPerUnit(acid, ml)) {
        Serial.println("[HMI UART] ph_delta_gain inválido");
        return false;
    }
    if (ctx_.mqtt) {
        ctx_.mqtt->publishPhMl(acid, ml);
    }
    return true;
}

bool HmiUartBridge::handleWifiConfig(JsonDocument& doc) {
    /* Mismo NVS que SoftAP (hydro_system) — vía B = vía A. */
    const char* ssid = doc["ssid"] | "";
    const char* password = doc["password"] | "";
    const char* deviceName = doc["device_name"] | "";
    const char* email = doc["email"] | "";
    const char* location = doc["location"] | "";

    StaticJsonDocument<256> ack;
    ack["t"] = "wifi_config_ack";
    if (ctx_.deviceIdFn) {
        ack["device_id"] = ctx_.deviceIdFn();
    }

    if (!ssid[0]) {
        Serial.println("[HMI UART] wifi_config inválido — ssid vacío");
        ack["ok"] = false;
        emitJson(ack);
        return false;
    }

    Preferences prefs;
    if (!prefs.begin("hydro_system", false)) {
        Serial.println("[HMI UART] wifi_config — no se pudo abrir NVS hydro_system");
        ack["ok"] = false;
        emitJson(ack);
        return false;
    }
    const size_t ssidBytes = prefs.putString("ssid", ssid);
    prefs.putString("password", password ? password : "");
    if (deviceName[0]) {
        prefs.putString("device_name", deviceName);
    }
    if (email[0]) {
        prefs.putString("user_email", email);
    }
    if (location[0]) {
        prefs.putString("location", location);
    }
    prefs.end();

    if (ssidBytes == 0) {
        Serial.println("[HMI UART] wifi_config — fallo al guardar ssid");
        ack["ok"] = false;
        emitJson(ack);
        return false;
    }

    /* Mirror wifi_creds (ESP-NOW broadcast de creds). */
    uint8_t ch = WiFi.channel();
    if (ch < 1 || ch > 13) {
        ch = 1;
    }
    WiFiCredentialsManager().saveCredentials(String(ssid), String(password ? password : ""), ch);

    ack["ok"] = true;
    ack["will_restart"] = true;
    emitJson(ack);

    /* Igual SoftAP: NVS listo → reinicio para salir de WIFI_CONFIG_MODE y conectar STA. */
    Serial.printf("[HMI UART] wifi_config NVS ok ssid=%s — reboot ~2.5s (parity SoftAP)\n", ssid);
    scheduleRestart(2500);
    return true;
}

void HmiUartBridge::scheduleRestart(unsigned long delayMs) {
    pendingRestart_ = true;
    pendingRestartAtMs_ = millis() + delayMs;
}

/** Suelta SDA/SCL antes del reinicio por software. El botón EN hace esto al cortar el chip; ESP.restart() solo no. */
static void releaseI2cBeforeRestart() {
    Wire.end();
    pinMode(I2C_SDA, INPUT);
    pinMode(I2C_SCL, INPUT);
    delay(50);
}

bool HmiUartBridge::handleMasterReboot() {
    Serial.println("[HMI UART] master_reboot — reinicio en ~600ms");
    scheduleRestart(600);
    return true;
}

bool HmiUartBridge::handleFactoryReset() {
    /* Soft factory: hydro_system + wifi_creds + solo /rules.json. No formatea SPIFFS. */
    Serial.println("[HMI UART] factory_reset — limpiando hydro_system + wifi_creds + /rules.json");
    Preferences prefs;
    if (prefs.begin("hydro_system", false)) {
        prefs.clear();
        prefs.end();
    }
    WiFiCredentialsManager().clearCredentials();
    if (SPIFFS.begin(true)) {
        if (SPIFFS.exists("/rules.json")) {
            if (SPIFFS.remove("/rules.json")) {
                Serial.println("[HMI UART] factory_reset — /rules.json borrado");
            } else {
                Serial.println("[HMI UART] factory_reset — no se pudo borrar /rules.json");
            }
        }
    } else {
        Serial.println("[HMI UART] factory_reset — SPIFFS no montado, /rules.json no tocado");
    }
    WiFi.disconnect(true, true);
    scheduleRestart(500);
    return true;
}

namespace {

struct RuleStateSlot {
    bool used;
    char name[20];
    char level[24];
    bool enabled;
};

RuleStateSlot ruleStates_[6];

void copyField(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

}  // namespace

bool HmiUartBridge::handleRuleState(JsonDocument& doc) {
    const char *name = doc["name"] | "";
    const char *level = doc["level"] | "";
    const bool enabled = (doc["enabled"] | 0) != 0;
    if (!name || !name[0]) {
        return false;
    }
    int slot = -1;
    int freeSlot = -1;
    for (int i = 0; i < 6; ++i) {
        if (ruleStates_[i].used && strcmp(ruleStates_[i].name, name) == 0) {
            slot = i;
            break;
        }
        if (!ruleStates_[i].used && freeSlot < 0) {
            freeSlot = i;
        }
    }
    if (slot < 0) {
        slot = freeSlot;
    }
    if (slot < 0) {
        slot = 0;
    }
    ruleStates_[slot].used = true;
    copyField(ruleStates_[slot].name, sizeof(ruleStates_[slot].name), name);
    copyField(ruleStates_[slot].level, sizeof(ruleStates_[slot].level),
              (level && level[0]) ? level : "vacio");
    ruleStates_[slot].enabled = enabled;
    Serial.printf("[REGLA] %s nivel=%s estado=%s\n",
                  ruleStates_[slot].name,
                  ruleStates_[slot].level,
                  enabled ? "activa" : "parada");
    return true;
}

namespace {

struct StepLine {
    char text[64];
    char until[16];
    char mac[18];
    int relay;
};

struct RuleSnap {
    char level[32];
    StepLine steps[4];
    uint8_t n;
};

void copyClip(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

int readRelayNumber(JsonObject o) {
    if (o.isNull()) {
        return -1;
    }
    if (!o["relay_number"].isNull()) {
        if (o["relay_number"].is<const char *>()) {
            return String(o["relay_number"].as<const char *>()).toInt();
        }
        return o["relay_number"].as<int>();
    }
    if (!o["relay"].isNull()) {
        if (o["relay"].is<const char *>()) {
            return String(o["relay"].as<const char *>()).toInt();
        }
        return o["relay"].as<int>();
    }
    if (!o["relayIndex"].isNull()) {
        return o["relayIndex"].as<int>();
    }
    if (!o["relay_index"].isNull()) {
        return o["relay_index"].as<int>();
    }
    return -1;
}

void relayCaption(JsonObject o, int relay, char *dst, size_t n) {
    const char *mac = "";
    if (o["slave_mac"].is<const char *>()) {
        mac = o["slave_mac"].as<const char *>();
    } else if (o["target_device_id"].is<const char *>()) {
        mac = o["target_device_id"].as<const char *>();
    }
    const char *catalog = RelayNameCatalog::nameFor(mac, relay);
    if (catalog && catalog[0]) {
        copyClip(dst, n, catalog);
        return;
    }
    const char *label = "";
    if (o["label"].is<const char *>()) {
        label = o["label"].as<const char *>();
    }
    if ((!label || !label[0]) && o["name"].is<const char *>()) {
        label = o["name"].as<const char *>();
    }
    if ((!label || !label[0]) && o["relay_names"].is<JsonArray>()) {
        JsonVariant first = o["relay_names"][0];
        if (first.is<const char *>()) {
            label = first.as<const char *>();
        }
    }
    if (label && label[0]) {
        copyClip(dst, n, label);
        return;
    }
    if (relay >= 0 && relay <= 7) {
        snprintf(dst, n, "Relé %d", relay + 1);
    } else {
        snprintf(dst, n, "Relé");
    }
}

void readMac(JsonObject o, char *dst, size_t n) {
    const char *mac = "";
    if (!o.isNull()) {
        if (o["slave_mac"].is<const char *>()) {
            mac = o["slave_mac"].as<const char *>();
        } else if (o["target_device_id"].is<const char *>()) {
            mac = o["target_device_id"].as<const char *>();
        }
    }
    copyClip(dst, n, mac);
}

void pushStep(RuleSnap &snap, const char *name, const char *until, const char *mac, int relay) {
    if (snap.n >= 4 || !name || !name[0]) {
        return;
    }
    StepLine &s = snap.steps[snap.n];
    if (until && until[0]) {
        snprintf(s.text, sizeof(s.text), "%s ON -> OFF", name);
        copyClip(s.until, sizeof(s.until), until);
    } else {
        snprintf(s.text, sizeof(s.text), "%s ON", name);
        s.until[0] = '\0';
    }
    copyClip(s.mac, sizeof(s.mac), mac ? mac : "");
    s.relay = relay;
    snap.n++;
}

void joinLevel(RuleSnap &snap) {
    snap.level[0] = '\0';
    for (uint8_t i = 0; i < snap.n; ++i) {
        if (!snap.steps[i].until[0]) {
            continue;
        }
        const size_t len = strlen(snap.level);
        if (len > 0 && len + 1 < sizeof(snap.level)) {
            snap.level[len] = '+';
            snap.level[len + 1] = '\0';
        }
        strncat(snap.level, snap.steps[i].until, sizeof(snap.level) - strlen(snap.level) - 1);
    }
    if (!snap.level[0]) {
        copyClip(snap.level, sizeof(snap.level), "-");
    }
}

void stepsFromScript(const String &json, RuleSnap &snap) {
    if (json.length() == 0 || snap.n >= 4) {
        return;
    }
    DynamicJsonDocument doc(3072);
    if (deserializeJson(doc, json)) {
        return;
    }
    JsonArray instrs = doc["instructions"].as<JsonArray>();
    if (instrs.isNull()) {
        return;
    }
    for (JsonObject ij : instrs) {
        if (snap.n >= 4) {
            break;
        }
        String type = ij["type"] | "";
        type.toLowerCase();
        if (type == "while") {
            JsonObject cond = ij["condition"].as<JsonObject>();
            String until;
            if (!cond.isNull() && cond["value"].is<const char *>()) {
                until = cond["value"].as<const char *>();
            }
            until.toLowerCase();
            until.trim();
            JsonArray body = ij["body"].as<JsonArray>();
            int relay = -1;
            char name[40] = {};
            JsonObject picked;
            if (!body.isNull()) {
                for (JsonObject bi : body) {
                    JsonObject target = bi;
                    String bt = bi["type"] | "";
                    bt.toLowerCase();
                    if (bt == "if") {
                        JsonArray thenArr = bi["then"].as<JsonArray>();
                        if (!thenArr.isNull() && thenArr.size() > 0) {
                            target = thenArr[0].as<JsonObject>();
                        }
                    }
                    const int r = readRelayNumber(target);
                    if (r >= 0) {
                        relay = r;
                        picked = target;
                        relayCaption(target, r, name, sizeof(name));
                        break;
                    }
                }
            }
            if (relay >= 0) {
                char mac[18];
                readMac(picked, mac, sizeof(mac));
                pushStep(snap, name, until.c_str(), mac, relay);
            }
            continue;
        }
        JsonObject act = ij["actuator"].as<JsonObject>();
        JsonObject target = act.isNull() ? ij : act;
        const int relay = readRelayNumber(target);
        if (relay >= 0) {
            char name[40];
            relayCaption(target, relay, name, sizeof(name));
            const char *until = ij["until"] | "";
            char mac[18];
            readMac(target, mac, sizeof(mac));
            pushStep(snap, name, until, mac, relay);
        }
    }
}

void stepsFromFsm(const String &json, RuleSnap &snap) {
    if (json.length() == 0 || snap.n > 0) {
        return;
    }
    DynamicJsonDocument doc(1536);
    if (deserializeJson(doc, json)) {
        return;
    }
    const char *keys[2] = {"drain", "fill"};
    for (int i = 0; i < 2 && snap.n < 4; ++i) {
        JsonObject o = doc[keys[i]].as<JsonObject>();
        const int relay = readRelayNumber(o);
        if (relay < 0) {
            continue;
        }
        char name[40];
        relayCaption(o, relay, name, sizeof(name));
        const char *until = o["until"] | "";
        char mac[18];
        readMac(o, mac, sizeof(mac));
        pushStep(snap, name, until, mac, relay);
    }
}

void stepsFromActions(const DecisionRule &rule, RuleSnap &snap) {
    if (snap.n > 0) {
        return;
    }
    for (const RuleAction &action : rule.actions) {
        if (snap.n >= 4) {
            break;
        }
        if (action.type != RELAY_ON && action.type != RELAY_PULSE) {
            continue;
        }
        char name[40];
        const char *catalog =
            RelayNameCatalog::nameFor(action.target_device_id.c_str(), action.target_relay);
        if (catalog && catalog[0]) {
            snprintf(name, sizeof(name), "%s", catalog);
        } else if (action.target_relay >= 0 && action.target_relay <= 7) {
            snprintf(name, sizeof(name), "Relé %d", action.target_relay + 1);
        } else {
            snprintf(name, sizeof(name), "Relé");
        }
        pushStep(snap, name, nullptr, action.target_device_id.c_str(), action.target_relay);
    }
}

void fillSnap(const DecisionRule &rule, RuleSnap &snap) {
    memset(&snap, 0, sizeof(snap));
    stepsFromScript(rule.script_json, snap);
    stepsFromFsm(rule.fsm_json, snap);
    stepsFromActions(rule, snap);
    joinLevel(snap);
}

void logRuleLine(const char *name, const char *level, bool enabled) {
    Serial.printf("[REGLA] %s nivel=%s estado=%s\n",
                  (name && name[0]) ? name : "Regla",
                  (level && level[0]) ? level : "-",
                  enabled ? "activa" : "parada");
}

}  // namespace

void HmiUartBridge::pushRelayCatalog() {
    {
        StaticJsonDocument<64> begin;
        begin["t"] = "relay_names_begin";
        emitJson(begin);
    }
    const int n = RelayNameCatalog::slotCount();
    for (int i = 0; i < n; ++i) {
        RelayNameCatalog::SlotView slot;
        if (!RelayNameCatalog::slotAt(i, slot) || !slot.mac) {
            continue;
        }
        StaticJsonDocument<1024> doc;
        doc["t"] = "relay_names";
        doc["mac"] = slot.mac;
        doc["place"] = slot.place ? slot.place : "";
        JsonArray names = doc.createNestedArray("names");
        for (int r = 0; r < 8; ++r) {
            names.add(slot.names[r] ? slot.names[r] : "");
        }
        emitJson(doc);
    }
    StaticJsonDocument<64> endDoc;
    endDoc["t"] = "relay_names_end";
    emitJson(endDoc);
}

void HmiUartBridge::sendRulesList() {
    pushRelayCatalog();
    {
        StaticJsonDocument<64> begin;
        begin["t"] = "rules_begin";
        emitJson(begin);
    }
    uint8_t sent = 0;
    if (ctx_.decisionEngine) {
        for (const DecisionRule &rule : ctx_.decisionEngine->getAllRules()) {
            if (sent >= 8) {
                break;
            }
            RuleSnap snap;
            fillSnap(rule, snap);
            const char *name = rule.name.length() ? rule.name.c_str() : rule.id.c_str();
            logRuleLine(name, snap.level, rule.enabled);
            StaticJsonDocument<1400> doc;
            doc["t"] = "rule";
            doc["id"] = rule.id;
            doc["name"] = name;
            doc["enabled"] = rule.enabled ? 1 : 0;
            doc["level"] = snap.level;
            JsonArray steps = doc.createNestedArray("steps");
            JsonArray relays = doc.createNestedArray("relays");
            for (uint8_t i = 0; i < snap.n; ++i) {
                steps.add(snap.steps[i].text);
                JsonObject slot = relays.createNestedObject();
                slot["r"] = snap.steps[i].relay;
                slot["mac"] = snap.steps[i].mac;
                slot["until"] = snap.steps[i].until;
            }
            emitJson(doc);
            sent++;
        }
    }
    StaticJsonDocument<64> endDoc;
    endDoc["t"] = "rules_end";
    emitJson(endDoc);
}

bool HmiUartBridge::handleRuleSet(JsonDocument &doc) {
    const char *id = doc["id"] | "";
    const bool enabled = (doc["enabled"] | 0) != 0;
    if (!id || !id[0] || !ctx_.applyRuleSetFn || !ctx_.decisionEngine) {
        return false;
    }
    if (!ctx_.applyRuleSetFn(id, enabled)) {
        return false;
    }
    DecisionRule *rule = ctx_.decisionEngine->getRule(String(id));
    if (rule) {
        RuleSnap snap;
        fillSnap(*rule, snap);
        const char *name = rule->name.length() ? rule->name.c_str() : rule->id.c_str();
        logRuleLine(name, snap.level, rule->enabled);
    }
    return true;
}

bool HmiUartBridge::handleRuleDelete(JsonDocument &doc) {
    const char *id = doc["id"] | "";
    if (!id || !id[0] || !ctx_.applyRuleDeleteFn) {
        return false;
    }
    return ctx_.applyRuleDeleteFn(id);
}

bool HmiUartBridge::handleRuleCreate(JsonDocument &doc) {
    const char *name = doc["name"] | "";
    const char *mac = doc["mac"] | "";
    const int relay = doc["relay"] | -1;
    const char *until = doc["until"] | "";
    const int relay2 = doc["relay2"] | -1;
    const char *until2 = doc["until2"] | "";
    if (!ctx_.applyRuleCreateFn) {
        return false;
    }
    const char *keepId = doc["id"] | "";
    return ctx_.applyRuleCreateFn(name, mac, relay, until, relay2,
                                  (until2 && until2[0]) ? until2 : nullptr,
                                  (keepId && keepId[0]) ? keepId : nullptr);
}

bool HmiUartBridge::handleRelayRename(JsonDocument &doc) {
    const char *mac = doc["mac"] | "";
    const int relay = doc["relay"] | -1;
    const char *name = doc["name"] | "";
    if (!ctx_.applyRelayRenameFn) {
        return false;
    }
    return ctx_.applyRelayRenameFn(mac, relay, name);
}

bool HmiUartBridge::handleCommand(JsonDocument& doc) {
    const char* t = doc["t"] | "";
    if (strcmp(t, "cmd") != 0) {
        return false;
    }
    const char* action = doc["action"] | "";
    if (!action[0]) {
        return false;
    }

    if (strcmp(action, "wifi_config") == 0) {
        Serial.printf("[HMI UART RX] {\"t\":\"cmd\",\"action\":\"wifi_config\",\"ssid\":\"%s\","
                      "\"password\":\"***\"}\n",
                      doc["ssid"] | "");
    } else {
        Serial.print("[HMI UART RX] ");
        serializeJson(doc, Serial);
        Serial.println();
    }

    bool ok = false;
    if (strcmp(action, "dose") == 0 || strcmp(action, "dose_stop") == 0 ||
        strcmp(action, "dose_hold") == 0) {
        ok = handleDose(doc, action);
    } else if (strcmp(action, "nutrient_proportions") == 0) {
        ok = handleNutrientProportions(doc);
    } else if (strcmp(action, "loop_control") == 0) {
        ok = handleLoopControl(doc);
    } else if (strcmp(action, "wifi_user") == 0) {
        const char* mode = doc["mode"] | "";
        if (strcmp(mode, "offline") == 0) {
            stateManager.setWifiUserOffline(true);
            ok = true;
        } else if (strcmp(mode, "online") == 0) {
            stateManager.setWifiUserOffline(false);
            ok = true;
        }
        if (ok) {
            publishTelemetryNow();
        }
    } else if (strcmp(action, "level_interlock") == 0) {
        const char* mode = doc["mode"] | "";
        if (!ctx_.hydro) {
            ok = false;
        } else if (strcmp(mode, "normal") == 0) {
            ok = ctx_.hydro->setLevelInterlockMode(HydroControl::LEVEL_INTERLOCK_NORMAL);
        } else if (strcmp(mode, "carrera") == 0) {
            ok = ctx_.hydro->setLevelInterlockMode(HydroControl::LEVEL_INTERLOCK_CARRERA);
        } else {
            ok = false;
        }
        if (ok) {
            publishTelemetryNow();
        }
    } else if (strcmp(action, "setpoint") == 0) {
        ok = handleSetpoint(doc);
    } else if (strcmp(action, "relay_local") == 0) {
        ok = handleRelayLocal(doc);
    } else if (strcmp(action, "relay_slave") == 0) {
        ok = handleRelaySlave(doc);
    } else if (strcmp(action, "calib") == 0) {
        ok = handleCalib(doc);
    } else if (strcmp(action, "pump_flow_calib") == 0) {
        ok = handlePumpFlowCalib(doc);
    } else if (strcmp(action, "ph_delta_gain") == 0) {
        ok = handlePhDeltaGain(doc);
    } else if (strcmp(action, "wifi_config") == 0) {
        handleWifiConfig(doc);
        return true;
    } else if (strcmp(action, "master_reboot") == 0) {
        /* Reboot inmediato tras ACK: schedule diferido fallaba si loop() tardaba
         * o si HMI reiniciaba antes de que Master volviera a llamar loop(). */
        Serial.println("[HMI UART] master_reboot — ACK + suelta I2C + ESP.restart()");
        sendCmdAck(action, true);
        HmiSerial.flush();
        delay(30);
        releaseI2cBeforeRestart();
        Serial.println("[HMI UART] I2C suelto (SDA/SCL entrada) — ESP.restart()");
        Serial.flush();
        ESP.restart();
        return true;  // unreachable
    } else if (strcmp(action, "factory_reset") == 0) {
        ok = handleFactoryReset();
        sendCmdAck(action, ok);
        return true;
    } else if (strcmp(action, "rule_state") == 0) {
        ok = handleRuleState(doc);
    } else if (strcmp(action, "rules_req") == 0) {
        sendRulesList();
        sendCmdAck(action, true);
        return true;
    } else if (strcmp(action, "rule_set") == 0) {
        ok = handleRuleSet(doc);
    } else if (strcmp(action, "rule_delete") == 0) {
        ok = handleRuleDelete(doc);
    } else if (strcmp(action, "rule_create") == 0) {
        ok = handleRuleCreate(doc);
    } else if (strcmp(action, "relay_rename") == 0) {
        ok = handleRelayRename(doc);
    } else if (strcmp(action, "sys_info_req") == 0) {
        sendSysInfo();
        sendCmdAck(action, true);
        return true;
    } else if (strcmp(action, "slaves_req") == 0) {
        sendSlavesList();
        sendCmdAck(action, true);
        return true;
    } else {
        Serial.printf("[HMI UART] action desconocida: %s\n", action);
        ok = false;
    }

    sendCmdAck(action, ok);
    return ok;
}

void HmiUartBridge::loop() {
    if (!ready_) {
        return;
    }
    /* Primero RX/comandos (pueden programar reboot); luego ejecutar pending. */
    while (HmiSerial.available() > 0) {
        const char c = static_cast<char>(HmiSerial.read());
#if UART_LINK_DEBUG
        hmiRxByteCount++;
#endif
        if (c == '\n' || c == '\r') {
            if (lineLen_ > 0) {
                lineBuf_[lineLen_] = '\0';
                /* Mismo hilo que sendSlavesList: el doc de parse no puede sumarse en la pila. */
                static StaticJsonDocument<kJsonCapacity> doc;
                doc.clear();
                const DeserializationError err = deserializeJson(doc, lineBuf_);
                if (!err) {
#if UART_LINK_DEBUG
                    hmiRxLineCount++;
#endif
                    handleCommand(doc);
                    if (!plantCfgPushed_ && ctx_.hydro) {
                        plantCfgPushed_ = true;
                        publishPlantCfg();
                    }
                } else if (err == DeserializationError::NoMemory) {
                    Serial.printf("[HMI UART] BUFFER LLENO (cap=%u, linea=%u B) — mensaje descartado\n",
                                  static_cast<unsigned>(kJsonCapacity),
                                  static_cast<unsigned>(lineLen_));
                } else {
#if UART_LINK_DEBUG
                    logInvalidLine(lineBuf_, err.c_str());
#else
                    Serial.printf("[HMI UART] JSON inválido: %s\n", err.c_str());
#endif
                }
                lineLen_ = 0;
            }
            continue;
        }
        if (lineLen_ + 1 < sizeof(lineBuf_)) {
            lineBuf_[lineLen_++] = c;
        } else {
            Serial.printf("[HMI UART] linea truncada (>%u B)\n",
                          static_cast<unsigned>(sizeof(lineBuf_) - 1));
            lineLen_ = 0;
        }
    }

    if (pendingRestart_ && static_cast<long>(millis() - pendingRestartAtMs_) >= 0) {
        pendingRestart_ = false;
        Serial.println("[HMI UART] ESP.restart()");
        ESP.restart();
    }

#if UART_LINK_DEBUG
    maybeLogHmiUartDebug(millis());
#endif
}

#endif // ENABLE_HMI_UART
