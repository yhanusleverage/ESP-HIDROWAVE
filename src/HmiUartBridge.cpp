#include "HmiUartBridge.h"

#if ENABLE_HMI_UART

#include "HydroControl.h"
#include "RelayCoordinator.h"
#include "MasterSlaveManager.h"
#include "SupabaseClient.h"
#include "ESPNowController.h"
#include "Controller.h"
#include <WiFi.h>
#include <HardwareSerial.h>
#include <cstring>
#include <math.h>

static HardwareSerial HmiSerial(1);

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

void HmiUartBridge::attach(const Context& ctx) {
    ctx_ = ctx;
}

void HmiUartBridge::begin() {
    HmiSerial.begin(HMI_UART_BAUD, SERIAL_8N1, HMI_UART_RX_PIN, HMI_UART_TX_PIN);
    lineLen_ = 0;
    ready_ = true;
    lastTelemetryMs_ = 0;
    Serial.printf("[HMI UART] RX=%d TX=%d baud=%d\n",
                  HMI_UART_RX_PIN, HMI_UART_TX_PIN, HMI_UART_BAUD);
#if UART_LINK_DEBUG
    Serial.println("[HMI UART DBG] cable: HMI TX(17)->Master RX(17), Master TX(18)->HMI RX(18), GND");
    Serial.println("[HMI UART DBG] NOTA: IO17=RX firmware (no UART2 TX del pinout)");
#endif
}

void HmiUartBridge::emitJson(const JsonDocument& doc) {
    serializeJson(doc, HmiSerial);
    HmiSerial.print('\n');
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
    emitJson(doc);
}

void HmiUartBridge::sendSlavesList() {
    StaticJsonDocument<kJsonCapacity> doc;
    doc["t"] = "slaves";
    JsonArray arr = doc.createNestedArray("slaves");

    JsonObject local = arr.createNestedObject();
    local["mac"] = "local";
    local["name"] = "Master";
    local["local"] = true;
    local["online"] = true;
    local["numRelays"] = 8;

    if (ctx_.masterManager) {
        ctx_.masterManager->forEachTrustedSlave([&](const TrustedSlave& slave) {
            JsonObject o = arr.createNestedObject();
            o["mac"] = ESPNowController::macToString(slave.macAddress);
            o["name"] = slave.deviceName;
            o["local"] = false;
            o["online"] = ctx_.masterManager->isSlaveReachable(slave);
            o["numRelays"] = slave.numRelays > 0 ? slave.numRelays : 8;
        });
    }
    emitJson(doc);
}

void HmiUartBridge::publishTelemetryNow() {
    if (!ctx_.hydro) {
        return;
    }
    HydroControl& hydro = *ctx_.hydro;
    StaticJsonDocument<256> doc;
    doc["t"] = "telemetry";
    if (hydro.isPhValidForTelemetry()) {
        doc["ph"] = hydro.getpH();
    }
    if (hydro.isEcValidForTelemetry()) {
        doc["ec"] = hydro.getEC();
    }
    const float temp = hydro.getWaterTemp();
    if (isfinite(temp)) {
        doc["temp_agua"] = temp;
    } else if (hydro.isTempValidForTelemetry()) {
        doc["temp_agua"] = hydro.getTemperature();
    }
    emitJson(doc);
}

