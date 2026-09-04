#include "ScriptRunner.h"
#include "DecisionEngine.h"
#include "Config.h"
#include <time.h>

ScriptRunnerManager& ScriptRunnerManager::instance() {
    static ScriptRunnerManager mgr;
    return mgr;
}

void ScriptRunnerManager::clear() {
    for (auto& script : scripts_) {
        releaseProcedureGate(script);
    }
    scripts_.clear();
}

bool ScriptRunnerManager::removeByRuleId(const String& ruleId) {
    for (auto it = scripts_.begin(); it != scripts_.end(); ++it) {
        if (it->ruleId == ruleId) {
            releaseProcedureGate(*it);
            scripts_.erase(it);
            return true;
        }
    }
    return false;
}

void ScriptRunnerManager::engageProcedureGate(ActiveScript& script) {
    if (script.priority < TANK_SCRIPT_PRIORITY_THRESHOLD || script.procedureGateHeld) {
        return;
    }
    if (!tankGateCb_) {
        return;
    }
    script.procedureGateHeld = true;
    tankGateCb_(true);
}

void ScriptRunnerManager::releaseProcedureGate(ActiveScript& script) {
    if (!script.procedureGateHeld) {
        return;
    }
    script.procedureGateHeld = false;
    if (tankGateCb_) {
        tankGateCb_(false);
    }
}

