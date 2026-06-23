#include "DecisionEngine.h"
#include "Config.h"
#include "MasterSlaveManager.h"
#include "RelayCoordinator.h"
#include <LittleFS.h>

namespace {

unsigned long tankHoldMsFromRule(const DecisionRule& rule) {
    unsigned long holdMs = TANK_SCRIPT_HOLD_DEFAULT_MS;
    for (const auto& action : rule.actions) {
        if (action.type == RELAY_ON || action.type == RELAY_PULSE || action.type == RELAY_PWM) {
            if (action.duration_ms > 0) {
                const unsigned long d = action.duration_ms + TANK_SCRIPT_HOLD_BUFFER_MS;
                if (d > holdMs) {
                    holdMs = d;
                }
            }
        }
    }
    return holdMs;
}

bool levelLabelExpectsWet(const String& label) {
    if (label == "alto" || label == "mojado" || label == "cheio" || label == "on" || label == "true") {
        return true;
    }
    if (label == "vazio" || label == "seco" || label == "baixo" || label == "off" || label == "false") {
        return false;
    }
    return false;
}

bool getLevelWetFromState(const String& sensorName, const SystemState& state) {
    if (sensorName == "level_1") return state.level_1;
    if (sensorName == "level_2") return state.level_2;
    if (sensorName == "level_3") return state.level_3;
    if (sensorName == "level_4") return state.level_4;
    return false;
}

bool evaluateDiscreteLevelCondition(const RuleCondition& condition, const SystemState& state) {
    const bool actualWet = getLevelWetFromState(condition.sensor_name, state);
    const String expected = condition.string_value.length() > 0
        ? condition.string_value
        : String(condition.value_min > 0.5f ? "alto" : "vazio");
    const bool expectWet = levelLabelExpectsWet(expected);

    switch (condition.op) {
        case OP_EQUAL:
            return actualWet == expectWet;
        case OP_NOT_EQUAL:
            return actualWet != expectWet;
        default:
            return false;
    }
}

bool evaluateWaterLevelCondition(const RuleCondition& condition, const SystemState& state) {
    const String actual = String(state.water_level);
    const String expected = condition.string_value.length() > 0
        ? condition.string_value
        : String("medio");

    switch (condition.op) {
        case OP_EQUAL:
            return actual.equalsIgnoreCase(expected);
        case OP_NOT_EQUAL:
            return !actual.equalsIgnoreCase(expected);
        default:
            return false;
    }
}

}  // namespace

// ===== CONSTRUTOR E DESTRUTOR =====
DecisionEngine::DecisionEngine() : 
    last_evaluation(0),
    evaluation_interval(DEFAULT_EVALUATION_INTERVAL),
    dry_run_mode(false),
    total_evaluations(0),
    total_actions_executed(0),
    total_safety_blocks(0),
    masterManager(nullptr),
    relayCoordinator(nullptr) {
}

DecisionEngine::~DecisionEngine() {
    end();
}

// ===== CONTROLE PRINCIPAL =====
bool DecisionEngine::begin() {
    Serial.println("🧠 Inicializando Decision Engine...");
    
    // Inicializar LittleFS se não estiver inicializado
    if (!LittleFS.begin()) {
        Serial.println("❌ Erro ao inicializar LittleFS");
        return false;
    }
    
    // Carregar regras do arquivo
    if (!loadRulesFromFile()) {
        Serial.println("⚠️ Nenhuma regra carregada - iniciando com regras padrão");
        
        // Criar regras de exemplo para demonstração
        createDefaultRules();
    }
    
    Serial.printf("✅ Decision Engine iniciado com %d regras\n", rules.size());
    Serial.printf("🔄 Intervalo de avaliação: %lu ms\n", evaluation_interval);
    Serial.printf("🧪 Modo dry-run: %s\n", dry_run_mode ? "ATIVADO" : "DESATIVADO");
    
    return true;
}

void DecisionEngine::loop() {
    unsigned long now = millis();
    
    // Verificar se é hora de avaliar regras
    if (now - last_evaluation >= evaluation_interval) {
        evaluateAllRules();
        last_evaluation = now;
        total_evaluations++;
    }
}

void DecisionEngine::end() {
    Serial.println("🧠 Finalizando Decision Engine...");
    rules.clear();
}

// ===== GERENCIAMENTO DE REGRAS =====
bool DecisionEngine::loadRulesFromFile(const String& filename) {
    if (!LittleFS.exists(filename)) {
        Serial.println("⚠️ Arquivo de regras não encontrado: " + filename);
        return false;
    }
    
    File file = LittleFS.open(filename, "r");
    if (!file) {
        Serial.println("❌ Erro ao abrir arquivo de regras");
        return false;
    }
    
    String json_str = file.readString();
    file.close();
    
    DynamicJsonDocument doc(JSON_BUFFER_SIZE);
    DeserializationError error = deserializeJson(doc, json_str);
    
    if (error) {
        Serial.println("❌ Erro ao parsear JSON das regras: " + String(error.c_str()));
        return false;
    }
    
    rules.clear();
    
    JsonArray rules_array = doc["rules"].as<JsonArray>();
    for (JsonObject rule_json : rules_array) {
        DecisionRule rule;
        if (parseRuleFromJSON(rule_json, rule)) {
            String validation_error;
            if (validateRule(rule, validation_error)) {
                rules.push_back(rule);
                Serial.println("✅ Regra carregada: " + rule.name);
            } else {
                Serial.println("❌ Regra inválida (" + rule.id + "): " + validation_error);
            }
        } else {
            Serial.println("❌ Erro ao parsear regra");
        }
    }
    
    Serial.printf("📋 %d regras carregadas do arquivo\n", rules.size());
    return true;
}