void HmiUartBridge::maybePublishTelemetry(unsigned long nowMs) {
    if (!ready_ || !ctx_.hydro) {
        return;
    }
    if (nowMs - lastTelemetryMs_ >= HMI_TELEMETRY_INTERVAL_MS) {
        publishTelemetryNow();
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

void HmiUartBridge::applyDeadbandFromLimits(HydroControl& hydro, float lo, float hi, bool isEc) {
    if (!(hi > lo)) {
        Serial.printf("[HMI UART] deadband inválida %s lo=%.2f hi=%.2f\n",
                      isEc ? "EC" : "pH", lo, hi);
        return;
    }
    if (isEc) {
        const float tol = max(1.0f, (hi - lo) / 2.0f);
        hydro.setECTolerance(tol, true);
        Serial.printf("[HMI UART] EC deadband=%.1f µS (lo=%.0f hi=%.0f)\n", tol, lo, hi);
    } else {
        const float tol = max(0.01f, (hi - lo) / 2.0f);
        hydro.setPHTolerance(tol);
        Serial.printf("[HMI UART] pH deadband=%.2f (lo=%.2f hi=%.2f)\n", tol, lo, hi);
    }
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
    return true;
}

bool HmiUartBridge::handleLoopControl(JsonDocument& doc) {
    if (!ctx_.hydro) {
        return false;
    }
    HydroControl& hydro = *ctx_.hydro;

    if (doc.containsKey("volumeL")) {
        const float vol = doc["volumeL"];
        if (vol > 0.0f) {
            hydro.getECController().setVolume(vol);
            hydro.saveECControllerConfig();
        }
    }
    if (doc.containsKey("homoSec")) {
        hydro.setTempoRecirculacaoSeconds((unsigned long)(doc["homoSec"] | 60));
    }
    if (doc.containsKey("pulseMl") || doc.containsKey("pulseGapSec")) {
        hydro.setEcPulseDosing(doc["pulseMl"] | 2.0f, doc["pulseGapSec"] | 2.0f);
    }
    if (doc.containsKey("autoEcIntervalSec")) {
        hydro.setAutoECInterval((int)(doc["autoEcIntervalSec"] | 30), true);
    }
    if (doc.containsKey("autoPhIntervalSec")) {
        hydro.setAutoPHInterval((int)(doc["autoPhIntervalSec"] | 300), true);
    }
    if (doc.containsKey("autoEc")) {
        hydro.setAutoECEnabled(doc["autoEc"] | false, true);
    }
    if (doc.containsKey("autoPh")) {
        hydro.setAutoPHEnabled(doc["autoPh"] | false, true);
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
    if (doc.containsKey("ecLo") && doc.containsKey("ecHi")) {
        applyDeadbandFromLimits(hydro, doc["ecLo"], doc["ecHi"], true);
    }
    if (doc.containsKey("phLo") && doc.containsKey("phHi")) {
        applyDeadbandFromLimits(hydro, doc["phLo"], doc["phHi"], false);
    }
    if (doc.containsKey("phUpRelay") || doc.containsKey("phDownRelay")) {
        int up = doc["phUpRelay"] | 0;
        int down = doc["phDownRelay"] | 0;
        if (up > 0) up -= 1;
        if (down > 0) down -= 1;
        hydro.setPhPumpConfig(up, down, 1.0f, 1.0f, 1.0f, 1.0f);
    }
    if (doc.containsKey("nutrientGapSec")) {
        Serial.printf("[HMI UART] nutrientGapSec=%d (NVS via MQTT config)\n",
                      (int)(doc["nutrientGapSec"] | 3));
    }
    if (doc.containsKey("dosingDelaySec") || doc.containsKey("dosingMode")) {
        Serial.println("[HMI UART] dosingDelay/mode recibido — sin handler v1");
    }
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
    if (relay < 0 || relay >= 8) {
        return false;
    }
    const uint32_t cmdId = ctx_.coordinator->actuateSlave(
        RelayOwner::Manual, mac, relay, state, duration, 0, 0, "");
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
    return true;
}

void HmiUartBridge::syncPumpFlowToCloud(int target) {
    if (!ctx_.supabase || !ctx_.deviceIdFn || WiFi.status() != WL_CONNECTED) {
        Serial.println("[HMI UART] cloud sync skip (WiFi/Supabase)");
        return;
    }
    if (!ctx_.supabase->isReady()) {
        Serial.println("[HMI UART] cloud sync skip (Supabase not ready)");
        return;
    }
    const String deviceId = ctx_.deviceIdFn();
    if (target == 2) {
        ctx_.supabase->patchPhConfigFlowRates(
            deviceId, ctx_.hydro->getFlowRatePhUp(), ctx_.hydro->getFlowRatePhDown());
        return;
    }
    String nutrientsJson;
    if (ctx_.hydro->buildNutrientsJsonForCloud(nutrientsJson)) {
        ctx_.supabase->patchEcConfigNutrients(deviceId, nutrientsJson);
    }
}

bool HmiUartBridge::handleWifiConfig(JsonDocument& doc) {
    const char* ssid = doc["ssid"] | "";
    Serial.printf("[HMI UART] wifi_config stub ssid=%s (use SoftAP Master)\n", ssid);
    StaticJsonDocument<256> ack;
    ack["t"] = "wifi_config_ack";
    ack["ok"] = false;
    if (ctx_.deviceIdFn) {
        ack["device_id"] = ctx_.deviceIdFn();
    }
    emitJson(ack);
    return false;
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

    Serial.print("[HMI UART RX] ");
    serializeJson(doc, Serial);
    Serial.println();

    bool ok = false;
    if (strcmp(action, "dose") == 0 || strcmp(action, "dose_stop") == 0 ||
        strcmp(action, "dose_hold") == 0) {
        ok = handleDose(doc, action);
    } else if (strcmp(action, "nutrient_proportions") == 0) {
        ok = handleNutrientProportions(doc);
    } else if (strcmp(action, "loop_control") == 0) {
        ok = handleLoopControl(doc);
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
    } else if (strcmp(action, "wifi_config") == 0) {
        handleWifiConfig(doc);
        return true;
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
    while (HmiSerial.available() > 0) {
        const char c = static_cast<char>(HmiSerial.read());
#if UART_LINK_DEBUG
        hmiRxByteCount++;
#endif
        if (c == '\n' || c == '\r') {
            if (lineLen_ > 0) {
                lineBuf_[lineLen_] = '\0';
                StaticJsonDocument<kJsonCapacity> doc;
                const DeserializationError err = deserializeJson(doc, lineBuf_);
                if (!err) {
#if UART_LINK_DEBUG
                    hmiRxLineCount++;
#endif
                    handleCommand(doc);
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
#if UART_LINK_DEBUG
            Serial.println("[HMI UART] linea truncada (>768 B)");
#endif
            lineLen_ = 0;
        }
    }

#if UART_LINK_DEBUG
    maybeLogHmiUartDebug(millis());
#endif
}

#endif // ENABLE_HMI_UART
