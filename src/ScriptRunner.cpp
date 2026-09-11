#include "ScriptRunner.h"
#include "DecisionEngine.h"
#include "Config.h"
#include <time.h>

ScriptRunnerManager& ScriptRunnerManager::instance() {
    static ScriptRunnerManager mgr;
    return mgr;
}

void ScriptRunnerManager::notifyProcedureFinished(ActiveScript& script, const char* status,
                                                   const char* reason) {
    if (!script.isProcedure || script.finishedNotified) {
        return;
    }
    script.finishedNotified = true;
    ProcedureFinishedEvent ev;
    ev.rule_id = script.ruleId;
    ev.status = status ? status : "aborted";
    ev.reason = reason ? reason : "";
    ev.kind = script.procedureKind.length() > 0 ? script.procedureKind : "procedure";
    Serial.printf("[SCRIPT] procedure_finished rule=%s status=%s reason=%s kind=%s\n",
                  ev.rule_id.c_str(), ev.status.c_str(), ev.reason.c_str(), ev.kind.c_str());
    if (procedureFinishedCb_) {
        procedureFinishedCb_(ev);
    }
}

void ScriptRunnerManager::abortScript(ActiveScript& script, const char* reason) {
    forceOffScriptActuators(script);
    script.whileStack.clear();
    script.inBody = false;
    script.bodyPc = 0;
    script.pc = script.instructions.size();
    script.delayTicksLeft = 0;
    script.waitLitersArmed = false;
    script.recircStarted = false;
    releaseProcedureGate(script);
    notifyProcedureFinished(script, "aborted", reason);
}

void ScriptRunnerManager::collectRelayTargets(
    const ScriptInstr& instr, std::vector<std::pair<int, String>>& out) const {
    if (instr.type == "relay_action") {
        int relay = instr.relay;
        String target = instr.targetDeviceId;
        if (instr.role.length() > 0 && roleResolveCb_) {
            if (!roleResolveCb_(instr.role, target, relay)) {
                return;
            }
        }
        if (relay < 0 || relay > 7) {
            return;
        }
        for (const auto& existing : out) {
            if (existing.first == relay && existing.second == target) {
                return;
            }
        }
        out.push_back({relay, target});
        return;
    }
    if (instr.type == "while") {
        for (const auto& child : instr.body) {
            collectRelayTargets(child, out);
        }
    }
}

void ScriptRunnerManager::forceOffScriptActuators(const ActiveScript& script) {
    if (!lastRelayFn_) {
        return;
    }
    std::vector<std::pair<int, String>> targets;
    for (const auto& instr : script.instructions) {
        collectRelayTargets(instr, targets);
    }
    for (const auto& t : targets) {
        Serial.printf("🛑 [SCRIPT] force OFF R%d device=%s rule=%s\n",
                      t.first, t.second.c_str(), script.ruleId.c_str());
        lastRelayFn_(t.first, false, t.second, 0, script.priority, script.ruleId);
    }
}

void ScriptRunnerManager::classifyScript(ActiveScript& script, const JsonObject& ruleJson) {
    String execClass = ruleJson["execution_class"] | "";
    execClass.toLowerCase();
    String kind = ruleJson["procedure_kind"] | "";
    kind.toLowerCase();

    bool hasWhile = false;
    bool hasWait = false;
    bool hasDrainRole = false;
    bool hasFillRole = false;
    for (const auto& ins : script.instructions) {
        if (ins.type == "while" || ins.type == "wait_level" || ins.type == "wait_liters" ||
            ins.type == "recirc" || ins.type == "block_auto") {
            hasWhile = hasWhile || (ins.type == "while");
            hasWait = hasWait || (ins.type == "wait_level" || ins.type == "wait_liters" ||
                                  ins.type == "recirc" || ins.type == "block_auto");
        }
        if (ins.type == "while") {
            for (const auto& child : ins.body) {
                if (child.role == "drain") hasDrainRole = true;
                if (child.role == "fill") hasFillRole = true;
            }
        }
        if (ins.role == "drain") hasDrainRole = true;
        if (ins.role == "fill") hasFillRole = true;
    }

    const bool looksProcedure = hasWhile || hasWait || execClass == "procedure";
    if (execClass == "simple") {
        script.isProcedure = false;
        script.procedureKind = "simple";
        return;
    }

    script.isProcedure = looksProcedure || execClass == "procedure";
    if (!script.isProcedure) {
        script.procedureKind = "simple";
        return;
    }

    if (kind.length() > 0) {
        script.procedureKind = kind;
    } else if (hasDrainRole && hasFillRole) {
        script.procedureKind = "full_recharge";
    } else if (hasDrainRole) {
        script.procedureKind = "drain_only";
    } else if (hasFillRole) {
        script.procedureKind = "fill_only";
    } else {
        script.procedureKind = "generic";
    }
}

