#ifndef SCRIPT_RUNNER_H
#define SCRIPT_RUNNER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include <vector>

struct SystemState;

struct ScriptInstr {
    String type;
    int relay = 0;
    String action;
    String targetDeviceId;
    /** "drain" | "fill" — resuelve MAC/relé vía RoleResolveFn. */
    String role;
    unsigned long durationMs = 0;
    unsigned long delayMs = 0;
    float liters = 0.0f;
    unsigned long recircSeconds = 0;
    String sensor;
    String op;
    String value;
    int maxIterations = 0;
    std::vector<ScriptInstr> body;
};

struct ScriptTimeWindow {
    bool active = false;
    int startMin = 0;
    int endMin = 0;
};

struct ScriptCycleWeek {
    bool active = false;
    int weekIndex = -1;
};

struct ActiveScript {
    String ruleId;
    int priority = 50;
    std::vector<ScriptInstr> instructions;
    ScriptTimeWindow timeWindow;
    ScriptCycleWeek cycleWeek;
    size_t pc = 0;
    int delayTicksLeft = 0;
    struct WhileCtx {
        size_t whilePc = 0;
        int iterations = 0;
        int maxIter = 0;
    };
    std::vector<WhileCtx> whileStack;
    bool inBody = false;
    size_t bodyPc = 0;
    /** true si ya se activó el gate P1 para este script (sin timer). */
    bool procedureGateHeld = false;
    bool waitLitersArmed = false;
    float waitLitersTarget = 0.0f;
    bool recircStarted = false;
};

class ScriptRunnerManager {
public:
    using RelayFn = std::function<void(int relay, bool on, const String& targetDeviceId, unsigned long durationMs)>;
    /** true = procedimento tanque activo; false = terminó. */
    using TankGateFn = std::function<void(bool active)>;
    using FlowResetFn = std::function<void()>;
    using FlowLitersFn = std::function<float()>;
    using RecircFn = std::function<void(bool starting)>;
    using DefaultRecircSecFn = std::function<unsigned long()>;
    using RoleResolveFn = std::function<bool(const String& role, String& outMac, int& outRelay)>;

    static ScriptRunnerManager& instance();

    void clear();
    bool loadFromRuleJson(const String& ruleId, int priority,
                          const JsonObject& ruleJson,
                          const JsonVariant& triggers = JsonVariant());
    void tickAll(const SystemState& state, RelayFn relayFn);

    void setCurrentGrowWeek(int weekIndex) { currentGrowWeek_ = weekIndex; }
    int currentGrowWeek() const { return currentGrowWeek_; }

    void setTankProcedureGateCallback(TankGateFn cb) { tankGateCb_ = cb; }
    void setFlowSessionCallbacks(FlowResetFn resetFn, FlowLitersFn litersFn) {
        flowResetCb_ = resetFn;
        flowLitersCb_ = litersFn;
    }
    void setRecircCallbacks(RecircFn recircFn, DefaultRecircSecFn defaultSecFn) {
        recircCb_ = recircFn;
        defaultRecircSecCb_ = defaultSecFn;
    }
    void setHydraulicRoleResolver(RoleResolveFn cb) { roleResolveCb_ = cb; }

private:
    ScriptRunnerManager() = default;

    bool parseInstr(const JsonObject& j, ScriptInstr& out);
    bool parseTriggers(const JsonVariant& triggers, ScriptTimeWindow& tw, ScriptCycleWeek& cw);
    bool parseHHMM(const String& hhmm, int& outMin);
    bool inTimeWindow(const ScriptTimeWindow& tw);
    bool evalCond(const ScriptInstr& cond, const SystemState& state);
    void runStep(ActiveScript& script, const SystemState& state, RelayFn relayFn);
    void engageProcedureGate(ActiveScript& script);
    void releaseProcedureGate(ActiveScript& script);

    std::vector<ActiveScript> scripts_;
    TankGateFn tankGateCb_;
    FlowResetFn flowResetCb_;
    FlowLitersFn flowLitersCb_;
    RecircFn recircCb_;
    DefaultRecircSecFn defaultRecircSecCb_;
    RoleResolveFn roleResolveCb_;
    int currentGrowWeek_ = 0;
};

#endif