bool DecisionEngine::saveRulesToFile(const String& filename) {
    DynamicJsonDocument doc(JSON_BUFFER_SIZE);
    JsonArray rules_array = doc.createNestedArray("rules");
    
    for (const auto& rule : rules) {
        JsonObject rule_json = rules_array.createNestedObject();
        ruleToJSON(rule, rule_json, doc);
    }
    
    File file = LittleFS.open(filename, "w");
    if (!file) {
        Serial.println("❌ Erro ao criar arquivo de regras");
        return false;
    }
    
    serializeJson(doc, file);
    file.close();
    
    Serial.println("✅ Regras salvas em: " + filename);
    return true;
}

bool DecisionEngine::addRule(const DecisionRule& rule) {
    if (rules.size() >= MAX_RULES) {
        Serial.println("❌ Limite máximo de regras atingido");
        return false;
    }
    
    // Verificar se ID já existe
    for (const auto& existing_rule : rules) {
        if (existing_rule.id == rule.id) {
            Serial.println("❌ Regra com ID já existe: " + rule.id);
            return false;
        }
    }
    
    String validation_error;
    if (!validateRule(rule, validation_error)) {
        Serial.println("❌ Regra inválida: " + validation_error);
        return false;
    }
    
    rules.push_back(rule);
    Serial.println("✅ Regra adicionada: " + rule.name);
    return true;
}

bool DecisionEngine::removeRule(const String& rule_id) {
    for (auto it = rules.begin(); it != rules.end(); ++it) {
        if (it->id == rule_id) {
            Serial.println("🗑️ Removendo regra: " + it->name);
            rules.erase(it);
            return true;
        }
    }
    Serial.println("❌ Regra não encontrada: " + rule_id);
    return false;
}

bool DecisionEngine::updateRule(const String& rule_id, const DecisionRule& new_rule) {
    for (auto& rule : rules) {
        if (rule.id == rule_id) {
            String validation_error;
            if (!validateRule(new_rule, validation_error)) {
                Serial.println("❌ Regra atualizada inválida: " + validation_error);
                return false;
            }
            rule = new_rule;
            Serial.println("✅ Regra atualizada: " + rule.name);
            return true;
        }
    }
    Serial.println("❌ Regra não encontrada para atualização: " + rule_id);
    return false;
}

DecisionRule* DecisionEngine::getRule(const String& rule_id) {
    for (auto& rule : rules) {
        if (rule.id == rule_id) {
            return &rule;
        }
    }
    return nullptr;
}

// ===== AVALIAÇÃO E EXECUÇÃO =====
void DecisionEngine::updateSystemState(const SystemState& state) {
    current_state = state;
    current_state.last_update = millis();
}

void DecisionEngine::evaluateAllRules() {
    if (rules.empty()) return;
    
    // Ordenar regras por prioridade (maior prioridade primeiro)
    std::sort(rules.begin(), rules.end(), [](const DecisionRule& a, const DecisionRule& b) {
        return a.priority > b.priority;
    });
    
    for (auto& rule : rules) {
        if (!rule.enabled) continue;

        // Mutex Auto pH: não executar ph_low_control quando Auto pH está ativo
        if (rule.id == "ph_low_control" && current_state.auto_ph_active) {
            continue;
        }
        
        // Verificar cooldown
        if (isInCooldown(rule)) continue;
        
        // Verificar limite por hora
        if (hasExceededHourlyLimit(rule)) continue;
        
        // Avaliar condição principal
        if (!evaluateCondition(rule.condition, current_state)) continue;
        
        // Verificar interlocks de segurança
        if (!checkSafetyConstraints(rule, current_state)) {
            total_safety_blocks++;
            logRuleExecution(rule.id, "BLOCKED_BY_SAFETY", false);
            continue;
        }
        
        // Executar ações
        if (dry_run_mode) {
            Serial.printf("🧪 [DRY-RUN] Executaria regra: %s\n", rule.name.c_str());
            for (const auto& action : rule.actions) {
                Serial.printf("   → Ação: %d no relé %d por %lu ms\n", 
                             action.type, action.target_relay, action.duration_ms);
            }
        } else {
            executeActions(rule.actions, rule.id);
            updateExecutionCounts(rule);
            total_actions_executed++;
            if (rule.priority >= TANK_SCRIPT_PRIORITY_THRESHOLD && tank_script_hold_callback) {
                tank_script_hold_callback(tankHoldMsFromRule(rule));
            }
        }
        
        logRuleExecution(rule.id, "EXECUTED", true);
        
        // Se trigger é on_change, marcar como executado
        if (rule.trigger_type == "on_change") {
            rule.currently_active = true;
        }
    }
}

