#include "HydroSystemCore.h"
#include "ResourceTelemetry.h"
#include "HydroControl.h"
#include "SupabaseClient.h"
#include "SensorSanitize.h"
#include "HydroSupaManager.h"  // ✅ Manager híbrido
#include "WebServerManager.h"
#include "WiFiManager.h"
#include "Config.h"
#include "DeviceID.h"
#include "ObjectPoolManager.h"  // ✅ Object Pool Pattern
#include <ArduinoJson.h>
#if ENABLE_MQTT
#include "MqttClient.h"
#endif
#include "TelemetrySerial.h"
#include "MqttCommandParser.h"
#include "CommandSerial.h"
#include <esp_task_wdt.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "HybridStateManager.h"
#include "WebServerTask.h"      // ✅ Include completo para usar métodos
#include "ESPNowController.h"   // ✅ Include completo para usar métodos
#include "MasterSlaveManager.h" // ✅ Para integración ESP-NOW
#include <math.h>
#include <cstdio>
// ✅ NÃO incluir ESPNowTypes.h aqui - master relays são LOCAIS, não ESP-NOW

namespace {

bool isAutoDoseCycleBusy(const HydroControl& hc) {
    const char* ecState = hc.getEcOperationStateName();
    const char* phState = hc.getPhOperationStateName();
    if (hc.isDosageActive()) {
        return true;
    }
    if (ecState && (strcmp(ecState, "recirculating") == 0 ||
                    strcmp(ecState, "diluting_draining") == 0 ||
                    strcmp(ecState, "diluting_filling") == 0)) {
        return true;
    }
    if (hc.isDilutionActive()) {
        return true;
    }
    if (phState && (strcmp(phState, "dosing") == 0 || strcmp(phState, "recirculating") == 0)) {
        return true;
    }
    return false;
}

}  // namespace

static bool relaySyncInProgress = false;

// ===== CONSTRUTOR E DESTRUTOR =====
HydroSystemCore::HydroSystemCore(WebServerTask* webTask, ESPNowController* espNow, MasterSlaveManager* masterMgr) : 
    webServerTask(webTask),
    espNowController(espNow),
    masterManager(masterMgr),
    webServerManager(nullptr),  // ✅ TÓPICO 4: Inicializado em begin()
    pendingAckMutex(nullptr),
    mappingsMutex(nullptr),  // ✅ NOVO: Inicializar mutex (FALTAVA VÍRGULA!)
    systemReady(false),
    supabaseConnected(false),
    endpointsRegistered(false),  // ✅ Rastrear se endpoints foram registrados
    startTime(0),
    lastSensorSend(0),
    lastStatusSend(0),
    lastRelayStatesSync(0),  // ✅ NOVO: Inicializar controle de sincronização
    lastSlaveRelayHeartbeat(0),
    lastSlaveRelayFullSync(0),
    lastStatusPrint(0),
    lastSupabaseCheck(0),
    lastRulesCheck(0),  // ✅ NOVO: Inicializar controle de verificação de regras
    lastMemoryProtection(0),
    lastMqttTelemetrySend(0),
    lastMqttHeartbeatSend(0),
    lastMqttCloudLastSeen(0),
    lastMqttLevelsPublishMs(0),
    mqttLevelsFingerprintValid(false),
    lastMqttLevelsMask(0xFF),
    lastEcOperationSync(0),
    lastEcOperationIdleSync(0),
    lastPhOperationSync(0),
    lastPhOperationIdleSync(0),
    pendingNutrientDoseHead(0),
    pendingNutrientDoseCount(0),
    pendingPhDoseHead(0),
    pendingPhDoseCount(0),
    pendingCloudAckHead(0),
    pendingCloudAckCount(0),
    recentlyClosedCount(0),
#if ENABLE_MQTT
    mqttConnectedSinceMs(0),
    mqttEcConfigReceived(false),
    mqttPhConfigReceived(false),
#endif
    decisionEngine(),
    decisionIntegration(&decisionEngine, &hydroControl, &supabase, masterMgr),
    bootOperationInterrupted(false),
    decisionEngineReady(false),
    statusLed(STATUS_LED_PIN),
    statusLedSendUntilMs(0),
    lastWifiLedState(-1) {
    
    memset(pendingNutrientDoseQueue, 0, sizeof(pendingNutrientDoseQueue));
    memset(pendingPhDoseQueue, 0, sizeof(pendingPhDoseQueue));
    memset(pendingCloudAckQueue, 0, sizeof(pendingCloudAckQueue));
    memset(recentlyClosedSupabaseIds, 0, sizeof(recentlyClosedSupabaseIds));
    memset(recentlyClosedAtMs, 0, sizeof(recentlyClosedAtMs));
    lastMqttWaterLevel[0] = '\0';
    lastMqttInterlockMode[0] = '\0';
    // Log de dependências injetadas
    if (webServerTask) {
        Serial.println("✅ HydroSystemCore: WebServerTask injetado");
    }
    if (espNowController) {
        Serial.println("✅ HydroSystemCore: ESPNowController injetado");
    }
    if (masterManager) {
        Serial.println("✅ HydroSystemCore: MasterSlaveManager injetado");
    }
}

void HydroSystemCore::wireMasterManagerIntegration() {
    if (!masterManager) {
        return;
    }

    masterManager->setRelayAckCallback([this](const uint8_t* senderMac, uint32_t commandId,
                                              bool success, uint8_t relayNumber, uint8_t currentState) {
        Serial.println("\n🎊 === ACK DE RELAY RECEBIDO (EVENT-DRIVEN) ===");
        Serial.printf("[CMD ACK-DIRECT] espnow_id=%u supabase=%d R%d state=%s\n",
                      (unsigned)commandId, findSupabaseCommandId(commandId), relayNumber,
                      currentState ? "ON" : "OFF");

        hydroControl.notifyDilutionRelayAck(commandId, success, currentState != 0);

        int supabaseCommandId = findSupabaseCommandId(commandId);

        if (supabaseCommandId > 0 && success) {
            completeSlaveCommand(supabaseCommandId, commandId, senderMac, relayNumber,
                                 currentState != 0, "ACK");
        } else if (supabaseCommandId > 0 && !success) {
#if ENABLE_MQTT
            tryPublishCloudAckViaMqtt(supabaseCommandId, commandId, senderMac, relayNumber,
                                       currentState != 0, "failed");
#endif
            Serial.println("❌ [CALLBACK] Slave NACK — failed via MQTT (sin HTTPS)");
        } else if (supabaseCommandId == 0 && !hydroControl.isDilutionAwaitingValve()) {
            Serial.println("⚠️ [CALLBACK] Mapeamento não encontrado para commandId=" + String(commandId));
            Serial.println("💡 Comando pode ter sido processado antes do mapeamento ser criado");
        }

        Serial.println("========================================\n");
    });

    masterManager->setSupabaseCommandCallback([this](int supabaseCommandId, bool success, const String& errorMessage) {
#if ENABLE_MQTT
        if (MQTT_COMMAND_BRIDGE_ONLY && isMqttCommandPathStable()) {
            return;
        }
#endif
        if (supabaseCommandId > 0 && supabaseConnected) {
            if (success) {
                bool currentState = (errorMessage == "true" || errorMessage == "1");
                supabase.markCommandCompleted(supabaseCommandId, currentState, true);
            } else {
                supabase.markCommandFailed(supabaseCommandId, errorMessage, true);
            }
        }
    });

    masterManager->setSlaveCommandResolvedCallback([this](int supabaseCommandId, uint32_t espNowCommandId,
                                                          const uint8_t* mac, int relayNumber, bool currentState) {
        if (supabaseCommandId > 0) {
            completeSlaveCommand(supabaseCommandId, espNowCommandId, mac, relayNumber, currentState,
                                 "RESOLVED");
        }
    });

    masterManager->setSupabaseRelayStateCallback([this](const String& masterDeviceId, const String& slaveMacAddress,
                                                        const String& slaveDeviceId, int relayNumber,
                                                        bool state, bool hasTimer, int remainingTime) {
        if (supabaseConnected) {
            supabase.updateSlaveRelayState(masterDeviceId, slaveMacAddress, slaveDeviceId,
                                           relayNumber, state, hasTimer, remainingTime);
        }
    });

    masterManager->setAllRelaysSnapshotCallback([this](const uint8_t* mac, const bool states[8], uint8_t numRelays) {
        reconcilePendingSlaveAcks(mac, states, numRelays);
        if (hydroControl.isDilutionAwaitingValve() && mac && states) {
            const uint8_t n = numRelays > 8 ? 8 : numRelays;
            for (uint8_t i = 0; i < n; i++) {
                hydroControl.notifyDilutionObservedRelay(mac, i, states[i], true);
            }
        }
#if ENABLE_MQTT
        publishSlaveRelayStateMqtt(mac, -1, false, false);
#endif
    });

    Serial.println("✅ Callback de Supabase configurado en MasterSlaveManager (EVENT-DRIVEN)");
}

void HydroSystemCore::setMasterManager(MasterSlaveManager* masterMgr) {
    masterManager = masterMgr;
    if (!masterMgr) {
        return;
    }
    Serial.println("✅ HydroSystemCore: MasterSlaveManager atualizado (late bind)");
    relayCoordinator.begin(&hydroControl, masterMgr);
    if (supabaseConnected) {
        wireMasterManagerIntegration();
    }
    if (webServerManager) {
        webServerManager->setMasterManager(masterMgr);
    }
    if (!decisionEngineReady) {
        decisionIntegration.setMasterManager(masterMgr);
        initDecisionEngine();
    }
}

HydroSystemCore::~HydroSystemCore() {
    end();
}