bool ScriptRunnerManager::parseHHMM(const String& hhmm, int& outMin) {
    int h = 0, m = 0;
    if (sscanf(hhmm.c_str(), "%d:%d", &h, &m) != 2) {
        return false;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    outMin = h * 60 + m;
    return true;
}

bool ScriptRunnerManager::parseTriggers(const JsonVariant& triggers, ScriptTimeWindow& tw,
                                        ScriptCycleWeek& cw) {
    tw = ScriptTimeWindow();
    cw = ScriptCycleWeek();
    if (triggers.isNull()) {
        return false;
    }
    auto tryWindow = [&](JsonObject t) -> bool {
        String type = t["type"] | "";
        if (type.equalsIgnoreCase("cycle_week")) {
            cw.active = true;
            cw.weekIndex = t["weekIndex"] | t["week_index"] | -1;
            return true;
        }
        if (!type.equalsIgnoreCase("time_window")) {
            return false;
        }
        String start = t["start"] | t["from"] | "";
        String end = t["end"] | t["to"] | "";
        if (start.length() > 0 && end.length() > 0 &&
            parseHHMM(start, tw.startMin) && parseHHMM(end, tw.endMin)) {
            tw.active = true;
            return true;
        }
        return false;
    };
    if (triggers.is<JsonArray>()) {
        bool any = false;
        for (JsonObject t : triggers.as<JsonArray>()) {
            if (tryWindow(t)) {
                any = true;
            }
        }
        return any;
    } else if (triggers.is<JsonObject>()) {
        return tryWindow(triggers.as<JsonObject>());
    }
    return false;
}

bool ScriptRunnerManager::inTimeWindow(const ScriptTimeWindow& tw) {
    if (!tw.active) {
        return true;
    }
    time_t nowSec = time(nullptr);
    if (nowSec < 100000) {
        return true;
    }
    struct tm tmNow;
    localtime_r(&nowSec, &tmNow);
    const int cur = tmNow.tm_hour * 60 + tmNow.tm_min;
    if (tw.startMin <= tw.endMin) {
        return cur >= tw.startMin && cur < tw.endMin;
    }
    return cur >= tw.startMin || cur < tw.endMin;
}

bool ScriptRunnerManager::parseInstr(const JsonObject& j, ScriptInstr& out) {
    out = ScriptInstr();
    out.type = j["type"] | "";
    if (out.type.isEmpty()) {
        return false;
    }
    out.type.toLowerCase();
    if (out.type == "relay_action") {
        out.relay = j["relay_number"] | j["relay"] | 0;
        out.action = j["action"] | "on";
        out.action.toLowerCase();
        if (j.containsKey("role")) {
            out.role = j["role"].as<String>();
            out.role.toLowerCase();
        }
        if (j.containsKey("target_device_id")) {
            out.targetDeviceId = j["target_device_id"].as<String>();
        } else {
            String target = j["target"] | "master";
            target.toLowerCase();
            if (target == "slave" && j.containsKey("slave_mac")) {
                out.targetDeviceId = j["slave_mac"].as<String>();
            } else if (target == "slave" && j.containsKey("target_device_id")) {
                out.targetDeviceId = j["target_device_id"].as<String>();
            }
        }
        if (j.containsKey("duration_ms")) {
            out.durationMs = j["duration_ms"];
        } else if (j.containsKey("duration_seconds")) {
            out.durationMs = static_cast<unsigned long>(j["duration_seconds"].as<int>()) * 1000UL;
        }
        return true;
    }
    if (out.type == "delay") {
        out.delayMs = j["duration_ms"] | j["delay_ms"] | 1000UL;
        return true;
    }
    if (out.type == "wait_liters") {
        out.liters = j["liters"] | j["volume_l"] | j["volume"] | 0.0f;
        return out.liters > 0.0f;
    }
    if (out.type == "wait_level") {
        out.sensor = j["sensor"] | j["level"] | "";
        out.op = j["operator"] | j["op"] | "==";
        if (j["value"].is<const char*>()) {
            out.value = j["value"].as<const char*>();
        } else {
            out.value = String(j["value"].as<float>());
        }
        out.sensor.toLowerCase();
        return out.sensor.length() > 0;
    }
    if (out.type == "recirc") {
        out.recircSeconds = j["seconds"] | j["duration_seconds"] | 0UL;
        return true;
    }
    if (out.type == "while") {
        JsonObject cond = j["condition"].as<JsonObject>();
        out.sensor = cond["sensor"] | "";
        out.op = cond["operator"] | cond["op"] | "==";
        if (cond["value"].is<const char*>()) {
            out.value = cond["value"].as<const char*>();
        } else {
            out.value = String(cond["value"].as<float>());
        }
        out.maxIterations = j["max_iterations"] | 0;
        JsonArray body = j["body"].as<JsonArray>();
        if (!body.isNull()) {
            for (JsonObject bi : body) {
                ScriptInstr child;
                if (parseInstr(bi, child)) {
                    out.body.push_back(child);
                }
            }
        }
        return true;
    }
    return false;
}

bool ScriptRunnerManager::loadFromRuleJson(const String& ruleId, int priority,
                                           const JsonObject& ruleJson,
                                           const JsonVariant& triggers) {
    JsonObject script = ruleJson["script"].as<JsonObject>();
    if (script.isNull()) {
        return false;
    }
    JsonArray instrs = script["instructions"].as<JsonArray>();
    if (instrs.isNull() || instrs.size() == 0) {
        return false;
    }

    ActiveScript active;
    active.ruleId = ruleId;
    active.priority = priority;
    for (JsonObject ij : instrs) {
        ScriptInstr ins;
        if (parseInstr(ij, ins)) {
            active.instructions.push_back(ins);
        }
    }
    if (active.instructions.empty()) {
        return false;
    }

    JsonVariant trigVar = triggers;
    if (trigVar.isNull()) {
        if (ruleJson.containsKey("procedure_triggers")) {
            trigVar = ruleJson["procedure_triggers"];
        } else if (ruleJson.containsKey("triggers")) {
            trigVar = ruleJson["triggers"];
        }
    }
    parseTriggers(trigVar, active.timeWindow, active.cycleWeek);

    for (auto it = scripts_.begin(); it != scripts_.end(); ++it) {
        if (it->ruleId == ruleId) {
            *it = active;
            return true;
        }
    }
    scripts_.push_back(active);
    return true;
}

static bool levelWet(const String& sensor, const SystemState& state) {
    if (sensor == "level_1") return state.level_1;
    if (sensor == "level_2") return state.level_2;
    if (sensor == "level_3") return state.level_3;
    if (sensor == "level_4") return state.level_4;
    return false;
}

bool ScriptRunnerManager::evalCond(const ScriptInstr& cond, const SystemState& state) {
    if (cond.sensor.startsWith("level_")) {
        const bool wet = levelWet(cond.sensor, state);
        const String expect = cond.value;
        const bool expectWet = (expect == "alto" || expect == "mojado" || expect == "cheio" ||
                                expect == "on" || expect == "true" || expect == "1");
        if (cond.op == "!=") return wet != expectWet;
        return wet == expectWet;
    }
    if (cond.sensor == "water_level") {
        String actual = String(state.water_level);
        String expect = cond.value;
        actual.toLowerCase();
        expect.toLowerCase();
        // Compat breve: medio_baixo ≡ medio (2/4)
        if (actual == "medio_baixo") actual = "medio";
        if (expect == "medio_baixo") expect = "medio";
        if (cond.op == "!=") return actual != expect;
        return actual == expect;
    }
    float sv = 0.0f;
    if (cond.sensor == "ph") sv = state.ph;
    else if (cond.sensor == "ec" || cond.sensor == "tds") sv = state.ec;
    else if (cond.sensor == "temp_water") sv = state.temp_water;
    else return false;
    const float tv = cond.value.toFloat();
    if (cond.op == "<") return sv < tv;
    if (cond.op == ">") return sv > tv;
    if (cond.op == "<=") return sv <= tv;
    if (cond.op == ">=") return sv >= tv;
    if (cond.op == "!=") return abs(sv - tv) >= 0.01f;
    return abs(sv - tv) < 0.01f;
}

void ScriptRunnerManager::runStep(ActiveScript& script, const SystemState& state, RelayFn relayFn) {
    if (script.delayTicksLeft > 0) {
        script.delayTicksLeft--;
        return;
    }

    const std::vector<ScriptInstr>* seq = &script.instructions;
    size_t* pc = &script.pc;

    if (script.inBody && !script.whileStack.empty()) {
        auto& frame = script.whileStack.back();
        const ScriptInstr& whileInstr = script.instructions[frame.whilePc];
        seq = &whileInstr.body;
        pc = &script.bodyPc;
    }

    if (*pc >= seq->size()) {
        if (script.inBody && !script.whileStack.empty()) {
            auto& frame = script.whileStack.back();
            const ScriptInstr& whileInstr = script.instructions[frame.whilePc];
            frame.iterations++;
            if (whileInstr.maxIterations > 0 && frame.iterations >= whileInstr.maxIterations) {
                script.whileStack.pop_back();
                script.inBody = false;
                script.bodyPc = 0;
                (*pc)++;
                return;
            }
            if (!evalCond(whileInstr, state)) {
                script.whileStack.pop_back();
                script.inBody = false;
                script.bodyPc = 0;
                (*pc)++;
                return;
            }
            script.bodyPc = 0;
            return;
        }
        // Secuencia terminada — liberar Auto EC/pH.
        releaseProcedureGate(script);
        return;
    }

    const ScriptInstr& ins = (*seq)[*pc];

    if (ins.type == "while") {
        engageProcedureGate(script);
        if (!evalCond(ins, state)) {
            (*pc)++;
            return;
        }
        ActiveScript::WhileCtx frame;
        frame.whilePc = *pc;
        frame.iterations = 0;
        frame.maxIter = ins.maxIterations;
        script.whileStack.push_back(frame);
        script.inBody = true;
        script.bodyPc = 0;
        return;
    }

    if (ins.type == "relay_action") {
        engageProcedureGate(script);
        int relay = ins.relay;
        String target = ins.targetDeviceId;
        if (ins.role.length() > 0 && roleResolveCb_) {
            if (!roleResolveCb_(ins.role, target, relay)) {
                Serial.printf("⚠️ [SCRIPT] role=%s no resuelto — skip relay_action\n",
                              ins.role.c_str());
                (*pc)++;
                return;
            }
        }
        const bool on = (ins.action == "on" || ins.action == "toggle");
        relayFn(relay, on, target, ins.durationMs, script.priority);
        (*pc)++;
        return;
    }

    if (ins.type == "delay") {
        engageProcedureGate(script);
        script.delayTicksLeft = max(1, (int)(ins.delayMs / 2000UL));
        (*pc)++;
        return;
    }

    if (ins.type == "wait_liters") {
        engageProcedureGate(script);
        if (!script.waitLitersArmed) {
            if (flowResetCb_) {
                flowResetCb_();
            }
            script.waitLitersArmed = true;
            script.waitLitersTarget = ins.liters;
            Serial.printf("⏳ [SCRIPT] wait_liters target=%.2f L\n", script.waitLitersTarget);
        }
        const float L = flowLitersCb_ ? flowLitersCb_() : 0.0f;
        if (L + 0.05f >= script.waitLitersTarget) {
            Serial.printf("✅ [SCRIPT] wait_liters done (%.2f L)\n", L);
            script.waitLitersArmed = false;
            script.waitLitersTarget = 0.0f;
            (*pc)++;
        }
        return;
    }

    if (ins.type == "wait_level") {
        engageProcedureGate(script);
        if (evalCond(ins, state)) {
            Serial.printf("✅ [SCRIPT] wait_level %s %s %s\n",
                          ins.sensor.c_str(), ins.op.c_str(), ins.value.c_str());
            (*pc)++;
        }
        return;
    }

    if (ins.type == "recirc") {
        engageProcedureGate(script);
        if (!script.recircStarted) {
            unsigned long sec = ins.recircSeconds;
            if (sec == 0 && defaultRecircSecCb_) {
                sec = defaultRecircSecCb_();
            }
            if (sec == 0) {
                sec = 60UL;
            }
            if (recircCb_) {
                recircCb_(true);
            }
            script.recircStarted = true;
            script.delayTicksLeft = max(1, (int)((sec * 1000UL) / 2000UL));
            Serial.printf("⏳ [SCRIPT] recirc %lu s\n", sec);
            return;
        }
        if (recircCb_) {
            recircCb_(false);
        }
        script.recircStarted = false;
        (*pc)++;
        return;
    }

    (*pc)++;
}

void ScriptRunnerManager::tickAll(const SystemState& state, RelayFn relayFn) {
    for (auto& script : scripts_) {
        if (!inTimeWindow(script.timeWindow)) {
            continue;
        }
        if (script.cycleWeek.active && script.cycleWeek.weekIndex >= 0 &&
            script.cycleWeek.weekIndex != currentGrowWeek_) {
            continue;
        }
        runStep(script, state, relayFn);
    }
}
