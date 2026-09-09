#include "DecisionEngineIntegration.h"
#include "RelayCoordinator.h"
#include "ScriptRunner.h"
#include "Config.h"
#include <ArduinoJson.h>
#include <math.h>

// ===== CONSTRUTOR E DESTRUTOR =====
DecisionEngineIntegration::DecisionEngineIntegration(DecisionEngine* engine, HydroControl* hydro, SupabaseClient* supa, MasterSlaveManager* masterMgr) :
    engine(engine),
    hydroControl(hydro),
    supabase(supa),
    masterManager(masterMgr),
    relayCoordinator(nullptr),
    emergency_mode(false),
    manual_override_active(false),
    total_relay_commands(0),
    total_alerts_sent(0),
    total_supabase_updates(0),
    log_index(0) {
    
    // Limpar buffer de logs
    for (size_t i = 0; i < LOG_BUFFER_SIZE; i++) {
        execution_log[i] = "";
    }
}

DecisionEngineIntegration::~DecisionEngineIntegration() {
    end();
}

// ===== CONTROLE PRINCIPAL =====
bool DecisionEngineIntegration::begin() {
    Serial.println("🔗 Inicializando DecisionEngine Integration...");
    
    if (!engine || !hydroControl) {
        Serial.println("❌ Ponteiros inválidos para engine ou hydroControl");
        return false;
    }
    
    // Configurar callbacks do DecisionEngine
    engine->setRelayControlCallback([this](int relay, bool state, unsigned long duration) {
        this->handleRelayControl(relay, state, duration);
    });
    
    engine->setAlertCallback([this](const String& message, bool is_critical) {
        this->handleAlert(message, is_critical);
    });
    
    engine->setLogCallback([this](const String& event, const String& data) {
        this->handleLogEvent(event, data);
    });

    engine->setTankProcedureGateCallback([this](bool active) {
        if (hydroControl) {
            hydroControl->setTankProcedureActive(active);
        }
    });

    // ScriptRunner: YFB5 + recirc + roles drain/fill (sin EventBus).
    ScriptRunnerManager::instance().setFlowSessionCallbacks(
        [this]() {
            if (hydroControl) {
                hydroControl->resetFlowSession();
            }
        },
        [this]() -> float {
            return hydroControl ? hydroControl->getFlowSessionLiters() : 0.0f;
        });
    ScriptRunnerManager::instance().setRecircCallbacks(
        [this](bool starting) {
            if (hydroControl) {
                hydroControl->setProcedureRecircActive(starting);
            }
        },
        [this]() -> unsigned long {
            if (!hydroControl) {
                return 60UL;
            }
            const unsigned long sec = hydroControl->getTempoRecirculacaoSeconds();
            return sec > 0 ? sec : 60UL;
        });
    ScriptRunnerManager::instance().setHydraulicRoleResolver(
        [this](const String& role, String& outMac, int& outRelay) -> bool {
            if (!hydroControl) {
                return false;
            }
            if (role == "drain") {
                outMac = hydroControl->getDilutionDrainSlaveMac();
                outRelay = hydroControl->getDilutionDrainRelay();
                return outMac.length() > 0 && outRelay >= 0;
            }
            if (role == "fill") {
                outMac = hydroControl->getDilutionFillSlaveMac();
                outRelay = hydroControl->getDilutionFillRelay();
                return outMac.length() > 0 && outRelay >= 0;
            }
            return false;
        });
    
    // ✅ INTEGRAÇÃO ESP-NOW: Configurar MasterSlaveManager no DecisionEngine
    if (masterManager) {
        engine->setMasterManager(masterManager);
        Serial.println("✅ MasterSlaveManager configurado no DecisionEngine");
    } else {
        Serial.println("⚠️ MasterSlaveManager não disponível - comandos ESP-NOW desabilitados");
    }
    
    Serial.println("✅ DecisionEngine Integration inicializada");
    Serial.printf("🔧 Callbacks configurados\n");
    Serial.printf("🛡️ Modo emergência: %s\n", emergency_mode ? "ATIVO" : "INATIVO");
    
    return true;
}

void DecisionEngineIntegration::loop() {
    // Atualizar estado do sistema com dados dos sensores
    updateSystemStateFromSensors();
    
    // Verificar interlocks de segurança
    performSafetyChecks();
    
    // Enviar telemetria HTTPS só se fallback cloud HTTPS estiver ativo.
    // Com MQTT_HYDRO_ONLY / HTTPS_RUNTIME_FALLBACK_DISABLED a tabela "telemetry" não existe → 404.
#if !(ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED)
    static unsigned long last_telemetry = 0;
    if (millis() - last_telemetry >= 60000) {
        sendTelemetryToSupabase();
        last_telemetry = millis();
    }
#endif
}

