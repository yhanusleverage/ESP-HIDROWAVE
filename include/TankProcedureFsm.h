#ifndef TANK_PROCEDURE_FSM_H
#define TANK_PROCEDURE_FSM_H

/**
 * Procedure FSM v2 — tanque (full_recharge / drain_only / fill_only).
 * Contrato: docs/engineering/PROCEDURE_FSM_V2.md (HIDROWAVE-main)
 *
 * Armed = enabled sin actuar. Start (MQTT/UI/schedule) → Drain/Fill → Complete|Aborted.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include <vector>

#include "ScriptRunner.h"  // ProcedureFinishedEvent, SystemState forward via DE

struct SystemState;

enum class TankProcPhase : uint8_t {
    Idle = 0,
    Armed,
    Drain,
    Fill,
    Complete,
    Aborted
};

enum class TankProcKind : uint8_t {
    FullRecharge = 0,
    DrainOnly,
    FillOnly
};

struct TankActuatorSpec {
    int relay = -1;
    String targetDeviceId;
    String until;      // vazio | alto | …
    uint32_t timeoutSec = 1800;
};

struct TankProcedureInstance {
    String ruleId;
    int priority = 50;
    TankProcKind kind = TankProcKind::FullRecharge;
    TankProcPhase phase = TankProcPhase::Idle;
    bool holdChemical = true;
    bool enabled = true;
    TankActuatorSpec drain;
    TankActuatorSpec fill;
    ScriptTimeWindow timeWindow;
    bool wasInWindow = false;
    bool gateHeld = false;
    bool finishedNotified = false;
    bool pendingStart = false;
    uint32_t phaseStartMs = 0;
    /** true si este tick ya aplicó ON del subestado */
    bool actuatorOn = false;
};

class TankProcedureFsmManager {
public:
    using RelayFn = ScriptRunnerManager::RelayFn;
    using TankGateFn = ScriptRunnerManager::TankGateFn;
    using ProcedureFinishedFn = ScriptRunnerManager::ProcedureFinishedFn;

    static TankProcedureFsmManager& instance();

    void clear();
    bool removeByRuleId(const String& ruleId);
    /** Carga desde rule_json.fsm (v2) o infiere desde script while water_level. */
    bool loadFromRuleJson(const String& ruleId, int priority, bool enabled,
                          const JsonObject& ruleJson, const JsonVariant& triggers);
    bool handleCmd(const String& ruleId, const String& op);

    void tickAll(const SystemState& state, RelayFn relayFn);

    void setTankProcedureGateCallback(TankGateFn cb) { tankGateCb_ = cb; }
    void setProcedureFinishedCallback(ProcedureFinishedFn cb) { procedureFinishedCb_ = cb; }

    bool hasRule(const String& ruleId) const;

private:
    TankProcedureFsmManager() = default;

    bool parseFsmObject(const JsonObject& fsm, TankProcedureInstance& out);
    bool inferFromScript(const JsonObject& ruleJson, TankProcedureInstance& out);
    bool parseTriggers(const JsonVariant& triggers, ScriptTimeWindow& tw);
    bool parseHHMM(const String& hhmm, int& outMin);
    bool inTimeWindow(const ScriptTimeWindow& tw) const;
    bool levelMatchesUntil(const SystemState& state, const String& until) const;
    /** Guard Drain/Fill: true mientras debe seguir ON (wl != until). */
    bool guardActive(const SystemState& state, const TankActuatorSpec& spec) const;

    void setPhase(TankProcedureInstance& p, TankProcPhase next, const char* why);
    void engageGate(TankProcedureInstance& p);
    void releaseGate(TankProcedureInstance& p);
    void forceOff(TankProcedureInstance& p, RelayFn relayFn);
    void relaySet(TankProcedureInstance& p, const TankActuatorSpec& spec, bool on, RelayFn relayFn);
    void notifyFinished(TankProcedureInstance& p, const char* status, const char* reason);
    void startRunning(TankProcedureInstance& p, const SystemState& state, RelayFn relayFn);
    void tickOne(TankProcedureInstance& p, const SystemState& state, RelayFn relayFn);
    const char* phaseName(TankProcPhase ph) const;
    const char* kindName(TankProcKind k) const;

    std::vector<TankProcedureInstance> procs_;
    TankGateFn tankGateCb_;
    ProcedureFinishedFn procedureFinishedCb_;
    RelayFn lastRelayFn_;
};

#endif
