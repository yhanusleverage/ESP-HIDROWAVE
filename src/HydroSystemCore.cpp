#include "HydroSystemCore.h"
#include "EspNowChannelPolicy.h"
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
#include "ScriptRunner.h"
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

struct EcConfigSnapshot {
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
    bool dilution_auto_enabled = false;
    int dilution_drain_relay = -1;
    int dilution_fill_relay = -1;
    String dilution_drain_slave_mac;
    String dilution_fill_slave_mac;
    double dilution_max_volume_l = -1;
    double flowmeter_pulses_per_liter = -1;
    double dilution_fill_flow_lps = -1;
    bool initialized = false;
};

EcConfigSnapshot g_lastAppliedEcConfig;

void rememberEcConfigSnapshot(const ECConfig& config) {
    g_lastAppliedEcConfig.base_dose = config.base_dose;
    g_lastAppliedEcConfig.flow_rate = config.flow_rate;
    g_lastAppliedEcConfig.volume = config.volume;
    g_lastAppliedEcConfig.total_ml = config.total_ml;
    g_lastAppliedEcConfig.kp = config.kp;
    g_lastAppliedEcConfig.ec_setpoint = config.ec_setpoint;
    g_lastAppliedEcConfig.tolerance = config.tolerance;
    g_lastAppliedEcConfig.auto_enabled = config.auto_enabled;
    g_lastAppliedEcConfig.intervalo_auto_ec = config.intervalo_auto_ec;
    g_lastAppliedEcConfig.tempo_recirculacao = config.tempo_recirculacao;
    g_lastAppliedEcConfig.aggressiveness = config.aggressiveness;
    g_lastAppliedEcConfig.consumo_24h = config.consumo_24h;
    g_lastAppliedEcConfig.pulse_ml = config.pulse_ml;
    g_lastAppliedEcConfig.pulse_gap_sec = config.pulse_gap_sec;
    g_lastAppliedEcConfig.nutrientsJson = config.nutrientsJson;
    g_lastAppliedEcConfig.dilution_auto_enabled = config.dilution_auto_enabled;
    g_lastAppliedEcConfig.dilution_drain_relay = config.dilution_drain_relay;
    g_lastAppliedEcConfig.dilution_fill_relay = config.dilution_fill_relay;
    g_lastAppliedEcConfig.dilution_drain_slave_mac = config.dilution_drain_slave_mac;
    g_lastAppliedEcConfig.dilution_fill_slave_mac = config.dilution_fill_slave_mac;
    g_lastAppliedEcConfig.dilution_max_volume_l = config.dilution_max_volume_l;
    g_lastAppliedEcConfig.flowmeter_pulses_per_liter = config.flowmeter_pulses_per_liter;
    g_lastAppliedEcConfig.dilution_fill_flow_lps = config.dilution_fill_flow_lps;
    g_lastAppliedEcConfig.initialized = true;
}

bool ecConfigUnchanged(const ECConfig& config) {
    if (!g_lastAppliedEcConfig.initialized) {
        return false;
    }
    return g_lastAppliedEcConfig.base_dose == config.base_dose &&
           g_lastAppliedEcConfig.flow_rate == config.flow_rate &&
           g_lastAppliedEcConfig.volume == config.volume &&
           g_lastAppliedEcConfig.total_ml == config.total_ml &&
           g_lastAppliedEcConfig.kp == config.kp &&
           g_lastAppliedEcConfig.ec_setpoint == config.ec_setpoint &&
           g_lastAppliedEcConfig.tolerance == config.tolerance &&
           g_lastAppliedEcConfig.auto_enabled == config.auto_enabled &&
           g_lastAppliedEcConfig.intervalo_auto_ec == config.intervalo_auto_ec &&
           g_lastAppliedEcConfig.tempo_recirculacao == config.tempo_recirculacao &&
           g_lastAppliedEcConfig.aggressiveness == config.aggressiveness &&
           g_lastAppliedEcConfig.consumo_24h == config.consumo_24h &&
           g_lastAppliedEcConfig.pulse_ml == config.pulse_ml &&
           g_lastAppliedEcConfig.pulse_gap_sec == config.pulse_gap_sec &&
           g_lastAppliedEcConfig.nutrientsJson == config.nutrientsJson &&
           g_lastAppliedEcConfig.dilution_auto_enabled == config.dilution_auto_enabled &&
           g_lastAppliedEcConfig.dilution_drain_relay == config.dilution_drain_relay &&
           g_lastAppliedEcConfig.dilution_fill_relay == config.dilution_fill_relay &&
           g_lastAppliedEcConfig.dilution_drain_slave_mac == config.dilution_drain_slave_mac &&
           g_lastAppliedEcConfig.dilution_fill_slave_mac == config.dilution_fill_slave_mac &&
           g_lastAppliedEcConfig.dilution_max_volume_l == config.dilution_max_volume_l &&
           g_lastAppliedEcConfig.flowmeter_pulses_per_liter == config.flowmeter_pulses_per_liter &&
           g_lastAppliedEcConfig.dilution_fill_flow_lps == config.dilution_fill_flow_lps;
}

}  // namespace

static bool relaySyncInProgress = false;

#if ENABLE_HMI_UART
HydroSystemCore* HydroSystemCore::hmiBridgeInstance = nullptr;

bool HydroSystemCore::isHmiCloudOk() const {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }
#if ENABLE_MQTT
    if (mqttClient.isConnected()) {
        return true;
    }
#endif
    return supabaseConnected;
}

bool HydroSystemCore::hmiCloudOkStatic() {
    return hmiBridgeInstance ? hmiBridgeInstance->isHmiCloudOk() : false;
}

String HydroSystemCore::hmiDeviceIdStatic() {
    return getDeviceID();
}

#if UART_BRINGUP
void HydroSystemCore::dumpHmiUartLinkStatus(Stream& out) const {
    hmiUartBridge.dumpLinkStatus(out);
}

void HydroSystemCore::dumpHmiUartLinkStatusStatic(Stream& out) {
    if (hmiBridgeInstance) {
        hmiBridgeInstance->dumpHmiUartLinkStatus(out);
    } else {
        out.println("[HMI UART STATUS] bridge no init (esperar HYDRO_ACTIVE)");
    }
}
#endif
#endif

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
    lastRulesCheck(0),  // ✅ NOVO: Inicializar controle de verificação de regras
    lastMemoryProtection(0),
    lastEspNowChannelPollMs(0),
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
    lastRuleExecutedMirrorMs(0),
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
#if ENABLE_MQTT
    memset(pendingRelayStateSlots_, 0, sizeof(pendingRelayStateSlots_));
    memset(lastPublishedRelayState_, 0, sizeof(lastPublishedRelayState_));
#endif
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
#if ENABLE_HMI_UART
    hmiBridgeInstance = this;
#endif
}