bool DecisionEngine::evaluateCondition(const RuleCondition& condition, const SystemState& state) {
    bool result = false;
    
    switch (condition.type) {
        case SENSOR_COMPARE: {
            if (condition.sensor_name == "water_level") {
                result = evaluateWaterLevelCondition(condition, state);
            } else if (condition.sensor_name.startsWith("level_")) {
                result = evaluateDiscreteLevelCondition(condition, state);
            } else {
                float sensor_value = getSensorValue(condition.sensor_name, state);
                result = compareValues(sensor_value, condition.op, condition.value_min, condition.value_max);
            }
            break;
        }
        
        case RELAY_STATE: {
            if (condition.sensor_name.startsWith("relay_")) {
                int relay_id = condition.sensor_name.substring(6).toInt();
                if (relay_id >= 0 && relay_id < MAX_RELAYS) {
                    result = state.relay_states[relay_id] == (condition.value_min > 0);
                }
            }
            break;
        }
        
        case SYSTEM_STATUS: {
            if (condition.sensor_name == "wifi_connected") {
                result = state.wifi_connected == (condition.value_min > 0);
            } else if (condition.sensor_name == "water_level_ok") {
                result = state.water_level_ok == (condition.value_min > 0);
            } else if (condition.sensor_name == "free_heap") {
                result = compareValues(state.free_heap, condition.op, condition.value_min, condition.value_max);
            }
            break;
        }
        
        case TIME_WINDOW: {
            // Implementar lógica de janela de tempo
            // Por enquanto, sempre verdadeiro
            result = true;
            break;
        }
        
        case COMPOSITE: {
            if (condition.logic_operator == "AND") {
                result = true;
                for (const auto& sub_cond : condition.sub_conditions) {
                    if (!evaluateCondition(sub_cond, state)) {
                        result = false;
                        break;
                    }
                }
            } else if (condition.logic_operator == "OR") {
                result = false;
                for (const auto& sub_cond : condition.sub_conditions) {
                    if (evaluateCondition(sub_cond, state)) {
                        result = true;
                        break;
                    }
                }
            }
            break;
        }
    }
    
    return condition.negate ? !result : result;
}

bool DecisionEngine::checkSafetyConstraints(const DecisionRule& rule, const SystemState& state) {
    for (const auto& safety_check : rule.safety_checks) {
        if (!evaluateCondition(safety_check.condition, state)) {
            Serial.printf("🛡️ Safety check falhou: %s\n", safety_check.name.c_str());
            
            if (alert_callback) {
                alert_callback("Safety check failed: " + safety_check.error_message, safety_check.is_critical);
            }
            
            if (safety_check.is_critical) {
                Serial.println("🚨 SAFETY CRÍTICA - Parando todas as operações!");
                // Implementar parada de emergência
                return false;
            }
            return false;
        }
    }
    return true;
}

void DecisionEngine::executeActions(const std::vector<RuleAction>& actions, const String& rule_id) {
    for (const auto& action : actions) {
        switch (action.type) {
            case RELAY_ON:
            case RELAY_OFF:
            case RELAY_PULSE:
            case RELAY_PWM:
                executeRelayAction(action, rule_id);
                break;
                
            case SYSTEM_ALERT:
                executeSystemAlert(action, rule_id);
                break;
                
            case LOG_EVENT:
                executeLogEvent(action, rule_id);
                break;
                
            case SUPABASE_UPDATE:
                // Implementar atualização Supabase
                if (log_callback) {
                    log_callback("SUPABASE_UPDATE", "Rule: " + rule_id + " - " + action.message);
                }
                break;
        }
    }
}

// ===== MÉTODOS AUXILIARES =====
float DecisionEngine::getSensorValue(const String& sensor_name, const SystemState& state) {
    if (sensor_name == "ph") return state.ph;
    if (sensor_name == "tds" || sensor_name == "ec") return state.ec;
    if (sensor_name == "temp_water") return state.temp_water;
    if (sensor_name == "temp_environment") return state.temp_environment;
    if (sensor_name == "humidity") return state.humidity;
    if (sensor_name == "uptime") return state.uptime / 1000.0; // em segundos
    if (sensor_name == "free_heap") return state.free_heap;
    
    return 0.0;
}

bool DecisionEngine::compareValues(float sensor_value, CompareOperator op, float target_min, float target_max) {
    switch (op) {
        case OP_LESS_THAN: return sensor_value < target_min;
        case OP_LESS_EQUAL: return sensor_value <= target_min;
        case OP_GREATER_THAN: return sensor_value > target_min;
        case OP_GREATER_EQUAL: return sensor_value >= target_min;
        case OP_EQUAL: return abs(sensor_value - target_min) < 0.01;
        case OP_NOT_EQUAL: return abs(sensor_value - target_min) >= 0.01;
        case OP_BETWEEN: return sensor_value >= target_min && sensor_value <= target_max;
        case OP_OUTSIDE: return sensor_value < target_min || sensor_value > target_max;
        default: return false;
    }
}