void ScriptRunnerManager::clear() {
    for (auto& script : scripts_) {
        if (script.isProcedure && !script.finishedNotified) {
            notifyProcedureFinished(script, "aborted", "clear");
        }
        forceOffScriptActuators(script);
        releaseProcedureGate(script);
    }
    scripts_.clear();
}

bool ScriptRunnerManager::removeByRuleId(const String& ruleId) {
    for (auto it = scripts_.begin(); it != scripts_.end(); ++it) {
        if (it->ruleId == ruleId) {
            if (it->isProcedure && !it->finishedNotified) {
                notifyProcedureFinished(*it, "aborted", "removed");
            }
            forceOffScriptActuators(*it);
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

void ScriptRunnerManager::holdAutoGate(ActiveScript& script) {
    if (script.procedureGateHeld) {
        return;
    }
    if (!tankGateCb_) {
        Serial.println("⚠️ [SCRIPT] block_auto — sin callback tank gate");
        return;
    }
    script.procedureGateHeld = true;
    tankGateCb_(true);
    Serial.printf("🔒 [SCRIPT] block_auto rule=%s — Auto EC/pH pausados\n",
                  script.ruleId.c_str());
}

void ScriptRunnerManager::releaseProcedureGate(ActiveScript& script) {
    if (!script.procedureGateHeld) {
        return;
    }
    script.procedureGateHeld = false;
    if (tankGateCb_) {
        tankGateCb_(false);
    }
    Serial.printf("🔓 [SCRIPT] unblock_auto/end rule=%s — Auto EC/pH liberados\n",
                  script.ruleId.c_str());
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
        // No encadenar JsonVariant con | (puede devolver default y caer a R0).
        if (!j["relay_number"].isNull()) {
            out.relay = j["relay_number"].is<const char*>()
                            ? String(j["relay_number"].as<const char*>()).toInt()
                            : j["relay_number"].as<int>();
        } else if (!j["relay"].isNull()) {
            out.relay = j["relay"].is<const char*>()
                            ? String(j["relay"].as<const char*>()).toInt()
                            : j["relay"].as<int>();
        } else {
            out.relay = -1;
        }
        if (out.relay < 0 || out.relay > 7) {
            Serial.println("⚠️ [SCRIPT] relay_action sin relay_number válido — skip");
            return false;
        }
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
            } else if (target == "slave") {
                Serial.printf("⚠️ [SCRIPT] relay_action slave sin MAC R%d — skip parse\n",
                              out.relay);
                return false;
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
    if (out.type == "block_auto" || out.type == "unblock_auto" ||
        out.type == "pause_auto" || out.type == "resume_auto") {
        // Aliases: pause_auto≡block_auto, resume_auto≡unblock_auto
        if (out.type == "pause_auto") {
            out.type = "block_auto";
        } else if (out.type == "resume_auto") {
            out.type = "unblock_auto";
        }
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
    classifyScript(active, ruleJson);

    int whileCount = 0;
    for (const auto& ins : active.instructions) {
        if (ins.type == "while") {
            whileCount++;
        }
    }

    bool replaced = false;
    for (auto it = scripts_.begin(); it != scripts_.end(); ++it) {
        if (it->ruleId == ruleId) {
            *it = active;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        scripts_.push_back(active);
    }

    Serial.printf("[SCRIPT] loaded rule=%s instr=%u while=%d kind=%s pri=%d heap=%u%s\n",
                  ruleId.c_str(),
                  static_cast<unsigned>(active.instructions.size()),
                  whileCount,
                  active.procedureKind.c_str(),
                  priority,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  replaced ? " (replaced)" : "");
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
        const auto& frame = script.whileStack.back();
        if (frame.whilePc >= script.instructions.size()) {
            Serial.printf("⚠️ [SCRIPT] whilePc OOB rule=%s — abort script\n", script.ruleId.c_str());
            abortScript(script, "abort");
            return;
        }
        const ScriptInstr& whileInstr = script.instructions[frame.whilePc];
        if (whileInstr.type != "while") {
            Serial.printf("⚠️ [SCRIPT] whilePc não é while rule=%s — abort script\n", script.ruleId.c_str());
            abortScript(script, "abort");
            return;
        }
        seq = &whileInstr.body;
        pc = &script.bodyPc;
    }

    if (*pc >= seq->size()) {
        if (script.inBody && !script.whileStack.empty()) {
            auto& frame = script.whileStack.back();
            if (frame.whilePc >= script.instructions.size()) {
                abortScript(script, "abort");
                return;
            }
            const ScriptInstr& whileInstr = script.instructions[frame.whilePc];
            frame.iterations++;
            // Timeout while = Aborted (Full recharge no finge éxito parcial).
            if (whileInstr.maxIterations > 0 && frame.iterations >= whileInstr.maxIterations) {
                Serial.printf("⚠️ [SCRIPT] while max_iterations rule=%s — aborted\n",
                              script.ruleId.c_str());
                abortScript(script, "while_timeout");
                return;
            }
            // Cap de segurança: evita loop infinito sem delay (LoadProhibited / WDT)
            if (whileInstr.maxIterations <= 0 && frame.iterations >= 500) {
                Serial.printf("⚠️ [SCRIPT] while sem max_iterations >500 — abort rule=%s\n",
                              script.ruleId.c_str());
                abortScript(script, "while_timeout");
                return;
            }
            if (!evalCond(whileInstr, state)) {
                Serial.printf("[SCRIPT] exit while rule=%s after %d iter (wl=%s)\n",
                              script.ruleId.c_str(), frame.iterations, state.water_level);
                script.whileStack.pop_back();
                script.inBody = false;
                script.bodyPc = 0;
                script.pc = frame.whilePc + 1;
                return;
            }
            script.bodyPc = 0;
            return;
        }
        // Secuencia terminada — liberar Auto EC/pH + Complete.
        releaseProcedureGate(script);
        notifyProcedureFinished(script, "completed", "end");
        return;
    }

    const ScriptInstr& ins = (*seq)[*pc];

    if (ins.type == "block_auto") {
        holdAutoGate(script);
        (*pc)++;
        return;
    }

    if (ins.type == "unblock_auto") {
        releaseProcedureGate(script);
        (*pc)++;
        return;
    }

    if (ins.type == "while") {
        if (script.whileStack.size() >= 4) {
            Serial.printf("⚠️ [SCRIPT] while aninhado demais rule=%s — skip\n",
                          script.ruleId.c_str());
            (*pc)++;
            return;
        }
        engageProcedureGate(script);
        if (!evalCond(ins, state)) {
            Serial.printf("[SCRIPT] skip while rule=%s %s %s %s (wl=%s)\n",
                          script.ruleId.c_str(),
                          ins.sensor.c_str(),
                          ins.op.c_str(),
                          ins.value.c_str(),
                          state.water_level);
            (*pc)++;
            return;
        }
        Serial.printf("[SCRIPT] enter while rule=%s %s %s %s (wl=%s) maxIter=%d\n",
                      script.ruleId.c_str(),
                      ins.sensor.c_str(),
                      ins.op.c_str(),
                      ins.value.c_str(),
                      state.water_level,
                      ins.maxIterations);
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
        if (relay < 0 || relay > 7) {
            Serial.printf("⚠️ [SCRIPT] relay=%d fora 0-7 rule=%s — skip\n",
                          relay, script.ruleId.c_str());
            (*pc)++;
            return;
        }
        const bool on = (ins.action == "on" || ins.action == "toggle");
        if (relayFn) {
            relayFn(relay, on, target, ins.durationMs, script.priority, script.ruleId);
        }
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
        float liters = flowLitersCb_ ? flowLitersCb_() : 0.0f;
        if (liters + 0.05f >= script.waitLitersTarget) {
            Serial.printf("✅ [SCRIPT] wait_liters done (%.2f L)\n", liters);
            script.waitLitersArmed = false;
            script.waitLitersTarget = 0.0f;
            (*pc)++;
        }
        return;
    }

    if (ins.type == "wait_level") {
        engageProcedureGate(script);
        if (evalCond(ins, state)) {
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
                sec = 60;
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

    // Tipo desconhecido — avançar para não travar
    Serial.printf("⚠️ [SCRIPT] instr tipo desconhecido '%s' rule=%s — skip\n",
                  ins.type.c_str(), script.ruleId.c_str());
    (*pc)++;
}

void ScriptRunnerManager::tickAll(const SystemState& state, RelayFn relayFn) {
    lastRelayFn_ = relayFn;
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