void DecisionEngineIntegration::end() {
    Serial.println("🔗 Finalizando DecisionEngine Integration...");
    locked_relays.clear();
}

// ===== CONTROLE DE MODO =====
void DecisionEngineIntegration::setEmergencyMode(bool enabled) {
    if (emergency_mode == enabled) {
        return;
    }
    emergency_mode = enabled;
    Serial.printf("🚨 Modo emergência %s\n", enabled ? "ATIVADO" : "DESATIVADO");

    if (enabled) {
        // OFF explícito tipado — sem reentrar em emergencyShutdown (evita ON/OFF por toggle)
        shutOffCirculationActuator();
    }

    addToExecutionLog("Emergency mode " + String(enabled ? "ENABLED" : "DISABLED"));
}

void DecisionEngineIntegration::setManualOverride(bool enabled) {
    if (manual_override_active != enabled) {
        manual_override_active = enabled;
        Serial.printf("🔧 Override manual %s\n", enabled ? "ATIVO" : "INATIVO");
        
        if (enabled) {
            engine->setDryRunMode(true);
            Serial.println("⚠️ DecisionEngine em modo dry-run devido ao override manual");
        } else {
            engine->setDryRunMode(false);
            Serial.println("✅ DecisionEngine retomou operação normal");
        }
        
        addToExecutionLog("Manual override " + String(enabled ? "ENABLED" : "DISABLED"));
    }
}

// ===== CONTROLE DE RELÉS =====
void DecisionEngineIntegration::lockRelay(int relay_id) {
    if (relay_id >= 0 && relay_id < MAX_RELAYS) {
        for (int locked : locked_relays) {
            if (locked == relay_id) return; // Já está travado
        }
        locked_relays.push_back(relay_id);
        Serial.printf("🔒 Relé %d travado\n", relay_id);
        addToExecutionLog("Relay " + String(relay_id) + " LOCKED");
    }
}

void DecisionEngineIntegration::unlockRelay(int relay_id) {
    for (auto it = locked_relays.begin(); it != locked_relays.end(); ++it) {
        if (*it == relay_id) {
            locked_relays.erase(it);
            Serial.printf("🔓 Relé %d destravado\n", relay_id);
            addToExecutionLog("Relay " + String(relay_id) + " UNLOCKED");
            break;
        }
    }
}

void DecisionEngineIntegration::unlockAllRelays() {
    int count = locked_relays.size();
    locked_relays.clear();
    Serial.printf("🔓 Todos os relés destravados (%d relés)\n", count);
    addToExecutionLog("All relays UNLOCKED (" + String(count) + " relays)");
}

bool DecisionEngineIntegration::isRelayLocked(int relay_id) {
    for (int locked : locked_relays) {
        if (locked == relay_id) return true;
    }
    return false;
}

// ===== ATUALIZAÇÃO DE ESTADO =====
void DecisionEngineIntegration::updateSystemStateFromSensors() {
    if (!hydroControl) return;
    
    SystemState state;
    
    // Dados dos sensores
    state.ph = hydroControl->getpH();
    state.ec = hydroControl->getEC();
    state.tds = state.ec;
    state.temp_water = hydroControl->getWaterTemp();
    state.temp_environment = hydroControl->getTemperature();
    state.water_level_ok = hydroControl->isWaterLevelOk();
    state.level_1 = hydroControl->isLevelWet(1);
    state.level_2 = hydroControl->isLevelWet(2);
    state.level_3 = hydroControl->isLevelWet(3);
    state.level_4 = hydroControl->isLevelWet(4);
    const char* wl = hydroControl->getWaterLevelAggregate();
    strncpy(state.water_level, wl ? wl : "vazio", sizeof(state.water_level) - 1);
    state.water_level[sizeof(state.water_level) - 1] = '\0';
    
    // Estados dos relés
    bool* relay_states = hydroControl->getRelayStates();
    for (int i = 0; i < MAX_RELAYS; i++) {
        state.relay_states[i] = relay_states[i];
    }
    
    // Status do sistema
    state.wifi_connected = WiFi.isConnected();
    state.supabase_connected = (supabase && supabase->isReady());
    state.auto_ph_active = hydroControl->isAutoPHEnabled();
    state.uptime = millis();
    state.free_heap = ESP.getFreeHeap();
    state.last_update = millis();
    
    // Atualizar o DecisionEngine
    engine->updateSystemState(state);
}