void DecisionEngine::executeRelayAction(const RuleAction& action, const String& rule_id) {
    bool state = (action.type == RELAY_ON || action.type == RELAY_PULSE);
    String actionStr = state ? "on" : "off";
    if (action.type == RELAY_PULSE) {
        actionStr = "toggle";
    }
    const uint32_t durationSec = action.duration_ms / 1000;

    if (action.target_device_id.isEmpty() || action.target_device_id == "local" || action.target_device_id == "MASTER") {
        if (relayCoordinator) {
            relayCoordinator->actuateLocal(
                RelayOwner::DecisionRule,
                action.target_relay,
                actionStr,
                (int)durationSec);
            Serial.printf("⚡ [LOCAL/COORD] Relé %d: %s por %lu ms (regra: %s)\n",
                action.target_relay, state ? "ON" : "OFF", action.duration_ms, rule_id.c_str());
        } else if (relay_control_callback) {
            relay_control_callback(action.target_relay, state, action.duration_ms);
            Serial.printf("⚡ [LOCAL] Relé %d: %s por %lu ms (regra: %s)\n",
                action.target_relay, state ? "ON" : "OFF", action.duration_ms, rule_id.c_str());
        }
        return;
    }

    if (!masterManager && !relayCoordinator) {
        Serial.printf("⚠️ [REMOTO] ESP-NOW não disponível - device_id=%s relay=%d (regra: %s)\n",
            action.target_device_id.c_str(), action.target_relay, rule_id.c_str());
        return;
    }

    const uint8_t* targetMac = nullptr;
    if (masterManager) {
        auto trustedSlaves = masterManager->getAllTrustedSlaves();
        for (const auto& slave : trustedSlaves) {
            if (slave.deviceName == action.target_device_id || slave.deviceName.equalsIgnoreCase(action.target_device_id)) {
                targetMac = slave.macAddress;
                break;
            }
        }
    }

    if (!targetMac) {
        Serial.printf("⚠️ [REMOTO] Slave '%s' não encontrado (regra: %s)\n",
            action.target_device_id.c_str(), rule_id.c_str());
        return;
    }

    bool success = false;
    if (relayCoordinator) {
        success = relayCoordinator->actuateSlave(
            RelayOwner::DecisionRule,
            targetMac,
            action.target_relay,
            actionStr,
            (int)durationSec) > 0;
    } else if (masterManager) {
        success = masterManager->sendRelayCommandToSlave(
            targetMac, action.target_relay, actionStr, (int)durationSec) > 0;
    }

    if (success) {
        Serial.printf("✅ [REMOTO] device_id=%s relay=%d: %s por %lu ms (regra: %s)\n",
            action.target_device_id.c_str(), action.target_relay,
            actionStr.c_str(), action.duration_ms, rule_id.c_str());
    } else {
        Serial.printf("❌ [REMOTO] Falha ao enviar comando - device_id=%s relay=%d (regra: %s)\n",
            action.target_device_id.c_str(), action.target_relay, rule_id.c_str());
    }
}

void DecisionEngine::executeSystemAlert(const RuleAction& action, const String& rule_id) {
    if (alert_callback) {
        alert_callback(action.message, false);
        Serial.printf("🔔 Alerta: %s (regra: %s)\n", action.message.c_str(), rule_id.c_str());
    }
}

void DecisionEngine::executeLogEvent(const RuleAction& action, const String& rule_id) {
    if (log_callback) {
        log_callback("RULE_EVENT", "Rule: " + rule_id + " - " + action.message);
    }
    Serial.printf("📝 Log: %s (regra: %s)\n", action.message.c_str(), rule_id.c_str());
}

bool DecisionEngine::isInCooldown(const DecisionRule& rule) {
    if (rule.cooldown_ms == 0) return false;
    return (millis() - rule.last_execution) < rule.cooldown_ms;
}

bool DecisionEngine::hasExceededHourlyLimit(const DecisionRule& rule) {
    if (rule.max_executions_per_hour == 0) return false;
    
    // Reset contador a cada hora
    unsigned long current_hour = millis() / 3600000; // Hora atual
    if (current_hour != rule.hour_reset_time) {
        const_cast<DecisionRule&>(rule).execution_count_hour = 0;
        const_cast<DecisionRule&>(rule).hour_reset_time = current_hour;
    }
    
    return rule.execution_count_hour >= rule.max_executions_per_hour;
}

void DecisionEngine::updateExecutionCounts(DecisionRule& rule) {
    rule.last_execution = millis();
    rule.execution_count_hour++;
}

void DecisionEngine::logRuleExecution(const String& rule_id, const String& action, bool success) {
    if (log_callback) {
        String log_data = "Rule: " + rule_id + ", Action: " + action + ", Success: " + (success ? "true" : "false");
        log_callback("RULE_EXECUTION", log_data);
    }
}

// ===== VALIDAÇÃO =====
bool DecisionEngine::validateRule(const DecisionRule& rule, String& error_message) {
    if (rule.id.isEmpty()) {
        error_message = "ID da regra não pode estar vazio";
        return false;
    }
    
    if (rule.name.isEmpty()) {
        error_message = "Nome da regra não pode estar vazio";
        return false;
    }
    
    if (rule.priority < 0 || rule.priority > 100) {
        error_message = "Prioridade deve estar entre 0 e 100";
        return false;
    }
    
    if (rule.actions.empty()) {
        error_message = "Regra deve ter pelo menos uma ação";
        return false;
    }
    
    // Validar ações
    for (const auto& action : rule.actions) {
        if (action.type == RELAY_ON || action.type == RELAY_OFF || 
            action.type == RELAY_PULSE || action.type == RELAY_PWM) {
            if (action.target_relay < 0 || action.target_relay >= MAX_RELAYS) {
                error_message = "ID do relé inválido: " + String(action.target_relay);
                return false;
            }
        }
        
        if (action.type == RELAY_PULSE && action.duration_ms == 0) {
            error_message = "Ação PULSE deve ter duração > 0";
            return false;
        }
    }
    
    return true;
}

