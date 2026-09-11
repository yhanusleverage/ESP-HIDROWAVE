#include "TankProcedureFsm.h"
#include "DecisionEngine.h"

#include <time.h>

TankProcedureFsmManager& TankProcedureFsmManager::instance() {
    static TankProcedureFsmManager mgr;
    return mgr;
}

void TankProcedureFsmManager::clear() {
    for (auto& p : procs_) {
        if (p.gateHeld && tankGateCb_) {
            tankGateCb_(false);
        }
        p.gateHeld = false;
    }
    procs_.clear();
}

bool TankProcedureFsmManager::removeByRuleId(const String& ruleId) {
    for (auto it = procs_.begin(); it != procs_.end(); ++it) {
        if (it->ruleId != ruleId) {
            continue;
        }
        if (lastRelayFn_ && (it->phase == TankProcPhase::Drain || it->phase == TankProcPhase::Fill)) {
            forceOff(*it, lastRelayFn_);
        }
        releaseGate(*it);
        if (it->phase == TankProcPhase::Drain || it->phase == TankProcPhase::Fill) {
            notifyFinished(*it, "aborted", "removed");
        }
        procs_.erase(it);
        return true;
    }
    return false;
}

bool TankProcedureFsmManager::hasRule(const String& ruleId) const {
    for (const auto& p : procs_) {
        if (p.ruleId == ruleId) {
            return true;
        }
    }
    return false;
}