SystemState DecisionEngineIntegration::getCurrentSystemState() {
    SystemState state;
    updateSystemStateFromSensors();
    return state;
}

// ===== CALLBACKS =====
void DecisionEngineIntegration::handleRelayControl(int relay, bool state, unsigned long duration) {
    total_relay_commands++;
    
    // Verificar se está em modo emergência
    if (emergency_mode) {
        Serial.printf("🚨 Comando de relé bloqueado - modo emergência ativo (relé %d)\n", relay);
        addToExecutionLog("Relay command BLOCKED - emergency mode (relay " + String(relay) + ")");
        return;
    }
    
    // Verificar se o relé está travado
    if (isRelayLocked(relay)) {
        Serial.printf("🔒 Comando de relé bloqueado - relé travado (relé %d)\n", relay);
        addToExecutionLog("Relay command BLOCKED - relay locked (relay " + String(relay) + ")");
        return;
    }
    
    // Validar comando
    if (!validateRelayCommand(relay, state, duration)) {
        Serial.printf("❌ Comando de relé inválido (relé %d, estado %d, duração %lu)\n", relay, state, duration);
        addToExecutionLog("Relay command INVALID (relay " + String(relay) + ")");
        return;
    }
    
    // Executar comando
    if (relayCoordinator) {
        String actionStr = state ? "on" : "off";
        const int durationSec = duration > 0 ? (int)(duration / 1000) : 0;
        if (relayCoordinator->actuateLocal(RelayOwner::DecisionRule, relay, actionStr, durationSec)) {
            Serial.printf("⚡ [COORD] Relé %d acionado owner=DecisionRule\n", relay);
            addToExecutionLog("Relay " + String(relay) + " via RelayCoordinator");
        }
    } else if (duration > 0) {
        hydroControl->setRelay(relay, state, duration / 1000); // HydroControl usa segundos
        Serial.printf("⚡ Relé %d %s por %lu ms\n", relay, state ? "ligado" : "desligado", duration);
        addToExecutionLog("Relay " + String(relay) + " pulsed for " + String(duration) + "ms");
    } else {
        hydroControl->setRelay(relay, state, 0); // OFF/ON explícito (nunca toggle)
        Serial.printf("⚡ Relé %d %s\n", relay, state ? "ligado" : "desligado");
        addToExecutionLog("Relay " + String(relay) + " " + (state ? "ON" : "OFF"));
    }
    
    // Atualizar Supabase se disponível
    if (supabase && supabase->isReady()) {
        updateSupabaseWithRuleExecution("relay_control", "relay_" + String(relay) + "_" + (state ? "on" : "off"), true);
    }
}

void DecisionEngineIntegration::handleAlert(const String& message, bool is_critical) {
    total_alerts_sent++;
    
    if (is_critical) {
        Serial.println("🚨 ALERTA CRÍTICO: " + message);
        addToExecutionLog("CRITICAL ALERT: " + message);
        
        // Para alertas críticos, considerar modo emergência
        if (!emergency_mode) {
            Serial.println("🚨 Ativando modo emergência devido a alerta crítico");
            setEmergencyMode(true);
        }
    } else {
        Serial.println("🔔 Alerta: " + message);
        addToExecutionLog("ALERT: " + message);
    }
    
    // Enviar para Supabase se disponível
    if (supabase && supabase->isReady()) {
        DynamicJsonDocument doc(512);
        doc["device_id"] = WiFi.macAddress();
        doc["alert_type"] = is_critical ? "critical" : "warning";
        doc["message"] = message;
        doc["timestamp"] = millis();
        
        String json_str;
        serializeJson(doc, json_str);
        
        // Assumindo que existe uma tabela de alertas
        supabase->insert("alerts", json_str);
        total_supabase_updates++;
    }
}

void DecisionEngineIntegration::handleLogEvent(const String& event, const String& data) {
    String log_entry = "[" + event + "] " + data;
    Serial.println("📝 " + log_entry);
    addToExecutionLog(log_entry);
    
    // Log detalhado pode ir para Supabase também
    if (supabase && supabase->isReady() && event != "RULE_EXECUTION") {
        // Evitar spam de logs de execução de regras
        updateSupabaseWithRuleExecution(event, data, true);
    }
}