// ===== SERIALIZAÇÃO JSON =====
namespace {

const char* conditionTypeToString(ConditionType type) {
    switch (type) {
        case SENSOR_COMPARE: return "SENSOR_COMPARE";
        case TIME_WINDOW: return "TIME_WINDOW";
        case RELAY_STATE: return "RELAY_STATE";
        case SYSTEM_STATUS: return "SYSTEM_STATUS";
        case COMPOSITE: return "COMPOSITE";
        default: return "SENSOR_COMPARE";
    }
}

ConditionType parseConditionType(const JsonVariant& value) {
    if (value.is<int>()) {
        const int v = value.as<int>();
        if (v >= SENSOR_COMPARE && v <= COMPOSITE) {
            return static_cast<ConditionType>(v);
        }
    }
    const char* s = value.as<const char*>();
    if (!s) return SENSOR_COMPARE;
    if (strcasecmp(s, "SENSOR_COMPARE") == 0 || strcasecmp(s, "sensor_compare") == 0) return SENSOR_COMPARE;
    if (strcasecmp(s, "TIME_WINDOW") == 0 || strcasecmp(s, "time_window") == 0) return TIME_WINDOW;
    if (strcasecmp(s, "RELAY_STATE") == 0 || strcasecmp(s, "relay_state") == 0) return RELAY_STATE;
    if (strcasecmp(s, "SYSTEM_STATUS") == 0 || strcasecmp(s, "system_status") == 0) return SYSTEM_STATUS;
    if (strcasecmp(s, "COMPOSITE") == 0 || strcasecmp(s, "composite") == 0) return COMPOSITE;
    return SENSOR_COMPARE;
}

const char* compareOperatorToString(CompareOperator op) {
    switch (op) {
        case OP_LESS_THAN: return "OP_LESS_THAN";
        case OP_LESS_EQUAL: return "OP_LESS_EQUAL";
        case OP_GREATER_THAN: return "OP_GREATER_THAN";
        case OP_GREATER_EQUAL: return "OP_GREATER_EQUAL";
        case OP_EQUAL: return "OP_EQUAL";
        case OP_NOT_EQUAL: return "OP_NOT_EQUAL";
        case OP_BETWEEN: return "OP_BETWEEN";
        case OP_OUTSIDE: return "OP_OUTSIDE";
        default: return "OP_GREATER_THAN";
    }
}

CompareOperator parseCompareOperator(const JsonVariant& value) {
    if (value.is<int>()) {
        const int v = value.as<int>();
        if (v >= OP_LESS_THAN && v <= OP_OUTSIDE) {
            return static_cast<CompareOperator>(v);
        }
    }
    const char* s = value.as<const char*>();
    if (!s) return OP_GREATER_THAN;
    if (strcmp(s, "<") == 0 || strcasecmp(s, "OP_LESS_THAN") == 0 || strcasecmp(s, "less_than") == 0) return OP_LESS_THAN;
    if (strcmp(s, "<=") == 0 || strcasecmp(s, "OP_LESS_EQUAL") == 0 || strcasecmp(s, "less_equal") == 0) return OP_LESS_EQUAL;
    if (strcmp(s, ">") == 0 || strcasecmp(s, "OP_GREATER_THAN") == 0 || strcasecmp(s, "greater_than") == 0) return OP_GREATER_THAN;
    if (strcmp(s, ">=") == 0 || strcasecmp(s, "OP_GREATER_EQUAL") == 0 || strcasecmp(s, "greater_equal") == 0) return OP_GREATER_EQUAL;
    if (strcmp(s, "==") == 0 || strcasecmp(s, "OP_EQUAL") == 0 || strcasecmp(s, "equal") == 0) return OP_EQUAL;
    if (strcmp(s, "!=") == 0 || strcasecmp(s, "OP_NOT_EQUAL") == 0 || strcasecmp(s, "not_equal") == 0) return OP_NOT_EQUAL;
    if (strcasecmp(s, "OP_BETWEEN") == 0 || strcasecmp(s, "between") == 0) return OP_BETWEEN;
    if (strcasecmp(s, "OP_OUTSIDE") == 0 || strcasecmp(s, "outside") == 0) return OP_OUTSIDE;
    return OP_GREATER_THAN;
}

const char* actionTypeToString(ActionType type) {
    switch (type) {
        case RELAY_ON: return "RELAY_ON";
        case RELAY_OFF: return "RELAY_OFF";
        case RELAY_PULSE: return "RELAY_PULSE";
        case RELAY_PWM: return "RELAY_PWM";
        case SYSTEM_ALERT: return "SYSTEM_ALERT";
        case LOG_EVENT: return "LOG_EVENT";
        case SUPABASE_UPDATE: return "SUPABASE_UPDATE";
        default: return "RELAY_ON";
    }
}

ActionType parseActionType(const JsonVariant& value) {
    if (value.is<int>()) {
        const int v = value.as<int>();
        if (v >= RELAY_ON && v <= SUPABASE_UPDATE) {
            return static_cast<ActionType>(v);
        }
    }
    const char* s = value.as<const char*>();
    if (!s) return RELAY_ON;
    if (strcasecmp(s, "RELAY_ON") == 0 || strcasecmp(s, "relay_on") == 0) return RELAY_ON;
    if (strcasecmp(s, "RELAY_OFF") == 0 || strcasecmp(s, "relay_off") == 0) return RELAY_OFF;
    if (strcasecmp(s, "RELAY_PULSE") == 0 || strcasecmp(s, "relay_pulse") == 0) return RELAY_PULSE;
    if (strcasecmp(s, "RELAY_PWM") == 0 || strcasecmp(s, "relay_pwm") == 0) return RELAY_PWM;
    if (strcasecmp(s, "SYSTEM_ALERT") == 0 || strcasecmp(s, "system_alert") == 0) return SYSTEM_ALERT;
    if (strcasecmp(s, "LOG_EVENT") == 0 || strcasecmp(s, "log_event") == 0) return LOG_EVENT;
    if (strcasecmp(s, "SUPABASE_UPDATE") == 0 || strcasecmp(s, "supabase_update") == 0) return SUPABASE_UPDATE;
    return RELAY_ON;
}

}  // namespace

