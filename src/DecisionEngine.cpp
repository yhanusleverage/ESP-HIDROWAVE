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
        ruleToJSON(rule, doc); // Implementar este método
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