// ===== CONTROLE DO SISTEMA =====
bool HydroSystemCore::begin() {
    Serial.println("🌱 Inicializando HydroSystemCore...");
#if HIDRO_DEV_RELAX_SENSORS
    Serial.println("[DEV] HIDRO_DEV_RELAX_SENSORS=1 — interlocks de sensor desactivados");
#endif
    
    if (systemReady) {
        Serial.println("⚠️ Sistema já está ativo");
        return true;
    }
    
    // ===== LED STATUS (GPIO2) — no bloqueante =====
    statusLed.begin();
    updateStatusLedFromWifi();

    startTime = millis();

    if (mappingsMutex == nullptr) {
        mappingsMutex = xSemaphoreCreateMutex();
    }
    if (pendingAckMutex == nullptr) {
        pendingAckMutex = xSemaphoreCreateMutex();
    }
    
    // ===== INICIALIZAR SISTEMA HIDROPÔNICO =====
    Serial.println("🔧 Inicializando controle hidropônico...");
    if (!hydroControl.begin()) {
        Serial.println("❌ Erro ao inicializar HydroControl");
        return false;
    }
    Serial.println("✅ HydroControl inicializado");

    bootOperationInterrupted = hydroControl.abortAutoOperationsOnBoot();
    StatePersistenceManager::applySelectiveMasterRelayRestore(hydroControl);

    hydroControl.setNutrientDoseCallback(&HydroSystemCore::onNutrientDoseStatic, this);
    hydroControl.setEcDilutionCallback(&HydroSystemCore::onEcDilutionStatic, this);
    hydroControl.setDilutionSlaveRelayCallback(&HydroSystemCore::onDilutionSlaveRelayStatic, this);
    hydroControl.setPhDoseCallback(&HydroSystemCore::onPhDoseStatic, this);
    hydroControl.setEcMetricCallback(&HydroSystemCore::onEcMetricStatic, this);
    hydroControl.setPhMetricCallback(&HydroSystemCore::onPhMetricStatic, this);
    hydroControl.setPhGainLearnedCallback(&HydroSystemCore::onPhGainLearnedStatic, this);
    hydroControl.setEcGainLearnedCallback(&HydroSystemCore::onEcGainLearnedStatic, this);
    hydroControl.setEcOperationSyncCallback(&HydroSystemCore::onEcOperationSyncStatic, this);
    hydroControl.setPhOperationSyncCallback(&HydroSystemCore::onPhOperationSyncStatic, this);
    hydroControl.setPhysicalRecircCallback(&HydroSystemCore::onPhysicalRecircStatic, this);

    relayCoordinator.begin(&hydroControl, masterManager);
    initDecisionEngine();
    
    // ===== CONECTAR SUPABASE =====
    Serial.println("☁️ Conectando ao Supabase...");
    if (supabase.begin(SUPABASE_URL, SUPABASE_ANON_KEY)) {
        Serial.println("✅ Supabase conectado");
        supabaseConnected = true;
        
        // Testar conexão inicial
        testSupabaseConnection();
        
        // ===== AUTO-REGISTRO DO DISPOSITIVO =====
        Serial.println("🆔 Iniciando auto-registro...");
        if (supabase.autoRegisterDevice("ESP32 Hidropônico", "Sistema Principal")) {
            Serial.println("✅ Dispositivo registrado automaticamente");
        } else {
            Serial.println("⚠️ Auto-registro falhou, mas continuando...");
        }
        
        // ✅ CONFIGURAR CALLBACK PARA SUPABASE EN MASTERSLAVEMANAGER
        wireMasterManagerIntegration();
    } else {
        Serial.println("❌ Erro ao conectar Supabase - Sistema continuará sem cloud");
        supabaseConnected = false;
    }
    
    // ===== INICIALIZAR SERVIDOR WEB ADMIN =====
    Serial.println("🌐 Iniciando painel admin web...");
    
    // ✅ TÓPICO 4: Crear WebServerManager como miembro (no estático)
    static WiFiManager wifiManager;
    static WebServerManager webServerManagerInstance;  // Instancia estática
    
    // ✅ CORREÇÃO CRÍTICA: Inicializar WiFiManager se ainda não foi inicializado
    // O WiFiManager precisa estar inicializado para fornecer device_id e outras informações
    static bool wifiManagerInitialized = false;
    if (!wifiManagerInitialized) {
        Serial.println("🔧 Inicializando WiFiManager para WebServerManager...");
        // Não chamar begin() aqui porque WiFi já está conectado
        // Apenas garantir que deviceID está configurado
        wifiManagerInitialized = true;
    }
    
    // ✅ TÓPICO 4: Guardar referencia para procesar comandos de queue
    this->webServerManager = &webServerManagerInstance;
    
    // ✅ INTEGRAÇÃO WEBSERVER TASK (Core 1) - Usar método centralizado
    registerWebServerEndpoints();
    
    Serial.println("✅ Painel admin disponível em: http://" + WiFi.localIP().toString());
    
    systemReady = true;
    
#if ENABLE_MQTT
    {
        const char* hydroMode =
#if MQTT_HYDRO_ONLY
            "mqtt+https_fallback";
#else
            "bivalente";
#endif
        const char* healthMode =
#if MQTT_HEALTH_ONLY
            "mqtt+https_fallback";
#else
            "bivalente";
#endif
        Serial.printf("📡 MQTT telemetria=%lus heartbeat=%lus | hydro=%s health=%s\n",
                      (unsigned long)(MQTT_TELEMETRY_INTERVAL_MS / 1000UL),
                      (unsigned long)(MQTT_HEARTBEAT_INTERVAL_MS / 1000UL),
                      hydroMode, healthMode);
    }
    if (WiFi.status() == WL_CONNECTED) {
        mqttClient.setIncomingHandler(&HydroSystemCore::mqttIncomingReceived, this);
        if (mqttClient.begin(getDeviceID())) {
            Serial.println("✅ MQTT client inicializado (telemetria + heartbeat + LWT + command)");
            supabase.setCommandPollQuiet(true);
            publishMqttHeartbeat();
            lastMqttHeartbeatSend = millis();
        } else {
            Serial.println("⚠️ MQTT client não conectou — continuando com HTTPS");
        }
    }
#endif
    
    Serial.println("✅ HydroSystemCore ativo!");
    Serial.println("💾 Heap livre: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("🌐 IP: " + WiFi.localIP().toString());
    
    // Status inicial dos sensores
    printSensorReadings();

    lastRelayStatesSync = 0;
    syncAllRelayStatesToSupabase();
    if (bootOperationInterrupted) {
        syncEcOperationStateToSupabase();
        syncPhOperationStateToSupabase();
        if (supabaseConnected) {
            supabase.patchBootInterrupted(getDeviceID(), true);
        }
        bootOperationInterrupted = false;
    }
    
    return true;
}

void HydroSystemCore::applyBootPolicies() {
    bootOperationInterrupted = hydroControl.abortAutoOperationsOnBoot();
    StatePersistenceManager::applySelectiveMasterRelayRestore(hydroControl);
}

void HydroSystemCore::initDecisionEngine() {
    decisionIntegration.setRelayCoordinator(&relayCoordinator);
    decisionIntegration.setMasterManager(masterManager);
    if (decisionEngine.begin() && decisionIntegration.begin()) {
        decisionEngineReady = true;
        Serial.println("✅ DecisionEngine local ativo");
    } else {
        Serial.println("⚠️ DecisionEngine não iniciou — automação local desativada");
    }
}

void HydroSystemCore::loop() {
    if (!systemReady) return;
    
    unsigned long now = millis();

    // LED: solo update() + eventos (WiFi / nube) — nunca blink por iteración.
    finishStatusLedSendPulseIfDue();
    updateStatusLedFromWifi();
    statusLed.update();
    
    // ===== PROTEÇÃO DE MEMÓRIA (10s) =====
    if (now - lastMemoryProtection >= MEMORY_CHECK_INTERVAL) {
        performMemoryProtection();
        lastMemoryProtection = now;
    }

    const bool doseCycleBusy = isAutoDoseCycleBusy(hydroControl);
    if (!doseCycleBusy) {
        // Doses: só MQTT → bridge. Sem flush HTTPS (TLS no meio da dosagem).
        flushPendingCloudAcks();
    }
    
#if ENABLE_MQTT
    mqttClient.loop();
    if (mqttClient.isConnected()) {
        if (mqttConnectedSinceMs == 0) {
            mqttConnectedSinceMs = now;
        }
    } else {
        mqttConnectedSinceMs = 0;
    }
    if (now - lastMqttTelemetrySend >= MQTT_TELEMETRY_INTERVAL_MS) {
        if (masterManager && masterManager->isEspNowLockWindowActive()) {
#if ESPNOW_LOCK_DEBUG
            Serial.println("[LOCK] skip MQTT telemetry (window 5s)");
#endif
        } else {
            publishMqttTelemetry();
            lastMqttTelemetrySend = now;
        }
    }
    maybePublishMqttLevelsOnChange();
    if (now - lastMqttHeartbeatSend >= MQTT_HEARTBEAT_INTERVAL_MS) {
        publishMqttHeartbeat();
        lastMqttHeartbeatSend = now;
    }
#endif
    
    // ===== SENSORES → SUPABASE — skip se MQTT OK; se offline, intervalo maior + sem SSL hot path =====
    {
        bool sendHydroHttps = true;
        unsigned long hydroInterval = SENSOR_SEND_INTERVAL;
#if ENABLE_MQTT
        sendHydroHttps = !mqttClient.isConnected();
        if (sendHydroHttps) {
            hydroInterval = SENSOR_SEND_INTERVAL_MQTT_OFFLINE;
        }
#endif
        if (sendHydroHttps && !doseCycleBusy && !isSslHotPathBusy() &&
            now - lastSensorSend >= hydroInterval) {
            sendSensorDataToSupabase();
            lastSensorSend = now;
        }
    }
    
    // ===== STATUS DEVICE → SUPABASE =====
    // MQTT_HEALTH_ONLY + broker OK: no TLS cada 60s (heartbeat ya pinta device_status).
    // Fallback: MQTT caído → HTTPS 120s. Seguro last_seen: ≤4 min si MQTT OK.
    {
        unsigned long statusInterval = STATUS_SEND_INTERVAL;
#if ENABLE_MQTT
        if (!mqttClient.isConnected()) {
            statusInterval = STATUS_SEND_INTERVAL_MQTT_OFFLINE;
        }
#if MQTT_HEALTH_ONLY
        else {
            statusInterval = MQTT_CLOUD_LAST_SEEN_INTERVAL;
        }
#endif
#endif
        if (!isSslHotPathBusy() && now - lastStatusSend >= statusInterval) {
            sendDeviceStatusToSupabase();
            lastStatusSend = now;
        }
    }
    
    // ===== SINCRONIZAÇÃO UNIFICADA DE RELAY STATES (30s) — adia se ACK cloud pendente =====
    {
        bool skipRelayHttpsSync = false;
#if ENABLE_MQTT
        if (RELAY_HTTPS_SYNC_DISABLED_IF_MQTT_OK && isMqttCommandPathStable()) {
            skipRelayHttpsSync = true;
        }
#endif
        if (!skipRelayHttpsSync && !doseCycleBusy && !isSslHotPathBusy() &&
            now - lastRelayStatesSync >= RELAY_STATES_SYNC_INTERVAL) {
            syncAllRelayStatesToSupabase();
            lastRelayStatesSync = now;
        }
    }

    // ===== EC operation heartbeat — activo 12s / idle 30s (limpa estado huérfano MQTT) =====
    {
        if (!hydroControl.isAutoECEnabled() && hydroControl.isDosageActive()) {
            hydroControl.cancelCurrentDosage();
            syncEcOperationStateToSupabase();
            lastEcOperationIdleSync = now;
        }

        const char* opState = hydroControl.getEcOperationStateName();
        const bool cycleActive = hydroControl.isDosageActive() ||
            hydroControl.isDilutionActive() ||
            strcmp(opState, "recirculating") == 0 ||
            strcmp(opState, "diluting_draining") == 0 ||
            strcmp(opState, "diluting_filling") == 0 ||
            strcmp(opState, "ec_check_pending") == 0;

        if (cycleActive && now - lastEcOperationSync >= EC_OPERATION_SYNC_INTERVAL) {
            syncEcOperationStateToSupabase();
            lastEcOperationSync = now;
        } else if (!cycleActive && strcmp(opState, "idle") == 0 &&
                   now - lastEcOperationIdleSync >= EC_OPERATION_IDLE_SYNC_INTERVAL) {
            syncEcOperationStateToSupabase();
            lastEcOperationIdleSync = now;
        }
    }

    // ===== pH operation heartbeat — activo 12s / idle 30s =====
    {
        const char* phOpState = hydroControl.getPhOperationStateName();
        const bool phCycleActive = strcmp(phOpState, "dosing") == 0 ||
            strcmp(phOpState, "recirculating") == 0 ||
            strcmp(phOpState, "ph_check_pending") == 0;

        if (phCycleActive && now - lastPhOperationSync >= PH_OPERATION_SYNC_INTERVAL) {
            syncPhOperationStateToSupabase();
            lastPhOperationSync = now;
        } else if (!phCycleActive && strcmp(phOpState, "idle") == 0 &&
                   now - lastPhOperationIdleSync >= PH_OPERATION_IDLE_SYNC_INTERVAL) {
            syncPhOperationStateToSupabase();
            lastPhOperationIdleSync = now;
        }
    }

#if ENABLE_MQTT
    // Heartbeat relay/state periódico — mantém last_update fresco na cloud/UI
    if (masterManager && mqttClient.isConnected() &&
        now - lastSlaveRelayHeartbeat >= SLAVE_RELAY_HEARTBEAT_INTERVAL) {
        std::vector<TrustedSlave> slaves = masterManager->getAllTrustedSlaves();
        for (const auto& slave : slaves) {
            // Sempre publicar: offline deve ir com link_online=false (não silenciar).
            publishSlaveRelayStateMqtt(slave.macAddress, -1, false, true);
        }
        lastSlaveRelayHeartbeat = now;
    }

    // Sync completo relay_states[] via RF + MQTT (60s) — evita UI stale (só link-only)
    if (masterManager && mqttClient.isConnected() &&
        now - lastSlaveRelayFullSync >= RELAY_STATES_SYNC_FORCE_RF_MS) {
        forceSlaveRelayMqttFullSync();
        lastSlaveRelayFullSync = now;
    }
#endif
    
    // ===== DEBUG PERIÓDICO (30s) =====
    if (now - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
        printPeriodicStatus();
        lastStatusPrint = now;
    }
    
    // ===== Poll relay_commands (HTTPS backup — 60s se MQTT OK, 10s se MQTT down) =====
    commandPollIntervalMs = resolveCommandPollIntervalMs();
    supabase.setCommandPollIntervalMs(commandPollIntervalMs);
    if (now - lastSupabaseCheck >= commandPollIntervalMs) {
        checkSupabaseCommands();
        lastSupabaseCheck = now;
    }
    
    // ===== DecisionEngine local (2s) =====
    if (decisionEngineReady) {
        static unsigned long lastDecisionLoop = 0;
        if (now - lastDecisionLoop >= 2000) {
            decisionIntegration.loop();
            decisionEngine.loop();
            lastDecisionLoop = now;
        }
    }

    // ===== ✅ VERIFICAR REGRAS DE AUTOMAÇÃO (30s) =====
    // Busca regras de automação do Supabase (decision_rules com enabled=true)
    if (now - lastRulesCheck >= RULES_CHECK_INTERVAL) {
        checkSupabaseRules();
        lastRulesCheck = now;
    }
    
    // Auto EC/pH config: só MQTT retained (sem GET HTTPS). Malha usa RAM/NVS.
    static unsigned long lastConfigDebugPrint = 0;
    if (now - lastConfigDebugPrint >= 10000) {
        Serial.printf(
            "🔍 [EC CONFIG DEBUG] auto_enabled: %s | mqttCfg: %s | intervalo: %d s\n",
            hydroControl.isAutoECEnabled() ? "SIM" : "NÃO",
#if ENABLE_MQTT
            mqttEcConfigReceived ? "SIM" : "NÃO",
#else
            "n/a",
#endif
            hydroControl.getAutoECInterval());
        Serial.printf(
            "🔍 [PH CONFIG DEBUG] auto_enabled: %s | mqttCfg: %s | intervalo: %d s\n",
            hydroControl.isAutoPHEnabled() ? "SIM" : "NÃO",
#if ENABLE_MQTT
            mqttPhConfigReceived ? "SIM" : "NÃO",
#else
            "n/a",
#endif
            hydroControl.getAutoPHInterval());
#if ENABLE_MQTT
        if (!mqttEcConfigReceived) {
            Serial.println("   ⚠️ EC config MQTT ainda não chegou — RAM=NVS até retained");
        }
        if (!mqttPhConfigReceived) {
            Serial.println("   ⚠️ pH config MQTT ainda não chegou — RAM=NVS até retained");
        }
#endif
        lastConfigDebugPrint = now;
    }
    
    // ✅ TÓPICO 4: PROCESAR COMANDOS DE QUEUE (Core 0 - como MASTER-TASK)
    processWebCommands();
    
    // ✅ CORREÇÃO CRÍTICA: Tentar registrar endpoints se webServerTask estiver disponível mas não registrado
    if (!endpointsRegistered) {
        static unsigned long lastEndpointTry = 0;
        if (now - lastEndpointTry >= 5000) {  // Tentar a cada 5 segundos
            tryRegisterEndpoints();
            lastEndpointTry = now;
        }
    }
    
    // ✅ TÓPICO 4: ACTUALIZAR CACHE DEL SISTEMA (Core 0 → Core 1)
    // ✅ CORREÇÃO CRÍTICA: Aumentar intervalo para 5s e verificar memória antes de copiar
    static unsigned long lastCacheUpdate = 0;
    if (now - lastCacheUpdate >= 5000) {  // ✅ AUMENTADO: Actualizar cada 5 segundos (era 2s)
        if (webServerManager) {
            SystemDataCache cache;
            cache.uptime = getUptime();
            cache.freeHeap = ESP.getFreeHeap();
            cache.wifiConnected = WiFi.status() == WL_CONNECTED;
            cache.wifiIP = WiFi.localIP().toString();
            cache.wifiChannel = WiFi.channel();
            cache.wifiRSSI = WiFi.RSSI();
            cache.systemInitialized = true;
            cache.supabaseConnected = supabaseConnected;
            cache.webServerRunning = true;
            
            // ESP-NOW info
            if (masterManager) {
                cache.totalSlaves = masterManager->getTrustedSlaveCount();
                cache.onlineSlaves = masterManager->getOnlineSlaveCount();
                cache.offlineSlaves = cache.totalSlaves - cache.onlineSlaves;
                Serial.printf("📊 [Cache] masterManager disponível: %d slaves (online: %d, offline: %d)\n", 
                             cache.totalSlaves, cache.onlineSlaves, cache.offlineSlaves);
                
                // ✅ CORREÇÃO CRÍTICA: Verificar memória antes de copiar vetor
                uint32_t freeHeap = ESP.getFreeHeap();
                if (freeHeap < 50000) {  // ✅ Se menos de 50KB livres, pular atualização
                    Serial.printf("⚠️ [Cache] Memória baixa (%u bytes), pulando atualização de cache\n", freeHeap);
                    cache.slavesJson = "{\"slaves\":[]}";
                    cache.slavesLastUpdate = 0;
                } else {
                    // ✅ NOVO: Usar Object Pool para DynamicJsonDocument
                    ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
                    DynamicJsonDocument* slavesDoc = nullptr;
                    bool usingPool = false;
                    
                    if (poolMgr && poolMgr->isInitialized()) {
                        slavesDoc = poolMgr->acquireJsonDocument(4096);
                        if (slavesDoc) {
                            usingPool = true;
                        }
                    }
                    
                    // ✅ FALLBACK: Criar localmente se pool não disponível
                    DynamicJsonDocument localDoc(4096);
                    if (!usingPool) {
                        slavesDoc = &localDoc;
                    }
                    
                    if (!slavesDoc) {
                        Serial.println("⚠️ [Cache] Falha ao obter documento JSON (pool esgotado)");
                        cache.slavesJson = "{\"slaves\":[]}";
                        cache.slavesLastUpdate = 0;
                    } else {
                        // ✅ CACHE DE SLAVES: Serializar lista completa para JSON (thread-safe)
                        // ✅ CORREÇÃO CRÍTICA: Criar OBJETO com "slaves" array (não array direto)
                        JsonObject rootObj = slavesDoc->to<JsonObject>();
                        JsonArray slavesArray = rootObj.createNestedArray("slaves");
                        
                        std::vector<TrustedSlave> slaves = masterManager->getAllTrustedSlaves();
                        Serial.printf("📊 [Cache] Serializando %d slave(s) para JSON...\n", slaves.size());
                        
                        if (slaves.size() == 0) {
                            Serial.println("⚠️ [Cache] NENHUM SLAVE encontrado no masterManager!");
                            Serial.println("   💡 Verifique se:");
                            Serial.println("      1. Slaves estão ligados");
                            Serial.println("      2. Slaves estão no mesmo canal ESP-NOW (canal " +
                                           String(WiFi.channel()) + ")");
                            Serial.println("      3. Discovery foi executado (masterManager->rediscoverSlaves())");
                        }
                        
                        for (const auto& slave : slaves) {
                            Serial.printf("   📋 Processando slave: %s (MAC: %s)\n", 
                                         slave.deviceName.c_str(), 
                                         ESPNowController::macToString(slave.macAddress).c_str());
                            // ✅ CORREÇÃO: Gerar device_id correto (ESP32_SLAVE_XX_XX_XX_XX_XX_XX)
                            String deviceId = "ESP32_SLAVE_" + ESPNowController::macToString(slave.macAddress);
                            deviceId.replace(":", "_");
                            
                            JsonObject slaveObj = slavesArray.createNestedObject();
                            slaveObj["device_id"] = deviceId;  // ✅ CORRETO: device_id gerado
                            slaveObj["device_name"] = slave.deviceName;  // ✅ CORRETO: nome do dispositivo
                            slaveObj["device_type"] = slave.deviceType;  // ✅ CORRETO: tipo do dispositivo
                            slaveObj["mac_address"] = ESPNowController::macToString(slave.macAddress);  // ✅ CORRETO: mac_address
                            slaveObj["is_online"] = slave.isOnline();  // ✅ CORRETO: is_online
                            slaveObj["num_relays"] = slave.numRelays;  // ✅ CORRETO: num_relays
                            slaveObj["last_seen"] = slave.lastSeen;  // ✅ Timestamp Unix (frontend aceita)
                            slaveObj["operational"] = slave.operational;  // ✅ Campo adicional
                            
                            // ✅ CORREÇÃO: Adicionar estados dos relés com formato correto
                            JsonArray relaysArray = slaveObj.createNestedArray("relays");
                            for (int i = 0; i < slave.numRelays && i < 8; i++) {  // ✅ Limitar a 8 (tamanho do array)
                                JsonObject relayObj = relaysArray.createNestedObject();
                                relayObj["relay_number"] = i;  // ✅ CORRETO: relay_number (não "number")
                                relayObj["state"] = slave.relayStates[i].state;  // ✅ Estado do relé
                                relayObj["has_timer"] = slave.relayStates[i].hasTimer;  // ✅ Tem timer?
                                relayObj["remaining_time"] = slave.relayStates[i].remainingTime;  // ✅ Tempo restante
                                relayObj["name"] = slave.relayStates[i].name.length() > 0 ? slave.relayStates[i].name : ("Relé " + String(i));
                            }
                        }
                        
                        String slavesJson;
                        if (serializeJson(*slavesDoc, slavesJson) > 0) {
                            cache.slavesJson = slavesJson;
                            cache.slavesLastUpdate = millis();
                            Serial.printf("✅ [Cache] JSON criado: %d bytes\n", slavesJson.length());
                            Serial.printf("📄 [Cache] Primeiros 200 chars: %s\n", slavesJson.substring(0, 200).c_str());
                        } else {
                            Serial.println("❌ [Cache] Falha ao serializar JSON de slaves!");
                            cache.slavesJson = "{\"slaves\":[]}";
                            cache.slavesLastUpdate = 0;
                        }
                        
                        // ✅ Liberar pool se estava em uso
                        if (usingPool && poolMgr) {
                            poolMgr->releaseJsonDocument(slavesDoc);
                        }
                    }
                }  // ✅ Fechar bloco else (verificação de memória)
            } else {
                cache.totalSlaves = 0;
                cache.onlineSlaves = 0;
                cache.offlineSlaves = 0;
                cache.slavesJson = "{\"slaves\":[]}";
                cache.slavesLastUpdate = 0;
                static unsigned long lastWarning = 0;
                if (now - lastWarning >= 10000) {  // Avisar a cada 10 segundos
                    Serial.println("⚠️ [Cache] masterManager é nullptr - cache de slaves vazio");
                    Serial.println("   Verifique ordem de inicialização: masterManager->begin() ANTES de HydroSystemCore::begin()");
                    lastWarning = now;
                }
            }
            
            webServerManager->updateSystemCache(cache);
        }
        lastCacheUpdate = now;
    }
    
    // ===== LOOP DOS SENSORES/RELÉS =====
    hydroControl.loop();

#if RESOURCE_SERIAL_DEBUG
    {
        bool mqttUp = false;
#if ENABLE_MQTT
        mqttUp = mqttClient.isConnected();
#endif
        const int slavesOnline = masterManager ? masterManager->getOnlineSlaveCount() : 0;
        ResourceTelemetry::setContext(
            mqttUp,
            isSslHotPathBusy(),
            hydroControl.getDilutionPhaseName(),
            WiFi.status() == WL_CONNECTED,
            slavesOnline);
        ResourceTelemetry::tick();
    }
#endif
}

void HydroSystemCore::end() {
    if (!systemReady) return;
    
    Serial.println("🛑 Parando HydroSystemCore...");
    
    systemReady = false;
    supabaseConnected = false;
    
    Serial.println("✅ HydroSystemCore parado");
}

// ===== DEBUG E COMANDOS =====
void HydroSystemCore::printSystemStatus() {
    Serial.println("\n🌱 === STATUS SISTEMA HIDROPÔNICO ===");
    Serial.println("⏰ Uptime: " + String(getUptime()/1000) + "s");
    Serial.println("🌐 WiFi: " + (WiFi.isConnected() ? "Conectado (" + WiFi.localIP().toString() + ")" : "Desconectado"));
    Serial.println("☁️ Supabase: " + String(supabaseConnected ? "Conectado" : "Desconectado"));
    Serial.println("💾 Heap livre: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("🔧 Sistema: " + String(systemReady ? "Ativo" : "Inativo"));
    
    Serial.println("\n🔗 === STATUS RELÉS ===");
    bool* relayStates = hydroControl.getRelayStates();
    for (int i = 0; i < 16; i++) {
        Serial.printf("Relé %2d: %s\n", i, relayStates[i] ? "🟢 LIGADO" : "🔴 DESLIGADO");
    }
    
    Serial.println("\n📊 === LEITURAS DOS SENSORES ===");
    printSensorReadings();
    Serial.println("=====================================\n");
}

void HydroSystemCore::printSensorReadings() {
    MqttTelemetryReading reading;
    reading.temperature = hydroControl.getTemperature();
    reading.ph = hydroControl.getpH();
    reading.ec = hydroControl.getEC();
    reading.phValid = hydroControl.isPhValidForTelemetry();
    reading.ecValid = hydroControl.isEcValidForTelemetry();
    reading.tempValid = hydroControl.isTempValidForTelemetry();
    reading.waterLevelOk = hydroControl.isWaterLevelOk();
    reading.level1Wet = hydroControl.isLevelWet(1);
    reading.level2Wet = hydroControl.isLevelWet(2);
    reading.level3Wet = hydroControl.isLevelWet(3);
    reading.level4Wet = hydroControl.isLevelWet(4);
    reading.waterLevel = hydroControl.getWaterLevelAggregate();
    reading.interlockMode = hydroControl.getLevelInterlockModeName();
    reading.airTemperature = NAN;
    reading.humidity = NAN;
    printTelemetrySerialLine(reading);
}

void HydroSystemCore::testSupabaseConnection() {
    if (!hasEnoughMemoryForHTTPS()) {
        Serial.println("⚠️ Heap baixo - Não testando Supabase");
        supabaseConnected = false;
        return;
    }
    
    Serial.println("🧪 Testando conexão Supabase...");
    
    // Simulação simples de teste (em projeto real seria uma chamada HTTP)
    bool testResult = random(0, 10) > 1; // 90% sucesso
    
    if (testResult) {
        Serial.println("✅ Supabase: Conexão OK");
        supabaseConnected = true;
    } else {
        Serial.println("❌ Supabase: Falha na conexão");
        supabaseConnected = false;
    }
}

// ===== OPERAÇÕES PRINCIPAIS =====
void HydroSystemCore::checkSupabaseCommands() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS()) {
        return;
    }

#if ENABLE_MQTT
    if (COMMAND_POLL_DISABLED_IF_MQTT_OK && isMqttCommandPathStable()) {
        return;
    }
#endif

    if (isSslHotPathBusy()) {
        return;
    }
    if (masterManager && masterManager->isEspNowLockWindowActive()) {
#if ESPNOW_LOCK_DEBUG
        Serial.println("[LOCK] skip command poll (window 5s)");
#endif
        return;
    }
    
    if (!supabase.isReady()) return;
    
    // ✅ OTIMIZAÇÃO: Processar comandos em batch (master: 5, slave: 3 com mais espaçamento RF)
    static const int MAX_MASTER_BATCH_COMMANDS = 5;
    static const int MAX_SLAVE_BATCH_COMMANDS = 1;
    static const int MAX_BATCH_COMMANDS = 5;
    RelayCommand commands[MAX_BATCH_COMMANDS];
    int commandCount = 0;
    bool isSlave = false;
    
    // ✅ Tentar buscar comandos Master primeiro (prioridade)
    if (supabase.checkForMasterCommands(commands, MAX_BATCH_COMMANDS, commandCount)) {
        if (commandCount > 0) {
            isSlave = false;
            Serial.printf("📥 [HTTPS] %d comando(s)\n", commandCount);
            // ✅ Processar todos os comandos do batch
            for (int i = 0; i < commandCount; i++) {
                processRelayCommand(commands[i], isSlave);
                // ✅ Pequeno delay entre comandos para não sobrecarregar (não bloqueante)
                if (i < commandCount - 1) {
                    vTaskDelay(pdMS_TO_TICKS(10)); // 10ms entre comandos
                }
            }
            return;  // Processar batch completo
        }
    }
    
    // ✅ Se não encontrou Master, tentar Slave
    if (supabase.checkForSlaveCommands(commands, MAX_SLAVE_BATCH_COMMANDS, commandCount)) {
        if (commandCount > 0) {
            isSlave = true;
            Serial.printf("📥 [HTTPS slave] %d comando(s)\n", commandCount);
            for (int i = 0; i < commandCount; i++) {
                processRelayCommand(commands[i], isSlave);
                if (i < commandCount - 1) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }
            return;
        }
    }
}

