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

class HmiUartBridge {
public:
    struct Context {
        HydroControl* hydro = nullptr;
        RelayCoordinator* coordinator = nullptr;
        MasterSlaveManager* masterManager = nullptr;
        SupabaseClient* supabase = nullptr;
        bool (*cloudOkFn)() = nullptr;
        String (*deviceIdFn)() = nullptr;
    };

    void attach(const Context& ctx);
    void begin();
    void loop();
    void maybePublishTelemetry(unsigned long nowMs);
    void dumpLinkStatus(Stream& out) const;

private:
    static const size_t kJsonCapacity = 768;

    Context ctx_;
    bool ready_ = false;
    unsigned long lastTelemetryMs_ = 0;
    uint32_t commandId_ = 0;
    char lineBuf_[768];
    size_t lineLen_ = 0;

    void emitJson(const JsonDocument& doc);
    void sendCmdAck(const char* action, bool ok);
    void sendSysInfo();
    void sendSlavesList();
    void publishTelemetryNow();

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
    void syncPumpFlowToCloud(int target);

    static int parseRelayChannel(const char* channel);
    static bool parseMacString(const char* macStr, uint8_t macOut[6]);
    static void applyRecipeGain(HydroControl& hydro, float baseDose, float totalMl);
    static void applyDeadbandFromLimits(HydroControl& hydro, float lo, float hi, bool isEc);
};

#endif // ENABLE_HMI_UART

#endif // HMI_UART_BRIDGE_H