bool TankProcedureFsmManager::parseHHMM(const String& hhmm, int& outMin) {
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

bool TankProcedureFsmManager::parseTriggers(const JsonVariant& triggers, ScriptTimeWindow& tw) {
    tw = ScriptTimeWindow();
    if (triggers.isNull()) {
        return false;
    }
    auto tryWindow = [&](JsonObject t) -> bool {
        String type = t["type"] | "";
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
    }
    if (triggers.is<JsonObject>()) {
        return tryWindow(triggers.as<JsonObject>());
    }
    return false;
}

bool TankProcedureFsmManager::inTimeWindow(const ScriptTimeWindow& tw) const {
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

static int readRelayNumberField(JsonObject o) {
    // Evitar `a | b | -1` (ArduinoJson puede fallar encadenando JsonVariant).
    if (!o["relay_number"].isNull()) {
        if (o["relay_number"].is<const char*>()) {
            return String(o["relay_number"].as<const char*>()).toInt();
        }
        return o["relay_number"].as<int>();
    }
    if (!o["relay"].isNull()) {
        if (o["relay"].is<const char*>()) {
            return String(o["relay"].as<const char*>()).toInt();
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

static String readSlaveMacField(JsonObject o) {
    if (o.containsKey("slave_mac") && !o["slave_mac"].isNull()) {
        return o["slave_mac"].as<String>();
    }
    if (o.containsKey("target_device_id") && !o["target_device_id"].isNull()) {
        return o["target_device_id"].as<String>();
    }
    return String();
}

bool TankProcedureFsmManager::parseFsmObject(const JsonObject& fsm, TankProcedureInstance& out) {
    if (fsm.isNull()) {
        return false;
    }
    out.holdChemical = fsm["hold_chemical"] | true;

    auto readAct = [](JsonObject o, TankActuatorSpec& a) -> bool {
        if (o.isNull()) {
            return false;
        }
        a.relay = readRelayNumberField(o);
        a.targetDeviceId = readSlaveMacField(o);
        a.until = o["until"] | "";
        a.until.toLowerCase();
        a.timeoutSec = o["timeout_s"] | o["timeout_sec"] | 1800;
        if (a.timeoutSec == 0) {
            a.timeoutSec = 1800;
        }
        return a.relay >= 0 && a.relay <= 7 && a.until.length() > 0;
    };

    JsonObject drain = fsm["drain"].as<JsonObject>();
    JsonObject fill = fsm["fill"].as<JsonObject>();
    const bool hasDrain = readAct(drain, out.drain);
    const bool hasFill = readAct(fill, out.fill);

    if (hasDrain && hasFill) {
        out.kind = TankProcKind::FullRecharge;
    } else if (hasDrain) {
        out.kind = TankProcKind::DrainOnly;
    } else if (hasFill) {
        out.kind = TankProcKind::FillOnly;
    } else {
        return false;
    }
    // Tipagem P1: drenagem/enchimento sempre no slave
    if (hasDrain && out.drain.targetDeviceId.isEmpty()) {
        Serial.println("[PROC] fsm refuse: drain sem slave_mac");
        return false;
    }
    if (hasFill && out.fill.targetDeviceId.isEmpty()) {
        Serial.println("[PROC] fsm refuse: fill sem slave_mac");
        return false;
    }
    return true;
}

bool TankProcedureFsmManager::inferFromScript(const JsonObject& ruleJson, TankProcedureInstance& out) {
    JsonObject script = ruleJson["script"].as<JsonObject>();
    if (script.isNull()) {
        Serial.println("[PROC] infer fail: sin script");
        return false;
    }
    JsonArray instrs = script["instructions"].as<JsonArray>();
    if (instrs.isNull() || instrs.size() == 0) {
        Serial.println("[PROC] infer fail: sin instructions");
        return false;
    }

    out.holdChemical = false;
    TankActuatorSpec steps[2];
    String untils[2];
    int stepN = 0;

    for (JsonObject ij : instrs) {
        String type = ij["type"] | "";
        type.toLowerCase();
        if (type == "block_auto" || type == "pause_auto") {
            out.holdChemical = true;
            continue;
        }
        if (type != "while") {
            continue;
        }
        JsonObject cond = ij["condition"].as<JsonObject>();
        if (cond.isNull()) {
            Serial.println("[PROC] infer: while sin condition — skip");
            continue;
        }
        String sensor = cond["sensor"] | "";
        sensor.toLowerCase();
        if (sensor != "water_level") {
            Serial.printf("[PROC] infer: while sensor=%s — skip (no tanque)\n", sensor.c_str());
            continue;
        }
        String op = cond["operator"] | cond["op"] | "!=";
        String value;
        if (cond["value"].is<const char*>()) {
            value = cond["value"].as<const char*>();
        } else if (cond["value"].is<String>()) {
            value = cond["value"].as<String>();
        } else {
            value = String(cond["value"].as<float>());
        }
        value.toLowerCase();
        value.trim();
        if (op != "!=" && op != "==") {
            Serial.printf("[PROC] infer: op=%s no soportado — skip\n", op.c_str());
            continue;
        }

        TankActuatorSpec act;
        act.until = value;
        act.timeoutSec = ij["max_iterations"] | 1800;
        if (act.timeoutSec == 0) {
            act.timeoutSec = 1800;
        }

        auto applyRelayObj = [&](JsonObject bi, bool preferOn) -> bool {
            if (bi.isNull()) {
                return false;
            }
            String bt = bi["type"] | "";
            bt.toLowerCase();
            if (bt != "relay_action") {
                return false;
            }
            String action = bi["action"] | "on";
            action.toLowerCase();
            const int relay = readRelayNumberField(bi);
            if (relay < 0 || relay > 7) {
                Serial.printf("[PROC] infer: relay_action sin número (keys type=%s action=%s)\n",
                              bt.c_str(), action.c_str());
                return false;
            }
            String mac = readSlaveMacField(bi);
            if (mac.isEmpty()) {
                String target = bi["target"] | "master";
                target.toLowerCase();
                if (target == "slave") {
                    Serial.println("[PROC] infer: slave sin MAC — skip relay");
                    return false;
                }
            }
            // preferOn: solo aceptar ON/toggle; si false, aceptar cualquier action
            if (preferOn && action != "on" && action != "toggle") {
                // guardar candidato OFF pero seguir buscando ON
                if (act.relay < 0) {
                    act.relay = relay;
                    act.targetDeviceId = mac;
                }
                return false;
            }
            act.relay = relay;
            act.targetDeviceId = mac;
            return action == "on" || action == "toggle" || !preferOn;
        };

        JsonArray body = ij["body"].as<JsonArray>();
        Serial.printf("[PROC] infer while until=%s body_size=%d\n",
                      value.c_str(), body.isNull() ? -1 : static_cast<int>(body.size()));
        if (!body.isNull()) {
            for (JsonVariant bv : body) {
                JsonObject bi = bv.as<JsonObject>();
                if (bi.isNull()) {
                    Serial.println("[PROC] infer: body item no-object — skip");
                    continue;
                }
                if (applyRelayObj(bi, true)) {
                    break;  // ON encontrado
                }
                // Formato legado: if/then con relay dentro
                String bt = bi["type"] | "";
                bt.toLowerCase();
                if (bt == "if") {
                    JsonArray thenArr = bi["then"].as<JsonArray>();
                    if (!thenArr.isNull()) {
                        for (JsonVariant tv : thenArr) {
                            if (applyRelayObj(tv.as<JsonObject>(), false)) {
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (act.relay < 0 || act.relay > 7) {
            Serial.printf("[PROC] infer: while until=%s sin relay ON válido\n", value.c_str());
            if (!body.isNull() && body.size() > 0) {
                Serial.print("[PROC] infer body[0]=");
                serializeJson(body[0], Serial);
                Serial.println();
            }
            continue;
        }
        if (stepN >= 2) {
            Serial.println("[PROC] infer: >2 while water_level — usar primeros 2");
            break;
        }
        Serial.printf("[PROC] infer step[%d] until=%s R%d mac=%s\n",
                      stepN, act.until.c_str(), act.relay, act.targetDeviceId.c_str());
        if (act.targetDeviceId.isEmpty()) {
            Serial.print("[PROC] infer WARN body[0]=");
            if (!body.isNull() && body.size() > 0) {
                serializeJson(body[0], Serial);
            }
            Serial.println(" — tanque P1 exige slave_mac");
            Serial.println("[PROC] infer refuse: drain/fill sem MAC (evita LOCAL R0)");
            return false;
        }
        steps[stepN] = act;
        untils[stepN] = value;
        stepN++;
    }

    if (stepN == 0) {
        Serial.println("[PROC] infer fail: 0 pasos water_level");
        return false;
    }

    auto isDrainUntil = [](const String& u) {
        return u == "vazio" || u == "seco" || u == "baixo" || u == "empty";
    };
    auto isFillUntil = [](const String& u) {
        return u == "alto" || u == "cheio" || u == "mojado" || u == "high" || u == "full";
    };

    if (stepN == 1) {
        if (isDrainUntil(untils[0]) || !isFillUntil(untils[0])) {
            // 1 paso: drain si until vacío/baixo; si until alto → fill; default drain
            if (isFillUntil(untils[0])) {
                out.kind = TankProcKind::FillOnly;
                out.fill = steps[0];
            } else {
                out.kind = TankProcKind::DrainOnly;
                out.drain = steps[0];
            }
            return true;
        }
    }

    // 2 pasos: por until o por orden (1º drain, 2º fill)
    if (isDrainUntil(untils[0]) && isFillUntil(untils[1])) {
        out.kind = TankProcKind::FullRecharge;
        out.drain = steps[0];
        out.fill = steps[1];
        return true;
    }
    if (isFillUntil(untils[0]) && isDrainUntil(untils[1])) {
        out.kind = TankProcKind::FullRecharge;
        out.fill = steps[0];
        out.drain = steps[1];
        return true;
    }
    // Fallback orden documental: primero esvaziar, luego encher
    out.kind = TankProcKind::FullRecharge;
    out.drain = steps[0];
    out.fill = steps[1];
    Serial.printf("[PROC] infer fallback order drain←%s fill←%s\n",
                  untils[0].c_str(), untils[1].c_str());
    return true;
}

bool TankProcedureFsmManager::loadFromRuleJson(const String& ruleId, int priority, bool enabled,
                                              const JsonObject& ruleJson,
                                              const JsonVariant& triggers) {
    TankProcedureInstance neu;
    neu.ruleId = ruleId;
    neu.priority = priority;
    neu.enabled = enabled;

    String kindStr = ruleJson["procedure_kind"] | "";
    kindStr.toLowerCase();

    JsonObject fsm = ruleJson["fsm"].as<JsonObject>();
    bool ok = false;
    if (!fsm.isNull()) {
        ok = parseFsmObject(fsm, neu);
    }
    if (!ok) {
        ok = inferFromScript(ruleJson, neu);
    }
    if (!ok) {
        // A veces script está en el envelope (rule.script) y no solo en rule_json
        Serial.println("[PROC] infer falló en rule_json — sin load");
        return false;
    }

    if (kindStr == "drain_only") {
        neu.kind = TankProcKind::DrainOnly;
    } else if (kindStr == "fill_only") {
        neu.kind = TankProcKind::FillOnly;
    } else if (kindStr == "full_recharge") {
        neu.kind = TankProcKind::FullRecharge;
    }

    JsonVariant trigVar = triggers;
    if (trigVar.isNull()) {
        if (ruleJson.containsKey("procedure_triggers")) {
            trigVar = ruleJson["procedure_triggers"];
        } else if (ruleJson.containsKey("triggers")) {
            trigVar = ruleJson["triggers"];
        }
    }
    parseTriggers(trigVar, neu.timeWindow);

    neu.phase = enabled ? TankProcPhase::Armed : TankProcPhase::Idle;
    neu.finishedNotified = false;
    neu.actuatorOn = false;
    neu.pendingStart = false;
    neu.wasInWindow = inTimeWindow(neu.timeWindow);

    // Replace or push
    for (auto& existing : procs_) {
        if (existing.ruleId == ruleId) {
            if (existing.gateHeld) {
                releaseGate(existing);
            }
            if (lastRelayFn_ &&
                (existing.phase == TankProcPhase::Drain || existing.phase == TankProcPhase::Fill)) {
                forceOff(existing, lastRelayFn_);
            }
            existing = neu;
            Serial.printf("[PROC] loaded rule=%s kind=%s phase=%s heap=%u (replaced)\n",
                          ruleId.c_str(), kindName(neu.kind), phaseName(neu.phase),
                          static_cast<unsigned>(ESP.getFreeHeap()));
            return true;
        }
    }
    procs_.push_back(neu);
    Serial.printf("[PROC] loaded rule=%s kind=%s phase=%s heap=%u\n",
                  ruleId.c_str(), kindName(neu.kind), phaseName(neu.phase),
                  static_cast<unsigned>(ESP.getFreeHeap()));
    return true;
}

bool TankProcedureFsmManager::handleCmd(const String& ruleId, const String& op) {
    String o = op;
    o.toLowerCase();
    for (auto& p : procs_) {
        if (p.ruleId != ruleId) {
            continue;
        }
        if (o == "start") {
            if (p.phase != TankProcPhase::Armed) {
                Serial.printf("[PROC] cmd start ignored rule=%s phase=%s\n",
                              ruleId.c_str(), phaseName(p.phase));
                return false;
            }
            if (!p.enabled) {
                Serial.printf("[PROC] cmd start ignored rule=%s disabled\n", ruleId.c_str());
                return false;
            }
            // start deferred to tick with state — mark pending via phase trick:
            // use Complete->Armed path: set a flag by setting phase Drain with phaseStartMs=0
            // Better: store pendingStart
            p.finishedNotified = false;
            p.pendingStart = true;
            Serial.printf("[PROC] cmd start queued rule=%s\n", ruleId.c_str());
            return true;
        }
        if (o == "abort") {
            if (p.phase != TankProcPhase::Drain && p.phase != TankProcPhase::Fill) {
                Serial.printf("[PROC] cmd abort ignored rule=%s phase=%s\n",
                              ruleId.c_str(), phaseName(p.phase));
                return false;
            }
            if (lastRelayFn_) {
                forceOff(p, lastRelayFn_);
            }
            releaseGate(p);
            setPhase(p, TankProcPhase::Aborted, "abort");
            notifyFinished(p, "aborted", "abort");
            return true;
        }
        if (o == "rearm") {
            if (p.phase != TankProcPhase::Complete && p.phase != TankProcPhase::Aborted) {
                Serial.printf("[PROC] cmd rearm ignored rule=%s phase=%s\n",
                              ruleId.c_str(), phaseName(p.phase));
                return false;
            }
            if (!p.enabled) {
                setPhase(p, TankProcPhase::Idle, "rearm_disabled");
                return true;
            }
            p.finishedNotified = false;
            p.actuatorOn = false;
            p.pendingStart = false;
            setPhase(p, TankProcPhase::Armed, "rearm");
            return true;
        }
        Serial.printf("[PROC] cmd unknown op=%s rule=%s\n", o.c_str(), ruleId.c_str());
        return false;
    }
    Serial.printf("[PROC] cmd rule not found %s\n", ruleId.c_str());
    return false;
}

const char* TankProcedureFsmManager::phaseName(TankProcPhase ph) const {
    switch (ph) {
        case TankProcPhase::Idle:
            return "Idle";
        case TankProcPhase::Armed:
            return "Armed";
        case TankProcPhase::Drain:
            return "Drain";
        case TankProcPhase::Fill:
            return "Fill";
        case TankProcPhase::Complete:
            return "Complete";
        case TankProcPhase::Aborted:
            return "Aborted";
        default:
            return "?";
    }
}

const char* TankProcedureFsmManager::kindName(TankProcKind k) const {
    switch (k) {
        case TankProcKind::FullRecharge:
            return "full_recharge";
        case TankProcKind::DrainOnly:
            return "drain_only";
        case TankProcKind::FillOnly:
            return "fill_only";
        default:
            return "generic";
    }
}

void TankProcedureFsmManager::setPhase(TankProcedureInstance& p, TankProcPhase next, const char* why) {
    Serial.printf("[PROC] %s %s → %s (%s) wl-pending\n",
                  p.ruleId.c_str(), phaseName(p.phase), phaseName(next),
                  why ? why : "");
    p.phase = next;
    p.phaseStartMs = millis();
    p.actuatorOn = false;
}

void TankProcedureFsmManager::engageGate(TankProcedureInstance& p) {
    if (p.gateHeld || !p.holdChemical || !tankGateCb_) {
        return;
    }
    p.gateHeld = true;
    tankGateCb_(true);
    Serial.printf("[PROC] gate ON rule=%s\n", p.ruleId.c_str());
}

void TankProcedureFsmManager::releaseGate(TankProcedureInstance& p) {
    if (!p.gateHeld) {
        return;
    }
    p.gateHeld = false;
    if (tankGateCb_) {
        tankGateCb_(false);
    }
    Serial.printf("[PROC] gate OFF rule=%s\n", p.ruleId.c_str());
}

void TankProcedureFsmManager::relaySet(TankProcedureInstance& p, const TankActuatorSpec& spec,
                                      bool on, RelayFn relayFn) {
    if (!relayFn || spec.relay < 0) {
        return;
    }
    relayFn(spec.relay, on, spec.targetDeviceId, 0, p.priority, p.ruleId);
}

void TankProcedureFsmManager::forceOff(TankProcedureInstance& p, RelayFn relayFn) {
    if (p.kind != TankProcKind::FillOnly) {
        relaySet(p, p.drain, false, relayFn);
    }
    if (p.kind != TankProcKind::DrainOnly) {
        relaySet(p, p.fill, false, relayFn);
    }
    p.actuatorOn = false;
}

void TankProcedureFsmManager::notifyFinished(TankProcedureInstance& p, const char* status,
                                            const char* reason) {
    if (p.finishedNotified) {
        return;
    }
    p.finishedNotified = true;
    ProcedureFinishedEvent ev;
    ev.rule_id = p.ruleId;
    ev.status = status;
    ev.reason = reason;
    ev.kind = kindName(p.kind);
    Serial.printf("[PROC] procedure_finished rule=%s status=%s reason=%s kind=%s\n",
                  ev.rule_id.c_str(), status, reason, ev.kind.c_str());
    if (procedureFinishedCb_) {
        procedureFinishedCb_(ev);
    }
}

bool TankProcedureFsmManager::levelMatchesUntil(const SystemState& state,
                                               const String& until) const {
    String actual = String(state.water_level);
    String expect = until;
    actual.toLowerCase();
    expect.toLowerCase();
    if (actual == "medio_baixo") {
        actual = "medio";
    }
    if (expect == "medio_baixo") {
        expect = "medio";
    }
    return actual == expect;
}

bool TankProcedureFsmManager::guardActive(const SystemState& state,
                                         const TankActuatorSpec& spec) const {
    // enquanto não for `until`
    return !levelMatchesUntil(state, spec.until);
}

void TankProcedureFsmManager::startRunning(TankProcedureInstance& p, const SystemState& state,
                                          RelayFn relayFn) {
    engageGate(p);
    p.finishedNotified = false;
    if (p.kind == TankProcKind::FillOnly) {
        setPhase(p, TankProcPhase::Fill, "start");
        if (!guardActive(state, p.fill)) {
            // ya en until → complete inmediato
            forceOff(p, relayFn);
            releaseGate(p);
            setPhase(p, TankProcPhase::Complete, "already_at_until");
            notifyFinished(p, "completed", "end");
            return;
        }
        relaySet(p, p.fill, true, relayFn);
        p.actuatorOn = true;
        return;
    }
    // Drain first (drain_only o full)
    setPhase(p, TankProcPhase::Drain, "start");
    if (!guardActive(state, p.drain)) {
        // ya vacío → skip a Fill o Complete
        relaySet(p, p.drain, false, relayFn);
        if (p.kind == TankProcKind::DrainOnly) {
            releaseGate(p);
            setPhase(p, TankProcPhase::Complete, "already_empty");
            notifyFinished(p, "completed", "end");
            return;
        }
        setPhase(p, TankProcPhase::Fill, "skip_drain");
        if (!guardActive(state, p.fill)) {
            forceOff(p, relayFn);
            releaseGate(p);
            setPhase(p, TankProcPhase::Complete, "already_full");
            notifyFinished(p, "completed", "end");
            return;
        }
        relaySet(p, p.fill, true, relayFn);
        p.actuatorOn = true;
        return;
    }
    relaySet(p, p.drain, true, relayFn);
    p.actuatorOn = true;
}

void TankProcedureFsmManager::tickOne(TankProcedureInstance& p, const SystemState& state,
                                     RelayFn relayFn) {
    if (p.phase == TankProcPhase::Armed && p.pendingStart) {
        p.pendingStart = false;
        startRunning(p, state, relayFn);
        return;
    }

    // Schedule rising edge → start
    const bool inWin = inTimeWindow(p.timeWindow);
    if (p.phase == TankProcPhase::Armed && p.timeWindow.active && inWin && !p.wasInWindow) {
        Serial.printf("[PROC] schedule start rule=%s\n", p.ruleId.c_str());
        startRunning(p, state, relayFn);
        p.wasInWindow = inWin;
        return;
    }
    p.wasInWindow = inWin;

    if (p.phase != TankProcPhase::Drain && p.phase != TankProcPhase::Fill) {
        return;
    }

    const TankActuatorSpec& spec =
        (p.phase == TankProcPhase::Drain) ? p.drain : p.fill;
    const uint32_t elapsedSec = (millis() - p.phaseStartMs) / 1000UL;
    if (elapsedSec >= spec.timeoutSec) {
        Serial.printf("[PROC] timeout rule=%s phase=%s\n", p.ruleId.c_str(),
                      phaseName(p.phase));
        forceOff(p, relayFn);
        releaseGate(p);
        setPhase(p, TankProcPhase::Aborted, "timeout");
        notifyFinished(p, "aborted", "timeout");
        return;
    }

    if (!guardActive(state, spec)) {
        // llegó al until
        relaySet(p, spec, false, relayFn);
        p.actuatorOn = false;
        Serial.printf("[PROC] exit %s rule=%s wl=%s\n", phaseName(p.phase), p.ruleId.c_str(),
                      state.water_level);
        if (p.phase == TankProcPhase::Drain && p.kind == TankProcKind::FullRecharge) {
            setPhase(p, TankProcPhase::Fill, "drain_done");
            if (!guardActive(state, p.fill)) {
                forceOff(p, relayFn);
                releaseGate(p);
                setPhase(p, TankProcPhase::Complete, "fill_skip");
                notifyFinished(p, "completed", "end");
                return;
            }
            relaySet(p, p.fill, true, relayFn);
            p.actuatorOn = true;
            return;
        }
        releaseGate(p);
        setPhase(p, TankProcPhase::Complete, "end");
        notifyFinished(p, "completed", "end");
        return;
    }

    // Keep ON (re-assert periódicamente ~ cada 2s vía tick frecuente)
    if (!p.actuatorOn) {
        relaySet(p, spec, true, relayFn);
        p.actuatorOn = true;
    }
}

void TankProcedureFsmManager::tickAll(const SystemState& state, RelayFn relayFn) {
    lastRelayFn_ = relayFn;
    for (auto& p : procs_) {
        if (!p.enabled && p.phase == TankProcPhase::Armed) {
            setPhase(p, TankProcPhase::Idle, "disabled");
            continue;
        }
        tickOne(p, state, relayFn);
    }
}