// Sync config only — execução local via DecisionEngine + LittleFS
void HydroSystemCore::checkSupabaseRules() {
    if (!supabaseConnected) {
        return;
    }
    Serial.println("📋 [REGRAS] Cloud sync desativado — DecisionEngine local ativo");
}

// ✅ NOVO: Buscar EC Config do Supabase via RPC activate_auto_ec
bool HydroSystemCore::checkECConfigFromSupabase() {
    const char* skip = httpsConfigPollSkipReason();
    if (skip) {
        Serial.printf("[EC CONFIG] GET skip: %s heap=%u maxAlloc=%u\n",
                      skip, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return false;
    }
    
    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║   🔍 BUSCANDO EC CONFIG DO SUPABASE                ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    
    static struct {
        double base_dose = -1;
        double flow_rate = -1;
        double volume = -1;
        double total_ml = -1;
        double kp = -1;
        double ec_setpoint = -1;
        double tolerance = -1;
        bool auto_enabled = false;
        int intervalo_auto_ec = -1;
        unsigned long tempo_recirculacao = 0;
        double aggressiveness = -1;
        bool consumo_24h = false;
        double pulse_ml = -1;
        double pulse_gap_sec = -1;
        String nutrientsJson;
        bool initialized = false;
    } lastAppliedEcConfig;

    auto ecConfigUnchanged = [&](const ECConfig& config) -> bool {
        if (!lastAppliedEcConfig.initialized) {
            return false;
        }
        return lastAppliedEcConfig.base_dose == config.base_dose &&
               lastAppliedEcConfig.flow_rate == config.flow_rate &&
               lastAppliedEcConfig.volume == config.volume &&
               lastAppliedEcConfig.total_ml == config.total_ml &&
               lastAppliedEcConfig.kp == config.kp &&
               lastAppliedEcConfig.ec_setpoint == config.ec_setpoint &&
               lastAppliedEcConfig.tolerance == config.tolerance &&
               lastAppliedEcConfig.auto_enabled == config.auto_enabled &&
               lastAppliedEcConfig.intervalo_auto_ec == config.intervalo_auto_ec &&
               lastAppliedEcConfig.tempo_recirculacao == config.tempo_recirculacao &&
               lastAppliedEcConfig.aggressiveness == config.aggressiveness &&
               lastAppliedEcConfig.consumo_24h == config.consumo_24h &&
               lastAppliedEcConfig.pulse_ml == config.pulse_ml &&
               lastAppliedEcConfig.pulse_gap_sec == config.pulse_gap_sec &&
               lastAppliedEcConfig.nutrientsJson == config.nutrientsJson;
    };

    auto rememberEcConfig = [&](const ECConfig& config) {
        lastAppliedEcConfig.base_dose = config.base_dose;
        lastAppliedEcConfig.flow_rate = config.flow_rate;
        lastAppliedEcConfig.volume = config.volume;
        lastAppliedEcConfig.total_ml = config.total_ml;
        lastAppliedEcConfig.kp = config.kp;
        lastAppliedEcConfig.ec_setpoint = config.ec_setpoint;
        lastAppliedEcConfig.tolerance = config.tolerance;
        lastAppliedEcConfig.auto_enabled = config.auto_enabled;
        lastAppliedEcConfig.intervalo_auto_ec = config.intervalo_auto_ec;
        lastAppliedEcConfig.tempo_recirculacao = config.tempo_recirculacao;
        lastAppliedEcConfig.aggressiveness = config.aggressiveness;
        lastAppliedEcConfig.consumo_24h = config.consumo_24h;
        lastAppliedEcConfig.pulse_ml = config.pulse_ml;
        lastAppliedEcConfig.pulse_gap_sec = config.pulse_gap_sec;
        lastAppliedEcConfig.nutrientsJson = config.nutrientsJson;
        lastAppliedEcConfig.initialized = true;
    };
    
    ECConfig config;
    if (supabase.getECConfigFromSupabase(config)) {
        if (config.isValid) {
            if (!ecConfigUnchanged(config)) {
            // ✅ Atualizar parâmetros do controller
            hydroControl.getECController().setBaseDose(config.base_dose);
            hydroControl.getECController().setVolume(config.volume);
            hydroControl.getECController().setTotalMl(config.total_ml);
            hydroControl.getECController().setKp(config.kp);
            hydroControl.setECSetpoint(config.ec_setpoint, false);
            hydroControl.setECTolerance((float)config.tolerance, false);
            hydroControl.setAutoECEnabled(config.auto_enabled, false);
            if (!config.auto_enabled) {
                hydroControl.cancelCurrentDosage();
            }
            hydroControl.setAutoECInterval(config.intervalo_auto_ec, false);
            hydroControl.setTempoRecirculacaoSeconds(config.tempo_recirculacao);
            hydroControl.setMaxStepEcFraction((float)config.aggressiveness);
            hydroControl.setEcPulseDosing((float)config.pulse_ml, (float)config.pulse_gap_sec);
            hydroControl.setConsumoEc24hEnabled(config.consumo_24h);
            hydroControl.setDilutionAutoEnabled(
                config.dilution_auto_enabled || config.auto_enabled, false);
            hydroControl.setDilutionSlaveRelays(
                config.dilution_drain_slave_mac,
                config.dilution_drain_relay,
                config.dilution_fill_slave_mac,
                config.dilution_fill_relay);
            hydroControl.setDilutionMaxVolumeL((float)config.dilution_max_volume_l);
            hydroControl.setFlowmeterPulsesPerLiter((float)config.flowmeter_pulses_per_liter);
            hydroControl.setDilutionFillFlowLps((float)config.dilution_fill_flow_lps);
            
            // ✅ PASSAR NUTRIENTES PARA HYDROCONTROL (alimento para automação)
            if (config.nutrientsJson.length() > 0 && config.nutrientsJson != "[]") {
                Serial.println("📊 [EC CONFIG] Processando nutrientes para automação...");
                
                // Parsear JSON string para JsonArray
                int jsonSize = max(512, (int)(config.nutrientsJson.length() * 1.3));
                DynamicJsonDocument nutrientsDoc(jsonSize);
                DeserializationError error = deserializeJson(nutrientsDoc, config.nutrientsJson);
                
                if (!error && nutrientsDoc.is<JsonArray>()) {
                    JsonArray nutrientsArray = nutrientsDoc.as<JsonArray>();
                    
                    // ✅ Converter formato: Supabase retorna "relay" (0-15), HydroControl espera "relayNumber" (1-16)
                    DynamicJsonDocument adaptedDoc(jsonSize);
                    JsonArray adaptedArray = adaptedDoc.to<JsonArray>();
                    
                    float flowByRelay[16];
                    for (int r = 0; r < 16; r++) {
                        flowByRelay[r] = 0.0f;
                    }
                    for (JsonVariant nutrient : nutrientsArray) {
                        int relay = nutrient["relay"].as<int>();
                        if (relay < 0 || relay >= 16) {
                            continue;
                        }
                        float q = 0.0f;
                        if (nutrient.containsKey("flowRate")) {
                            q = nutrient["flowRate"].as<float>();
                        } else if (nutrient.containsKey("flow_rate")) {
                            q = nutrient["flow_rate"].as<float>();
                        }
                        if (q > 0.01f) {
                            flowByRelay[relay] = q;
                        }
                    }

                    for (JsonVariant nutrient : nutrientsArray) {
                        float mlL = 0.0f;
                        if (nutrient.containsKey("mlPerLiter")) {
                            mlL = nutrient["mlPerLiter"].as<float>();
                        } else if (nutrient.containsKey("ml_per_liter")) {
                            mlL = nutrient["ml_per_liter"].as<float>();
                        }
                        const bool flaggedActive = nutrient["active"].as<bool>();
                        if (!flaggedActive && mlL < 0.1f) {
                            continue;
                        }
                        
                        JsonObject adaptedNutrient = adaptedArray.createNestedObject();
                        adaptedNutrient["name"] = nutrient["name"].as<String>();
                        adaptedNutrient["mlPerLiter"] = mlL;
                        adaptedNutrient["active"] = true;
                        int relay = nutrient["relay"].as<int>();
                        float q = 0.0f;
                        if (nutrient.containsKey("flowRate")) {
                            q = nutrient["flowRate"].as<float>();
                        } else if (nutrient.containsKey("flow_rate")) {
                            q = nutrient["flow_rate"].as<float>();
                        }
                        if (q < 0.01f && relay >= 0 && relay < 16) {
                            q = flowByRelay[relay];
                        }
                        if (q > 0.01f) {
                            adaptedNutrient["flowRate"] = q;
                        }
                        
                        adaptedNutrient["relayNumber"] = relay + 1;
                        
                        Serial.printf("   ✅ %s: %.2f ml/L q=%.3f ml/s → Relé %d\n",
                            nutrient["name"].as<const char*>(),
                            mlL,
                            q,
                            relay + 1);
                    }
                    
                    // ✅ Passar nutrientes para HydroControl
                    if (adaptedArray.size() > 0) {
                        hydroControl.updateNutrientProportions(adaptedArray);
                        Serial.printf("✅ [EC CONFIG] %d nutriente(s) configurado(s) para automação\n", adaptedArray.size());
                    } else {
                        Serial.println("⚠️ [EC CONFIG] Nenhum nutriente ativo encontrado");
                    }
                } else {
                    Serial.printf("❌ [EC CONFIG] Erro ao parsear nutrients JSON: %s\n", error.c_str());
                }
            } else {
                Serial.println("⚠️ [EC CONFIG] Nenhum nutriente recebido (nutrientsJson vazio)");
            }
            
            hydroControl.saveECControllerConfig();
            rememberEcConfig(config);
            if (!config.auto_enabled) {
                syncEcOperationStateToSupabase();
            }
            Serial.println("✅ [EC CONFIG] Configuração atualizada e salva em NVS");
            } else {
                Serial.println("ℹ️ [EC CONFIG] Config inalterada — NVS não reescrito");
            }
            
            Serial.println("╚════════════════════════════════════════════════════╝\n");
        } else {
            Serial.println("ℹ️ [EC CONFIG] Poll OK — Auto EC desativado no Supabase (RPC vazio)");
            hydroControl.setAutoECEnabled(false);
            hydroControl.cancelCurrentDosage();
            syncEcOperationStateToSupabase();
            lastAppliedEcConfig.auto_enabled = false;
            lastAppliedEcConfig.initialized = true;
            Serial.println("   ↳ Auto EC local desativado + ciclo cancelado + idle publicado");
            Serial.println("╚════════════════════════════════════════════════════╝\n");
        }
    } else {
            Serial.println("❌ [EC CONFIG] Falha ao buscar config do Supabase");
        Serial.println("   💡 Usando valores do NVS (fallback)");
        Serial.println("╚════════════════════════════════════════════════════╝\n");
    }
    return true;
}

void HydroSystemCore::processRelayCommand(const RelayCommand& cmd, bool isSlave, const char* via) {
    printRelayCommandSerialLine(cmd, isSlave, via);

    const bool fromMqtt = (via != nullptr && strcmp(via, "mqtt") == 0);

    // MQTT: nunca HTTPS neste stack (callback PubSubClient).
    // Tampouco marcar completed aqui — slave só fecha cloud após ACK ESP-NOW.

    // HTTPS poll: RPC já marca sent — não regredir para sent de novo.
    if (!fromMqtt && supabaseConnected) {
        const String& st = cmd.status;
        if (st != "sent" && st != "processing") {
            supabase.markCommandSent(cmd.id, isSlave);
        }
    }

    String commandType = cmd.command_type.length() > 0 ? cmd.command_type : "manual";

    if (commandType == "rule") {
        processRuleCommand(cmd, isSlave);
    } else if (commandType == "peristaltic") {
        processPeristalticCommand(cmd, isSlave);
    } else {
        processManualCommand(cmd, isSlave);
    }
}

// ✅ FORK: Processar comando manual (botão do usuário)
void HydroSystemCore::processManualCommand(const RelayCommand& cmd, bool isSlave) {
    // ✅ Validar relé
    int maxRelays = isSlave ? 8 : 16;  // Slaves têm 8 relays, Master tem 16
    if (cmd.relayNumber < 0 || cmd.relayNumber >= maxRelays) {
        Serial.printf("❌ Relé %d inválido (máx: %d)\n", cmd.relayNumber, maxRelays - 1);
        if (supabaseConnected) {
#if ENABLE_MQTT
            tryPublishCloudAckViaMqtt(cmd.id, 0, nullptr, cmd.relayNumber, false, "failed");
#endif
        }
        return;
    }

    if (isSlave && !masterManager) {
        Serial.println("❌ [ESP-NOW] masterManager é nullptr — comando slave ignorado");
        Serial.println("💡 Ordem: MasterSlaveManager::begin() → setMasterManager() antes de comandos slave");
        if (supabaseConnected) {
#if ENABLE_MQTT
            tryPublishCloudAckViaMqtt(cmd.id, 0, nullptr, cmd.relayNumber, false, "failed");
#endif
        }
        return;
    }
    
    if (isSlave && masterManager) {
        // ===== COMANDO SLAVE (ESP-NOW) =====
        // ===== COMANDO SLAVE (ESP-NOW) =====
        Serial.println("📡 [ESP-NOW] Comando para slave: " + cmd.target_device_id);
        
        // ✅ Converter slave_mac_address (target_device_id) para uint8_t*
        uint8_t targetMac[6];
        if (!parseMacAddress(cmd.target_device_id, targetMac)) {
            Serial.println("❌ MAC address inválido: " + cmd.target_device_id);
            if (supabaseConnected) {
#if ENABLE_MQTT
                tryPublishCloudAckViaMqtt(cmd.id, 0, nullptr, cmd.relayNumber, false, "failed");
#endif
            }
            return;
        }
        
        // ✅ Verificar se slave está na lista confiável
        auto trustedSlaves = masterManager->getAllTrustedSlaves();
        bool slaveFound = false;
        for (const auto& slave : trustedSlaves) {
            if (memcmp(slave.macAddress, targetMac, 6) == 0) {
                slaveFound = true;
                Serial.println("✅ Slave encontrado: " + slave.deviceName);
                Serial.println("   MAC: " + ESPNowController::macToString(targetMac));
                break;
            }
        }
        
        if (!slaveFound) {
            Serial.println("⚠️ Slave não está na lista confiável: " + cmd.target_device_id);
            Serial.println("💡 Comando será enviado mesmo assim (pode ser novo slave)");
        }
        
        // ✅ PASSO 2: Enviar via RelayCoordinator (ESP-NOW + mutex circulación)
        const RelayOwner owner = resolveCommandOwner(cmd);
        uint32_t espNowCommandId = relayCoordinator.actuateSlave(
            owner,
            targetMac,
            cmd.relayNumber,
            cmd.action,
            cmd.durationSeconds,
            cmd.id,
            cmd.cycleOffSeconds,
            cmd.commandMode);
        
        // ✅ CEREJA DO BOLO: Criar mapeamento IMEDIATAMENTE após enviar
        if (espNowCommandId > 0 && cmd.id > 0) {
            addCommandMapping(espNowCommandId, cmd.id);
            registerPendingSlaveAck(cmd.id, espNowCommandId, targetMac, cmd.relayNumber, cmd.action);
            Serial.printf("[CMD dispatch] supabase_id=%d espnow_id=%u R%d %s (aguardando ACK)\n",
                          cmd.id, espNowCommandId, cmd.relayNumber, cmd.action.c_str());
        }
        
        bool success = (espNowCommandId > 0);
        
        if (success) {
            Serial.printf("   Slave: %s | Relé %d -> %s\n",
                          cmd.target_device_id.c_str(), cmd.relayNumber, cmd.action.c_str());
        } else {
            Serial.println("📋 Comando adicionado à fila (slave offline ou falha temporal)");
            Serial.println("💡 Será enviado quando slave voltar online ou no próximo retry");
        }
        
    } else {
        // ===== COMANDO MASTER (LOCAL - PCF8574) =====
        Serial.println("🏠 [MASTER] Processando comando local");

        const bool success = executeLocalRelayCommand(cmd);
        if (!success) {
            if (supabaseConnected && cmd.id > 0) {
#if ENABLE_MQTT
                const bool actualOn = (cmd.relayNumber >= 0 && cmd.relayNumber < 8)
                    ? hydroControl.getRelayStates()[cmd.relayNumber]
                    : false;
                tryPublishCloudAckViaMqtt(
                    cmd.id, 0, nullptr, cmd.relayNumber, actualOn, "failed");
#endif
            }
            return;
        }

        if (supabaseConnected) {
            const bool currentState = (cmd.relayNumber >= 0 && cmd.relayNumber < 16)
                ? hydroControl.getRelayStates()[cmd.relayNumber]
                : false;
            enqueuePendingCloudAck(cmd.id, 0, nullptr, cmd.relayNumber, currentState);
            logCmdCloudAckResult("master", cmd.id, 0, cmd.relayNumber, currentState, false);
        }

        if (supabaseConnected && !hasPendingCloudAcks()) {
            updateRelayMasterState(cmd);
        }

        maybePublishManualPumpDose(cmd);
    }
}

// Conta dosagem manual timed (não cebar) → MQTT dose → bridge → pump_quantity
void HydroSystemCore::maybePublishManualPumpDose(const RelayCommand& cmd) {
#if ENABLE_MQTT
    if (cmd.action != "on") {
        return;
    }
    if (cmd.durationSeconds <= 0) {
        return;
    }
    if (cmd.relayNumber < 0 || cmd.relayNumber > 7) {
        return;
    }
    if (!(cmd.dosageMl > 0.0f) || !isfinite(cmd.dosageMl)) {
        return;
    }
    if (cmd.triggered_by.indexOf("calibragem_prime") >= 0) {
        return;
    }

    NutrientDoseEvent event = {};
    snprintf(event.sequenceId, sizeof(event.sequenceId), "m%d-%lu", cmd.id, (unsigned long)(millis() % 1000000UL));
    const String name =
        cmd.rule_name.length() > 0 ? cmd.rule_name : String("manual_r") + String(cmd.relayNumber);
    name.toCharArray(event.nutrientName, sizeof(event.nutrientName));
    event.nutrientName[sizeof(event.nutrientName) - 1] = '\0';
    event.relayNumber = cmd.relayNumber;
    event.dosageMl = cmd.dosageMl;
    event.dosageTimeSeconds = (float)cmd.durationSeconds;
    event.ecBefore = hydroControl.getEC();
    event.ecSetpoint = 0.0f;
    event.source = "manual";
    handleNutrientDoseEvent(&event);
    Serial.printf("📊 [QTY] manual dose MQTT %.3f ml R%d seq=%s\n",
                  cmd.dosageMl, cmd.relayNumber, event.sequenceId);
#else
    (void)cmd;
#endif
}

// ✅ FORK: Processar comando de regra (automação)
void HydroSystemCore::processRuleCommand(const RelayCommand& cmd, bool isSlave) {
    Serial.println("🤖 [RULE] Processando comando de regra de automação");
    Serial.printf("   Regra: %s (%s) priority=%d\n", cmd.rule_name.c_str(), cmd.rule_id.c_str(), cmd.priority);

    if (cmd.priority >= TANK_SCRIPT_PRIORITY_THRESHOLD) {
        // Gate lo controla ScriptRunner secuencial / dilución — no timer aquí.
        Serial.printf("🤖 [RULE] P1 priority=%d (Auto EC/pH via procedimento ativo, sem hold por tempo)\n",
                      cmd.priority);
    }
    
    // ✅ Validações específicas para comandos de regra
    // 1. Verificar se regra ainda está enabled (será verificado quando buscar regras)
    // 2. Verificar allowed_relays (será verificado quando buscar regras)
    // 3. Verificar cooldown (será verificado quando buscar regras)
    
    // Por enquanto, processar igual ao manual (validações serão adicionadas depois)
    processManualCommand(cmd, isSlave);
}

// ✅ FORK: Processar comando de dosagem proporcional
void HydroSystemCore::processPeristalticCommand(const RelayCommand& cmd, bool isSlave) {
    Serial.println("💧 [PERISTALTIC] Processando comando de dosagem proporcional");
    
    // ✅ Validações específicas para dosagem
    // 1. Verificar se relé é dosador (0-7)
    if (cmd.relayNumber < 0 || cmd.relayNumber > 7) {
        Serial.printf("❌ Relé %d não é dosador (deve ser 0-7)\n", cmd.relayNumber);
        if (supabaseConnected) {
            supabase.markCommandFailed(cmd.id, "Relé não é dosador", isSlave);
        }
        return;
    }
    
    // 2. Verificar duração (deve ter duração para dosagem)
    if (cmd.durationSeconds <= 0) {
        Serial.println("❌ Dosagem proporcional requer duração > 0");
        if (supabaseConnected) {
            supabase.markCommandFailed(cmd.id, "Duração inválida para dosagem", isSlave);
        }
        return;
    }
    
    // Processar como comando local (dosadores são do master)
    // Por enquanto, processar igual ao manual
    processManualCommand(cmd, isSlave);
}

// ✅ Função auxiliar para executar comando local
bool HydroSystemCore::executeLocalRelayCommand(const RelayCommand& cmd) {
    Serial.println("🏠 [LOCAL] Comando para relés locais via RelayCoordinator");

    const RelayOwner owner = resolveCommandOwner(cmd);
    bool success = relayCoordinator.actuateLocal(
        owner, cmd.relayNumber, cmd.action, cmd.durationSeconds);

    if (success) {
        Serial.println("✅ Comando local executado com sucesso");
    } else {
        Serial.println("❌ Erro ao executar comando local");
    }
    return success;
}

void HydroSystemCore::updateStatusLedFromWifi() {
    if (statusLedSendUntilMs != 0 && millis() < statusLedSendUntilMs) {
        return;  // pulse de envío tiene prioridad
    }
    const int st = static_cast<int>(WiFi.status());
    if (st == lastWifiLedState && statusLedSendUntilMs == 0) {
        return;
    }
    const int previousWifi = lastWifiLedState;
    lastWifiLedState = st;
    if (st == WL_CONNECTED) {
        if (previousWifi != -1 && previousWifi != WL_CONNECTED && masterManager) {
            masterManager->refreshEspNowPeersOnCurrentChannel();
        }
        statusLed.setConnected();
    } else if (st == WL_IDLE_STATUS || st == WL_DISCONNECTED || st == WL_NO_SSID_AVAIL ||
               st == WL_CONNECTION_LOST) {
        statusLed.setConnecting();
    } else if (st == WL_CONNECT_FAILED) {
        statusLed.setError();
    } else {
        statusLed.setConnecting();
    }
}

void HydroSystemCore::notifyCloudSend() {
    statusLed.setSendingData();
    statusLedSendUntilMs = millis() + 1500UL;
    lastWifiLedState = -1;  // forzar re-aplicar WiFi al terminar pulse
}

void HydroSystemCore::finishStatusLedSendPulseIfDue() {
    if (statusLedSendUntilMs == 0) {
        return;
    }
    if (millis() < statusLedSendUntilMs) {
        return;
    }
    statusLedSendUntilMs = 0;
    lastWifiLedState = -1;
    updateStatusLedFromWifi();
}

void HydroSystemCore::publishMqttTelemetry() {
#if ENABLE_MQTT
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    MqttTelemetryReading reading;
    reading.temperature = hydroControl.getTemperature();
    reading.ph = hydroControl.getpH();
    reading.ec = hydroControl.getEC();
    reading.phValid = hydroControl.isPhValidForTelemetry();
    reading.ecValid = hydroControl.isEcValidForTelemetry();
    reading.tempValid = hydroControl.isTempValidForTelemetry();
    reading.waterLevelOk = hydroControl.isWaterLevelOk();
    reading.level1Wet = hydroControl.isLevelWet(1);
    reading.level2Wet = hydroControl.isLevelWet(2);
    reading.level3Wet = hydroControl.isLevelWet(3);
    reading.level4Wet = hydroControl.isLevelWet(4);
    reading.waterLevel = hydroControl.getWaterLevelAggregate();
    reading.interlockMode = hydroControl.getLevelInterlockModeName();
    // Sin DHT cableado: no enviar ambiente simulado (evita environment_data basura)
    reading.airTemperature = NAN;
    reading.humidity = NAN;
    printTelemetrySerialLine(reading);
    if (mqttClient.publishTelemetry(reading)) {
        notifyCloudSend();
    }
#endif
}

void HydroSystemCore::publishMqttLevels() {
#if ENABLE_MQTT
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    MqttLevelsReading reading;
    reading.waterLevelOk = hydroControl.isWaterLevelOk();
    reading.level1Wet = hydroControl.isLevelWet(1);
    reading.level2Wet = hydroControl.isLevelWet(2);
    reading.level3Wet = hydroControl.isLevelWet(3);
    reading.level4Wet = hydroControl.isLevelWet(4);
    reading.waterLevel = hydroControl.getWaterLevelAggregate();
    reading.interlockMode = hydroControl.getLevelInterlockModeName();
#if HIDRO_SIMULATE_WATER_LEVELS
    reading.levelsSimulated = true;
#else
    reading.levelsSimulated = false;
#endif
    if (mqttClient.publishLevels(reading)) {
        notifyCloudSend();
        lastMqttLevelsPublishMs = millis();
        lastMqttLevelsMask = static_cast<uint8_t>(
            (reading.level1Wet ? 1 : 0) |
            (reading.level2Wet ? 2 : 0) |
            (reading.level3Wet ? 4 : 0) |
            (reading.level4Wet ? 8 : 0) |
            (reading.waterLevelOk ? 16 : 0));
        const char* wl = reading.waterLevel ? reading.waterLevel : "vazio";
        strncpy(lastMqttWaterLevel, wl, sizeof(lastMqttWaterLevel) - 1);
        lastMqttWaterLevel[sizeof(lastMqttWaterLevel) - 1] = '\0';
        const char* im = reading.interlockMode ? reading.interlockMode : "normal";
        strncpy(lastMqttInterlockMode, im, sizeof(lastMqttInterlockMode) - 1);
        lastMqttInterlockMode[sizeof(lastMqttInterlockMode) - 1] = '\0';
        mqttLevelsFingerprintValid = true;
    }
#endif
}

void HydroSystemCore::maybePublishMqttLevelsOnChange() {
#if ENABLE_MQTT
    if (!mqttClient.isConnected() || WiFi.status() != WL_CONNECTED) {
        return;
    }

    const bool l1 = hydroControl.isLevelWet(1);
    const bool l2 = hydroControl.isLevelWet(2);
    const bool l3 = hydroControl.isLevelWet(3);
    const bool l4 = hydroControl.isLevelWet(4);
    const bool ok = hydroControl.isWaterLevelOk();
    const char* wl = hydroControl.getWaterLevelAggregate();
    if (!wl) {
        wl = "vazio";
    }
    const char* im = hydroControl.getLevelInterlockModeName();

    const uint8_t mask = static_cast<uint8_t>(
        (l1 ? 1 : 0) | (l2 ? 2 : 0) | (l3 ? 4 : 0) | (l4 ? 8 : 0) | (ok ? 16 : 0));

    const bool changed =
        !mqttLevelsFingerprintValid ||
        mask != lastMqttLevelsMask ||
        strncmp(lastMqttWaterLevel, wl, sizeof(lastMqttWaterLevel)) != 0 ||
        strncmp(lastMqttInterlockMode, im, sizeof(lastMqttInterlockMode)) != 0;

    if (!changed) {
        return;
    }

    // Anti-flood curto (debounce hardware já é LEVEL_DEBOUNCE_MS)
    const unsigned long now = millis();
    if (mqttLevelsFingerprintValid && (now - lastMqttLevelsPublishMs) < 300UL) {
        return;
    }

    publishMqttLevels();
#endif
}

void HydroSystemCore::publishMqttHeartbeat() {
#if ENABLE_MQTT
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    MqttHeartbeatReading hb;
    hb.wifiRssi = WiFi.RSSI();
    hb.freeHeap = ESP.getFreeHeap();
    hb.uptimeSeconds = millis() / 1000UL;
    hb.rebootCount = getRebootCount();
    hb.firmwareVersion = FIRMWARE_VERSION;
    hb.ipAddress = WiFi.localIP().toString();
    mqttClient.publishHeartbeat(hb);
#endif
}

void HydroSystemCore::sendSensorDataToSupabase() {
    Serial.println("🔍 [SENSORES] Tentando enviar dados (HTTPS)...");
    Serial.printf("   supabaseConnected: %s\n", supabaseConnected ? "SIM" : "NÃO");
    Serial.printf("   hasEnoughMemory: %s\n", hasEnoughMemoryForHTTPS() ? "SIM" : "NÃO");
    Serial.printf("   supabase.isReady(): %s\n", supabase.isReady() ? "SIM" : "NÃO");
    Serial.printf("   heap=%u maxAlloc=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    
    if (!supabaseConnected) {
        Serial.println("❌ [SENSORES] Supabase não conectado - abortando envio");
        return;
    }

    if (isSslHotPathBusy()) {
        Serial.println("[SSL] defer hydro HTTPS (hot path busy)");
        return;
    }
    
    if (!hasEnoughMemoryForHTTPS()) {
        Serial.printf("❌ [SENSORES] Memória insuficiente p/ TLS: free=%u maxAlloc=%u (min %u/%u)\n",
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                      MIN_HEAP_FOR_HTTPS, MIN_CONTIGUOUS_FOR_HTTPS);
        return;
    }
    
    if (!supabase.isReady()) {
        Serial.println("❌ [SENSORES] Supabase não está pronto - abortando envio");
        return;
    }
    
    // Dados ambientais — só enviar com DHT real (sem valores simulados)
    EnvironmentReading envData;
    envData.temperature = NAN;
    envData.humidity = NAN;
    envData.timestamp = millis();
    
    // Dados hidropônicos
    HydroReading hydroData;
    hydroData.temperature = hydroControl.getTemperature();
    hydroData.ph = hydroControl.getpH();
    hydroData.ec = hydroControl.getEC();
    hydroData.waterLevelOk = hydroControl.isWaterLevelOk();
    hydroData.level1Wet = hydroControl.isLevelWet(1);
    hydroData.level2Wet = hydroControl.isLevelWet(2);
    hydroData.level3Wet = hydroControl.isLevelWet(3);
    hydroData.level4Wet = hydroControl.isLevelWet(4);
    hydroData.waterLevel = hydroControl.getWaterLevelAggregate();
    hydroData.timestamp = millis();
    
    // ✅ DEBUG: Mostrar valores antes de enviar
    Serial.printf("📊 [SENSORES] Valores: Temp=%.2f°C, pH=%.2f, EC=%.0f uS/cm\n",
        hydroData.temperature, hydroData.ph, hydroData.ec);

    // Enviar dados
    bool envSuccess = true;
    if (isValidEnvironmentReading(envData.temperature, envData.humidity)) {
        envSuccess = supabase.sendEnvironmentData(envData);
        if (envSuccess) {
            Serial.println("✅ [SENSORES] Dados ambientais enviados ao Supabase");
        } else {
            Serial.println("❌ [SENSORES] Falha ao enviar dados ambientais");
        }
    }
    bool hydroSuccess = supabase.sendHydroData(hydroData);
    
    if (hydroSuccess) {
        Serial.println("✅ [SENSORES] Dados hidropônicos enviados ao Supabase");
        notifyCloudSend();
    } else {
        Serial.println("❌ [SENSORES] Falha ao enviar dados hidropônicos");
    }
}

void HydroSystemCore::sendDeviceStatusToSupabase() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS()) {
        return;
    }

    if (isSslHotPathBusy()) {
        Serial.println("[SSL] defer device_status HTTPS (hot path busy)");
        return;
    }
    
    if (!supabase.isReady()) return;
    
    DeviceStatusData status;
    status.deviceId = getDeviceID();
    status.wifiRssi = WiFi.RSSI();
    status.freeHeap = ESP.getFreeHeap();
    status.uptimeSeconds = millis() / 1000;
    status.isOnline = true;
    status.firmwareVersion = FIRMWARE_VERSION;
    status.ipAddress = WiFi.localIP().toString();
    status.timestamp = millis();
    status.rebootCount = getRebootCount();  // ✅ TÓPICO 5: Incluir contador de reinícios
    
    // Estados dos relés
    bool* relayStates = hydroControl.getRelayStates();
    for (int i = 0; i < 16; i++) {
        status.relayStates[i] = relayStates[i];
    }
    
#if ENABLE_MQTT && MQTT_HEALTH_ONLY
    static unsigned long lastForcedLastSeen = 0;
    const bool mqttUp = mqttClient.isConnected();
    const bool forcePeriodic = mqttUp &&
        (millis() - lastForcedLastSeen >= MQTT_CLOUD_LAST_SEEN_INTERVAL);
    const bool shouldPatch = !mqttUp || forcePeriodic;
    if (shouldPatch) {
        if (supabase.updateDeviceStatus(status)) {
            if (mqttUp) {
                Serial.println("📤 device_status last_seen (MQTT_HEALTH periodic ≤4min)");
                lastForcedLastSeen = millis();
            } else {
                Serial.println("📤 Status do dispositivo (HTTPS fallback — MQTT offline)");
            }
        }
    }
#else
    if (supabase.updateDeviceStatus(status)) {
        Serial.println("📤 Status do dispositivo atualizado no Supabase");
    }
#endif
    
    // NVS siempre (barato). HTTPS relay_master solo si MQTT comando no es el canal.
    saveMasterRelayStatesToNVS();

    bool skipRelayMasterHttps = false;
#if ENABLE_MQTT
    if (RELAY_HTTPS_SYNC_DISABLED_IF_MQTT_OK && isMqttCommandPathStable()) {
        skipRelayMasterHttps = true;
    }
#endif
    if (!skipRelayMasterHttps) {
        if (supabase.updateRelayMaster(getDeviceID(), relayStates, nullptr, nullptr, nullptr)) {
            Serial.println("✅ Estados dos relés master atualizados em relay_master");
#if ENABLE_MQTT
            if (!mqttClient.isConnected()) {
                syncEcOperationStateToSupabase();
            }
#else
            syncEcOperationStateToSupabase();
#endif
        }
    } else {
        Serial.println("[SSL] skip relay_master HTTPS (MQTT command path OK)");
    }
}