bool DecisionEngine::parseConditionFromJSON(const JsonObject& json_cond, RuleCondition& condition) {
    if (json_cond.isNull()) return false;

    if (json_cond.containsKey("type")) {
        condition.type = parseConditionType(json_cond["type"]);
    }
    if (json_cond.containsKey("sensor_name")) {
        condition.sensor_name = json_cond["sensor_name"].as<String>();
    } else if (json_cond.containsKey("sensor")) {
        condition.sensor_name = json_cond["sensor"].as<String>();
    }
    if (json_cond.containsKey("op")) {
        condition.op = parseCompareOperator(json_cond["op"]);
    } else if (json_cond.containsKey("operator")) {
        condition.op = parseCompareOperator(json_cond["operator"]);
    }
    if (json_cond.containsKey("value_min")) {
        condition.value_min = json_cond["value_min"];
    } else if (json_cond.containsKey("value")) {
        condition.value_min = json_cond["value"];
    }
    if (json_cond.containsKey("value_max")) {
        condition.value_max = json_cond["value_max"];
    }
    if (json_cond.containsKey("string_value")) {
        condition.string_value = json_cond["string_value"].as<String>();
    }
    if (json_cond.containsKey("negate")) {
        condition.negate = json_cond["negate"];
    }
    if (json_cond.containsKey("logic_operator")) {
        condition.logic_operator = json_cond["logic_operator"].as<String>();
    }

    condition.sub_conditions.clear();
    if (json_cond.containsKey("sub_conditions")) {
        for (JsonObject sub_json : json_cond["sub_conditions"].as<JsonArray>()) {
            RuleCondition sub;
            if (parseConditionFromJSON(sub_json, sub)) {
                condition.sub_conditions.push_back(sub);
            }
        }
    }

    return !condition.sensor_name.isEmpty() || condition.type == COMPOSITE || condition.type == TIME_WINDOW;
}

void DecisionEngine::conditionToJSON(const RuleCondition& condition, JsonObject& out, JsonDocument& doc) {
    out["type"] = conditionTypeToString(condition.type);
    out["sensor_name"] = condition.sensor_name;
    out["op"] = compareOperatorToString(condition.op);
    out["value_min"] = condition.value_min;
    out["value_max"] = condition.value_max;
    out["string_value"] = condition.string_value;
    out["negate"] = condition.negate;
    out["logic_operator"] = condition.logic_operator;

    JsonArray sub_array = out.createNestedArray("sub_conditions");
    for (const auto& sub : condition.sub_conditions) {
        JsonObject sub_json = sub_array.createNestedObject();
        conditionToJSON(sub, sub_json, doc);
    }
}

bool DecisionEngine::parseActionFromJSON(const JsonObject& json_action, RuleAction& action) {
    if (json_action.isNull()) return false;

    if (json_action.containsKey("type")) {
        action.type = parseActionType(json_action["type"]);
    }
    if (json_action.containsKey("target_relay")) {
        action.target_relay = json_action["target_relay"];
    } else if (json_action.containsKey("relay_number")) {
        action.target_relay = json_action["relay_number"];
    } else if (json_action.containsKey("relay_ids")) {
        JsonArray relay_ids = json_action["relay_ids"].as<JsonArray>();
        if (!relay_ids.isNull() && relay_ids.size() > 0) {
            action.target_relay = relay_ids[0];
        }
    }
    if (json_action.containsKey("target_device_id")) {
        action.target_device_id = json_action["target_device_id"].as<String>();
    }
    if (json_action.containsKey("duration_ms")) {
        action.duration_ms = json_action["duration_ms"];
    } else if (json_action.containsKey("duration")) {
        action.duration_ms = static_cast<unsigned long>(json_action["duration"].as<unsigned long>()) * 1000UL;
    } else if (json_action.containsKey("duration_seconds")) {
        action.duration_ms = static_cast<unsigned long>(json_action["duration_seconds"].as<unsigned long>()) * 1000UL;
    }
    if (json_action.containsKey("value")) {
        action.value = json_action["value"];
    }
    if (json_action.containsKey("message")) {
        action.message = json_action["message"].as<String>();
    }
    if (json_action.containsKey("repeat")) {
        action.repeat = json_action["repeat"];
    }
    if (json_action.containsKey("repeat_interval_ms")) {
        action.repeat_interval_ms = json_action["repeat_interval_ms"];
    }

    return true;
}

void DecisionEngine::actionToJSON(const RuleAction& action, JsonObject& out, JsonDocument& doc) {
    (void)doc;
    out["type"] = actionTypeToString(action.type);
    out["target_relay"] = action.target_relay;
    out["target_device_id"] = action.target_device_id;
    out["duration_ms"] = action.duration_ms;
    out["value"] = action.value;
    out["message"] = action.message;
    out["repeat"] = action.repeat;
    out["repeat_interval_ms"] = action.repeat_interval_ms;
}

