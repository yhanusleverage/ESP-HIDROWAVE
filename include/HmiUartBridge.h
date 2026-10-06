#ifndef HMI_UART_BRIDGE_H
#define HMI_UART_BRIDGE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Config.h"

#if ENABLE_HMI_UART

class HydroControl;
class RelayCoordinator;
class MasterSlaveManager;
class SupabaseClient;
class DecisionEngine;
class MqttClientWrapper;

class HmiUartBridge {
public:
    struct Context {
        HydroControl* hydro = nullptr;
        RelayCoordinator* coordinator = nullptr;
        MasterSlaveManager* masterManager = nullptr;
        SupabaseClient* supabase = nullptr;
        DecisionEngine* decisionEngine = nullptr;
        MqttClientWrapper* mqtt = nullptr;
        bool (*cloudOkFn)() = nullptr;
        String (*deviceIdFn)() = nullptr;
    };

    void attach(const Context& ctx);
    /** Solo el puntero MSM. No reabre UART ni pisa hydro/coordinator. */
    void bindMasterManager(MasterSlaveManager* masterManager);
    void begin();
    void end();
    void loop();
    void maybePublishTelemetry(unsigned long nowMs);
    void dumpLinkStatus(Stream& out) const;
    /** Careo auditoría: último telemetry TX + EC/pH/temp internos. */
    void dumpLastTelemetry(Stream& out) const;
    bool isReady() const { return ready_; }
    static HmiUartBridge* activeInstance();
    /** ml/L y ml/min de cada bomba, los mismos que sube a la web. */
    void publishPlantCfg();
    /** Un bit confirmado por RELAY-ACK. No reconstruye t:slaves. */
    void notifyRelayState(const uint8_t mac[6], int relay, bool on);

private:
    static const size_t kJsonCapacity = 1536;
    static const size_t kTelemetryJsonCapacity = 384;
    static const size_t kLastTelemetryMax = 384;

    static HmiUartBridge* s_active_;

    Context ctx_;
    bool ready_ = false;
    bool plantCfgPushed_ = false;
    unsigned long lastTelemetryMs_ = 0;
    uint32_t commandId_ = 0;
    char lineBuf_[1536];
    size_t lineLen_ = 0;
    char lastTelemetryJson_[kLastTelemetryMax];
    bool lastTelemetryValid_ = false;
    bool pendingRestart_ = false;
    unsigned long pendingRestartAtMs_ = 0;

    void emitJson(const JsonDocument& doc);
    void sendCmdAck(const char* action, bool ok);
    void sendSysInfo();
    void sendSlavesList();
    void publishTelemetryNow();
    /** SoftAP / sin HydroControl: heartbeat vacío para mantener MasterLink. */
    void publishConfigHeartbeat();

    bool handleCommand(JsonDocument& doc);
    bool handleDose(JsonDocument& doc, const char* action);
    bool handleNutrientProportions(JsonDocument& doc);
    bool handleLoopControl(JsonDocument& doc);
    bool handleSetpoint(JsonDocument& doc);
    bool handleRelayLocal(JsonDocument& doc);
    bool handleRelaySlave(JsonDocument& doc);
    bool handleCalib(JsonDocument& doc);
    bool handleWifiConfig(JsonDocument& doc);
    bool handlePumpFlowCalib(JsonDocument& doc);
    bool handleMasterReboot();
    bool handleFactoryReset();
    void syncPumpFlowToCloud(int target);
    void scheduleRestart(unsigned long delayMs);
    /** true si MAC+relay está lastreado a regra enabled o ciclo Auto. */
    bool isSlaveRelayAutomationLocked(const char* macStr, int relay,
                                      char* reasonOut, size_t reasonLen,
                                      char* labelOut, size_t labelLen) const;

    static int parseRelayChannel(const char* channel);
    static bool parseMacString(const char* macStr, uint8_t macOut[6]);
    static void applyRecipeGain(HydroControl& hydro, float baseDose, float totalMl);
    static bool applyDeadbandFromLimits(HydroControl& hydro, float lo, float hi, bool isEc);
};

#endif // ENABLE_HMI_UART

#endif // HMI_UART_BRIDGE_H