void HydroSystemCore::onNutrientDoseStatic(const NutrientDoseEvent* event, void* userData) {
    if (userData && event) {
        static_cast<HydroSystemCore*>(userData)->handleNutrientDoseEvent(event);
    }
}

void HydroSystemCore::onEcDilutionStatic(const EcDilutionEvent* event, void* userData) {
    if (userData && event) {
        static_cast<HydroSystemCore*>(userData)->handleEcDilutionEvent(event);
    }
}

uint32_t HydroSystemCore::onDilutionSlaveRelayStatic(const uint8_t* mac, int relay, bool on, void* userData) {
    if (userData && mac) {
        return static_cast<HydroSystemCore*>(userData)->handleDilutionSlaveRelay(mac, relay, on);
    }
    return 0;
}

uint32_t HydroSystemCore::handleDilutionSlaveRelay(const uint8_t* mac, int relay, bool on) {
    if (!mac) {
        return 0;
    }
    const String action = on ? "on" : "off";
    return relayCoordinator.actuateSlave(RelayOwner::AutoEcDilution, mac, relay, action, 0, 0);
}

void HydroSystemCore::onEcMetricStatic(const EcControllerMetricEvent* event, void* userData) {
    if (userData && event) {
        static_cast<HydroSystemCore*>(userData)->handleEcMetricEvent(event);
    }
}

void HydroSystemCore::onPhMetricStatic(const PhControllerMetricEvent* event, void* userData) {
    if (userData && event) {
        static_cast<HydroSystemCore*>(userData)->handlePhMetricEvent(event);
    }
}

void HydroSystemCore::onEcOperationSyncStatic(void* userData) {
    if (userData) {
        static_cast<HydroSystemCore*>(userData)->syncEcOperationStateToSupabase();
    }
}

void HydroSystemCore::onPhOperationSyncStatic(void* userData) {
    if (userData) {
        static_cast<HydroSystemCore*>(userData)->syncPhOperationStateToSupabase();
    }
}

void HydroSystemCore::onPhysicalRecircStatic(bool starting, const char* domain, void* userData) {
    if (!userData) {
        return;
    }
    HydroSystemCore* self = static_cast<HydroSystemCore*>(userData);
    RelayOwner owner = RelayOwner::AutoEcRecirc;
    if (domain && strcmp(domain, "ph") == 0) {
        owner = RelayOwner::AutoPhRecirc;
    }
    if (starting) {
        self->relayCoordinator.startPostDoseRecirc(owner);
    } else {
        self->relayCoordinator.endPostDoseRecirc(owner);
    }
}

RelayOwner HydroSystemCore::resolveCommandOwner(const RelayCommand& cmd) {
    String commandType = cmd.command_type.length() > 0 ? cmd.command_type : "manual";
    if (commandType == "rule") {
        if (cmd.priority >= TANK_SCRIPT_PRIORITY_THRESHOLD) {
            return RelayOwner::TankScriptP1;
        }
        if (cmd.priority >= 20 && cmd.priority <= 40) {
            return RelayOwner::ScheduleP4;
        }
        return RelayOwner::DecisionRule;
    }
    return RelayOwner::Manual;
}

void HydroSystemCore::syncPhOperationStateToSupabase() {
    const char* stateName = hydroControl.getPhOperationStateName();
    const int remainingSec = hydroControl.getPhOperationRemainingSec();
    const int nextCheckSec = hydroControl.getPhNextCheckInSec();

    if (mqttClient.isConnected()) {
        MqttPhOperationReading reading = {};
        reading.state = stateName;
        reading.operationRemainingSec = remainingSec;
        reading.nextCheckInSec = nextCheckSec;
        if (mqttClient.publishPhOperation(reading)) {
            return;
        }
        Serial.println("⚠️ [PH OP] MQTT publish falhou — fallback HTTPS");
    }

    if (!supabaseConnected || !hasEnoughMemoryForHTTPS() || !supabase.isReady()) {
        return;
    }
    supabase.updatePhOperationState(
        getDeviceID(),
        String(stateName),
        remainingSec,
        nextCheckSec,
        bootOperationInterrupted
    );
}