bool DecisionEngine::parseRuleFromJSON(const JsonObject& json_rule, DecisionRule& rule) {
    if (json_rule.isNull()) return false;

    if (json_rule.containsKey("id")) {
        rule.id = json_rule["id"].as<String>();
    } else if (json_rule.containsKey("rule_id")) {
        rule.id = json_rule["rule_id"].as<String>();
    }
    if (json_rule.containsKey("name")) {
        rule.name = json_rule["name"].as<String>();
    } else if (json_rule.containsKey("rule_name")) {
        rule.name = json_rule["rule_name"].as<String>();
    }
    if (json_rule.containsKey("description")) {
        rule.description = json_rule["description"].as<String>();
    } else if (json_rule.containsKey("rule_description")) {
        rule.description = json_rule["rule_description"].as<String>();
    }
    if (json_rule.containsKey("enabled")) {
        rule.enabled = json_rule["enabled"];
    }
    if (json_rule.containsKey("priority")) {
        rule.priority = json_rule["priority"];
    }
    if (json_rule.containsKey("trigger_type")) {
        rule.trigger_type = json_rule["trigger_type"].as<String>();
    }
    if (json_rule.containsKey("trigger_interval_ms")) {
        rule.trigger_interval_ms = json_rule["trigger_interval_ms"];
    }
    if (json_rule.containsKey("cooldown_ms")) {
        rule.cooldown_ms = json_rule["cooldown_ms"];
    }
    if (json_rule.containsKey("max_executions_per_hour")) {
        rule.max_executions_per_hour = json_rule["max_executions_per_hour"];
    }

    rule.actions.clear();
    rule.safety_checks.clear();

    JsonObject rule_body = json_rule;
    if (json_rule.containsKey("rule_json")) {
        rule_body = json_rule["rule_json"].as<JsonObject>();
    }

    if (rule_body.containsKey("condition")) {
        if (!parseConditionFromJSON(rule_body["condition"].as<JsonObject>(), rule.condition)) {
            return false;
        }
    } else if (rule_body.containsKey("conditions")) {
        JsonVariant conditions = rule_body["conditions"];
        if (conditions.is<JsonArray>()) {
            JsonArray cond_array = conditions.as<JsonArray>();
            if (cond_array.size() == 0) return false;
            if (cond_array.size() == 1) {
                if (!parseConditionFromJSON(cond_array[0].as<JsonObject>(), rule.condition)) {
                    return false;
                }
            } else {
                rule.condition.type = COMPOSITE;
                rule.condition.logic_operator = "AND";
                for (JsonObject cond_json : cond_array) {
                    RuleCondition sub;
                    if (parseConditionFromJSON(cond_json, sub)) {
                        rule.condition.sub_conditions.push_back(sub);
                    }
                }
            }
        } else if (conditions.is<JsonObject>()) {
            if (!parseConditionFromJSON(conditions.as<JsonObject>(), rule.condition)) {
                return false;
            }
        }
    } else if (json_rule.containsKey("condition")) {
        if (!parseConditionFromJSON(json_rule["condition"].as<JsonObject>(), rule.condition)) {
            return false;
        }
    } else {
        return false;
    }

    JsonArray actions_array;
    if (rule_body.containsKey("actions")) {
        actions_array = rule_body["actions"].as<JsonArray>();
    } else if (json_rule.containsKey("actions")) {
        actions_array = json_rule["actions"].as<JsonArray>();
    }

    if (!actions_array.isNull()) {
        for (JsonObject action_json : actions_array) {
            RuleAction action;
            if (parseActionFromJSON(action_json, action)) {
                rule.actions.push_back(action);
            }
        }
    }

    JsonArray safety_array;
    if (rule_body.containsKey("safety_checks")) {
        safety_array = rule_body["safety_checks"].as<JsonArray>();
    } else if (json_rule.containsKey("safety_checks")) {
        safety_array = json_rule["safety_checks"].as<JsonArray>();
    }

    if (!safety_array.isNull()) {
        for (JsonObject safety_json : safety_array) {
            SafetyCheck safety;
            safety.name = safety_json["name"] | "";
            safety.error_message = safety_json["error_message"] | "";
            safety.is_critical = safety_json["is_critical"] | false;
            if (safety_json.containsKey("condition")) {
                parseConditionFromJSON(safety_json["condition"].as<JsonObject>(), safety.condition);
            }
            rule.safety_checks.push_back(safety);
        }
    }

    if (rule_body.containsKey("interval_between_executions")) {
        rule.cooldown_ms = static_cast<unsigned long>(rule_body["interval_between_executions"].as<unsigned long>()) * 1000UL;
    }
    if (rule_body.containsKey("priority") && !json_rule.containsKey("priority")) {
        rule.priority = rule_body["priority"];
    }

    return !rule.id.isEmpty() && !rule.actions.empty();
}