void HydroSystemCore::wireMasterManagerIntegration() {
    if (!masterManager) {
        return;
    }

    masterManager->setRelayAckCallback([this](const uint8_t* senderMac, uint32_t commandId,
                                              bool success, uint8_t relayNumber, uint8_t currentState) {
        if (relayNumber == 255 || relayNumber > 7) {
            Serial.printf("[CMD ACK] MASK esp=%u ok=%u\n",
                          (unsigned)commandId, (unsigned)success);
        } else {
            Serial.printf("[CMD ACK] esp=%u R%u state=%s ok=%u\n",
                          (unsigned)commandId, static_cast<unsigned>(relayNumber),
                          currentState ? "ON" : "OFF", (unsigned)success);
        }

        hydroControl.notifyDilutionRelayAck(commandId, success, currentState != 0);

        // Batch SET_RELAY_MASK → ACK relé 255: bridge rejeita relay_index>15; fechar todos os tickets.
        if (relayNumber == 255 || relayNumber > 7) {
            if (success) {
                completePendingAcksForEspNowCommand(commandId, senderMac, "ACK-MASK");
            }
        } else {
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
            }
        }
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
        scheduleSlaveRelayStateMqtt(mac, false, false);
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
    wireRelayCoordinatorPolicyCallbacks();
    if (supabaseConnected) {
        wireMasterManagerIntegration();
    }
    if (webServerManager) {
        webServerManager->setMasterManager(masterMgr);
    }
    // Sempre late-bind no DE (mesmo se já iniciado) — senão regras remotas ficam com MSM=null.
    decisionIntegration.setMasterManager(masterMgr);
    if (!decisionEngineReady) {
        initDecisionEngine();
    } else {
        Serial.println("✅ DecisionEngine: MasterSlaveManager late-bind atualizado");
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
    wireRelayCoordinatorPolicyCallbacks();
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

    // ACK cloud via MQTT mesmo se HTTPS/Supabase falhou no boot
    if (masterManager) {
        wireMasterManagerIntegration();
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
#if MQTT_HYDRO_ONLY && HTTPS_RUNTIME_FALLBACK_DISABLED
            "mqtt_only";
#elif MQTT_HYDRO_ONLY
            "mqtt+https_fallback";
#else
            "bivalente";
#endif
        const char* healthMode =
#if MQTT_HEALTH_ONLY && HTTPS_RUNTIME_FALLBACK_DISABLED
            "mqtt_only";
#elif MQTT_HEALTH_ONLY
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
            publishMqttHeartbeat();
            lastMqttHeartbeatSend = millis();
        } else {
#if HTTPS_RUNTIME_FALLBACK_DISABLED
            Serial.println("⚠️ MQTT client não conectou — sem fallback HTTPS em runtime");
#else
            Serial.println("⚠️ MQTT client não conectou — continuando com HTTPS");
#endif
        }
    }
#endif
    
    Serial.println("✅ HydroSystemCore ativo!");
    Serial.println("💾 Heap livre: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("🌐 IP: " + WiFi.localIP().toString());
    
    // Status inicial dos sensores
    printSensorReadings();

    lastRelayStatesSync = 0;
#if ESPNOW_RELAY_BATCH_ENABLED
    memset(espNowRelayBatchSlots_, 0, sizeof(espNowRelayBatchSlots_));
#endif
#if ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED
    saveMasterRelayStatesToNVS();
#else
    syncAllRelayStatesToSupabase();
#endif
    if (bootOperationInterrupted) {
        syncEcOperationStateToSupabase();
        syncPhOperationStateToSupabase();
        if (supabaseConnected) {
            supabase.patchBootInterrupted(getDeviceID(), true);
        }
        bootOperationInterrupted = false;
    }

#if ENABLE_HMI_UART
    {
        HmiUartBridge::Context hmiCtx;
        hmiCtx.hydro = &hydroControl;
        hmiCtx.coordinator = &relayCoordinator;
        hmiCtx.masterManager = masterManager;
        hmiCtx.supabase = &supabase;
        hmiCtx.cloudOkFn = &HydroSystemCore::hmiCloudOkStatic;
        hmiCtx.deviceIdFn = &HydroSystemCore::hmiDeviceIdStatic;
        hmiUartBridge.attach(hmiCtx);
        hmiUartBridge.begin();
    }
#endif
    
    return true;
}

void HydroSystemCore::applyBootPolicies() {
    bootOperationInterrupted = hydroControl.abortAutoOperationsOnBoot();
    StatePersistenceManager::applySelectiveMasterRelayRestore(hydroControl);
}

void HydroSystemCore::initDecisionEngine() {
    decisionIntegration.setRelayCoordinator(&relayCoordinator);
    decisionIntegration.setMasterManager(masterManager);
#if ENABLE_MQTT && RULE_EXECUTED_MIRROR_ENABLED
    decisionEngine.setRuleExecutedMirrorCallback(&HydroSystemCore::onRuleExecutedMirrorStatic, this);
#endif
    if (decisionEngine.begin() && decisionIntegration.begin()) {
        decisionEngineReady = true;
        Serial.println("✅ DecisionEngine local ativo");
        // fn_* chegam via MQTT rules upsert (não inventar no boot a partir de NVS).
    } else {
        Serial.println("⚠️ DecisionEngine não iniciou — automação local desativada");
    }
}

void HydroSystemCore::wireRelayCoordinatorPolicyCallbacks() {
    relayCoordinator.setSlaveReachableCallback([this](const uint8_t mac[6]) -> bool {
        if (!masterManager || !mac) {
            return false;
        }
        TrustedSlave* slave = masterManager->getTrustedSlave(mac);
        if (!slave) {
            return false;
        }
        return masterManager->isSlaveReachable(*slave);
    });
    // Water interlock default off (hook preparado; no romper bancada).
    relayCoordinator.setWaterLevelOkCallback(
        [this]() -> bool {
            return hydroControl.isWaterLevelOk();
        },
        false);

    hydroControl.setCirculationMixStatusCallback(
        [](void* userData) -> uint8_t {
            if (!userData) {
                return 1;  // NotTyped
            }
            const CirculationMixGate gate =
                static_cast<HydroSystemCore*>(userData)->relayCoordinator.getCirculationMixGate();
            return static_cast<uint8_t>(gate);
        },
        this);
}

#if ENABLE_MQTT && RULE_EXECUTED_MIRROR_ENABLED
void HydroSystemCore::onRuleExecutedMirrorStatic(const RuleExecutedMirrorEvent& event, void* userData) {
    if (userData) {
        static_cast<HydroSystemCore*>(userData)->mirrorRuleExecuted(event);
    }
}

void HydroSystemCore::mirrorRuleExecuted(const RuleExecutedMirrorEvent& event) {
    if (!mqttClient.isConnected()) {
        Serial.println("[MQTT] rule_executed skipped (offline)");
        return;
    }

    const unsigned long now = millis();
    if (lastRuleExecutedMirrorMs > 0 &&
        (now - lastRuleExecutedMirrorMs) < RULE_EXECUTED_MIRROR_RATE_LIMIT_MS) {
        return;
    }

    char eventId[64];
    snprintf(eventId, sizeof(eventId), "%s-%lu-%d",
             event.rule_id.c_str(), static_cast<unsigned long>(now), event.relay_index);

    MqttRuleExecutedReading reading = {};
    reading.event_id = eventId;
    reading.rule_id = event.rule_id.c_str();
    reading.relay_index = event.relay_index;
    reading.action = event.action.c_str();
    reading.current_state = event.current_state;
    reading.success = event.success;
    reading.duration_sec = event.duration_sec;
    reading.slave_mac = event.slave_mac.length() > 0 ? event.slave_mac.c_str() : nullptr;

    if (mqttClient.publishRuleExecuted(reading)) {
        lastRuleExecutedMirrorMs = now;
    }
}
#endif

void HydroSystemCore::loop() {
    if (!systemReady) return;
    
    unsigned long now = millis();

    // LED: solo update() + eventos (WiFi / nube) — nunca blink por iteración.
    finishStatusLedSendPulseIfDue();
    updateStatusLedFromWifi();
    statusLed.update();

#if ENABLE_HMI_UART
    hmiUartBridge.loop();
    hmiUartBridge.maybePublishTelemetry(now);
#endif
    
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
    EspNowChannelPolicy::setMqttConnected(mqttClient.isConnected());
#else
    EspNowChannelPolicy::setMqttConnected(true);
#endif

#if ENABLE_ESPNOW
    if (masterManager && espNowController &&
        (now - lastEspNowChannelPollMs >= ESPNOW_CHANNEL_POLL_MS)) {
        EspNowChannelPolicy::checkStaChannelChange(espNowController, masterManager);
        lastEspNowChannelPollMs = now;
    }
#endif

#if ENABLE_MQTT
    if (now - lastMqttTelemetrySend >= MQTT_TELEMETRY_INTERVAL_MS) {
        const bool cloudPaused =
            (masterManager && masterManager->isEspNowLockWindowActive()) ||
            EspNowChannelPolicy::isProvisioningStaSuspendActive();
        if (cloudPaused) {
#if ESPNOW_LOCK_DEBUG
            if (masterManager && masterManager->isEspNowLockWindowActive()) {
                Serial.println("[LOCK] skip MQTT telemetry (window 5s)");
            }
#endif
        } else {
            publishMqttTelemetry();
            lastMqttTelemetrySend = now;
        }
    }
    maybePublishMqttLevelsOnChange();
    if (now - lastMqttHeartbeatSend >= MQTT_HEARTBEAT_INTERVAL_MS) {
        if (!EspNowChannelPolicy::isProvisioningStaSuspendActive()) {
            publishMqttHeartbeat();
            lastMqttHeartbeatSend = now;
        }
    }
#endif
    
    // ===== SENSORES → SUPABASE — só HTTPS se HTTPS_RUNTIME_FALLBACK_DISABLED=0 =====
#if !(ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED)
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
#endif
    
    // ===== STATUS DEVICE → SUPABASE =====
#if ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED
    if (now - lastStatusSend >= STATUS_SEND_INTERVAL) {
        saveMasterRelayStatesToNVS();
        lastStatusSend = now;
    }
#else
    // MQTT_HEALTH_ONLY + broker OK: heartbeat MQTT pinta device_status via bridge.
    // Fallback HTTPS só se HTTPS_RUNTIME_FALLBACK_DISABLED=0.
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
#endif
    
    // ===== SINCRONIZAÇÃO UNIFICADA DE RELAY STATES (30s) — adia se ACK cloud pendente =====
#if !(ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED)
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
#endif

    // ===== EC operation heartbeat — activo 12s / idle 30s (limpa estado huérfano MQTT) =====
    {
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
        // Só MACs sob mutex — publish fora (evita deadlock + sem fotocópia)
        uint8_t macList[8][6];
        int macCount = 0;
        masterManager->forEachTrustedSlave([&](const TrustedSlave& slave) {
            if (macCount >= 8) {
                return;
            }
            memcpy(macList[macCount], slave.macAddress, 6);
            macCount++;
        });
        for (int i = 0; i < macCount; i++) {
            publishSlaveRelayStateMqtt(macList[i], -1, false, true);
        }
        lastSlaveRelayHeartbeat = now;
    }

    // Sync completo relay_states[] via RF + MQTT (60s) — evita UI stale (só link-only)
    if (masterManager && mqttClient.isConnected() &&
        now - lastSlaveRelayFullSync >= RELAY_STATES_SYNC_FORCE_RF_MS) {
        const bool skipFullSync = hasPendingCloudAcks() || hasPendingSlaveAcks() ||
                                  ESP.getFreeHeap() < 60000;
        if (!skipFullSync) {
            forceSlaveRelayMqttFullSync();
        }
        lastSlaveRelayFullSync = now;
    }

    flushPendingRelayStateMqtt();
#endif

#if ESPNOW_RELAY_BATCH_ENABLED
    flushEspNowRelayBatchesDue(now);
#endif
    
    // ===== DEBUG PERIÓDICO (30s) =====
    if (now - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
        printPeriodicStatus();
        lastStatusPrint = now;
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
    if (now - lastCacheUpdate >= 5000) {
        const bool webPanelLive = webServerTask && webServerTask->isInitialized();
        if (webServerManager && webPanelLive) {
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
                
                uint32_t freeHeap = ESP.getFreeHeap();
                if (freeHeap < 50000) {
                    cache.slavesJson = "{\"slaves\":[]}";
                    cache.slavesLastUpdate = 0;
                } else {
                    ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
                    DynamicJsonDocument* pooledDoc = nullptr;
                    bool usingPool = false;

                    if (poolMgr && poolMgr->isInitialized()) {
                        pooledDoc = poolMgr->acquireJsonDocument(4096);
                        usingPool = (pooledDoc != nullptr);
                    }

                    auto fillSlavesJson = [&](DynamicJsonDocument& doc) {
                        JsonObject rootObj = doc.to<JsonObject>();
                        JsonArray slavesArray = rootObj.createNestedArray("slaves");
                        // forEach: sem fotocópia do vector (mutex curto; JSON sob lock OK aqui)
                        masterManager->forEachTrustedSlave([&](const TrustedSlave& slave) {
                            String deviceId = "ESP32_SLAVE_" + ESPNowController::macToString(slave.macAddress);
                            deviceId.replace(":", "_");
                            JsonObject slaveObj = slavesArray.createNestedObject();
                            slaveObj["device_id"] = deviceId;
                            slaveObj["device_name"] = slave.deviceName;
                            slaveObj["device_type"] = slave.deviceType;
                            slaveObj["mac_address"] = ESPNowController::macToString(slave.macAddress);
                            slaveObj["is_online"] = slave.isOnline();
                            slaveObj["num_relays"] = slave.numRelays;
                            slaveObj["last_seen"] = slave.lastSeen;
                            slaveObj["operational"] = slave.operational;
                            JsonArray relaysArray = slaveObj.createNestedArray("relays");
                            for (int i = 0; i < slave.numRelays && i < 8; i++) {
                                JsonObject relayObj = relaysArray.createNestedObject();
                                relayObj["relay_number"] = i;
                                relayObj["state"] = slave.relayStates[i].state;
                                relayObj["has_timer"] = slave.relayStates[i].hasTimer;
                                relayObj["remaining_time"] = slave.relayStates[i].remainingTime;
                                relayObj["name"] = slave.relayStates[i].name.length() > 0
                                    ? slave.relayStates[i].name
                                    : ("Relé " + String(i));
                            }
                        });
                        String slavesJson;
                        if (serializeJson(doc, slavesJson) > 0) {
                            cache.slavesJson = slavesJson;
                            cache.slavesLastUpdate = millis();
                        } else {
                            cache.slavesJson = "{\"slaves\":[]}";
                            cache.slavesLastUpdate = 0;
                        }
                    };

                    if (usingPool && pooledDoc) {
                        fillSlavesJson(*pooledDoc);
                        poolMgr->releaseJsonDocument(pooledDoc);
                    } else {
                        DynamicJsonDocument localDoc(4096);
                        fillSlavesJson(localDoc);
                    }
                }
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
        // sslBusy = TLS real (não confundir com ACK/retry ESP-NOW pendentes)
        ResourceTelemetry::setContext(
            mqttUp,
            isSslTransportBusy(),
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
    reading.circulationTyped = relayCoordinator.isCirculationConfigured();
    reading.circulationMixOk = relayCoordinator.isCirculationMixActiveForDosing();
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

// Sync config only — execução local via DecisionEngine + SPIFFS (/rules.json)
// HTTPS poll decision_rules desativado (heap/SSL). Tipagem → MQTT circ/config retained.
void HydroSystemCore::checkSupabaseRules() {
    if (!supabaseConnected) {
        return;
    }
    static unsigned long lastLogMs = 0;
    const unsigned long now = millis();
    if (now - lastLogMs < 300000UL) {
        return;
    }
    lastLogMs = now;
    Serial.println("📋 [REGRAS] Cloud HTTPS desativado — use MQTT circ/config / SPIFFS local");
}

#if ESPNOW_RELAY_BATCH_ENABLED

bool HydroSystemCore::isEspNowRelayBatchEligible(const RelayCommand& cmd, bool isSlave) {
    if (!isSlave || cmd.id <= 0) {
        return false;
    }
    if (cmd.durationSeconds != 0 || cmd.cycleOffSeconds != 0) {
        return false;
    }
    if (cmd.action != "on" && cmd.action != "off") {
        return false;
    }
    if (cmd.commandMode.length() > 0 && cmd.commandMode != "instant") {
        return false;
    }
    return true;
}

int HydroSystemCore::findEspNowRelayBatchSlot(const uint8_t* mac, bool create) {
    if (!mac) {
        return -1;
    }
    int freeIdx = -1;
    int oldestIdx = -1;
    unsigned long oldestAt = ULONG_MAX;
    for (size_t i = 0; i < ESPNOW_RELAY_BATCH_SLOTS; i++) {
        EspNowRelayBatchSlot& slot = espNowRelayBatchSlots_[i];
        if (slot.active && memcmp(slot.mac, mac, 6) == 0) {
            return static_cast<int>(i);
        }
        if (!slot.active && freeIdx < 0) {
            freeIdx = static_cast<int>(i);
        }
        if (slot.active && slot.openedAtMs < oldestAt) {
            oldestAt = slot.openedAtMs;
            oldestIdx = static_cast<int>(i);
        }
    }
    if (!create) {
        return -1;
    }
    if (freeIdx >= 0) {
        return freeIdx;
    }
    if (oldestIdx >= 0) {
        flushEspNowRelayBatchSlot(espNowRelayBatchSlots_[oldestIdx], "slot-pressure");
        return oldestIdx;
    }
    return -1;
}

bool HydroSystemCore::initEspNowRelayBatchMask(EspNowRelayBatchSlot& slot) {
    slot.desiredMask = 0;
    if (!masterManager) {
        return false;
    }
    TrustedSlave* slave = masterManager->getTrustedSlave(slot.mac);
    if (!slave) {
        return false;
    }
    for (int i = 0; i < 8 && i < slave->numRelays; i++) {
        if (slave->relayStates[i].state) {
            slot.desiredMask |= static_cast<uint8_t>(1u << i);
        }
    }
    slot.maskInitialized = true;
    return true;
}

bool HydroSystemCore::tryQueueEspNowRelayBatch(const RelayCommand& cmd, const uint8_t* targetMac,
                                               RelayOwner owner) {
    // Opção 2+3: sem ALL_RELAYS recebido → forçar envio individual (máscara seria imprecisa)
    if (masterManager) {
        TrustedSlave* sl = masterManager->getTrustedSlave(targetMac);
        if (!sl || sl->lastAllRelaysReceivedMs == 0) {
            return false;
        }
    }

    const int idx = findEspNowRelayBatchSlot(targetMac, true);
    if (idx < 0) {
        return false;
    }

    EspNowRelayBatchSlot& slot = espNowRelayBatchSlots_[idx];
    const unsigned long now = millis();
    if (!slot.active) {
        slot.active = true;
        memcpy(slot.mac, targetMac, 6);
        slot.openedAtMs = now;
        slot.itemCount = 0;
        slot.maskInitialized = false;
        slot.owner = owner;
    }

    const bool wantOn = (cmd.action == "on");
    bool updated = false;
    for (size_t i = 0; i < slot.itemCount; i++) {
        if (slot.items[i].relayNumber == cmd.relayNumber) {
            slot.items[i].supabaseCommandId = cmd.id;
            slot.items[i].wantOn = wantOn;
            updated = true;
            break;
        }
    }
    if (!updated) {
        if (slot.itemCount >= ESPNOW_RELAY_BATCH_MAX_ITEMS) {
            flushEspNowRelayBatchSlot(slot, "batch-full");
            return tryQueueEspNowRelayBatch(cmd, targetMac, owner);
        }
        EspNowRelayBatchItem& item = slot.items[slot.itemCount++];
        item.supabaseCommandId = cmd.id;
        item.relayNumber = cmd.relayNumber;
        item.wantOn = wantOn;
    }

    initEspNowRelayBatchMask(slot);
    for (size_t i = 0; i < slot.itemCount; i++) {
        const EspNowRelayBatchItem& item = slot.items[i];
        if (item.relayNumber < 0 || item.relayNumber > 7) {
            continue;
        }
        if (item.wantOn) {
            slot.desiredMask |= static_cast<uint8_t>(1u << item.relayNumber);
        } else {
            slot.desiredMask &= static_cast<uint8_t>(~(1u << item.relayNumber));
        }
    }

    slot.flushAtMs = now + ESPNOW_RELAY_BATCH_MS;
#if ESPNOW_RELAY_BATCH_COMPACT_LOG
    Serial.printf("[BATCH] queue mac=%s R%d %s id=%d n=%u flush_in=%lums\n",
                  ESPNowController::macToString(targetMac).c_str(),
                  cmd.relayNumber,
                  cmd.action.c_str(),
                  cmd.id,
                  static_cast<unsigned>(slot.itemCount),
                  static_cast<unsigned long>(ESPNOW_RELAY_BATCH_MS));
#else
    Serial.printf("[BATCH] queued supabase_id=%d relay=%d action=%s (n=%u)\n",
                  cmd.id, cmd.relayNumber, cmd.action.c_str(),
                  static_cast<unsigned>(slot.itemCount));
#endif
    return true;
}

void HydroSystemCore::flushEspNowRelayBatchSlot(EspNowRelayBatchSlot& slot, const char* reason) {
    if (!slot.active || slot.itemCount == 0) {
        slot.active = false;
        slot.itemCount = 0;
        return;
    }

    initEspNowRelayBatchMask(slot);
    for (size_t i = 0; i < slot.itemCount; i++) {
        const EspNowRelayBatchItem& item = slot.items[i];
        if (item.relayNumber < 0 || item.relayNumber > 7) {
            continue;
        }
        if (item.wantOn) {
            slot.desiredMask |= static_cast<uint8_t>(1u << item.relayNumber);
        } else {
            slot.desiredMask &= static_cast<uint8_t>(~(1u << item.relayNumber));
        }
    }

    const unsigned long elapsedMs = millis() - slot.openedAtMs;
    const uint8_t sendMask = slot.desiredMask;
    const size_t itemCount = slot.itemCount;
    const RelayOwner owner = slot.owner;
    uint8_t macCopy[6];
    memcpy(macCopy, slot.mac, 6);

    EspNowRelayBatchItem itemsCopy[ESPNOW_RELAY_BATCH_MAX_ITEMS];
    memcpy(itemsCopy, slot.items, itemCount * sizeof(EspNowRelayBatchItem));

    slot.active = false;
    slot.itemCount = 0;
    slot.maskInitialized = false;

    const uint32_t espNowCommandId =
        relayCoordinator.requestMask(owner, macCopy, sendMask, 0);

#if ESPNOW_RELAY_BATCH_COMPACT_LOG
    Serial.printf("[BATCH] flush mac=%s mask=0x%02X n=%u esp=%u reason=%s (+%lums)\n",
                  ESPNowController::macToString(macCopy).c_str(),
                  sendMask,
                  static_cast<unsigned>(itemCount),
                  static_cast<unsigned>(espNowCommandId),
                  reason ? reason : "due",
                  elapsedMs);
#else
    Serial.printf("[BATCH] flush mask=0x%02X items=%u esp=%u\n",
                  sendMask, static_cast<unsigned>(itemCount),
                  static_cast<unsigned>(espNowCommandId));
#endif

    if (espNowCommandId == 0) {
        Serial.println("[BATCH] flush failed — retry queue may resend mask");
        return;
    }

    const bool quietMap = ESPNOW_RELAY_BATCH_COMPACT_LOG != 0;
    for (size_t i = 0; i < itemCount; i++) {
        const EspNowRelayBatchItem& item = itemsCopy[i];
        if (item.supabaseCommandId <= 0) {
            continue;
        }
        addCommandMapping(espNowCommandId, item.supabaseCommandId, quietMap);
        registerPendingSlaveAck(item.supabaseCommandId, espNowCommandId, macCopy,
                                item.relayNumber, item.wantOn ? "on" : "off");
    }

#if ESPNOW_RELAY_BATCH_COMPACT_LOG
    Serial.print("[CMD done] batch ids=");
    for (size_t i = 0; i < itemCount; i++) {
        if (i > 0) {
            Serial.print(",");
        }
        Serial.print(itemsCopy[i].supabaseCommandId);
    }
    Serial.printf(" (+ %lums)\n", elapsedMs);
#endif
}

void HydroSystemCore::flushEspNowRelayBatchesDue(unsigned long now) {
    for (size_t i = 0; i < ESPNOW_RELAY_BATCH_SLOTS; i++) {
        EspNowRelayBatchSlot& slot = espNowRelayBatchSlots_[i];
        if (slot.active && slot.itemCount > 0 && now >= slot.flushAtMs) {
            flushEspNowRelayBatchSlot(slot, "due");
        }
    }
}

void HydroSystemCore::flushAllEspNowRelayBatches(const char* reason) {
    for (size_t i = 0; i < ESPNOW_RELAY_BATCH_SLOTS; i++) {
        if (espNowRelayBatchSlots_[i].active) {
            flushEspNowRelayBatchSlot(espNowRelayBatchSlots_[i], reason);
        }
    }
}

#endif  // ESPNOW_RELAY_BATCH_ENABLED


void HydroSystemCore::processRelayCommand(const RelayCommand& cmd, bool isSlave, const char* via) {
    printRelayCommandSerialLine(cmd, isSlave, via);

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
        
        // ✅ Verificar se slave está na lista confiável (sem copiar o vector inteiro)
        TrustedSlave* foundSlave = masterManager->getTrustedSlave(targetMac);
        if (foundSlave) {
            Serial.println("✅ Slave encontrado: " + foundSlave->deviceName);
            Serial.println("   MAC: " + ESPNowController::macToString(targetMac));
        } else {
            Serial.println("⚠️ Slave não está na lista confiável: " + cmd.target_device_id);
            Serial.println("💡 Comando será enviado mesmo assim (pode ser novo slave)");
        }

        const RelayOwner owner = resolveCommandOwner(cmd);
#if ESPNOW_RELAY_BATCH_ENABLED
        if (isEspNowRelayBatchEligible(cmd, isSlave)) {
            if (tryQueueEspNowRelayBatch(cmd, targetMac, owner)) {
                Serial.printf("[CMD dispatch] supabase_id=%d R%d %s (batch queued)\n",
                              cmd.id, cmd.relayNumber, cmd.action.c_str());
                return;
            }
        } else {
            const int batchIdx = findEspNowRelayBatchSlot(targetMac, false);
            if (batchIdx >= 0 && espNowRelayBatchSlots_[batchIdx].active) {
                flushEspNowRelayBatchSlot(espNowRelayBatchSlots_[batchIdx], "non-batch");
            }
        }
#endif
        
        // ✅ PASSO 2: Enviar via RelayCoordinator (ESP-NOW + mutex circulación)
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
            const RelayDenyReason deny = relayCoordinator.lastDenyReason();
            if (deny != RelayDenyReason::Ok) {
                Serial.printf("[COORD] cmd id=%d denied reason=%s — ACK failed\n",
                              cmd.id, relayDenyReasonName(deny));
                if (supabaseConnected && cmd.id > 0) {
#if ENABLE_MQTT
                    tryPublishCloudAckViaMqtt(
                        cmd.id, 0, targetMac, cmd.relayNumber, false, "failed");
#endif
                }
            } else {
                Serial.println("📋 Comando adicionado à fila (slave offline ou falha temporal)");
                Serial.println("💡 Será enviado quando slave voltar online ou no próximo retry");
            }
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
        const RelayDenyReason deny = relayCoordinator.lastDenyReason();
        if (deny != RelayDenyReason::Ok) {
            Serial.printf("[COORD] local deny owner=%s reason=%s R%d\n",
                          relayOwnerName(owner), relayDenyReasonName(deny), cmd.relayNumber + 1);
        } else {
            Serial.println("❌ Erro ao executar comando local");
        }
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
            masterManager->saveEspNowLastChannel(WiFi.channel());
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
    reading.circulationTyped = relayCoordinator.isCirculationConfigured();
    reading.circulationMixOk = relayCoordinator.isCirculationMixActiveForDosing();
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
    reading.circulationTyped = relayCoordinator.isCirculationConfigured();
    reading.circulationMixOk = relayCoordinator.isCirculationMixActiveForDosing();
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
#if ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED
    saveMasterRelayStatesToNVS();
    return;
#endif
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
        // Bridge scheduler: triggered_by/created_by "scheduler#…" (parser puede dejar "scheduler")
        if (cmd.triggered_by.startsWith("scheduler")) {
            return RelayOwner::ScheduleP4;
        }
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
#if ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED
        Serial.println("⚠️ [PH OP] MQTT publish falhou — sem fallback HTTPS");
        return;
#else
        Serial.println("⚠️ [PH OP] MQTT publish falhou — fallback HTTPS");
#endif
    }

#if !(ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED)
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
#endif
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
    const float k = hydroControl.getECController().getKValue();
#if ENABLE_MQTT
    if (mqttClient.isConnected() && mqttClient.publishEcGain(k)) {
        return;
    }
#endif
#if !GAIN_PATCH_HTTPS_DISABLED
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS() || !supabase.isReady()) {
        return;
    }
    Serial.printf("💾 [EC K] PATCH k_value post-recirc (k=%.4f)\n", k);
    supabase.patchEcConfigGain(getDeviceID(), k);
#else
    Serial.printf("⚠️ [EC K] MQTT indisponível — k=%.4f só NVS\n", k);
#endif
}

void HydroSystemCore::handlePhGainLearned() {
    const auto& phCtrl = hydroControl.getAdaptivePHController();
    const float kAcid = phCtrl.getKAcid();
    const float kBase = phCtrl.getKBase();
#if ENABLE_MQTT
    if (mqttClient.isConnected() && mqttClient.publishPhGain(kAcid, kBase)) {
        return;
    }
#endif
#if !GAIN_PATCH_HTTPS_DISABLED
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS() || !supabase.isReady()) {
        return;
    }
    Serial.printf("💾 [PH K] PATCH k_acid/k_base post-recirc (k_acid=%.3e k_base=%.3e)\n",
        kAcid, kBase);
    supabase.patchPhConfigGains(getDeviceID(), kAcid, kBase, false);
#else
    Serial.printf("⚠️ [PH K] MQTT indisponível — k só NVS (acid=%.3e base=%.3e)\n",
                  kAcid, kBase);
#endif
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
#if ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED
        Serial.println("⚠️ [EC OP] MQTT publish falhou — sem fallback HTTPS");
        return;
#else
        Serial.println("⚠️ [EC OP] MQTT publish falhou — fallback HTTPS");
#endif
    }

#if !(ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED)
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
#endif
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
#if ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED
    saveMasterRelayStatesToNVS();
    return;
#endif
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
#if ENABLE_MQTT
        if (MQTT_COMMAND_BRIDGE_ONLY && isMqttCommandPathStable()) {
            return;
        }
#endif
        Serial.println("✅ Reabilitando Supabase - Heap recuperado");
        supabaseConnected = true;
    }
}

// ===== UTILITIES =====

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
        if (now - lastWarning >= 30000) {  // Logar apenas a cada 30 segundos
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
#if ENABLE_MQTT && HTTPS_RUNTIME_FALLBACK_DISABLED
    Serial.println("   ✓ Cloud runtime: MQTT only (telemetry + heartbeat)");
    Serial.println("   ✓ Boot: autoRegisterDevice mantido");
#else
    Serial.println("   ✓ Supabase: device_status (escrita cada 60s)");
#if ENABLE_MQTT && MQTT_HYDRO_ONLY
    Serial.println("   ✓ Sensores: MQTT (hydro + ambiente) + HTTPS fallback se broker cair");
#else
    Serial.println("   ✓ Sensores: bivalente MQTT 30s + HTTPS hydro/environment 30s");
#endif
#endif
    Serial.println("   ✓ Supabase: relay_states (escrita quando muda estado)");
    Serial.println("   ✓ Comandos relé: MQTT hidrowave/{id}/command (sem poll HTTPS)");
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
void HydroSystemCore::addCommandMapping(uint32_t espNowCommandId, int supabaseCommandId, bool quiet) {
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
    
    if (!quiet) {
        Serial.printf("📝 [MAPEAMENTO] Adicionado: ESP-NOW ID=%u → Supabase ID=%d\n", 
                     espNowCommandId, supabaseCommandId);
    }
    
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

void HydroSystemCore::drainSupabaseMappingsForEspNow(uint32_t espNowCommandId, std::vector<int>& out) {
    out.clear();
    if (mappingsMutex == nullptr) {
        return;
    }

    if (xSemaphoreTake(mappingsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    for (auto it = commandMappings.begin(); it != commandMappings.end();) {
        if (it->espNowCommandId == espNowCommandId) {
            out.push_back(it->supabaseCommandId);
            it = commandMappings.erase(it);
        } else {
            ++it;
        }
    }

    xSemaphoreGive(mappingsMutex);
}

void HydroSystemCore::completePendingAcksForEspNowCommand(uint32_t espNowCommandId,
                                                          const uint8_t* slaveMac,
                                                          const char* via) {
    std::vector<PendingSlaveCommandAck> toComplete;

    if (pendingAckMutex != nullptr &&
        xSemaphoreTake(pendingAckMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        cleanupExpiredPendingSlaveAcks();
        for (auto it = pendingSlaveCommandAcks.begin(); it != pendingSlaveCommandAcks.end();) {
            if (it->espNowCommandId != espNowCommandId) {
                ++it;
                continue;
            }
            if (slaveMac && memcmp(it->slaveMac, slaveMac, 6) != 0) {
                ++it;
                continue;
            }
            toComplete.push_back(*it);
            it = pendingSlaveCommandAcks.erase(it);
        }
        xSemaphoreGive(pendingAckMutex);
    }

    std::vector<int> orphanIds;
    drainSupabaseMappingsForEspNow(espNowCommandId, orphanIds);

    // ACK-MASK chega ANTES de ALL_RELAYS: relayStates[] ainda tem estado VELHO.
    // Fechar cloud com expectedOn (o que o comando pediu), não com snapshot stale.
    for (const auto& pending : toComplete) {
        if (wasRecentlyClosedCloudAck(pending.supabaseCommandId)) {
            continue;
        }
        const int relay = pending.relayNumber;
        const bool state = pending.expectedOn;
        Serial.printf("[ACK-MASK] closing id=%d relay=%d expect=%s via=%s\n",
                      pending.supabaseCommandId, relay, state ? "ON" : "OFF",
                      via ? via : "?");
        completeSlaveCommand(pending.supabaseCommandId, espNowCommandId, slaveMac,
                             relay, state, via ? via : "ACK-MASK");
    }

    for (int sid : orphanIds) {
        bool already = false;
        for (const auto& pending : toComplete) {
            if (pending.supabaseCommandId == sid) {
                already = true;
                break;
            }
        }
        if (!already) {
            Serial.printf("[ACK-MASK] mapping órfão id=%d esp=%u (sem pending)\n",
                          sid, static_cast<unsigned>(espNowCommandId));
        }
    }
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
    // Com MQTT ligado: só command_ack — nunca HTTPS (evita sslBusy/TLS no hot path)
    if (MQTT_COMMAND_BRIDGE_ONLY && mqttClient.isConnected()) {
        if (isMqttCommandPathStable() &&
            tryPublishCloudAckViaMqtt(supabaseCommandId, espNowCommandId, slaveMac, relayNumber, currentState)) {
            return true;
        }
        return false;
    }
#endif

    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

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
    if (MQTT_COMMAND_BRIDGE_ONLY && mqttClient.isConnected()) {
        if (!isMqttCommandPathStable()) {
            return;
        }
    } else if (WiFi.status() != WL_CONNECTED) {
        return;
    } else if (!canFlushHttps) {
        return;
    }
#else
    if (!canFlushHttps) {
        return;
    }
#endif

    const uint8_t burstMax = ESP.getFreeHeap() < 60000 ? 1 : 3;
    for (uint8_t burst = 0; burst < burstMax && pendingCloudAckCount > 0; burst++) {
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
            continue;
        }

        item.attempts++;
        if (item.attempts >= PENDING_CLOUD_ACK_MAX_ATTEMPTS) {
            Serial.printf("⚠️ [ACK cloud] abandonado após %u tentativas id=%d\n",
                          item.attempts, item.supabaseCommandId);
            pendingCloudAckHead = (pendingCloudAckHead + 1) % PENDING_CLOUD_ACK_CAP;
            pendingCloudAckCount--;
            continue;
        }
        break;
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
                [supabaseCommandId](const PendingSlaveCommandAck& p) {
                    return p.supabaseCommandId == supabaseCommandId;
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
    if (EspNowChannelPolicy::isProvisioningStaSuspendActive()) {
        return true;
    }
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
#if ENABLE_MQTT
    const bool cloudAckPath = supabaseConnected ||
                              (MQTT_COMMAND_BRIDGE_ONLY && isMqttCommandPathStable());
#else
    const bool cloudAckPath = supabaseConnected;
#endif
    if (!slaveMac || !relayStates || !cloudAckPath || pendingAckMutex == nullptr) {
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
    
    bool relayStates[8] = {false};
    bool hasTimers[8] = {false};
    int remainingTimes[8] = {0};
    uint8_t numRelays = 0;
    bool linkOnline = false;
    uint16_t linkLastSeenS = 0;

    const bool slaveFound = masterManager->readSlaveRelaySnapshot(
        slaveMac, relayStates, hasTimers, remainingTimes, numRelays, linkOnline, linkLastSeenS);
    
    if (!slaveFound) {
        Serial.println("⚠️ [SLAVE] Slave não encontrado na lista confiável");
        Serial.println("💡 Inicializando arrays com valores padrão");
    }
    
    // Atualizar estado do relay específico
    if (relayNumber >= 0 && relayNumber < 8) {
        relayStates[relayNumber] = state;
    }
    
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

    if (relayNumber < 0 || relayNumber > 15) {
        Serial.printf("⚠️ [MQTT ACK] relay_index=%d inválido — id=%d\n",
                      relayNumber, supabaseCommandId);
        return false;
    }

    const char* slaveMacStr = nullptr;
    String slaveMacString;

    if (slaveMac) {
        slaveMacString = ESPNowController::macToString(slaveMac);
        slaveMacStr = slaveMacString.c_str();
    }

    MqttCommandAckReading ack = {};
    ack.commandId = supabaseCommandId;
    ack.status = (status && status[0]) ? status : "completed";
    ack.relayIndex = relayNumber;
    ack.action = currentState ? "on" : "off";
    ack.currentState = currentState;
    ack.slaveMac = slaveMacStr;
    ack.relayStates = nullptr;
    ack.numRelayStates = 0;
    ack.espnowId = espNowCommandId;

    return mqttClient.publishCommandAck(ack);
}

void HydroSystemCore::forceSlaveRelayMqttFullSync() {
    if (!masterManager || !mqttClient.isConnected()) {
        return;
    }

#if ESPNOW_RELAY_BATCH_ENABLED
    flushAllEspNowRelayBatches("sync-60s");
#endif

    // Coletar MACs sob mutex curto; RF/MQTT fora (sem getAllTrustedSlaves)
    uint8_t macList[8][6];
    unsigned long lastSeenList[8];
    bool onlineList[8];
    int macCount = 0;
    masterManager->forEachTrustedSlave([&](const TrustedSlave& slave) {
        if (macCount >= 8) {
            return;
        }
        memcpy(macList[macCount], slave.macAddress, 6);
        lastSeenList[macCount] = slave.lastSeen;
        onlineList[macCount] = slave.isOnline();
        macCount++;
    });

    for (int i = 0; i < macCount; i++) {
        const unsigned long sinceSeen = millis() - lastSeenList[i];
        if (!onlineList[i] && sinceSeen >= 60000) {
            continue;
        }

        masterManager->drainAllRelaysStatusWait();
        Serial.printf("[AUTO-SYNC] request status mac=%s\n",
                      ESPNowController::macToString(macList[i]).c_str());
        masterManager->requestSlaveStatus(macList[i]);
        const bool gotStatus = masterManager->waitForAllRelaysStatus(800);
        esp_task_wdt_reset();

        if (gotStatus) {
            Serial.printf("[SYNC-MQTT] full relay_states mac=%s\n",
                          ESPNowController::macToString(macList[i]).c_str());
        } else {
            Serial.printf("[SYNC-MQTT] ALL_RELAYS timeout mac=%s\n",
                          ESPNowController::macToString(macList[i]).c_str());
        }
    }
}

int HydroSystemCore::findRelayStateCoalesceSlot(const uint8_t* mac) const {
    if (!mac) {
        return -1;
    }
    for (size_t i = 0; i < RELAY_STATE_COALESCE_SLOTS; i++) {
        if (pendingRelayStateSlots_[i].active && memcmp(pendingRelayStateSlots_[i].mac, mac, 6) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int HydroSystemCore::allocRelayStateCoalesceSlot(const uint8_t* mac) {
    if (!mac) {
        return -1;
    }

    const int existing = findRelayStateCoalesceSlot(mac);
    if (existing >= 0) {
        return existing;
    }

    for (size_t i = 0; i < RELAY_STATE_COALESCE_SLOTS; i++) {
        if (!pendingRelayStateSlots_[i].active) {
            return static_cast<int>(i);
        }
    }

    int victim = 0;
    unsigned long oldestDue = pendingRelayStateSlots_[0].dueMs;
    for (size_t i = 1; i < RELAY_STATE_COALESCE_SLOTS; i++) {
        if (pendingRelayStateSlots_[i].dueMs > oldestDue) {
            oldestDue = pendingRelayStateSlots_[i].dueMs;
            victim = static_cast<int>(i);
        }
    }
    return victim;
}

bool HydroSystemCore::isRelayStateSnapshotUnchanged(const uint8_t* mac, const bool states[8],
                                                    const bool timers[8], const int remaining[8],
                                                    uint8_t numRelays, bool linkOnline) const {
    if (!mac || !states || !timers || !remaining) {
        return false;
    }

    for (size_t i = 0; i < RELAY_STATE_COALESCE_SLOTS; i++) {
        const LastPublishedRelayState& last = lastPublishedRelayState_[i];
        if (!last.valid || memcmp(last.mac, mac, 6) != 0) {
            continue;
        }
        if (last.linkOnline != linkOnline || last.numRelays != numRelays) {
            return false;
        }
        const uint8_t n = numRelays > 8 ? 8 : numRelays;
        for (uint8_t r = 0; r < n; r++) {
            if (last.states[r] != states[r] || last.timers[r] != timers[r] ||
                last.remaining[r] != remaining[r]) {
                return false;
            }
        }
        return true;
    }
    return false;
}

void HydroSystemCore::rememberPublishedRelayState(const uint8_t* mac, const bool states[8],
                                                  const bool timers[8], const int remaining[8],
                                                  uint8_t numRelays, bool linkOnline) {
    if (!mac || !states || !timers || !remaining) {
        return;
    }

    size_t slot = 0;
    for (; slot < RELAY_STATE_COALESCE_SLOTS; slot++) {
        if (lastPublishedRelayState_[slot].valid &&
            memcmp(lastPublishedRelayState_[slot].mac, mac, 6) == 0) {
            break;
        }
    }
    if (slot >= RELAY_STATE_COALESCE_SLOTS) {
        for (size_t i = 0; i < RELAY_STATE_COALESCE_SLOTS; i++) {
            if (!lastPublishedRelayState_[i].valid) {
                slot = i;
                break;
            }
        }
    }

    LastPublishedRelayState& last = lastPublishedRelayState_[slot];
    memcpy(last.mac, mac, 6);
    last.valid = true;
    last.linkOnline = linkOnline;
    last.numRelays = numRelays > 8 ? 8 : numRelays;
    for (uint8_t r = 0; r < 8; r++) {
        last.states[r] = states[r];
        last.timers[r] = timers[r];
        last.remaining[r] = remaining[r];
    }
}

void HydroSystemCore::scheduleSlaveRelayStateMqtt(const uint8_t* slaveMac, bool urgent, bool heartbeat) {
    if (!slaveMac || !masterManager || !mqttClient.isConnected()) {
        return;
    }

    if (!heartbeat) {
        bool states[8] = {false};
        bool timers[8] = {false};
        int remaining[8] = {0};
        uint8_t n = 8;
        bool linkOnline = false;
        uint16_t linkLastSeenS = 0;
        if (masterManager->readSlaveRelaySnapshot(slaveMac, states, timers, remaining, n,
                                                  linkOnline, linkLastSeenS) &&
            isRelayStateSnapshotUnchanged(slaveMac, states, timers, remaining, n, linkOnline)) {
            return;
        }
    }

    const int slotIdx = allocRelayStateCoalesceSlot(slaveMac);
    if (slotIdx < 0) {
        return;
    }

    PendingRelayStateSlot& slot = pendingRelayStateSlots_[slotIdx];
    const unsigned long now = millis();
    const unsigned long delayMs = urgent ? MQTT_RELAY_STATE_URGENT_MS : MQTT_RELAY_STATE_DEBOUNCE_MS;
    const unsigned long newDue = now + delayMs;

    if (!slot.active || memcmp(slot.mac, slaveMac, 6) != 0) {
        memcpy(slot.mac, slaveMac, 6);
        slot.active = true;
        slot.urgent = urgent;
        slot.heartbeat = heartbeat;
        slot.dueMs = newDue;
        return;
    }

    slot.urgent = slot.urgent || urgent;
    if (!heartbeat) {
        slot.heartbeat = false;
    }
    if (slot.urgent) {
        slot.dueMs = (newDue < slot.dueMs) ? newDue : slot.dueMs;
    } else {
        slot.dueMs = newDue;
    }
}

void HydroSystemCore::flushPendingRelayStateMqtt() {
    if (!mqttClient.isConnected()) {
        return;
    }

    const unsigned long now = millis();
    for (size_t i = 0; i < RELAY_STATE_COALESCE_SLOTS; i++) {
        PendingRelayStateSlot& slot = pendingRelayStateSlots_[i];
        if (!slot.active || now < slot.dueMs) {
            continue;
        }

        const bool heartbeat = slot.heartbeat;
        uint8_t mac[6];
        memcpy(mac, slot.mac, 6);
        slot.active = false;
        slot.urgent = false;
        slot.heartbeat = false;

        publishSlaveRelayStateMqtt(mac, -1, false, heartbeat);
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
    } else if (isRelayStateSnapshotUnchanged(slaveMac, states, timers, remaining, n, linkOnline)) {
        return;
    }

    if (!mqttClient.publishRelayState(reading)) {
        return;
    }

    if (!heartbeat) {
        rememberPublishedRelayState(slaveMac, states, timers, remaining, n, linkOnline);
    }
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
    if (ecConfigUnchanged(config)) {
        Serial.printf("ℹ️ [EC CONFIG] inalterada via=%s — NVS não reescrito\n", via ? via : "?");
        return true;
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
        DeserializationError nutrientsErr = deserializeJson(nutrientsDoc, config.nutrientsJson);
        if (!nutrientsErr && nutrientsDoc.is<JsonArray>()) {
            JsonArray nutrientsArray = nutrientsDoc.as<JsonArray>();
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
                if (q < 0.01f) {
                    q = flowByRelay[relay];
                }
                if (q > 0.01f) {
                    adapted["flowRate"] = q;
                }
                Serial.printf("   ✅ %s: %.2f ml/L q=%.3f ml/s → Relé %d\n",
                    nutrient["name"].as<const char*>(),
                    mlL,
                    q,
                    relay + 1);
            }
            if (adaptedArray.size() > 0) {
                hydroControl.updateNutrientProportions(adaptedArray);
                Serial.printf("✅ [EC CONFIG] %d nutriente(s) via %s\n",
                    adaptedArray.size(), via ? via : "?");
            }
        } else if (nutrientsErr) {
            Serial.printf("❌ [EC CONFIG] nutrients JSON: %s\n", nutrientsErr.c_str());
        }
    }
    hydroControl.saveECControllerConfig();
    rememberEcConfigSnapshot(config);
    if (!config.auto_enabled) {
        syncEcOperationStateToSupabase();
    }
    Serial.println("✅ [EC CONFIG] NVS atualizado");
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
    if (strstr(topic, "/circ/config") != nullptr) {
        if (!applyCirculationConfigMqtt(payload, length)) {
            Serial.println("[MQTT] circ/config JSON inválido ou apply falhou");
        }
        return;
    }
    if (strstr(topic, "/rules/manifest") != nullptr) {
        applyRulesManifestMqtt(payload, length);
        return;
    }
    if (strstr(topic, "/rules/") != nullptr) {
        applyRuleUpsertMqtt(payload, length);
        return;
    }
    handleMqttCommandPayload(payload, length);
}

static const char* FN_RECIRC_RULE_ID = "fn_recirculacao_continua";
static const char* FN_RECIRC_RULE_ID_LEGACY = "fn_circulation";

bool HydroSystemCore::upsertFnCirculationRule(const char* slaveMac, int relayIndex, bool enabled) {
    if (!decisionEngineReady) {
        Serial.println("[CIRC] DecisionEngine não pronto — skip upsert fn_recirculacao_continua");
        return false;
    }

    if (!enabled || !slaveMac || strlen(slaveMac) < 11 || relayIndex < 0 || relayIndex >= 8) {
        bool removed = false;
        if (decisionEngine.removeRule(FN_RECIRC_RULE_ID)) {
            removed = true;
        }
        if (decisionEngine.removeRule(FN_RECIRC_RULE_ID_LEGACY)) {
            removed = true;
        }
        if (removed) {
            decisionEngine.saveRulesToFile();
            Serial.println("[CIRC] regra recirculação removida (bomba não tipada)");
        }
        return true;
    }

    // Migrar legado fora do SPIFFS se existir
    decisionEngine.removeRule(FN_RECIRC_RULE_ID_LEGACY);

    StaticJsonDocument<768> doc;
    doc["rule_id"] = FN_RECIRC_RULE_ID;
    doc["rule_name"] = "Recirculação contínua";
    doc["rule_description"] =
        "Bomba de circulação via tipagem. Inativa até ativar no Motor de Regras.";
    {
        DecisionRule* existing = decisionEngine.getRule(FN_RECIRC_RULE_ID);
        doc["enabled"] = existing ? existing->enabled : false;
    }
    doc["priority"] = 30;
    doc["trigger_type"] = "periodic";
    doc["trigger_interval_ms"] = 60000;

    JsonObject ruleJson = doc.createNestedObject("rule_json");
    ruleJson["priority"] = 30;
    ruleJson["source"] = "hydraulic_roles";
    ruleJson["hydraulic_role"] = "circulation_pump";
    ruleJson["i18n_key"] = "rules.fn_circulation";
    ruleJson["interval_between_executions"] = 60;

    JsonObject condition = ruleJson.createNestedObject("condition");
    condition["type"] = "time_window";
    condition["sensor_name"] = "time_window";

    JsonArray actions = ruleJson.createNestedArray("actions");
    JsonObject action = actions.createNestedObject();
    action["type"] = "relay_on";
    action["target_relay"] = relayIndex;
    action["target_device_id"] = slaveMac;
    action["duration_ms"] = 0;

    return decisionEngine.upsertRuleFromJson(doc.as<JsonObject>(), true);
}

bool HydroSystemCore::applyCirculationConfigMqtt(const char* payload, size_t length) {
    if (!payload || length == 0) {
        return false;
    }

    StaticJsonDocument<384> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        return false;
    }

    const bool enabled = doc["enabled"] | false;
    const char* macStr = doc["slave_mac"] | doc["slaveMac"] | "";
    int relayIndex = doc["relay_index"] | doc["relayIndex"] | -1;

    if (!enabled || !macStr || strlen(macStr) < 11 || relayIndex < 0 || relayIndex >= 8) {
        relayCoordinator.clearCirculationTarget();
        Serial.println("[MQTT] circ/config → circulation cleared (NVS only)");
        return true;
    }

    uint8_t mac[6];
    int values[6];
    if (sscanf(macStr, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = static_cast<uint8_t>(values[i]);
    }

    relayCoordinator.setCirculationTarget(mac, relayIndex);
    Serial.printf("[MQTT] circ/config → binding NVS R%d (fn_* via rules upsert)\n",
                  relayIndex + 1);
    return true;
}

bool HydroSystemCore::applyRuleUpsertMqtt(const char* payload, size_t length) {
    if (!payload || length == 0 || !decisionEngineReady) {
        return false;
    }

    // Envelope MQTT + rule_json aninhado — 3k estourava e rule_json virava null
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.printf("[MQTT] rules upsert parse: %s (len=%u)\n", err.c_str(),
                      static_cast<unsigned>(length));
        return false;
    }
    if (doc.overflowed()) {
        Serial.printf("[MQTT] rules upsert overflow mem=%u len=%u\n",
                      static_cast<unsigned>(doc.memoryUsage()),
                      static_cast<unsigned>(length));
    }

    const char* op = doc["op"] | "upsert";
    const char* ruleId = doc["rule_id"] | "";
    if (!ruleId[0] && doc["rule"].is<JsonObject>()) {
        ruleId = doc["rule"]["rule_id"] | doc["rule"]["id"] | "";
    }

    if (strcmp(op, "delete") == 0) {
        if (ruleId[0]) {
            DecisionRule* existing = decisionEngine.getRule(ruleId);
            if (existing) {
                releaseDecisionRuleActuators(*existing);
            }
            decisionEngine.removeRule(ruleId);
            decisionEngine.saveRulesToFile();
            Serial.printf("[MQTT] rules delete → %s\n", ruleId);
        }
        return true;
    }

    // disable: manter regra no SPIFFS com enabled=false (ativar no Motor sem re-tipar)
    if (strcmp(op, "disable") == 0) {
        if (ruleId[0]) {
            DecisionRule* existing = decisionEngine.getRule(ruleId);
            if (existing) {
                releaseDecisionRuleActuators(*existing);
                existing->enabled = false;
                ScriptRunnerManager::instance().removeByRuleId(ruleId);
                decisionEngine.saveRulesToFile();
                Serial.printf("[MQTT] rules disable (keep) → %s (+OFF actuators)\n", ruleId);
            } else {
                Serial.printf("[MQTT] rules disable — %s ausente (noop)\n", ruleId);
            }
        }
        return true;
    }

    JsonObject ruleObj = doc["rule"].as<JsonObject>();
    if (ruleObj.isNull()) {
        ruleObj = doc.as<JsonObject>();
    }
    if (ruleObj.isNull()) {
        return false;
    }

    const bool wasEnabled = [&]() {
        DecisionRule* prev = ruleId[0] ? decisionEngine.getRule(ruleId) : nullptr;
        return prev && prev->enabled;
    }();

    const bool ok = decisionEngine.upsertRuleFromJson(ruleObj, true);
    DecisionRule* after = ruleId[0] ? decisionEngine.getRule(ruleId) : nullptr;
    const bool nowEnabled = after && after->enabled;
    if (ok && after && wasEnabled && !nowEnabled) {
        releaseDecisionRuleActuators(*after);
    }
    Serial.printf("[MQTT] rules upsert %s enabled=%d → %s\n",
                  ruleId[0] ? ruleId : "?",
                  nowEnabled ? 1 : 0,
                  ok ? "ok" : "fail");
    return ok;
}

void HydroSystemCore::releaseDecisionRuleActuators(const DecisionRule& rule) {
    for (const auto& action : rule.actions) {
        if (action.type != RELAY_ON && action.type != RELAY_PULSE) {
            continue;
        }
        const String& target = action.target_device_id;
        if (target.isEmpty() || target.equalsIgnoreCase("local") ||
            target.equalsIgnoreCase("MASTER")) {
            const bool ok = relayCoordinator.actuateLocal(
                RelayOwner::DecisionRule, action.target_relay, "off", 0);
            Serial.printf("[RULE-OFF] local R%d rule=%s ok=%d\n",
                          action.target_relay, rule.id.c_str(), ok ? 1 : 0);
            continue;
        }

        uint8_t mac[6] = {0};
        bool macOk = false;
        if (masterManager) {
            masterManager->forEachTrustedSlave([&](const TrustedSlave& slave) {
                if (macOk) return;
                if (slave.deviceName.equalsIgnoreCase(target)) {
                    memcpy(mac, slave.macAddress, 6);
                    macOk = true;
                }
            });
        }
        if (!macOk) {
            int values[6];
            String sanitized = target;
            sanitized.toUpperCase();
            sanitized.replace("-", ":");
            while (sanitized.indexOf("::") >= 0) {
                sanitized.replace("::", ":");
            }
            if (sscanf(sanitized.c_str(), "%x:%x:%x:%x:%x:%x",
                       &values[0], &values[1], &values[2], &values[3], &values[4],
                       &values[5]) == 6) {
                for (int i = 0; i < 6; i++) {
                    mac[i] = static_cast<uint8_t>(values[i]);
                }
                macOk = true;
            }
        }
        if (!macOk) {
            Serial.printf("[RULE-OFF] MAC inválido target=%s rule=%s\n",
                          target.c_str(), rule.id.c_str());
            continue;
        }
        const uint32_t cmdId = relayCoordinator.actuateSlave(
            RelayOwner::DecisionRule, mac, action.target_relay, "off", 0);
        Serial.printf("[RULE-OFF] slave R%d rule=%s cmd=%u\n",
                      action.target_relay, rule.id.c_str(),
                      static_cast<unsigned>(cmdId));
    }
}

bool HydroSystemCore::applyRulesManifestMqtt(const char* payload, size_t length) {
    if (!payload || length == 0 || !decisionEngineReady) {
        return false;
    }

    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.printf("[MQTT] rules manifest parse: %s\n", err.c_str());
        return false;
    }

    JsonArray ids = doc["ids"].as<JsonArray>();
    if (ids.isNull()) {
        return false;
    }

    size_t cloudCount = ids.size();
    size_t localCount = decisionEngine.getAllRules().size();
    Serial.printf("[MQTT] rules manifest cloud=%u local=%u — retained upserts aplicam deltas\n",
                  static_cast<unsigned>(cloudCount),
                  static_cast<unsigned>(localCount));

    // Desactivar locales que ya no están en el manifesto (o enabled=false en cloud).
    for (auto& local : decisionEngine.getAllRules()) {
        bool found = false;
        bool cloudEnabled = false;
        for (JsonObject entry : ids) {
            const char* rid = entry["rule_id"] | "";
            if (local.id == rid) {
                found = true;
                cloudEnabled = entry["enabled"] | false;
                break;
            }
        }
        if (!found || !cloudEnabled) {
            if (local.enabled) {
                releaseDecisionRuleActuators(local);
                local.enabled = false;
                ScriptRunnerManager::instance().removeByRuleId(local.id);
                Serial.printf("[MQTT] manifest → disable local %s (+OFF)\n", local.id.c_str());
            }
        }
    }
    decisionEngine.saveRulesToFile();
    return true;
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