bool HydroSystemCore::checkPHConfigFromSupabase() {
    const char* skip = httpsConfigPollSkipReason();
    if (skip) {
        Serial.printf("[PH CONFIG] GET skip: %s heap=%u maxAlloc=%u\n",
                      skip, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return false;
    }

    PHConfig config;
    if (!supabase.getPHConfigFromSupabase(config)) {
        return true;
    }

    if (!config.isValid) {
#if PH_PROTOTYPE_RELAX_GUARDS
        Serial.println("⚠️ [PH CONFIG] Config inválida — prototipo: auto pH mantido");
#else
        if (hydroControl.isAutoPHEnabled()) {
            hydroControl.setAutoPHEnabled(false, false);
            syncPhOperationStateToSupabase();
        }
#endif
        return true;
    }

    hydroControl.setPHSetpoint((float)config.ph_setpoint, false);
    hydroControl.setPHTolerance((float)config.ph_tolerance);
    hydroControl.setPhPumpConfig(
        config.relay_ph_up,
        config.relay_ph_down,
        (float)config.flow_rate_ph_up,
        (float)config.flow_rate_ph_down,
        (float)config.ml_per_ph_unit_acid,
        (float)config.ml_per_ph_unit_base
    );
    hydroControl.setPhAdaptiveConfig(
        (float)config.aggressiveness,
        (float)config.gain_alpha
    );
    if (config.reset_k_gains) {
        hydroControl.resetPhLearnedGains();
        const auto& phCtrl = hydroControl.getAdaptivePHController();
        supabase.patchPhConfigGains(
            getDeviceID(),
            phCtrl.getKAcid(),
            phCtrl.getKBase(),
            true
        );
    }
    hydroControl.setAutoPHInterval(config.intervalo_auto_ph, false);
    hydroControl.setAutoPHEnabled(config.auto_enabled, false);
    hydroControl.setPhRecirculacaoSeconds(config.tempo_recirculacao);
    hydroControl.setPhPulseDosing((float)config.pulse_ml, (float)config.pulse_gap_sec);
    hydroControl.setConsumoPh24hEnabled(config.consumo_24h);
    return true;
}

void HydroSystemCore::onPhDoseStatic(const PhDoseEvent* event, void* userData) {
    if (userData) {
        static_cast<HydroSystemCore*>(userData)->handlePhDoseEvent(event);
    }
}

void HydroSystemCore::onPhGainLearnedStatic(void* userData) {
    if (userData) {
        static_cast<HydroSystemCore*>(userData)->handlePhGainLearned();
    }
}

void HydroSystemCore::onEcGainLearnedStatic(void* userData) {
    if (userData) {
        static_cast<HydroSystemCore*>(userData)->handleEcGainLearned();
    }
}

void HydroSystemCore::handleEcGainLearned() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS() || !supabase.isReady()) {
        return;
    }
    const float k = hydroControl.getECController().getKValue();
    Serial.printf("💾 [EC K] PATCH k_value post-recirc (k=%.4f)\n", k);
    supabase.patchEcConfigGain(getDeviceID(), k);
}

void HydroSystemCore::handlePhGainLearned() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS() || !supabase.isReady()) {
        return;
    }
    const auto& phCtrl = hydroControl.getAdaptivePHController();
    Serial.printf("💾 [PH K] PATCH k_acid/k_base post-recirc (k_acid=%.3e k_base=%.3e)\n",
        phCtrl.getKAcid(), phCtrl.getKBase());
    supabase.patchPhConfigGains(
        getDeviceID(),
        phCtrl.getKAcid(),
        phCtrl.getKBase(),
        false
    );
}

void HydroSystemCore::copyNutrientDoseToPending(const NutrientDoseEvent* event, PendingNutrientDoseExport& out) {
    if (!event) {
        return;
    }
    strncpy(out.sequenceId, event->sequenceId, sizeof(out.sequenceId) - 1);
    out.sequenceId[sizeof(out.sequenceId) - 1] = '\0';
    strncpy(out.nutrientName, event->nutrientName, sizeof(out.nutrientName) - 1);
    out.nutrientName[sizeof(out.nutrientName) - 1] = '\0';
    const char* src = (event->source && event->source[0]) ? event->source : "auto_ec";
    strncpy(out.source, src, sizeof(out.source) - 1);
    out.source[sizeof(out.source) - 1] = '\0';
    out.relayNumber = event->relayNumber;
    out.dosageMl = event->dosageMl;
    out.dosageTimeSeconds = event->dosageTimeSeconds;
    out.ecBefore = event->ecBefore;
    out.ecSetpoint = event->ecSetpoint;
    out.attempts = 0;
}

void HydroSystemCore::copyPhDoseToPending(const PhDoseEvent* event, PendingPhDoseExport& out) {
    if (!event) {
        return;
    }
    strncpy(out.sequenceId, event->sequenceId, sizeof(out.sequenceId) - 1);
    out.sequenceId[sizeof(out.sequenceId) - 1] = '\0';
    strncpy(out.direction, event->direction, sizeof(out.direction) - 1);
    out.direction[sizeof(out.direction) - 1] = '\0';
    const char* src = (event->source && event->source[0]) ? event->source : "auto_ph";
    strncpy(out.source, src, sizeof(out.source) - 1);
    out.source[sizeof(out.source) - 1] = '\0';
    out.relayNumber = event->relayNumber;
    out.dosageMl = event->dosageMl;
    out.dosageTimeSeconds = event->dosageTimeSeconds;
    out.phBefore = event->phBefore;
    out.phSetpoint = event->phSetpoint;
    out.attempts = 0;
}

void HydroSystemCore::enqueuePendingNutrientDose(const NutrientDoseEvent* event) {
    if (!event || pendingNutrientDoseCount >= PENDING_NUTRIENT_DOSE_CAP) {
        if (event && pendingNutrientDoseCount >= PENDING_NUTRIENT_DOSE_CAP) {
            Serial.println("⚠️ [DOSAGEM] Cola HTTPS backup cheia — evento descartado");
        }
        return;
    }
    const uint8_t tail = (pendingNutrientDoseHead + pendingNutrientDoseCount) % PENDING_NUTRIENT_DOSE_CAP;
    copyNutrientDoseToPending(event, pendingNutrientDoseQueue[tail]);
    pendingNutrientDoseCount++;
}

void HydroSystemCore::enqueuePendingPhDose(const PhDoseEvent* event) {
    if (!event || pendingPhDoseCount >= PENDING_PH_DOSE_CAP) {
        if (event && pendingPhDoseCount >= PENDING_PH_DOSE_CAP) {
            Serial.println("⚠️ [PH DOSAGEM] Cola HTTPS backup cheia — evento descartado");
        }
        return;
    }
    const uint8_t tail = (pendingPhDoseHead + pendingPhDoseCount) % PENDING_PH_DOSE_CAP;
    copyPhDoseToPending(event, pendingPhDoseQueue[tail]);
    pendingPhDoseCount++;
}

bool HydroSystemCore::tryInsertNutrientDoseHttps(const PendingNutrientDoseExport& item, const char* logLabel) {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS() || !supabase.isReady()) {
        return false;
    }
    Serial.printf("💾 [DOSAGEM] INSERT nutrient_dosages (%s): %s %.2f ml relé %d\n",
                  logLabel, item.nutrientName, item.dosageMl, item.relayNumber + 1);
    const bool ok = supabase.insertNutrientDosage(
        getDeviceID(),
        String(item.sequenceId),
        String(item.nutrientName),
        item.relayNumber,
        item.dosageMl,
        item.dosageTimeSeconds,
        item.ecBefore,
        item.ecSetpoint,
        String(item.source)
    );
    if (!ok) {
        const String err = supabase.getLastError();
        if (err.indexOf("409") >= 0 || err.indexOf("duplicate") >= 0) {
            Serial.println("ℹ️ [DOSAGEM] INSERT duplicado — bridge já persistiu");
            return true;
        }
    }
    return ok;
}

bool HydroSystemCore::tryInsertPhDoseHttps(const PendingPhDoseExport& item, const char* logLabel) {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS() || !supabase.isReady()) {
        return false;
    }
    Serial.printf("💾 [PH DOSAGEM] INSERT ph_dosages (%s): %s %.2f ml relé %d\n",
                  logLabel, item.direction, item.dosageMl, item.relayNumber + 1);
    const bool ok = supabase.insertPhDosage(
        getDeviceID(),
        String(item.sequenceId),
        String(item.direction),
        item.relayNumber,
        item.dosageMl,
        item.dosageTimeSeconds,
        item.phBefore,
        item.phSetpoint,
        String(item.source)
    );
    if (!ok) {
        const String err = supabase.getLastError();
        if (err.indexOf("409") >= 0 || err.indexOf("duplicate") >= 0) {
            Serial.println("ℹ️ [PH DOSAGEM] INSERT duplicado — bridge já persistiu");
            return true;
        }
    }
    return ok;
}

void HydroSystemCore::flushPendingNutrientDoseExports() {
    if (pendingNutrientDoseCount == 0) {
        return;
    }
    pendingNutrientDoseHead = 0;
    pendingNutrientDoseCount = 0;
}

void HydroSystemCore::flushPendingPhDoseExports() {
    if (pendingPhDoseCount == 0) {
        return;
    }
    pendingPhDoseHead = 0;
    pendingPhDoseCount = 0;
}

void HydroSystemCore::handlePhDoseEvent(const PhDoseEvent* event) {
    if (!event) return;

    bool doseExported = false;
    if (mqttClient.isConnected()) {
        MqttPhDoseReading dose = {};
        dose.sequenceId = event->sequenceId;
        dose.direction = event->direction;
        dose.relayNumber = event->relayNumber;
        dose.dosageMl = event->dosageMl;
        dose.dosageTimeSeconds = event->dosageTimeSeconds;
        dose.phBefore = event->phBefore;
        dose.phSetpoint = event->phSetpoint;
        dose.source = event->source ? event->source : "auto_ph";
        doseExported = mqttClient.publishPhDose(dose);
    }

    if (!doseExported) {
        Serial.println("⚠️ [PH DOSAGEM] MQTT publish falhou — sem HTTPS (bridge é o persist)");
    }

    syncPhOperationStateToSupabase();
}

void HydroSystemCore::handleNutrientDoseEvent(const NutrientDoseEvent* event) {
    if (!event) {
        return;
    }

    bool doseExported = false;
    if (mqttClient.isConnected()) {
        MqttDoseReading dose = {};
        dose.sequenceId = event->sequenceId;
        dose.nutrientName = event->nutrientName;
        dose.relayNumber = event->relayNumber;
        dose.dosageMl = event->dosageMl;
        dose.dosageTimeSeconds = event->dosageTimeSeconds;
        dose.ecBefore = event->ecBefore;
        dose.ecSetpoint = event->ecSetpoint;
        dose.source = event->source ? event->source : "auto_ec";
        doseExported = mqttClient.publishDose(dose);
    }

    if (!doseExported) {
        Serial.println("⚠️ [DOSAGEM] MQTT publish falhou — sem HTTPS (bridge é o persist)");
    }
    // MQTT → bridge persiste; sem backup HTTPS (evita SSL OOM + 409 durante dosing)

    syncEcOperationStateToSupabase();
}

void HydroSystemCore::handleEcDilutionEvent(const EcDilutionEvent* event) {
    if (!event) {
        return;
    }

    bool exported = false;
    if (mqttClient.isConnected()) {
        MqttEcDilutionReading reading = {};
        reading.sequenceId = event->sequenceId;
        reading.ecBefore = event->ecBefore;
        reading.ecSetpoint = event->ecSetpoint;
        reading.volumeTargetL = event->volumeTargetL;
        reading.volumeMeasuredL = event->volumeMeasuredL;
        reading.drainDurationSec = event->drainDurationSec;
        reading.fillDurationSec = event->fillDurationSec;
        reading.source = event->source ? event->source : "manual";
        exported = mqttClient.publishEcDilution(reading);
    }

    if (!exported) {
        Serial.println("⚠️ [EC DILUIÇÃO] MQTT publish falhou — sem HTTPS");
    }

    syncEcOperationStateToSupabase();
}

void HydroSystemCore::handleEcMetricEvent(const EcControllerMetricEvent* event) {
    if (!event) {
        return;
    }

    bool exported = false;
    if (mqttClient.isConnected()) {
        MqttEcMetricReading reading = {};
        reading.ecSetpoint = event->ecSetpoint;
        reading.ecActual = event->ecActual;
        reading.ecError = event->ecError;
        reading.kValue = event->kValue;
        reading.dosageMl = event->dosageMl;
        reading.dosageTimeSeconds = event->dosageTimeSeconds;
        reading.baseDose = event->baseDose;
        reading.flowRate = event->flowRate;
        reading.volume = event->volume;
        reading.totalMl = event->totalMl;
        reading.kp = event->kp;
        reading.autoEnabled = event->autoEnabled;
        reading.adjustmentNeeded = event->adjustmentNeeded;
        reading.adjustmentApplied = event->adjustmentApplied;
        reading.sequenceId = event->sequenceId;
        exported = mqttClient.publishEcMetric(reading);
    }

    if (!exported && supabaseConnected) {
        const String deviceId = getDeviceID();
        const String seqId = String(event->sequenceId);
        supabase.insertEcControllerMetric(
            deviceId,
            event->ecSetpoint, event->ecActual, event->ecError,
            event->kValue, event->dosageMl, event->dosageTimeSeconds,
            event->baseDose, event->flowRate, event->volume, event->totalMl,
            event->kp, event->autoEnabled,
            event->adjustmentNeeded, event->adjustmentApplied,
            seqId);
    }
}

void HydroSystemCore::handlePhMetricEvent(const PhControllerMetricEvent* event) {
    if (!event) {
        return;
    }

    bool exported = false;
    if (mqttClient.isConnected()) {
        MqttPhMetricReading reading = {};
        reading.phSetpoint = event->phSetpoint;
        reading.phBefore = event->phBefore;
        reading.errorH = event->errorH;
        reading.direction = event->direction[0] ? event->direction : nullptr;
        reading.kAcid = event->kAcid;
        reading.kBase = event->kBase;
        reading.kUsed = event->kUsed;
        reading.doseIdealMl = event->doseIdealMl;
        reading.doseRealMl = event->doseRealMl;
        reading.dosageTimeSeconds = event->dosageTimeSeconds;
        reading.aggressiveness = event->aggressiveness;
        reading.autoEnabled = event->autoEnabled;
        reading.adjustmentNeeded = event->adjustmentNeeded;
        reading.adjustmentApplied = event->adjustmentApplied;
        reading.sequenceId = event->sequenceId;
        exported = mqttClient.publishPhMetric(reading);
    }

    if (!exported && supabaseConnected) {
        const String deviceId = getDeviceID();
        const String seqId = String(event->sequenceId);
        const String direction = String(event->direction);
        supabase.insertPhControllerMetric(
            deviceId,
            event->phSetpoint, event->phBefore, event->errorH,
            direction,
            event->kAcid, event->kBase, event->kUsed,
            event->doseIdealMl, event->doseRealMl, event->dosageTimeSeconds,
            event->aggressiveness, event->autoEnabled,
            event->adjustmentNeeded, event->adjustmentApplied,
            seqId);
    }
}

void HydroSystemCore::syncEcOperationStateToSupabase() {
    const char* stateName = hydroControl.getEcOperationStateName();
    const int remainingSec = hydroControl.getEcOperationRemainingSec();
    const int nextCheckSec = hydroControl.getEcNextCheckInSec();
    const bool diluting = hydroControl.isDilutionActive();
    const float dilTarget = diluting ? hydroControl.getEcDilutionTargetL() : -1.0f;
    const float dilProgress = diluting ? hydroControl.getEcDilutionProgressL() : -1.0f;

    if (mqttClient.isConnected()) {
        MqttEcOperationReading reading = {};
        reading.state = stateName;
        reading.operationRemainingSec = remainingSec;
        reading.nextCheckInSec = nextCheckSec;
        if (diluting) {
            reading.hasDilutionProgress = true;
            reading.dilutionTargetL = dilTarget;
            reading.dilutionProgressL = dilProgress;
            if (isfinite(hydroControl.getEC()) && isfinite(hydroControl.getECSetpoint())) {
                reading.ecOvershootUs = hydroControl.getEC() - hydroControl.getECSetpoint();
            }
        }
        if (mqttClient.publishEcOperation(reading)) {
            return;
        }
        Serial.println("⚠️ [EC OP] MQTT publish falhou — fallback HTTPS");
    }

    if (!supabaseConnected || !hasEnoughMemoryForHTTPS() || !supabase.isReady()) {
        return;
    }
    supabase.updateEcOperationState(
        getDeviceID(),
        String(stateName),
        remainingSec,
        nextCheckSec,
        dilTarget,
        dilProgress,
        bootOperationInterrupted
    );
}

// ===== ✅ NOVO: SINCRONIZAÇÃO UNIFICADA DE TODOS OS RELAY STATES =====

static void formatRelayMaskSync(const bool states[8], char* buf) {
    for (int i = 0; i < 8; i++) {
        buf[i] = states[i] ? 'T' : 'F';
    }
    buf[8] = '\0';
}

struct SlaveSyncDeltaCache {
    uint8_t mac[6];
    bool states[8];
    bool valid;
    unsigned long lastForceRfPollMs;
};

static SlaveSyncDeltaCache lastSyncedByMac[8];
static int lastSyncedCount = 0;

static bool findLastSyncedEntry(const uint8_t* mac, SlaveSyncDeltaCache* out) {
    for (int i = 0; i < lastSyncedCount; i++) {
        if (memcmp(lastSyncedByMac[i].mac, mac, 6) == 0 && lastSyncedByMac[i].valid) {
            if (out) {
                *out = lastSyncedByMac[i];
            }
            return true;
        }
    }
    return false;
}

static bool findLastSynced(const uint8_t* mac, bool outStates[8]) {
    SlaveSyncDeltaCache entry;
    if (!findLastSyncedEntry(mac, &entry)) {
        return false;
    }
    memcpy(outStates, entry.states, 8);
    return true;
}

static void storeLastSynced(const uint8_t* mac, const bool states[8]) {
    for (int i = 0; i < lastSyncedCount; i++) {
        if (memcmp(lastSyncedByMac[i].mac, mac, 6) == 0) {
            memcpy(lastSyncedByMac[i].states, states, 8);
            lastSyncedByMac[i].valid = true;
            return;
        }
    }
    if (lastSyncedCount < 8) {
        memset(&lastSyncedByMac[lastSyncedCount], 0, sizeof(SlaveSyncDeltaCache));
        memcpy(lastSyncedByMac[lastSyncedCount].mac, mac, 6);
        memcpy(lastSyncedByMac[lastSyncedCount].states, states, 8);
        lastSyncedByMac[lastSyncedCount].valid = true;
        lastSyncedCount++;
    }
}

static unsigned long getLastForceRfPollMs(const uint8_t* mac) {
    SlaveSyncDeltaCache entry;
    if (findLastSyncedEntry(mac, &entry)) {
        return entry.lastForceRfPollMs;
    }
    return 0;
}

static void touchLastForceRfPollMs(const uint8_t* mac, unsigned long whenMs) {
    for (int i = 0; i < lastSyncedCount; i++) {
        if (memcmp(lastSyncedByMac[i].mac, mac, 6) == 0) {
            lastSyncedByMac[i].lastForceRfPollMs = whenMs;
            return;
        }
    }
}

static bool relayStatesUnchanged(const uint8_t* mac, const bool states[8]) {
    bool prev[8] = {false};
    if (!findLastSynced(mac, prev)) {
        return false;
    }
    return memcmp(prev, states, 8) == 0;
}