void DecisionEngine::ruleToJSON(const DecisionRule& rule, JsonObject& out, JsonDocument& doc) {
    out["id"] = rule.id;
    out["name"] = rule.name;
    out["description"] = rule.description;
    out["enabled"] = rule.enabled;
    out["priority"] = rule.priority;
    out["trigger_type"] = rule.trigger_type;
    out["trigger_interval_ms"] = rule.trigger_interval_ms;
    out["cooldown_ms"] = rule.cooldown_ms;
    out["max_executions_per_hour"] = rule.max_executions_per_hour;

    JsonObject condition_json = out.createNestedObject("condition");
    conditionToJSON(rule.condition, condition_json, doc);

    JsonArray actions_array = out.createNestedArray("actions");
    for (const auto& action : rule.actions) {
        JsonObject action_json = actions_array.createNestedObject();
        actionToJSON(action, action_json, doc);
    }

    if (!rule.safety_checks.empty()) {
        JsonArray safety_array = out.createNestedArray("safety_checks");
        for (const auto& safety : rule.safety_checks) {
            JsonObject safety_json = safety_array.createNestedObject();
            safety_json["name"] = safety.name;
            safety_json["error_message"] = safety.error_message;
            safety_json["is_critical"] = safety.is_critical;
            JsonObject safety_cond = safety_json.createNestedObject("condition");
            conditionToJSON(safety.condition, safety_cond, doc);
        }
    }
}

// ===== ESTATÍSTICAS =====
void DecisionEngine::printStatistics() {
    Serial.println("\n📊 === ESTATÍSTICAS DO DECISION ENGINE ===");
    Serial.printf("🔄 Total de avaliações: %lu\n", total_evaluations);
    Serial.printf("⚡ Total de ações executadas: %lu\n", total_actions_executed);
    Serial.printf("🛡️ Total bloqueios de segurança: %lu\n", total_safety_blocks);
    Serial.printf("📋 Regras carregadas: %d\n", rules.size());
    Serial.printf("🧪 Modo dry-run: %s\n", dry_run_mode ? "ATIVADO" : "DESATIVADO");
    Serial.printf("⏱️ Intervalo de avaliação: %lu ms\n", evaluation_interval);
    Serial.println("============================================\n");
}

void DecisionEngine::printRuleStatus() {
    Serial.println("\n📋 === STATUS DAS REGRAS ===");
    for (const auto& rule : rules) {
        Serial.printf("🔹 %s (ID: %s)\n", rule.name.c_str(), rule.id.c_str());
        Serial.printf("   Status: %s | Prioridade: %d\n", 
                     rule.enabled ? "ATIVA" : "INATIVA", rule.priority);
        Serial.printf("   Execuções/hora: %lu/%lu\n", 
                     rule.execution_count_hour, rule.max_executions_per_hour);
        Serial.printf("   Última execução: %lu ms atrás\n", 
                     millis() - rule.last_execution);
    }
    Serial.println("==============================\n");
}

// ===== REGRAS PADRÃO (DEMONSTRAÇÃO) =====
void DecisionEngine::createDefaultRules() {
    // Regra 1: Controle de pH baixo
    DecisionRule ph_low_rule;
    ph_low_rule.id = "ph_low_control";
    ph_low_rule.name = "Correção pH Baixo";
    ph_low_rule.description = "Ativa bomba de pH+ quando pH < 5.8";
    ph_low_rule.enabled = true;
    ph_low_rule.priority = 80;
    ph_low_rule.trigger_type = "periodic";
    ph_low_rule.trigger_interval_ms = 30000;
    ph_low_rule.cooldown_ms = 300000; // 5 minutos
    ph_low_rule.max_executions_per_hour = 6;
    
    // Condição: pH < 5.8
    ph_low_rule.condition.type = SENSOR_COMPARE;
    ph_low_rule.condition.sensor_name = "ph";
    ph_low_rule.condition.op = OP_LESS_THAN;
    ph_low_rule.condition.value_min = 5.8;
    
    // Ação: Ligar bomba pH por 5 segundos
    RuleAction ph_action;
    ph_action.type = RELAY_PULSE;
    ph_action.target_relay = 2; // Bomba pH
    ph_action.duration_ms = 5000;
    ph_action.message = "Corrigindo pH baixo";
    ph_low_rule.actions.push_back(ph_action);
    
    // Safety: Não executar se nível de água baixo
    SafetyCheck water_level_check;
    water_level_check.name = "Verificação nível água";
    water_level_check.condition.type = SYSTEM_STATUS;
    water_level_check.condition.sensor_name = "water_level_ok";
    water_level_check.condition.value_min = 1;
    water_level_check.error_message = "Nível de água baixo";
    water_level_check.is_critical = false;
    ph_low_rule.safety_checks.push_back(water_level_check);
    
    rules.push_back(ph_low_rule);
    
    // Regra 2: Recirculação periódica
    DecisionRule circulation_rule;
    circulation_rule.id = "circulation_control";
    circulation_rule.name = "Recirculação Periódica";
    circulation_rule.description = "Liga bomba de circulação a cada 30 minutos por 10 minutos";
    circulation_rule.enabled = true;
    circulation_rule.priority = 60;
    circulation_rule.trigger_type = "periodic";
    circulation_rule.trigger_interval_ms = 1800000; // 30 minutos
    circulation_rule.cooldown_ms = 0;
    
    // Condição: Sempre verdadeira (sistema funcionando)
    circulation_rule.condition.type = SYSTEM_STATUS;
    circulation_rule.condition.sensor_name = "water_level_ok";
    circulation_rule.condition.value_min = 1;
    
    // Ação: Ligar bomba de circulação por 10 minutos
    RuleAction circ_action;
    circ_action.type = RELAY_PULSE;
    circ_action.target_relay = 6; // Bomba Circulação
    circ_action.duration_ms = 600000; // 10 minutos
    circ_action.message = "Recirculação periódica";
    circulation_rule.actions.push_back(circ_action);
    
    rules.push_back(circulation_rule);
    
    Serial.println("✅ Regras padrão criadas");
}