// ===== VALIDAÇÃO E SEGURANÇA =====
bool DecisionEngineIntegration::validateRelayCommand(int relay_id, bool state, unsigned long duration) {
    (void)state;
    // Verificar ID do relé
    if (relay_id < 0 || relay_id >= MAX_RELAYS) {
        Serial.printf("❌ ID de relé inválido: %d\n", relay_id);
        return false;
    }
    
    // Verificar duração máxima (24 horas)
    if (duration > 86400000) {
        Serial.printf("❌ Duração muito longa: %lu ms\n", duration);
        return false;
    }
    
    // Heap / emergência — nível de água NÃO bloqueia recirculação (só Auto EC/pH)
    if (!isSystemHealthy()) {
        Serial.println("❌ Sistema não está saudável para executar comandos");
        return false;
    }

    return true;
}

void DecisionEngineIntegration::performSafetyChecks() {
    static unsigned long last_check = 0;
    if (millis() - last_check < 5000) return; // A cada 5 segundos
    last_check = millis();

    // Nível de água: NÃO apaga recirculação tipada (evita pelea con fn_recirculacao_continua).
    // Auto EC/pH siguen pausados vía HydroControl::isAutoDosingPausedByInterlock / tankLevelOk.

    // Temp fora de faixa só se leitura for válida (NaN/desconhecida = OK)
    if (!checkTemperatureInterlock()) {
        Serial.println("⚠️ [SAFETY] Temperatura fora da faixa — aviso (sem emergência)");
    }

    // Heap crítico: hard — emergência real
    if (!checkMemoryInterlock() && !emergency_mode) {
        Serial.println("🚨 [SAFETY] Heap baixo — emergência");
        emergencyShutdown("Low memory");
    }
}

void DecisionEngineIntegration::emergencyShutdown(const String& reason) {
    Serial.println("🚨 PARADA DE EMERGÊNCIA: " + reason);

    shutOffCirculationActuator();

    if (!emergency_mode) {
        emergency_mode = true;
        Serial.println("🚨 Modo emergência ATIVADO");
        addToExecutionLog("EMERGENCY SHUTDOWN: " + reason);
    }

    // Sem POST HTTPS aqui: em stress de heap o SSL (BIGNUM) falha e piora a situação
    Serial.println("ℹ️ [SAFETY] emergency_shutdown registado só em serial (sem HTTPS)");
}

void DecisionEngineIntegration::shutOffCirculationActuator() {
    if (!relayCoordinator || !relayCoordinator->isCirculationConfigured()) {
        Serial.println("ℹ️ [SAFETY] Sem recirculação tipada — skip OFF");
        return;
    }

    const RelayTarget target = relayCoordinator->getCirculationTarget();
    if (target.isLocal) {
        if (relayCoordinator->actuateLocal(RelayOwner::DecisionRule, target.relay, "off", 0, "safety_off")) {
            Serial.printf("🔌 [SAFETY] Recirculação local R%d → OFF\n", target.relay);
        }
        return;
    }

    const uint32_t cmdId = relayCoordinator->actuateSlave(
        RelayOwner::DecisionRule,
        target.slaveMac,
        target.relay,
        "off",
        0,
        0,
        0,
        "",
        "safety_off");
    if (cmdId > 0) {
        Serial.printf("🔌 [SAFETY] Recirculação slave R%d → OFF (cmd=%lu)\n",
                      target.relay, static_cast<unsigned long>(cmdId));
    }
}

bool DecisionEngineIntegration::isLocalCirculationRelay(int relay_id) const {
    if (!relayCoordinator || !relayCoordinator->isCirculationConfigured()) {
        return false;
    }
    const RelayTarget target = relayCoordinator->getCirculationTarget();
    return target.isLocal && target.relay == relay_id;
}

// ===== INTERLOCKS DE SEGURANÇA =====
bool DecisionEngineIntegration::checkWaterLevelInterlock() {
    if (!hydroControl) return false;
    return hydroControl->isWaterLevelOk();
}

bool DecisionEngineIntegration::checkTemperatureInterlock() {
    if (!hydroControl) return true; // Se não há sensor, assumir OK

    const float water_temp = hydroControl->getWaterTemp();
    const float env_temp = hydroControl->getTemperature();

    // NaN / não finito = sensor ainda não leu → não tratar como falha
    auto inRangeOrUnknown = [](float t, float lo, float hi) -> bool {
        if (!isfinite(t)) {
            return true;
        }
        return (t >= lo && t <= hi);
    };

    const bool water_temp_ok = inRangeOrUnknown(water_temp, 15.0f, 35.0f);
    const bool env_temp_ok = inRangeOrUnknown(env_temp, 10.0f, 40.0f);

    return water_temp_ok && env_temp_ok;
}

bool DecisionEngineIntegration::checkPowerSupplyInterlock() {
    // Verificar tensão de alimentação via ADC (se implementado)
    // Por enquanto, assumir OK
    return true;
}