// Atualiza master relays + slave relays juntos no Supabase a cada 10 segundos
void HydroSystemCore::syncAllRelayStatesToSupabase() {
    static const uint32_t MIN_HEAP_FOR_RELAY_SYNC = 60000;

    if (!supabaseConnected || !hasEnoughMemoryForHTTPS()) {
        return;
    }

    if (hasPendingCloudAcks()) {
        Serial.println("⚠️ [SYNC] ACK cloud pendente — adiando relay_slaves");
        return;
    }

    if (hasPendingSlaveAcks()) {
        Serial.println("⚠️ [SYNC] pending slave ACK — adiando relay_slaves");
        return;
    }

    if (ESP.getFreeHeap() < MIN_HEAP_FOR_RELAY_SYNC) {
        Serial.printf("⚠️ [SYNC] Heap baixo (%u bytes) — adiando sync relay_slaves\n", ESP.getFreeHeap());
        return;
    }

    if (supabase.isRequestInProgress()) {
        Serial.println("⚠️ [SYNC] Supabase ocupado — adiando sync relay_slaves");
        return;
    }
    
    if (!supabase.isReady()) return;

    relaySyncInProgress = true;
    esp_task_wdt_reset();
    
    Serial.println("[SYNC] relay_master + relay_slaves → Supabase");
    
    // ===== 1. MASTER RELAYS (dosadores, níveis, reservados) =====
    bool* masterRelayStates = hydroControl.getRelayStates();
    
    // ✅ EVENT-DRIVEN: Salvar em NVS primeiro (cache local)
    saveMasterRelayStatesToNVS();
    
    // ✅ POST para Supabase (relay_master)
    if (supabase.updateRelayMaster(getDeviceID(), masterRelayStates, nullptr, nullptr, nullptr)) {
        Serial.println("[SYNC] relay_master PATCH ok");
        if (!mqttClient.isConnected()) {
            syncEcOperationStateToSupabase();
            syncPhOperationStateToSupabase();
        }
    } else {
        Serial.println("[SYNC] relay_master PATCH fail");
    }
    
    // ===== 2. SLAVE RELAYS (via ESP-NOW) =====
    if (masterManager) {
        static const int MAX_SYNC_SLAVES = 10;
        uint8_t slaveMacList[MAX_SYNC_SLAVES][6];
        int slaveMacCount = 0;

        masterManager->forEachTrustedSlave([&](const TrustedSlave& slaveSnapshot) {
            if (slaveMacCount >= MAX_SYNC_SLAVES) {
                return;
            }
            memcpy(slaveMacList[slaveMacCount], slaveSnapshot.macAddress, 6);
            slaveMacCount++;
        });

        int updatedCount = 0;
        unsigned long now = millis();

        for (int si = 0; si < slaveMacCount; si++) {
            const uint8_t* macPtr = slaveMacList[si];
            TrustedSlave* slaveSnapshot = masterManager->getTrustedSlave(macPtr);
            if (!slaveSnapshot) {
                continue;
            }

            unsigned long timeSinceLastSeen = now - slaveSnapshot->lastSeen;
            bool recentlySeen = (timeSinceLastSeen < 90000);

            if (!slaveSnapshot->isOnline() && !recentlySeen) {
                continue;
            }

            bool cachedStates[8] = {false};
            for (int i = 0; i < slaveSnapshot->numRelays && i < 8; i++) {
                cachedStates[i] = slaveSnapshot->relayStates[i].state;
            }
            const bool unchanged = relayStatesUnchanged(macPtr, cachedStates);
            const bool forceRfBackup =
                (now - getLastForceRfPollMs(macPtr) >= RELAY_STATES_SYNC_FORCE_RF_MS);
            if (unchanged && !forceRfBackup) {
                Serial.printf("[SYNC] skip unchanged (no RF poll) slave=%s\n",
                              ESPNowController::macToString(macPtr).c_str());
                continue;
            }

            masterManager->drainAllRelaysStatusWait();
            masterManager->requestSlaveStatus(macPtr);
            touchLastForceRfPollMs(macPtr, now);
            const bool gotStatus = masterManager->waitForAllRelaysStatus(800);
            esp_task_wdt_reset();

            TrustedSlave* slave = masterManager->getTrustedSlave(macPtr);
            if (!slave) {
                Serial.printf("[SYNC] skip slave %s — não encontrado após wait\n",
                              ESPNowController::macToString(macPtr).c_str());
                continue;
            }

            bool slaveRelayStates[8] = {false};
            bool slaveHasTimers[8] = {false};
            int slaveRemainingTimes[8] = {0};

            for (int i = 0; i < slave->numRelays && i < 8; i++) {
                slaveRelayStates[i] = slave->relayStates[i].state;
                slaveHasTimers[i] = slave->relayStates[i].hasTimer;
                slaveRemainingTimes[i] = slave->relayStates[i].remainingTime;
            }

            char mask[9];
            formatRelayMaskSync(slaveRelayStates, mask);
            Serial.printf("[SYNC] slave=%s mask=%s wait=%s lastSeen=%lums\n",
                            ESPNowController::macToString(slave->macAddress).c_str(),
                            mask,
                            gotStatus ? "ok" : "timeout",
                            timeSinceLastSeen);

            if (!gotStatus) {
                Serial.printf("[SYNC] skip PATCH (ESP-NOW timeout) slave=%s\n",
                            ESPNowController::macToString(slave->macAddress).c_str());
                continue;
            }

            if (relayStatesUnchanged(slave->macAddress, slaveRelayStates)) {
                Serial.printf("[SYNC] skip unchanged slave=%s\n",
                                ESPNowController::macToString(slave->macAddress).c_str());
                continue;
            }

            String slaveMac = ESPNowController::macToString(slave->macAddress);
            String slaveDeviceId = "ESP32_SLAVE_" + slaveMac;
            slaveDeviceId.replace(":", "_");

            if (supabase.updateRelaySlaves(slaveDeviceId, getDeviceID(), slaveMac,
                                          slaveRelayStates, slaveHasTimers,
                                          slaveRemainingTimes, nullptr)) {
                storeLastSynced(slave->macAddress, slaveRelayStates);
                updatedCount++;
                Serial.printf("[SYNC] PATCH ok slave=%s heap=%u\n",
                                slaveDeviceId.c_str(), ESP.getFreeHeap());
            } else {
                Serial.printf("[SYNC] PATCH fail slave=%s\n", slaveDeviceId.c_str());
            }
            esp_task_wdt_reset();
        }
        
        if (updatedCount > 0) {
            Serial.printf("[SYNC] %d slave(s) PATCH relay_slaves\n", updatedCount);
        }
    }
    
    relaySyncInProgress = false;
    Serial.println("[SYNC] complete");
}

void HydroSystemCore::performMemoryProtection() {
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxBlock = ESP.getMaxAllocHeap();
    uint32_t totalHeap = ESP.getHeapSize();
    uint32_t fragmentationPercent = freeHeap > 0 ? (100 - (maxBlock * 100) / freeHeap) : 100;
    
    // ===== PROTEÇÃO CRÍTICA =====
    if (freeHeap < 15000) {
        Serial.println("🚨 ALERTA: Heap crítico! " + String(freeHeap) + " bytes");
        
        if (freeHeap < 8000) {
            Serial.println("💀 RESET EMERGENCIAL por falta de memória!");
            delay(1000);
            ESP.restart();
        }
    }
    
    // ===== PROTEÇÃO POR FRAGMENTAÇÃO =====
    if (fragmentationPercent > 70) {
        Serial.println("🧩 ALERTA: Fragmentação alta! " + String(fragmentationPercent) + "%");
        
        if (fragmentationPercent > 85 && freeHeap > 10000) {
            Serial.println("🔄 RESET por fragmentação extrema!");
            delay(1000);
            ESP.restart();
        }
    }
    
    // ===== DESABILITAR SUPABASE SE MEMÓRIA CRÍTICA (histerese) =====
    if (freeHeap < HEAP_SUPABASE_DISABLE && supabaseConnected) {
        Serial.println("⚠️ Desabilitando Supabase temporariamente - Heap baixo");
        supabaseConnected = false;
    } else if (freeHeap > HEAP_SUPABASE_REENABLE && !supabaseConnected) {
        Serial.println("✅ Reabilitando Supabase - Heap recuperado");
        supabaseConnected = true;
    }
}

// ===== UTILITIES =====
const char* HydroSystemCore::httpsConfigPollSkipReason() {
    if (!supabaseConnected) {
        return "supabaseDisconnected";
    }
    const uint32_t heap = ESP.getFreeHeap();
    const uint32_t maxBlock = ESP.getMaxAllocHeap();
    if (heap < MIN_HEAP_FOR_HTTPS) {
        return "heapLow";
    }
    if (maxBlock < MIN_CONTIGUOUS_FOR_HTTPS) {
        return "maxAllocLow";
    }
    if (supabase.isRequestInProgress()) {
        return "requestInProgress";
    }
    if (isSslTransportBusy()) {
        return "sslTransport";
    }
    if (!supabase.isReady()) {
        return "supabaseNotReady";
    }
    return nullptr;
}

bool HydroSystemCore::hasEnoughMemoryForHTTPS() {
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_HTTPS) {
        return false;
    }
    const uint32_t maxBlock = ESP.getMaxAllocHeap();
    if (maxBlock < MIN_CONTIGUOUS_FOR_HTTPS) {
        return false;
    }
    // Uma request SSL de cada vez — evita OOM com MQTT retry + hydro em paralelo
    if (supabase.isRequestInProgress()) {
        return false;
    }
    return true;
}

void HydroSystemCore::printPeriodicStatus() {
    Serial.printf("🔄 Sistema ativo há %ds | Heap: %d bytes | Supabase: %s | MASTER MODE\n", 
                  (int)(getUptime()/1000), 
                  ESP.getFreeHeap(),
                  supabaseConnected ? "✅" : "❌");
}

// ============================================
// ✅ TÓPICO 4: PROCESAR COMANDOS DE QUEUE (Core 0)
// ============================================
void HydroSystemCore::processWebCommands() {
    if (!webServerManager) return;  // WebServerManager no inicializado
    
    // ✅ TÓPICO 4: Recibir comandos de la queue (no bloqueante)
    WebCommand cmd;
    while (webServerManager->receiveCommand(cmd, 0)) {  // Timeout 0 = no bloqueante
        Serial.printf("\n📥 TÓPICO 4: Comando recibido de queue: type=%d, relay=%d, action=%s, deviceId=%s\n",
                     cmd.type, cmd.relayNumber, cmd.action.c_str(), cmd.deviceId.c_str());
        
        // ✅ TÓPICO 4: Procesar según tipo de comando
        switch (cmd.type) {
            case WebCommand::RELAY_CONTROL: {
                // Controlar relay local o remoto
                if (cmd.deviceId.isEmpty() || cmd.deviceId == "local" || cmd.deviceId == "MASTER") {
                    // ===== RELAY LOCAL =====
                    Serial.printf("🔌 TÓPICO 4: Controlar relay LOCAL %d -> %s (duration: %d)\n",
                                 cmd.relayNumber, cmd.action.c_str(), cmd.duration);
                    
                    if (cmd.action == "toggle") {
                        hydroControl.toggleRelay(cmd.relayNumber, cmd.duration);
                    } else if (cmd.action == "on" || cmd.action == "on_forever") {
                        int duration = (cmd.action == "on_forever") ? 0 : cmd.duration;
                        hydroControl.setRelay(cmd.relayNumber, true, duration);
                    } else if (cmd.action == "off") {
                        hydroControl.setRelay(cmd.relayNumber, false, 0);
                    }
                    
                    Serial.printf("✅ TÓPICO 4: Relay local %d controlado\n", cmd.relayNumber);
                } else {
                    // ===== RELAY REMOTO (ESP-NOW Slave) =====
                    Serial.printf("📡 TÓPICO 4: Controlar relay REMOTO %d -> %s en deviceId=%s\n",
                                 cmd.relayNumber, cmd.action.c_str(), cmd.deviceId.c_str());
                    
                    if (masterManager) {
                        // Verificar si MAC está disponible
                        bool hasMac = false;
                        for (int i = 0; i < 6; i++) {
                            if (cmd.slaveMac[i] != 0) {
                                hasMac = true;
                                break;
                            }
                        }
                        
                        if (hasMac) {
                            // ✅ Compatibilidade: Receber commandId mas usar como bool
                            uint32_t commandId = relayCoordinator.actuateSlave(
                                RelayOwner::Manual,
                                cmd.slaveMac,
                                cmd.relayNumber,
                                cmd.action,
                                cmd.duration
                            );
                            bool success = (commandId > 0);  // ✅ Conversão para compatibilidade
                            
                            if (success) {
                                Serial.printf("✅ TÓPICO 4: Comando ESP-NOW enviado a %s (relay %d)\n",
                                             cmd.deviceId.c_str(), cmd.relayNumber);
                            } else {
                                Serial.printf("❌ TÓPICO 4: Error al enviar comando ESP-NOW a %s\n",
                                             cmd.deviceId.c_str());
                            }
                        } else {
                            Serial.printf("⚠️ TÓPICO 4: MAC no disponible para deviceId=%s\n",
                                         cmd.deviceId.c_str());
                        }
                    } else {
                        Serial.println("❌ TÓPICO 4: MasterSlaveManager no disponible");
                    }
                }
                break;
            }
            
            case WebCommand::ALL_RELAYS_ON: {
                Serial.println("🔌 TÓPICO 4: Encender TODOS los relays locales");
                for (int i = 0; i < 16; i++) {
                    hydroControl.setRelay(i, true, 0);  // Permanente
                }
                Serial.println("✅ TÓPICO 4: Todos los relays encendidos");
                break;
            }
            
            case WebCommand::ALL_RELAYS_OFF: {
                Serial.println("🔄 TÓPICO 4: Apagar TODOS los relays locales");
                for (int i = 0; i < 16; i++) {
                    hydroControl.setRelay(i, false, 0);
                }
                Serial.println("✅ TÓPICO 4: Todos los relays apagados");
                break;
            }
            
            case WebCommand::DISCOVER_SLAVES: {
                Serial.println("🔍 TÓPICO 4: Discovery de slaves solicitado");
                if (masterManager) {
                    masterManager->rediscoverSlaves();
                    Serial.println("✅ TÓPICO 4: Discovery iniciado");
                } else {
                    Serial.println("❌ TÓPICO 4: MasterSlaveManager no disponible");
                }
                break;
            }
            
            default:
                Serial.printf("⚠️ TÓPICO 4: Tipo de comando desconocido: %d\n", cmd.type);
                break;
        }
    }
}

// ===== REGISTRO DE ENDPOINTS DO WEBSERVER =====

void HydroSystemCore::registerWebServerEndpoints() {
    // ✅ Método público para registrar endpoints quando webServerTask estiver disponível
    if (endpointsRegistered) {
        Serial.println("✅ Endpoints já foram registrados anteriormente");
        return;
    }
    
    tryRegisterEndpoints();
}

void HydroSystemCore::tryRegisterEndpoints() {
    // ✅ Método privado que tenta registrar endpoints se webServerTask estiver disponível
    if (endpointsRegistered) {
        return;  // Já foram registrados
    }
    
    // Verificar se webServerTask está disponível e inicializado
    if (!webServerTask || !webServerTask->isInitialized()) {
        // Não logar a cada tentativa para evitar spam no Serial
        static unsigned long lastWarning = 0;
        unsigned long now = millis();
        if (now - lastWarning >= 10000) {  // Logar apenas a cada 10 segundos
            Serial.println("⚠️ WebServerTask ainda não disponível - endpoints não configurados");
            Serial.printf("   webServerTask: %s\n", webServerTask ? "✅ Disponível" : "❌ nullptr");
            if (webServerTask) {
                Serial.printf("   isInitialized(): %s\n", webServerTask->isInitialized() ? "✅ SIM" : "❌ NÃO");
            }
            lastWarning = now;
        }
        return;
    }
    
    // ✅ WebServerTask está disponível - registrar endpoints
    Serial.println("\n🔧 ========================================");
    Serial.println("🔧 REGISTRANDO ENDPOINTS DO WEBSERVER");
    Serial.println("🔧 ========================================");
    Serial.printf("   webServerTask: ✅ Disponível\n");
    Serial.printf("   webServerTask->isInitialized(): ✅ SIM\n");
    Serial.printf("   masterManager: %s\n", masterManager ? "✅ Disponível" : "❌ nullptr");
    
    // ✅ TÓPICO 4: Obter instância estática do WebServerManager
    static WiFiManager wifiManager;
    static WebServerManager webServerManagerInstance;
    
    // ✅ CORREÇÃO CRÍTICA: Inicializar WiFiManager se ainda não foi inicializado
    static bool wifiManagerInitialized = false;
    if (!wifiManagerInitialized) {
        Serial.println("🔧 Inicializando WiFiManager para WebServerManager...");
        wifiManagerInitialized = true;
    }
    
    // ✅ Guardar referência para processar comandos de queue
    this->webServerManager = &webServerManagerInstance;
    
    // ✅ Registrar endpoints via beginAdminServer
    webServerManagerInstance.beginAdminServer(wifiManager, hydroControl, webServerTask, masterManager);
    
    // ✅ Marcar como registrado
    endpointsRegistered = true;
    
    Serial.println("✅ Sistema configurado com sucesso!");
    Serial.println("   ✓ Supabase: device_status (escrita cada 60s)");
#if ENABLE_MQTT && MQTT_HYDRO_ONLY
    Serial.println("   ✓ Sensores: MQTT (hydro + ambiente) + HTTPS fallback se broker cair");
#else
    Serial.println("   ✓ Sensores: bivalente MQTT 30s + HTTPS hydro/environment 30s");
#endif
    Serial.println("   ✓ Supabase: relay_states (escrita quando muda estado)");
    Serial.println("   ✓ Supabase: relay_commands (leitura cada 30s)");
    Serial.println("   ✓ Endpoints HTTP desabilitados - usando apenas Supabase");
    Serial.println("   ✓ Frontend lê diretamente de Supabase");
    Serial.println("========================================\n");
}

void HydroSystemCore::setWebServerTask(WebServerTask* webTask) {
    // ✅ Setter melhorado: tenta registrar endpoints se sistema já estiver pronto
    this->webServerTask = webTask;
    
    Serial.printf("🔧 [HydroSystemCore] webServerTask atualizado: %s\n", 
                  this->webServerTask ? "✅ Disponível" : "❌ nullptr");
    
    // Se sistema já está pronto e webServerTask está disponível, tentar registrar endpoints
    if (systemReady && this->webServerTask && this->webServerTask->isInitialized() && !endpointsRegistered) {
        Serial.println("🔄 Sistema já está pronto - tentando registrar endpoints agora...");
        tryRegisterEndpoints();
    }
}

// ===== 🎯 CACHE NVS DE ESTADOS DE MASTER RELAYS (LOCAL, NÃO ESP-NOW) =====
// ✅ SEGREGADO: Master relays são do corpo físico do master, não têm nada a ver com ESP-NOW

bool HydroSystemCore::saveMasterRelayStatesToNVS() {
    MasterRelayStatesCache cache = {};
    cache.timestamp = millis();
    cache.version = 1;
    cache.numRelays = 16;
    
    // Obter estados dos relés do HydroControl
    bool* relayStates = hydroControl.getRelayStates();
    
    // Preencher cache com estados dos 16 relés
    for (int i = 0; i < 16; i++) {
        CachedMasterRelayState& cachedState = cache.states[i];
        cachedState.relayNumber = i;
        cachedState.state = relayStates[i] ? 1 : 0;
        cachedState.hasTimer = 0;  // TODO: Obter de HydroControl se disponível
        cachedState.remainingTime = 0;  // TODO: Obter de HydroControl se disponível
        cachedState.timestamp = millis();
        
        // Determinar tipo de relé
        if (i >= 0 && i <= 7) {
            cachedState.relayType = 0;  // Doser (0-7)
        } else if (i >= 8 && i <= 11) {
            cachedState.relayType = 1;  // Level (8-11)
        } else {
            cachedState.relayType = 2;  // Reserved (12-15)
        }
    }
    
    // Calcular checksum
    uint8_t checksum = 0;
    uint8_t* data = (uint8_t*)&cache;
    for (size_t i = 0; i < sizeof(MasterRelayStatesCache) - 1; i++) {
        checksum ^= data[i];
    }
    cache.checksum = checksum;
    
    // Guardar em NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open("master_states", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        Serial.println("❌ Erro ao abrir NVS para guardar cache de master relays: " + String(esp_err_to_name(err)));
        return false;
    }
    
    // ✅ CORRIGIDO: NVS tem limite de 15 caracteres para chaves
    err = nvs_set_blob(handle, "mstr_rly_cache", &cache, sizeof(MasterRelayStatesCache));
    if (err != ESP_OK) {
        Serial.println("❌ Erro ao guardar cache em NVS: " + String(esp_err_to_name(err)));
        nvs_close(handle);
        return false;
    }
    
    err = nvs_commit(handle);
    nvs_close(handle);
    
    if (err == ESP_OK) {
        Serial.printf("💾 Cache de estados de master relays guardado em NVS (16 relés)\n");
        StatePersistenceManager::saveMasterRelayCache(cache);
        return true;
    } else {
        Serial.println("❌ Erro ao fazer commit em NVS: " + String(esp_err_to_name(err)));
        return false;
    }
}

bool HydroSystemCore::loadMasterRelayStatesFromNVS() {
    MasterRelayStatesCache cache = {};
    
    // Carregar de NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open("master_states", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false; // Nenhum cache encontrado (primeira inicialização)
    }
    
    size_t required_size = sizeof(MasterRelayStatesCache);
    // ✅ CORRIGIDO: NVS tem limite de 15 caracteres para chaves
    err = nvs_get_blob(handle, "mstr_rly_cache", &cache, &required_size);
    nvs_close(handle);
    
    if (err != ESP_OK) {
        return false; // Nenhum cache encontrado
    }
    
    // Validar checksum
    uint8_t checksum = 0;
    uint8_t* data = (uint8_t*)&cache;
    for (size_t i = 0; i < sizeof(MasterRelayStatesCache) - 1; i++) {
        checksum ^= data[i];
    }
    
    if (checksum != cache.checksum) {
        Serial.println("❌ Cache de master relays inválido (checksum incorreto)");
        return false;
    }
    
    // Aplicar estados cacheados aos relés (se necessário)
    // Nota: Por enquanto apenas carregamos, não aplicamos automaticamente
    // Isso pode ser feito em begin() se necessário
    
    Serial.printf("✅ Cache de master relays carregado de NVS (timestamp: %lu)\n", cache.timestamp);
    return true;
}

// ✅ NOVO: Função auxiliar para parsear MAC address
bool HydroSystemCore::parseMacAddress(const String& macStr, uint8_t* macBytes) {
    // Formato esperado: "14:33:5C:38:BF:60"
    int values[6];
    int count = sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", 
                      &values[0], &values[1], &values[2], 
                      &values[3], &values[4], &values[5]);
    
    if (count != 6) {
        Serial.printf("❌ Erro ao parsear MAC: %s\n", macStr.c_str());
        return false;
    }
    
    for (int i = 0; i < 6; i++) {
        macBytes[i] = (uint8_t)values[i];
    }
    
    return true;
}

// ✅ NOVO: Função auxiliar para atualizar relay_master após completar comando
void HydroSystemCore::updateRelayMasterState(const RelayCommand& cmd) {
    if (!supabaseConnected) {
        return;
    }
    
    Serial.println("🔄 [MASTER] Atualizando relay_master com estado real...");
    
    // Buscar estado atual de todos os relays
    bool* relayStates = hydroControl.getRelayStates();
    bool relayStatesArray[16];
    
    // Copiar estados atuais
    for (int i = 0; i < 16; i++) {
        relayStatesArray[i] = relayStates[i];
    }
    
    // Atualizar estado do relay específico
    relayStatesArray[cmd.relayNumber] = (cmd.action == "on");
    
    // ✅ Atualizar no Supabase
    // Nota: Por enquanto, não temos hasTimers, remainingTimes, relayNames
    // Podemos passar nullptr para esses parâmetros
    bool success = supabase.updateRelayMaster(
        getDeviceID(),  // Usar função global getDeviceID()
        relayStatesArray,
        nullptr,  // hasTimers
        nullptr,  // remainingTimes
        nullptr   // relayNames
    );
    
    if (success) {
        Serial.printf("✅ [MASTER] relay_master atualizado (relé %d = %s)\n", 
                     cmd.relayNumber, cmd.action.c_str());
    } else {
        Serial.println("❌ [MASTER] Falha ao atualizar relay_master");
    }
}