bool DecisionEngineIntegration::checkMemoryInterlock() {
    uint32_t free_heap = ESP.getFreeHeap();
    return free_heap > 15000; // Mínimo 15KB
}

// ===== TELEMETRIA =====
void DecisionEngineIntegration::sendTelemetryToSupabase() {
#if ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED
    return;  // hydro via MQTT + bridge; não POST /telemetry (404)
#endif
    if (!supabase || !supabase->isReady()) return;
    
    DynamicJsonDocument doc(1024);
    doc["device_id"] = WiFi.macAddress();
    doc["timestamp"] = millis();
    doc["uptime"] = millis() / 1000;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["emergency_mode"] = emergency_mode;
    doc["manual_override"] = manual_override_active;
    doc["locked_relays_count"] = locked_relays.size();
    doc["total_relay_commands"] = total_relay_commands;
    doc["total_alerts"] = total_alerts_sent;
    
    // Dados dos sensores
    if (hydroControl) {
        doc["ph"] = hydroControl->getpH();
        doc["ec"] = hydroControl->getEC();
        doc["water_temp"] = hydroControl->getWaterTemp();
        doc["env_temp"] = hydroControl->getTemperature();
        doc["water_level_ok"] = hydroControl->isWaterLevelOk();
        doc["sensors_ok"] = hydroControl->areSensorsWorking();
    }
    
    String json_str;
    serializeJson(doc, json_str);
    
    if (supabase->insert("telemetry", json_str)) {
        total_supabase_updates++;
        Serial.println("📊 Telemetria enviada para Supabase");
    } else {
        Serial.println("❌ Erro ao enviar telemetria");
    }
}

// ===== MÉTODOS AUXILIARES =====
void DecisionEngineIntegration::addToExecutionLog(const String& log_entry) {
    String timestamp = String(millis() / 1000) + "s: ";
    execution_log[log_index] = timestamp + log_entry;
    log_index = (log_index + 1) % LOG_BUFFER_SIZE;
}

bool DecisionEngineIntegration::isSystemHealthy() {
    // Nível/temp são soft-interlocks (não bloqueiam o DE globalmente).
    // Emergência + heap crítico sim.
    return checkMemoryInterlock() && !emergency_mode;
}

void DecisionEngineIntegration::updateSupabaseWithRuleExecution(const String& rule_id, const String& action, bool success) {
    if (!supabase || !supabase->isReady()) return;
    
    DynamicJsonDocument doc(512);
    doc["device_id"] = WiFi.macAddress();
    doc["rule_id"] = rule_id;
    doc["action"] = action;
    doc["success"] = success;
    doc["timestamp"] = millis();
    
    String json_str;
    serializeJson(doc, json_str);
    
    supabase->insert("rule_executions", json_str);
    total_supabase_updates++;
}

String DecisionEngineIntegration::getExecutionLogJSON() {
    DynamicJsonDocument doc(2048);
    JsonArray logs = doc.createNestedArray("execution_log");
    
    // Adicionar logs em ordem cronológica
    for (size_t i = 0; i < LOG_BUFFER_SIZE; i++) {
        size_t index = (log_index + i) % LOG_BUFFER_SIZE;
        if (!execution_log[index].isEmpty()) {
            logs.add(execution_log[index]);
        }
    }
    
    doc["emergency_mode"] = emergency_mode;
    doc["manual_override"] = manual_override_active;
    doc["total_commands"] = total_relay_commands;
    doc["total_alerts"] = total_alerts_sent;
    doc["locked_relays"] = locked_relays.size();
    
    String result;
    serializeJson(doc, result);
    return result;
}

void DecisionEngineIntegration::printIntegrationStatistics() {
    Serial.println("\n🔗 === ESTATÍSTICAS DE INTEGRAÇÃO ===");
    Serial.printf("⚡ Comandos de relé executados: %lu\n", total_relay_commands);
    Serial.printf("🔔 Alertas enviados: %lu\n", total_alerts_sent);
    Serial.printf("☁️ Atualizações Supabase: %lu\n", total_supabase_updates);
    Serial.printf("🚨 Modo emergência: %s\n", emergency_mode ? "ATIVO" : "INATIVO");
    Serial.printf("🔧 Override manual: %s\n", manual_override_active ? "ATIVO" : "INATIVO");
    Serial.printf("🔒 Relés travados: %d\n", locked_relays.size());
    Serial.printf("🛡️ Sistema saudável: %s\n", isSystemHealthy() ? "SIM" : "NÃO");
    Serial.println("=====================================\n");
}