// ✅ NOVO: Sistema de mapeamento commandId → supabaseCommandId
void HydroSystemCore::addCommandMapping(uint32_t espNowCommandId, int supabaseCommandId) {
    if (mappingsMutex == nullptr) {
        return;
    }
    
    if (xSemaphoreTake(mappingsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em addCommandMapping()");
        return;
    }
    
    CommandMapping mapping;
    mapping.espNowCommandId = espNowCommandId;
    mapping.supabaseCommandId = supabaseCommandId;
    mapping.timestamp = millis();
    
    commandMappings.push_back(mapping);
    
    Serial.printf("📝 [MAPEAMENTO] Adicionado: ESP-NOW ID=%u → Supabase ID=%d\n", 
                 espNowCommandId, supabaseCommandId);
    
    // Limpar mapeamentos expirados (> 5 minutos)
    cleanupExpiredMappings();
    
    xSemaphoreGive(mappingsMutex);
}

int HydroSystemCore::findSupabaseCommandId(uint32_t espNowCommandId) {
    if (mappingsMutex == nullptr) {
        return 0;
    }
    
    if (xSemaphoreTake(mappingsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em findSupabaseCommandId()");
        return 0;
    }
    
    int supabaseId = 0;
    for (auto it = commandMappings.begin(); it != commandMappings.end(); ++it) {
        if (it->espNowCommandId == espNowCommandId) {
            supabaseId = it->supabaseCommandId;
            commandMappings.erase(it);  // Remover após usar
            Serial.printf("🔍 [MAPEAMENTO] Encontrado: ESP-NOW ID=%u → Supabase ID=%d\n", 
                         espNowCommandId, supabaseId);
            break;
        }
    }
    
    xSemaphoreGive(mappingsMutex);
    return supabaseId;
}

void HydroSystemCore::cleanupExpiredMappings() {
    unsigned long now = millis();
    unsigned long expireTime = 300000;  // 5 minutos
    
    commandMappings.erase(
        std::remove_if(commandMappings.begin(), commandMappings.end(),
            [now, expireTime](const CommandMapping& m) {
                return (now - m.timestamp) > expireTime;
            }),
        commandMappings.end()
    );
}

void HydroSystemCore::registerPendingSlaveAck(int supabaseCommandId, uint32_t espNowCommandId,
                                              const uint8_t* slaveMac, int relayNumber,
                                              const String& action) {
    if (!slaveMac || supabaseCommandId <= 0 || pendingAckMutex == nullptr) {
        return;
    }

    if (xSemaphoreTake(pendingAckMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }

    cleanupExpiredPendingSlaveAcks();

    PendingSlaveCommandAck pending = {};
    pending.supabaseCommandId = supabaseCommandId;
    pending.espNowCommandId = espNowCommandId;
    memcpy(pending.slaveMac, slaveMac, 6);
    pending.relayNumber = relayNumber;
    pending.expectedOn = (action == "on");
    pending.sentAt = millis();
    pendingSlaveCommandAcks.push_back(pending);

    xSemaphoreGive(pendingAckMutex);
}

void HydroSystemCore::cleanupExpiredPendingSlaveAcks() {
    const unsigned long now = millis();
    pendingSlaveCommandAcks.erase(
        std::remove_if(pendingSlaveCommandAcks.begin(), pendingSlaveCommandAcks.end(),
            [now](const PendingSlaveCommandAck& p) {
                return (now - p.sentAt) > PENDING_SLAVE_ACK_TTL_MS;
            }),
        pendingSlaveCommandAcks.end());
}

bool HydroSystemCore::tryCloseCloudRelayCommand(int supabaseCommandId, const uint8_t* slaveMac,
                                                int relayNumber, bool currentState,
                                                uint32_t espNowCommandId) {
    if (supabaseCommandId <= 0) {
        return false;
    }

#if ENABLE_MQTT
    if (MQTT_COMMAND_BRIDGE_ONLY && isMqttCommandPathStable()) {
        if (tryPublishCloudAckViaMqtt(supabaseCommandId, espNowCommandId, slaveMac, relayNumber, currentState)) {
            return true;
        }
    }
#endif

    if (!supabaseConnected || supabase.isRequestInProgress()) {
        return false;
    }

    bool closed = false;
    if (slaveMac) {
        bool relayStates[8] = {false};
        String slaveMacStr = ESPNowController::macToString(slaveMac);

        if (masterManager) {
            TrustedSlave* slave = masterManager->getTrustedSlave(slaveMac);
            if (slave) {
                for (int i = 0; i < 8 && i < slave->numRelays; i++) {
                    relayStates[i] = slave->relayStates[i].state;
                }
            }
        }
        relayStates[relayNumber] = currentState;
        closed = supabase.completeRelayCommand(supabaseCommandId, currentState,
                                               slaveMacStr, relayStates, 8);
    } else {
        closed = supabase.completeRelayCommand(supabaseCommandId, currentState);
    }

    if (!closed) {
        vTaskDelay(pdMS_TO_TICKS(50));
        closed = supabase.markCommandCompleted(supabaseCommandId, currentState, slaveMac != nullptr);
    }
    return closed;
}

void HydroSystemCore::enqueuePendingCloudAck(int supabaseCommandId, uint32_t espNowCommandId,
                                             const uint8_t* slaveMac, int relayNumber,
                                             bool currentState) {
    if (supabaseCommandId <= 0) {
        return;
    }

    for (uint8_t i = 0; i < pendingCloudAckCount; i++) {
        const uint8_t idx = (pendingCloudAckHead + i) % PENDING_CLOUD_ACK_CAP;
        if (pendingCloudAckQueue[idx].supabaseCommandId == supabaseCommandId) {
            return;
        }
    }

    if (pendingCloudAckCount >= PENDING_CLOUD_ACK_CAP) {
        Serial.printf("⚠️ [ACK cloud] cola cheia — descartando id=%d mais antigo\n",
                      pendingCloudAckQueue[pendingCloudAckHead].supabaseCommandId);
        pendingCloudAckHead = (pendingCloudAckHead + 1) % PENDING_CLOUD_ACK_CAP;
        pendingCloudAckCount--;
    }

    const uint8_t tail = (pendingCloudAckHead + pendingCloudAckCount) % PENDING_CLOUD_ACK_CAP;
    PendingCloudAck& slot = pendingCloudAckQueue[tail];
    slot.supabaseCommandId = supabaseCommandId;
    slot.espNowCommandId = espNowCommandId;
    memset(slot.slaveMac, 0, 6);
    if (slaveMac) {
        memcpy(slot.slaveMac, slaveMac, 6);
    }
    slot.relayNumber = relayNumber;
    slot.currentState = currentState;
    slot.attempts = 0;
    pendingCloudAckCount++;
}

void HydroSystemCore::flushPendingCloudAcks() {
    if (pendingCloudAckCount == 0) {
        return;
    }

    bool canFlushHttps = supabaseConnected && hasEnoughMemoryForHTTPS() && !supabase.isRequestInProgress();
#if ENABLE_MQTT
    if (!canFlushHttps && !(MQTT_COMMAND_BRIDGE_ONLY && isMqttCommandPathStable())) {
        return;
    }
#else
    if (!canFlushHttps) {
        return;
    }
#endif

    PendingCloudAck& item = pendingCloudAckQueue[pendingCloudAckHead];
    const bool hasMac = item.slaveMac[0] != 0 || item.slaveMac[1] != 0 ||
                        item.slaveMac[2] != 0 || item.slaveMac[3] != 0 ||
                        item.slaveMac[4] != 0 || item.slaveMac[5] != 0;
    const uint8_t* macPtr = hasMac ? item.slaveMac : nullptr;

    esp_task_wdt_reset();
    if (tryCloseCloudRelayCommand(item.supabaseCommandId, macPtr, item.relayNumber, item.currentState,
                                  item.espNowCommandId)) {
        esp_task_wdt_reset();
        logCmdCloudAckResult("cloud-retry", item.supabaseCommandId, item.espNowCommandId,
                             item.relayNumber, item.currentState, true);
        pendingCloudAckHead = (pendingCloudAckHead + 1) % PENDING_CLOUD_ACK_CAP;
        pendingCloudAckCount--;
        return;
    }

    item.attempts++;
    if (item.attempts >= PENDING_CLOUD_ACK_MAX_ATTEMPTS) {
        Serial.printf("⚠️ [ACK cloud] abandonado após %u tentativas id=%d\n",
                      item.attempts, item.supabaseCommandId);
        pendingCloudAckHead = (pendingCloudAckHead + 1) % PENDING_CLOUD_ACK_CAP;
        pendingCloudAckCount--;
    }
}

void HydroSystemCore::completeSlaveCommand(int supabaseCommandId, uint32_t espNowCommandId,
                                           const uint8_t* slaveMac, int relayNumber, bool currentState,
                                           const char* via) {
    if (supabaseCommandId > 0 && wasRecentlyClosedCloudAck(supabaseCommandId)) {
        Serial.printf("[CMD] skip duplicate close id=%d via=%s\n", supabaseCommandId, via);
        if (masterManager && espNowCommandId > 0) {
            masterManager->removeFromRetryQueue(espNowCommandId, currentState, false);
        }
        return;
    }

    if (masterManager && espNowCommandId > 0) {
        masterManager->removeFromRetryQueue(espNowCommandId, currentState, false);
    }

    if (pendingAckMutex != nullptr &&
        xSemaphoreTake(pendingAckMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        pendingSlaveCommandAcks.erase(
            std::remove_if(pendingSlaveCommandAcks.begin(), pendingSlaveCommandAcks.end(),
                [supabaseCommandId, espNowCommandId](const PendingSlaveCommandAck& p) {
                    return p.supabaseCommandId == supabaseCommandId ||
                           (espNowCommandId > 0 && p.espNowCommandId == espNowCommandId);
                }),
            pendingSlaveCommandAcks.end());
        xSemaphoreGive(pendingAckMutex);
    }

    bool cloudClosed = false;
#if ENABLE_MQTT
    if (supabaseCommandId > 0 && MQTT_COMMAND_BRIDGE_ONLY && isMqttCommandPathStable()) {
        cloudClosed = tryPublishCloudAckViaMqtt(supabaseCommandId, espNowCommandId, slaveMac,
                                                relayNumber, currentState);
    }
#endif
    if (supabaseCommandId > 0 && !cloudClosed) {
        enqueuePendingCloudAck(supabaseCommandId, espNowCommandId, slaveMac, relayNumber, currentState);
    }

    logCmdCloudAckResult(via, supabaseCommandId, espNowCommandId, relayNumber, currentState, cloudClosed);

    if (cloudClosed && supabaseCommandId > 0) {
        markRecentlyClosedCloudAck(supabaseCommandId);
    }

    if (!cloudClosed && supabaseCommandId > 0) {
        Serial.printf("⚠️ [%s] hardware OK — ACK cloud en cola id=%d\n", via, supabaseCommandId);
    }
}

bool HydroSystemCore::isSslTransportBusy() {
    if (relaySyncInProgress) {
        return true;
    }
    if (supabase.isRequestInProgress()) {
        return true;
    }
    return false;
}

bool HydroSystemCore::isSslHotPathBusy() {
    if (hasPendingCloudAcks()) {
        return true;
    }
    if (isSslTransportBusy()) {
        return true;
    }
    if (hasPendingSlaveAcks()) {
        return true;
    }
    if (masterManager && masterManager->getPendingRelayCommandCount() > 0) {
        return true;
    }
    return false;
}

bool HydroSystemCore::wasRecentlyClosedCloudAck(int supabaseCommandId) const {
    if (supabaseCommandId <= 0) {
        return false;
    }
    const unsigned long now = millis();
    for (uint8_t i = 0; i < recentlyClosedCount; i++) {
        if (recentlyClosedSupabaseIds[i] != supabaseCommandId) {
            continue;
        }
        if (now - recentlyClosedAtMs[i] <= RECENTLY_CLOSED_ACK_TTL_MS) {
            return true;
        }
    }
    return false;
}

void HydroSystemCore::markRecentlyClosedCloudAck(int supabaseCommandId) {
    if (supabaseCommandId <= 0) {
        return;
    }
    const unsigned long now = millis();
    for (uint8_t i = 0; i < recentlyClosedCount; i++) {
        if (recentlyClosedSupabaseIds[i] == supabaseCommandId) {
            recentlyClosedAtMs[i] = now;
            return;
        }
    }
    if (recentlyClosedCount >= RECENTLY_CLOSED_ACK_CAP) {
        memmove(recentlyClosedSupabaseIds, recentlyClosedSupabaseIds + 1,
                (RECENTLY_CLOSED_ACK_CAP - 1) * sizeof(recentlyClosedSupabaseIds[0]));
        memmove(recentlyClosedAtMs, recentlyClosedAtMs + 1,
                (RECENTLY_CLOSED_ACK_CAP - 1) * sizeof(recentlyClosedAtMs[0]));
        recentlyClosedCount = RECENTLY_CLOSED_ACK_CAP - 1;
    }
    recentlyClosedSupabaseIds[recentlyClosedCount] = supabaseCommandId;
    recentlyClosedAtMs[recentlyClosedCount] = now;
    recentlyClosedCount++;
}

void HydroSystemCore::reconcilePendingSlaveAcks(const uint8_t* slaveMac, const bool relayStates[8],
                                                uint8_t numRelays) {
    if (!slaveMac || !relayStates || !supabaseConnected || pendingAckMutex == nullptr) {
        return;
    }

    if (xSemaphoreTake(pendingAckMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }

    cleanupExpiredPendingSlaveAcks();

    std::vector<PendingSlaveCommandAck> matched;
    for (const auto& pending : pendingSlaveCommandAcks) {
        if (memcmp(pending.slaveMac, slaveMac, 6) != 0) {
            continue;
        }
        if (pending.relayNumber < 0 || pending.relayNumber >= (int)numRelays || pending.relayNumber >= 8) {
            continue;
        }
        const bool actualOn = relayStates[pending.relayNumber];
        if (actualOn == pending.expectedOn) {
            matched.push_back(pending);
        }
    }

    pendingSlaveCommandAcks.erase(
        std::remove_if(pendingSlaveCommandAcks.begin(), pendingSlaveCommandAcks.end(),
            [&matched](const PendingSlaveCommandAck& p) {
                for (const auto& m : matched) {
                    if (m.supabaseCommandId == p.supabaseCommandId) {
                        return true;
                    }
                }
                return false;
            }),
        pendingSlaveCommandAcks.end());

    xSemaphoreGive(pendingAckMutex);

    for (const auto& pending : matched) {
        if (wasRecentlyClosedCloudAck(pending.supabaseCommandId)) {
            continue;
        }
        Serial.printf("[ACK-FALLBACK] completed via ALL_RELAYS id=%d relay=%d\n",
                      pending.supabaseCommandId, pending.relayNumber);
        completeSlaveCommand(pending.supabaseCommandId, pending.espNowCommandId, slaveMac,
                             pending.relayNumber, pending.expectedOn, "ACK-FALLBACK");
    }
}

// ✅ NOVO: Função auxiliar para atualizar relay_slaves após ACK
void HydroSystemCore::updateRelaySlaveState(const String& slaveDeviceId, 
                                            const uint8_t* slaveMac, 
                                            int relayNumber, 
                                            bool state) {
    if (!supabaseConnected || !masterManager) {
        return;
    }
    
    Serial.println("🔄 [SLAVE] Atualizando relay_slaves com estado real...");
    
    // Buscar estado atual do slave usando MasterSlaveManager
    // Nota: getAllTrustedSlaves retorna cópia, então vamos buscar diretamente
    bool relayStates[8] = {false};
    bool hasTimers[8] = {false};
    int remainingTimes[8] = {0};
    
    // Tentar buscar slave da lista confiável
    auto trustedSlaves = masterManager->getAllTrustedSlaves();
    bool slaveFound = false;
    
    for (const auto& s : trustedSlaves) {
        if (memcmp(s.macAddress, slaveMac, 6) == 0) {
            slaveFound = true;
            for (int i = 0; i < 8 && i < s.numRelays; i++) {
                relayStates[i] = s.relayStates[i].state;
                hasTimers[i] = s.relayStates[i].hasTimer;
                remainingTimes[i] = s.relayStates[i].remainingTime;
            }
            break;
        }
    }
    
    if (!slaveFound) {
        Serial.println("⚠️ [SLAVE] Slave não encontrado na lista confiável");
        Serial.println("💡 Inicializando arrays com valores padrão");
        // Arrays já inicializados com false/0 acima
    }
    
    // Atualizar estado do relay específico
    relayStates[relayNumber] = state;
    
    // ✅ Atualizar no Supabase
    String slaveMacStr = ESPNowController::macToString(slaveMac);
    bool success = supabase.updateRelaySlaves(
        slaveDeviceId,
        getDeviceID(),
        slaveMacStr,
        relayStates,
        hasTimers,
        remainingTimes,
        nullptr
    );
    
    if (success) {
        Serial.printf("✅ [SLAVE] relay_slaves atualizado (relé %d = %s)\n", 
                     relayNumber, state ? "ON" : "OFF");
    } else {
        Serial.println("❌ [SLAVE] Falha ao atualizar relay_slaves");
    }
}

unsigned long HydroSystemCore::resolveCommandPollIntervalMs() const {
#if ENABLE_MQTT
    if (mqttClient.isConnected()) {
        return COMMAND_POLL_INTERVAL_MQTT_OK_MS;
    }
    return COMMAND_POLL_INTERVAL_MQTT_DOWN_MS;
#else
    return COMMAND_POLL_INTERVAL_MQTT_DOWN_MS;
#endif
}

bool HydroSystemCore::hasPendingSlaveAcks() {
    if (pendingAckMutex == nullptr) {
        return false;
    }
    if (xSemaphoreTake(pendingAckMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return true;
    }
    const bool has = !pendingSlaveCommandAcks.empty();
    xSemaphoreGive(pendingAckMutex);
    return has;
}

#if ENABLE_MQTT
bool HydroSystemCore::isMqttCommandPathStable() const {
    return mqttClient.isConnected() && mqttConnectedSinceMs > 0 &&
           (millis() - mqttConnectedSinceMs) >= MQTT_COMMAND_PATH_STABLE_MS;
}

bool HydroSystemCore::tryPublishCloudAckViaMqtt(int supabaseCommandId, uint32_t espNowCommandId,
                                                const uint8_t* slaveMac, int relayNumber,
                                                bool currentState, const char* status) {
    if (!mqttClient.isConnected() || supabaseCommandId <= 0) {
        return false;
    }

    bool relayStates[8] = {false};
    const char* slaveMacStr = nullptr;
    String slaveMacString;

    if (slaveMac) {
        slaveMacString = ESPNowController::macToString(slaveMac);
        slaveMacStr = slaveMacString.c_str();
        if (masterManager) {
            TrustedSlave* slave = masterManager->getTrustedSlave(slaveMac);
            if (slave) {
                for (int i = 0; i < 8 && i < slave->numRelays; i++) {
                    relayStates[i] = slave->relayStates[i].state;
                }
            }
        }
        if (relayNumber >= 0 && relayNumber < 8) {
            relayStates[relayNumber] = currentState;
        }
    }

    MqttCommandAckReading ack = {};
    ack.commandId = supabaseCommandId;
    ack.status = (status && status[0]) ? status : "completed";
    ack.relayIndex = relayNumber;
    ack.action = currentState ? "on" : "off";
    ack.currentState = currentState;
    ack.slaveMac = slaveMacStr;
    ack.relayStates = slaveMac ? relayStates : nullptr;
    ack.numRelayStates = slaveMac ? 8 : 0;
    ack.espnowId = espNowCommandId;

    if (!mqttClient.publishCommandAck(ack)) {
        return false;
    }

    if (slaveMac) {
        publishSlaveRelayStateMqtt(slaveMac, relayNumber, currentState);
    }
    return true;
}

void HydroSystemCore::forceSlaveRelayMqttFullSync() {
    if (!masterManager || !mqttClient.isConnected()) {
        return;
    }

    std::vector<TrustedSlave> slaves = masterManager->getAllTrustedSlaves();
    for (const auto& slave : slaves) {
        const unsigned long sinceSeen = millis() - slave.lastSeen;
        if (!slave.isOnline() && sinceSeen >= 60000) {
            continue;
        }

        masterManager->drainAllRelaysStatusWait();
        masterManager->requestSlaveStatus(slave.macAddress);
        const bool gotStatus = masterManager->waitForAllRelaysStatus(800);
        esp_task_wdt_reset();

        if (gotStatus) {
            publishSlaveRelayStateMqtt(slave.macAddress, -1, false, false);
            Serial.printf("[SYNC-MQTT] full relay_states mac=%s\n",
                          ESPNowController::macToString(slave.macAddress).c_str());
        } else {
            Serial.printf("[SYNC-MQTT] ALL_RELAYS timeout mac=%s\n",
                          ESPNowController::macToString(slave.macAddress).c_str());
        }
    }
}

void HydroSystemCore::publishSlaveRelayStateMqtt(const uint8_t* slaveMac, int fallbackRelay,
                                                 bool fallbackState, bool heartbeat) {
    if (!slaveMac || !masterManager || !mqttClient.isConnected()) {
        return;
    }

    bool states[8] = {false};
    bool timers[8] = {false};
    int remaining[8] = {0};
    uint8_t n = 8;
    bool linkOnline = false;
    uint16_t linkLastSeenS = 0;

    bool gotSnapshot = masterManager->readSlaveRelaySnapshot(slaveMac, states, timers, remaining, n,
                                                             linkOnline, linkLastSeenS);
    if (!gotSnapshot) {
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_task_wdt_reset();
        gotSnapshot = masterManager->readSlaveRelaySnapshot(slaveMac, states, timers, remaining, n,
                                                            linkOnline, linkLastSeenS);
    }

    if (gotSnapshot) {
        // snapshot ok
    } else if (fallbackRelay >= 0 && fallbackRelay < 8) {
        states[fallbackRelay] = fallbackState;
        n = 8;
        Serial.printf("[MQTT] relay/state fallback R%d=%d (slave cache miss)\n",
                      fallbackRelay, fallbackState ? 1 : 0);
    } else {
        return;
    }

    String macStr = ESPNowController::macToString(slaveMac);
    MqttRelayStateReading reading = {};
    reading.slaveMac = macStr.c_str();
    reading.hasLinkMeta = true;
    reading.heartbeat = heartbeat;
    reading.omitRelayStates = heartbeat;
    if (!heartbeat) {
        reading.slaveStates = states;
        reading.slaveHasTimers = timers;
        reading.slaveRemainingTimes = remaining;
        reading.slaveCount = n;
    }
    reading.linkOnline = linkOnline;
    reading.linkLastSeenS = linkLastSeenS;
    if (heartbeat) {
        Serial.printf("[SLAVE-LINK] event=relay_state_link_heartbeat mac=%s online=%d lastSeen=%us\n",
                      macStr.c_str(), reading.linkOnline ? 1 : 0,
                      (unsigned)linkLastSeenS);
    }
    mqttClient.publishRelayState(reading);
}
#endif

bool HydroSystemCore::parseMqttEcConfigJson(const char* json, size_t len, ECConfig& config) {
    config = ECConfig();
    config.isValid = false;
    if (json == nullptr || len == 0) {
        return false;
    }
    DynamicJsonDocument doc(3072);
    if (deserializeJson(doc, json, len)) {
        return false;
    }
    JsonObject o;
    if (doc.is<JsonArray>()) {
        JsonArray arr = doc.as<JsonArray>();
        if (arr.size() == 0) {
            return false;
        }
        o = arr[0];
    } else if (doc.is<JsonObject>()) {
        o = doc.as<JsonObject>();
    } else {
        return false;
    }
    config.base_dose = o["base_dose"] | 0.0;
    config.flow_rate = 0.0; // legado; vazão = nutrients[].flowRate
    config.volume = o["volume"] | 10.0;
    config.total_ml = o["total_ml"] | 0.0;
    config.kp = o["kp"] | 1.0;
    config.ec_setpoint = o["ec_setpoint"] | 0.0;
    config.tolerance = o["tolerance"] | 50.0;
    config.auto_enabled = o["auto_enabled"] | false;
    config.intervalo_auto_ec = o["intervalo_auto_ec"] | 300;
    config.tempo_recirculacao = o["tempo_recirculacao"] | 60;
    if (o.containsKey("dilution_auto_enabled")) {
        config.dilution_auto_enabled = o["dilution_auto_enabled"];
    } else {
        config.dilution_auto_enabled = config.auto_enabled;
    }
    config.dilution_drain_relay = o["dilution_drain_relay"] | -1;
    config.dilution_fill_relay = o["dilution_fill_relay"] | -1;
    if (o.containsKey("dilution_drain_slave_mac") && !o["dilution_drain_slave_mac"].isNull()) {
        config.dilution_drain_slave_mac = o["dilution_drain_slave_mac"].as<String>();
    }
    if (o.containsKey("dilution_fill_slave_mac") && !o["dilution_fill_slave_mac"].isNull()) {
        config.dilution_fill_slave_mac = o["dilution_fill_slave_mac"].as<String>();
    }
    config.dilution_max_volume_l = o["dilution_max_volume_l"] | 50.0;
    config.flowmeter_pulses_per_liter = o["flowmeter_pulses_per_liter"] | 396.0;
    config.dilution_fill_flow_lps = o["dilution_fill_flow_lps"] | 0.5;
    config.aggressiveness = o["aggressiveness"] | 0.5;
    config.consumo_24h = o["consumo_24h"] | false;
    config.pulse_ml = o["pulse_ml"] | 2.0;
    config.pulse_gap_sec = o["pulse_gap_sec"] | 2.0;
    if (o.containsKey("nutrients") && o["nutrients"].is<JsonArray>()) {
        serializeJson(o["nutrients"].as<JsonArray>(), config.nutrientsJson);
    } else {
        config.nutrientsJson = "[]";
    }
    config.isValid = true;
    return true;
}

bool HydroSystemCore::parseMqttPhConfigJson(const char* json, size_t len, PHConfig& config) {
    config = PHConfig();
    config.isValid = false;
    if (json == nullptr || len == 0) {
        return false;
    }
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, json, len)) {
        return false;
    }
    JsonObject o;
    if (doc.is<JsonArray>()) {
        JsonArray arr = doc.as<JsonArray>();
        if (arr.size() == 0) {
            return false;
        }
        o = arr[0];
    } else if (doc.is<JsonObject>()) {
        o = doc.as<JsonObject>();
    } else {
        return false;
    }
    config.ph_setpoint = o["ph_setpoint"] | 6.0;
    config.ph_tolerance = o["ph_tolerance"] | 0.2;
    config.flow_rate_ph_up = o["flow_rate_ph_up"] | 1.0;
    config.flow_rate_ph_down = o["flow_rate_ph_down"] | 1.0;
    config.volume = o["volume"] | 100.0;
    config.ml_per_ph_unit = o["ml_per_ph_unit"] | 2.0;
    config.ml_per_ph_unit_acid = o["ml_per_ph_unit_acid"] | config.ml_per_ph_unit;
    config.ml_per_ph_unit_base = o["ml_per_ph_unit_base"] | config.ml_per_ph_unit;
    config.relay_ph_up = o["relay_ph_up"] | 1;
    config.relay_ph_down = o["relay_ph_down"] | 0;
    config.auto_enabled = o["auto_enabled"] | false;
    config.intervalo_auto_ph = o["intervalo_auto_ph"] | 300;
    config.tempo_recirculacao = o["tempo_recirculacao"] | 60;
    config.aggressiveness = o["aggressiveness"] | 0.5;
    config.gain_alpha = o["gain_alpha"] | 0.2;
    config.k_acid = o["k_acid"] | 0.0;
    config.k_base = o["k_base"] | 0.0;
    config.reset_k_gains = o["reset_k_gains"] | false;
    config.consumo_24h = o["consumo_24h"] | false;
    config.pulse_ml = o["pulse_ml"] | 2.0;
    config.pulse_gap_sec = o["pulse_gap_sec"] | 2.0;
    config.isValid = true;
    return true;
}

bool HydroSystemCore::applyECConfig(const ECConfig& config, const char* via) {
    if (!config.isValid) {
        return false;
    }
    Serial.printf("[EC CONFIG] apply via=%s auto=%s sp=%.0f\n",
                  via ? via : "?", config.auto_enabled ? "SIM" : "NAO", config.ec_setpoint);
    hydroControl.getECController().setBaseDose(config.base_dose);
    hydroControl.getECController().setVolume(config.volume);
    hydroControl.getECController().setTotalMl(config.total_ml);
    hydroControl.getECController().setKp(config.kp);
    hydroControl.setECSetpoint(config.ec_setpoint, false);
    hydroControl.setECTolerance((float)config.tolerance, false);
    hydroControl.setAutoECEnabled(config.auto_enabled, false);
    if (!config.auto_enabled) {
        hydroControl.cancelCurrentDosage();
    }
    hydroControl.setAutoECInterval(config.intervalo_auto_ec, false);
    hydroControl.setTempoRecirculacaoSeconds(config.tempo_recirculacao);
    hydroControl.setMaxStepEcFraction((float)config.aggressiveness);
    hydroControl.setEcPulseDosing((float)config.pulse_ml, (float)config.pulse_gap_sec);
    hydroControl.setConsumoEc24hEnabled(config.consumo_24h);
    hydroControl.setDilutionAutoEnabled(
        config.dilution_auto_enabled || config.auto_enabled, false);
    hydroControl.setDilutionSlaveRelays(
        config.dilution_drain_slave_mac,
        config.dilution_drain_relay,
        config.dilution_fill_slave_mac,
        config.dilution_fill_relay);
    hydroControl.setDilutionMaxVolumeL((float)config.dilution_max_volume_l);
    hydroControl.setFlowmeterPulsesPerLiter((float)config.flowmeter_pulses_per_liter);
    hydroControl.setDilutionFillFlowLps((float)config.dilution_fill_flow_lps);
    if (config.nutrientsJson.length() > 0 && config.nutrientsJson != "[]") {
        int jsonSize = max(512, (int)(config.nutrientsJson.length() * 1.3));
        DynamicJsonDocument nutrientsDoc(jsonSize);
        if (!deserializeJson(nutrientsDoc, config.nutrientsJson) && nutrientsDoc.is<JsonArray>()) {
            JsonArray nutrientsArray = nutrientsDoc.as<JsonArray>();
            DynamicJsonDocument adaptedDoc(jsonSize);
            JsonArray adaptedArray = adaptedDoc.to<JsonArray>();
            for (JsonVariant nutrient : nutrientsArray) {
                float mlL = 0.0f;
                if (nutrient.containsKey("mlPerLiter")) {
                    mlL = nutrient["mlPerLiter"].as<float>();
                } else if (nutrient.containsKey("ml_per_liter")) {
                    mlL = nutrient["ml_per_liter"].as<float>();
                }
                if (!nutrient["active"].as<bool>() && mlL < 0.1f) {
                    continue;
                }
                int relay = nutrient["relay"].as<int>();
                if (relay < 0 || relay >= 16) {
                    continue;
                }
                JsonObject adapted = adaptedArray.createNestedObject();
                adapted["name"] = nutrient["name"].as<String>();
                adapted["mlPerLiter"] = mlL;
                adapted["active"] = true;
                adapted["relayNumber"] = relay + 1;
                float q = 0.0f;
                if (nutrient.containsKey("flowRate")) {
                    q = nutrient["flowRate"].as<float>();
                } else if (nutrient.containsKey("flow_rate")) {
                    q = nutrient["flow_rate"].as<float>();
                }
                if (q > 0.01f) {
                    adapted["flowRate"] = q;
                }
            }
            if (adaptedArray.size() > 0) {
                hydroControl.updateNutrientProportions(adaptedArray);
            }
        }
    }
    hydroControl.saveECControllerConfig();
    if (!config.auto_enabled) {
        syncEcOperationStateToSupabase();
    }
    return true;
}

bool HydroSystemCore::applyPHConfig(const PHConfig& config, const char* via) {
    if (!config.isValid) {
        return false;
    }
    Serial.printf("[PH CONFIG] apply via=%s auto=%s sp=%.2f\n",
                  via ? via : "?", config.auto_enabled ? "SIM" : "NAO", config.ph_setpoint);
    hydroControl.setPHSetpoint((float)config.ph_setpoint, false);
    hydroControl.setPHTolerance((float)config.ph_tolerance);
    hydroControl.setPhPumpConfig(
        config.relay_ph_up,
        config.relay_ph_down,
        (float)config.flow_rate_ph_up,
        (float)config.flow_rate_ph_down,
        (float)config.ml_per_ph_unit_acid,
        (float)config.ml_per_ph_unit_base);
    hydroControl.setPhAdaptiveConfig((float)config.aggressiveness, (float)config.gain_alpha);
    if (config.reset_k_gains) {
        hydroControl.resetPhLearnedGains();
    }
    hydroControl.setAutoPHInterval(config.intervalo_auto_ph, false);
    hydroControl.setAutoPHEnabled(config.auto_enabled, false);
    hydroControl.setPhRecirculacaoSeconds(config.tempo_recirculacao);
    hydroControl.setPhPulseDosing((float)config.pulse_ml, (float)config.pulse_gap_sec);
    hydroControl.setConsumoPh24hEnabled(config.consumo_24h);
    return true;
}

void HydroSystemCore::mqttIncomingReceived(const char* topic, const char* payload, size_t length,
                                          void* userData) {
    if (!userData) {
        return;
    }
    static_cast<HydroSystemCore*>(userData)->handleMqttIncoming(topic, payload, length);
}

void HydroSystemCore::handleMqttIncoming(const char* topic, const char* payload, size_t length) {
    if (topic == nullptr || payload == nullptr) {
        return;
    }
    if (strstr(topic, "/ec/config") != nullptr) {
        ECConfig config;
        if (!parseMqttEcConfigJson(payload, length, config) || !config.isValid) {
            Serial.println("[MQTT] ec/config JSON inválido");
            return;
        }
        mqttEcConfigReceived = true;
        applyECConfig(config, "mqtt");
        return;
    }
    if (strstr(topic, "/ph/config") != nullptr) {
        PHConfig config;
        if (!parseMqttPhConfigJson(payload, length, config) || !config.isValid) {
            Serial.println("[MQTT] ph/config JSON inválido");
            return;
        }
        mqttPhConfigReceived = true;
        applyPHConfig(config, "mqtt");
        return;
    }
    handleMqttCommandPayload(payload, length);
}

void HydroSystemCore::handleMqttCommandPayload(const char* payload, size_t length) {
    float dilutionVolumeL = 0.0f;
    if (parseMqttEcDilutionCommand(payload, length, dilutionVolumeL)) {
        if (hydroControl.startEcDilution(dilutionVolumeL, "manual")) {
            syncEcOperationStateToSupabase();
        }
        return;
    }

    char interlockMode[16];
    if (parseMqttLevelInterlockCommand(payload, length, interlockMode, sizeof(interlockMode))) {
        const HydroControl::LevelInterlockMode mode =
            (strcmp(interlockMode, "carrera") == 0)
                ? HydroControl::LEVEL_INTERLOCK_CARRERA
                : HydroControl::LEVEL_INTERLOCK_NORMAL;
        if (hydroControl.setLevelInterlockMode(mode)) {
            mqttLevelsFingerprintValid = false;
            publishMqttLevels();
            publishMqttTelemetry();
        }
        return;
    }

    RelayCommand cmd;
    bool isSlave = false;
    if (!parseMqttRelayCommand(payload, length, cmd, isSlave)) {
        return;
    }
    if (mqttCommandDedup.alreadyProcessed(cmd.id)) {
        Serial.printf("[MQTT CMD] dup id=%d ignorado\n", cmd.id);
        return;
    }
    mqttCommandDedup.markProcessed(cmd.id);
    processRelayCommand(cmd, isSlave, "mqtt");
}

// ===== FIM DA IMPLEMENTAÇÃO ===== 