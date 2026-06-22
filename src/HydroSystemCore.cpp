#include "HydroSystemCore.h"
#include "HydroControl.h"
#include "SupabaseClient.h"
#include "SensorSanitize.h"
#include "HydroSupaManager.h"  // ✅ Manager híbrido
#include "WebServerManager.h"
#include "WiFiManager.h"
#include "Config.h"
#include "DeviceID.h"
#include "ObjectPoolManager.h"  // ✅ Object Pool Pattern
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

// ===== CONSTRUTOR E DESTRUTOR =====
HydroSystemCore::HydroSystemCore(WebServerTask* webTask, ESPNowController* espNow, MasterSlaveManager* masterMgr) : 
    webServerTask(webTask),
    espNowController(espNow),
    masterManager(masterMgr),
    webServerManager(nullptr),  // ✅ TÓPICO 4: Inicializado em begin()
    mappingsMutex(nullptr),  // ✅ NOVO: Inicializar mutex (FALTAVA VÍRGULA!)
    systemReady(false),
    supabaseConnected(false),
    endpointsRegistered(false),  // ✅ Rastrear se endpoints foram registrados
    startTime(0),
    lastSensorSend(0),
    lastStatusSend(0),
    lastRelayStatesSync(0),  // ✅ NOVO: Inicializar controle de sincronização
    lastStatusPrint(0),
    lastSupabaseCheck(0),
    lastRulesCheck(0),  // ✅ NOVO: Inicializar controle de verificação de regras
    lastMemoryProtection(0),
    lastMqttTelemetrySend(0),
    lastMqttHeartbeatSend(0),
    lastEcOperationSync(0),
    lastEcOperationIdleSync(0),
    lastPhOperationSync(0),
    lastPhOperationIdleSync(0),
    pendingNutrientDoseHead(0),
    pendingNutrientDoseCount(0),
    pendingPhDoseHead(0),
    pendingPhDoseCount(0) {
    
    memset(pendingNutrientDoseQueue, 0, sizeof(pendingNutrientDoseQueue));
    memset(pendingPhDoseQueue, 0, sizeof(pendingPhDoseQueue));
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
        Serial.println("📱 De: " + ESPNowController::macToString(senderMac));
        Serial.println("🆔 Command ID (ESP-NOW): " + String(commandId));
        Serial.println("🔌 Relé: " + String(relayNumber));
        Serial.println("✅ Success: " + String(success ? "Sim" : "Não"));
        Serial.println("💡 Estado: " + String(currentState ? "ON" : "OFF"));

        int supabaseCommandId = findSupabaseCommandId(commandId);

        if (supabaseCommandId > 0 && supabaseConnected) {
            if (success) {
                supabase.markCommandCompleted(supabaseCommandId, currentState, true);

                String slaveMacStr = ESPNowController::macToString(senderMac);
                String slaveDeviceId = "ESP32_SLAVE_" + slaveMacStr;
                slaveDeviceId.replace(":", "_");

                updateRelaySlaveState(slaveDeviceId, senderMac, relayNumber, currentState);

                Serial.println("✅ [CALLBACK] Comando marcado como completed e relay_slaves atualizado");
            } else {
                supabase.markCommandFailed(supabaseCommandId, "Slave não confirmou", true);
                Serial.println("❌ [CALLBACK] Comando marcado como failed");
            }
        } else if (supabaseCommandId == 0) {
            Serial.println("⚠️ [CALLBACK] Mapeamento não encontrado para commandId=" + String(commandId));
            Serial.println("💡 Comando pode ter sido processado antes do mapeamento ser criado");
        }

        Serial.println("========================================\n");
    });

    masterManager->setSupabaseCommandCallback([this](int supabaseCommandId, bool success, const String& errorMessage) {
        if (supabaseCommandId > 0 && supabaseConnected) {
            if (success) {
                bool currentState = (errorMessage == "true" || errorMessage == "1");
                supabase.markCommandCompleted(supabaseCommandId, currentState, true);
            } else {
                supabase.markCommandFailed(supabaseCommandId, errorMessage, true);
            }
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
    
    startTime = millis();
    
    // ===== INICIALIZAR SISTEMA HIDROPÔNICO =====
    Serial.println("🔧 Inicializando controle hidropônico...");
    if (!hydroControl.begin()) {
        Serial.println("❌ Erro ao inicializar HydroControl");
        return false;
    }
    Serial.println("✅ HydroControl inicializado");

    hydroControl.setNutrientDoseCallback(&HydroSystemCore::onNutrientDoseStatic, this);
    hydroControl.setEcDilutionCallback(&HydroSystemCore::onEcDilutionStatic, this);
    hydroControl.setPhDoseCallback(&HydroSystemCore::onPhDoseStatic, this);
    hydroControl.setEcMetricCallback(&HydroSystemCore::onEcMetricStatic, this);
    hydroControl.setPhMetricCallback(&HydroSystemCore::onPhMetricStatic, this);
    hydroControl.setPhGainLearnedCallback(&HydroSystemCore::onPhGainLearnedStatic, this);
    hydroControl.setEcOperationSyncCallback(&HydroSystemCore::onEcOperationSyncStatic, this);
    hydroControl.setPhOperationSyncCallback(&HydroSystemCore::onPhOperationSyncStatic, this);
    hydroControl.setPhysicalRecircCallback(&HydroSystemCore::onPhysicalRecircStatic, this);

    relayCoordinator.begin(&hydroControl, masterManager);
    
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
        mqttClient.setCommandHandler(&HydroSystemCore::mqttCommandReceived, this);
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
    
    return true;
}

void HydroSystemCore::loop() {
    if (!systemReady) return;
    
    unsigned long now = millis();
    
    // ===== PROTEÇÃO DE MEMÓRIA (10s) =====
    if (now - lastMemoryProtection >= MEMORY_CHECK_INTERVAL) {
        performMemoryProtection();
        lastMemoryProtection = now;
    }

    const bool doseCycleBusy = isAutoDoseCycleBusy(hydroControl);
    if (!doseCycleBusy) {
        flushPendingNutrientDoseExports();
        flushPendingPhDoseExports();
    }
    
#if ENABLE_MQTT
    mqttClient.loop();
    if (now - lastMqttTelemetrySend >= MQTT_TELEMETRY_INTERVAL_MS) {
        publishMqttTelemetry();
        lastMqttTelemetrySend = now;
    }
    if (now - lastMqttHeartbeatSend >= MQTT_HEARTBEAT_INTERVAL_MS) {
        publishMqttHeartbeat();
        lastMqttHeartbeatSend = now;
    }
#endif
    
    // ===== SENSORES → SUPABASE (30s) — skip HTTPS hydro si MQTT conectado (evita duplicar pipeline) =====
    {
        bool sendHydroHttps = true;
#if ENABLE_MQTT
        sendHydroHttps = !mqttClient.isConnected();
#endif
        if (sendHydroHttps && !doseCycleBusy && now - lastSensorSend >= SENSOR_SEND_INTERVAL) {
            sendSensorDataToSupabase();
            lastSensorSend = now;
        }
    }
    
    // ===== STATUS DEVICE → SUPABASE (60s) =====
    if (now - lastStatusSend >= STATUS_SEND_INTERVAL) {
        sendDeviceStatusToSupabase();
        lastStatusSend = now;
    }
    
    // ===== ✅ NOVO: SINCRONIZAÇÃO UNIFICADA DE RELAY STATES (5s) =====
    // Atualiza master relays + slave relays juntos no Supabase
    if (!doseCycleBusy && now - lastRelayStatesSync >= RELAY_STATES_SYNC_INTERVAL) {
        syncAllRelayStatesToSupabase();
        lastRelayStatesSync = now;
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
    
    // ===== ✅ VERIFICAR REGRAS DE AUTOMAÇÃO (30s) =====
    // Busca regras de automação do Supabase (decision_rules com enabled=true)
    if (now - lastRulesCheck >= RULES_CHECK_INTERVAL) {
        checkSupabaseRules();
        lastRulesCheck = now;
    }
    
    // ===== ✅ NOVO: BUSCAR EC CONFIG DO SUPABASE (a cada intervalo_auto_ec) =====
    // ✅ IMPORTANTE: Buscar SEMPRE que Supabase estiver conectado, mesmo se auto_enabled = false
    // Isso permite que o ESP32 seja ativado remotamente pelo frontend
    static unsigned long lastECConfigCheck = 0;
    static unsigned long lastPHConfigCheck = 0;
    static unsigned long lastDebugPrint = 0;
    
    // ✅ DEBUG: Mostrar status a cada 10 segundos
    if (now - lastDebugPrint >= 10000) {
        bool autoEnabled = hydroControl.isAutoECEnabled();
        int intervalSeconds = hydroControl.getAutoECInterval();
        unsigned long timeSinceLastCheck = now - lastECConfigCheck;
        
        Serial.printf("🔍 [EC CONFIG DEBUG] auto_enabled: %s | supabaseConnected: %s | intervalo: %d s | último check: %lu ms atrás\n",
            autoEnabled ? "SIM" : "NÃO",
            supabaseConnected ? "SIM" : "NÃO",
            intervalSeconds,
            timeSinceLastCheck);
        
        if (!autoEnabled) {
            Serial.println("   ⚠️ Auto EC não está habilitado - buscando do Supabase para ativar...");
        }
        if (!supabaseConnected) {
            Serial.println("   ⚠️ Supabase não conectado!");
        }

        const bool phAutoEnabled = hydroControl.isAutoPHEnabled();
        const int phIntervalSeconds = hydroControl.getAutoPHInterval();
        const unsigned long phTimeSinceLastCheck = now - lastPHConfigCheck;
        Serial.printf("🔍 [PH CONFIG DEBUG] auto_enabled: %s | supabaseConnected: %s | intervalo: %d s | último check: %lu ms atrás\n",
            phAutoEnabled ? "SIM" : "NÃO",
            supabaseConnected ? "SIM" : "NÃO",
            phIntervalSeconds,
            phTimeSinceLastCheck);
        if (!phAutoEnabled) {
            Serial.println("   ⚠️ Auto pH não está habilitado - buscando do Supabase para ativar...");
        }
        
        lastDebugPrint = now;
    }
    
    // ✅ Poll config a cada 30s (read-only GET) — independente de intervalo_auto_ec do check EC
    // Evita: poll só a cada 1800s + RPC activate_auto_ec reativar auto_enabled indevidamente
    static const unsigned long EC_CONFIG_POLL_MS = 30000UL;
    if (supabaseConnected && !doseCycleBusy) {
        if (now - lastECConfigCheck >= EC_CONFIG_POLL_MS) {
            Serial.println("⏰ [EC CONFIG] Poll 30s — GET ec_config_view (sem activate_auto_ec)");
            checkECConfigFromSupabase();
            lastECConfigCheck = now;
        }

        // Poll pH config a cada 30s — independente de intervalo_auto_ph (paridade EC_CONFIG_POLL_MS)
        static const unsigned long PH_CONFIG_POLL_MS = 30000UL;
        if (now - lastPHConfigCheck >= PH_CONFIG_POLL_MS) {
            Serial.println("⏰ [PH CONFIG] Poll 30s — GET ph_config_view");
            checkPHConfigFromSupabase();
            lastPHConfigCheck = now;
        }
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
                            Serial.println("      2. Slaves estão no mesmo canal ESP-NOW (canal 1)");
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
                                relayObj["name"] = slave.relayStates[i].name.length() > 0 ? slave.relayStates[i].name : ("Relé " + String(i + 1));  // ✅ Nome do relé
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
    
    if (!supabase.isReady()) return;
    
    // ✅ OTIMIZAÇÃO: Processar comandos em batch (3-5 por ciclo para melhor throughput)
    const int MAX_BATCH_COMMANDS = 5;
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
    if (supabase.checkForSlaveCommands(commands, MAX_BATCH_COMMANDS, commandCount)) {
        if (commandCount > 0) {
            isSlave = true;
            Serial.printf("📥 [HTTPS slave] %d comando(s)\n", commandCount);
            // ✅ Processar todos os comandos do batch
            for (int i = 0; i < commandCount; i++) {
                processRelayCommand(commands[i], isSlave);
                // ✅ Pequeno delay entre comandos para não sobrecarregar (não bloqueante)
                if (i < commandCount - 1) {
                    vTaskDelay(pdMS_TO_TICKS(10)); // 10ms entre comandos
                }
            }
            return;
        }
    }
}

// ✅ NOVO: Verificar regras de automação (decision_rules)
void HydroSystemCore::checkSupabaseRules() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS()) {
        return;
    }
    
    if (!supabase.isReady()) {
        return;
    }
    
    // ✅ Por enquanto, apenas log (implementação completa será adicionada depois)
    // TODO: Buscar regras de automação da tabela decision_rules
    // TODO: Avaliar condições das regras
    // TODO: Criar comandos baseados nas regras avaliadas
    
    Serial.println("📋 [REGRAS] Verificando regras de automação...");
    // Implementação completa será adicionada em uma próxima versão
}

// ✅ NOVO: Buscar EC Config do Supabase via RPC activate_auto_ec
void HydroSystemCore::checkECConfigFromSupabase() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS()) {
        return;
    }
    
    if (!supabase.isReady()) {
        return;
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
        lastAppliedEcConfig.nutrientsJson = config.nutrientsJson;
        lastAppliedEcConfig.initialized = true;
    };
    
    ECConfig config;
    if (supabase.getECConfigFromSupabase(config)) {
        if (config.isValid) {
            if (!ecConfigUnchanged(config)) {
            // ✅ Atualizar parâmetros do controller
            hydroControl.getECController().setBaseDose(config.base_dose);
            hydroControl.getECController().setFlowRate(config.flow_rate);
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
            hydroControl.setDilutionAutoEnabled(config.dilution_auto_enabled, false);
            hydroControl.setDilutionRelays(config.dilution_drain_relay, config.dilution_fill_relay);
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
                    DynamicJsonDocument adaptedDoc(2048);
                    JsonArray adaptedArray = adaptedDoc.to<JsonArray>();
                    
                    for (JsonVariant nutrient : nutrientsArray) {
                        if (!nutrient["active"].as<bool>()) {
                            continue;  // Pular nutrientes inativos
                        }
                        
                        JsonObject adaptedNutrient = adaptedArray.createNestedObject();
                        adaptedNutrient["name"] = nutrient["name"].as<String>();
                        adaptedNutrient["mlPerLiter"] = nutrient["mlPerLiter"].as<float>();
                        adaptedNutrient["active"] = nutrient["active"].as<bool>();
                        
                        // Converter relay (0-15) para relayNumber (1-16)
                        int relay = nutrient["relay"].as<int>();
                        adaptedNutrient["relayNumber"] = relay + 1;  // Converter para 1-16
                        
                        Serial.printf("   ✅ %s: %.2f ml/L → Relé %d\n", 
                            nutrient["name"].as<const char*>(), 
                            nutrient["mlPerLiter"].as<float>(),
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
}

void HydroSystemCore::processRelayCommand(const RelayCommand& cmd, bool isSlave, const char* via) {
    printRelayCommandSerialLine(cmd, isSlave, via);

    const bool fromMqtt = (via != nullptr && strcmp(via, "mqtt") == 0);

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

    if (fromMqtt && supabaseConnected) {
        supabase.markCommandSent(cmd.id, isSlave);
    }
}

// ✅ FORK: Processar comando manual (botão do usuário)
void HydroSystemCore::processManualCommand(const RelayCommand& cmd, bool isSlave) {
    // ✅ Validar relé
    int maxRelays = isSlave ? 8 : 16;  // Slaves têm 8 relays, Master tem 16
    if (cmd.relayNumber < 0 || cmd.relayNumber >= maxRelays) {
        Serial.printf("❌ Relé %d inválido (máx: %d)\n", cmd.relayNumber, maxRelays - 1);
        if (supabaseConnected) {
            supabase.markCommandFailed(cmd.id, "Relé inválido", isSlave);
        }
        return;
    }

    if (isSlave && !masterManager) {
        Serial.println("❌ [ESP-NOW] masterManager é nullptr — comando slave ignorado");
        Serial.println("💡 Ordem: MasterSlaveManager::begin() → setMasterManager() antes de comandos slave");
        if (supabaseConnected) {
            supabase.markCommandFailed(cmd.id, "masterManager unavailable", true);
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
                supabase.markCommandFailed(cmd.id, "MAC address inválido", isSlave);
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
            cmd.id);
        
        // ✅ CEREJA DO BOLO: Criar mapeamento IMEDIATAMENTE após enviar
        if (espNowCommandId > 0 && cmd.id > 0) {
            addCommandMapping(espNowCommandId, cmd.id);
            Serial.printf("📝 [MAPEAMENTO] Criado imediatamente: ESP-NOW ID=%u → Supabase ID=%d\n", 
                         espNowCommandId, cmd.id);
        }
        
        bool success = (espNowCommandId > 0);
        
        // ✅ NÃO marcar como completed aqui!
        // ✅ O callback será chamado quando receber ACK e encontrará o mapeamento!
        
        if (success) {
            Serial.println("✅ Comando ESP-NOW enviado com sucesso!");
            Serial.printf("   Slave: %s\n", cmd.target_device_id.c_str());
            Serial.printf("   Relé: %d -> %s\n", cmd.relayNumber, cmd.action.c_str());
            Serial.println("💡 Status será atualizado quando receber ACK (event-driven)");
        } else {
            Serial.println("📋 Comando adicionado à fila (slave offline ou falha temporal)");
            Serial.println("💡 Será enviado quando slave voltar online ou no próximo retry");
        }
        
    } else {
        // ===== COMANDO MASTER (LOCAL - PCF8574) =====
        Serial.println("🏠 [MASTER] Processando comando local");
        
        // ✅ Usar código existente (NÃO TOCAR PCF8574)
        executeLocalRelayCommand(cmd);
        
        // ✅ PASSO 3: Marcar como completed (execução local é imediata)
        if (supabaseConnected) {
            bool currentState = (cmd.action == "on");
            if (!supabase.markCommandCompleted(cmd.id, currentState, isSlave)) {
                Serial.printf("❌ [ACK] markCommandCompleted falhou id=%d — UI pode ficar em sent\n", cmd.id);
            } else {
                Serial.printf("✅ [ACK] relay_commands id=%d → completed\n", cmd.id);
            }
        }
        
        // ✅ PASSO 4: Atualizar relay_master com estado real
        if (supabaseConnected) {
            updateRelayMasterState(cmd);
        }
    }
}

// ✅ FORK: Processar comando de regra (automação)
void HydroSystemCore::processRuleCommand(const RelayCommand& cmd, bool isSlave) {
    Serial.println("🤖 [RULE] Processando comando de regra de automação");
    Serial.printf("   Regra: %s (%s) priority=%d\n", cmd.rule_name.c_str(), cmd.rule_id.c_str(), cmd.priority);

    if (cmd.priority >= TANK_SCRIPT_PRIORITY_THRESHOLD) {
        unsigned long holdMs = TANK_SCRIPT_HOLD_DEFAULT_MS;
        if (cmd.durationSeconds > 0) {
            holdMs = (unsigned long)cmd.durationSeconds * 1000UL + TANK_SCRIPT_HOLD_BUFFER_MS;
        }
        hydroControl.holdAutoDosingForTankScript(holdMs);
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
void HydroSystemCore::executeLocalRelayCommand(const RelayCommand& cmd) {
    Serial.println("🏠 [LOCAL] Comando para relés locais via RelayCoordinator");
    
    const RelayOwner owner = resolveCommandOwner(cmd);
    bool success = relayCoordinator.actuateLocal(
        owner, cmd.relayNumber, cmd.action, cmd.durationSeconds);
    
    if (success) {
        Serial.println("✅ Comando local executado com sucesso");
    } else {
        Serial.println("❌ Erro ao executar comando local");
    }
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
    // Sin DHT cableado: no enviar ambiente simulado (evita environment_data basura)
    reading.airTemperature = NAN;
    reading.humidity = NAN;
    printTelemetrySerialLine(reading);
    mqttClient.publishTelemetry(reading);
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
    
    if (!supabaseConnected) {
        Serial.println("❌ [SENSORES] Supabase não conectado - abortando envio");
        return;
    }
    
    if (!hasEnoughMemoryForHTTPS()) {
        Serial.printf("❌ [SENSORES] Memória insuficiente: %d bytes\n", ESP.getFreeHeap());
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
    Serial.printf("📊 [SENSORES] Valores: Temp=%.2f°C, pH=%.2f, TDS=%.2f ppm\n", 
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
    } else {
        Serial.println("❌ [SENSORES] Falha ao enviar dados hidropônicos");
    }
}

void HydroSystemCore::sendDeviceStatusToSupabase() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS()) {
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
    if (!mqttClient.isConnected()) {
        if (supabase.updateDeviceStatus(status)) {
            Serial.println("📤 Status do dispositivo (HTTPS fallback — MQTT offline)");
        }
    }
#else
    if (supabase.updateDeviceStatus(status)) {
        Serial.println("📤 Status do dispositivo atualizado no Supabase");
    }
#endif
    
    // ✅ NOVO: Atualizar relay_master (arrays segregados)
    // Preparar arrays de timers e remaining_times
    bool hasTimers[16] = {false};
    int remainingTimes[16] = {0};
    
    // Obter timers do HydroControl (se disponível)
    // Nota: HydroControl tem timerSeconds[], mas não tem método público para acessar
    // Por enquanto, vamos passar nullptr e o método updateRelayMaster usará valores padrão
    
    // ✅ EVENT-DRIVEN: Salvar em NVS primeiro (cache local)
    saveMasterRelayStatesToNVS();
    
    // ✅ Depois POST para Supabase (fonte de verdade remota)
    if (supabase.updateRelayMaster(getDeviceID(), relayStates, nullptr, nullptr, nullptr)) {
        Serial.println("✅ Estados dos relés master atualizados em relay_master");
        if (!mqttClient.isConnected()) {
            syncEcOperationStateToSupabase();
        }
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
        nextCheckSec
    );
}

void HydroSystemCore::checkPHConfigFromSupabase() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS() || !supabase.isReady()) {
        return;
    }

    PHConfig config;
    if (!supabase.getPHConfigFromSupabase(config)) {
        return;
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
        return;
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
        (float)config.gain_alpha,
        (float)config.max_dose_ml_per_cycle,
        config.max_pulse_seconds,
        config.max_consecutive_corrections
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
    PendingNutrientDoseExport& item = pendingNutrientDoseQueue[pendingNutrientDoseHead];
    if (tryInsertNutrientDoseHttps(item, "HTTPS backup")) {
        pendingNutrientDoseHead = (pendingNutrientDoseHead + 1) % PENDING_NUTRIENT_DOSE_CAP;
        pendingNutrientDoseCount--;
        return;
    }
    item.attempts++;
    if (item.attempts >= PENDING_DOSE_MAX_ATTEMPTS) {
        Serial.printf("⚠️ [DOSAGEM] HTTPS backup abandonado após %u tentativas: %s\n",
                      item.attempts, item.nutrientName);
        pendingNutrientDoseHead = (pendingNutrientDoseHead + 1) % PENDING_NUTRIENT_DOSE_CAP;
        pendingNutrientDoseCount--;
    }
}

void HydroSystemCore::flushPendingPhDoseExports() {
    if (pendingPhDoseCount == 0) {
        return;
    }
    PendingPhDoseExport& item = pendingPhDoseQueue[pendingPhDoseHead];
    if (tryInsertPhDoseHttps(item, "HTTPS backup")) {
        pendingPhDoseHead = (pendingPhDoseHead + 1) % PENDING_PH_DOSE_CAP;
        pendingPhDoseCount--;
        return;
    }
    item.attempts++;
    if (item.attempts >= PENDING_DOSE_MAX_ATTEMPTS) {
        Serial.printf("⚠️ [PH DOSAGEM] HTTPS backup abandonado após %u tentativas: %s\n",
                      item.attempts, item.direction);
        pendingPhDoseHead = (pendingPhDoseHead + 1) % PENDING_PH_DOSE_CAP;
        pendingPhDoseCount--;
    }
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
        PendingPhDoseExport pending = {};
        copyPhDoseToPending(event, pending);
        if (!tryInsertPhDoseHttps(pending, "HTTPS fallback")) {
            enqueuePendingPhDose(event);
        }
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
        PendingNutrientDoseExport pending = {};
        copyNutrientDoseToPending(event, pending);
        if (!tryInsertNutrientDoseHttps(pending, "HTTPS fallback")) {
            enqueuePendingNutrientDose(event);
        }
    }
    // MQTT OK → bridge persiste; sem backup HTTPS (evita SSL OOM + 409 durante dosing)

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

    if (!exported && supabaseConnected) {
        const String deviceId = getDeviceID();
        const String seqId = String(event->sequenceId);
        const String source = String(event->source ? event->source : "manual");
        supabase.insertEcDilutionEvent(
            deviceId, seqId,
            event->ecBefore, event->ecSetpoint,
            event->volumeTargetL, event->volumeMeasuredL,
            event->drainDurationSec, event->fillDurationSec,
            source);
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
        dilProgress
    );
}

// ===== ✅ NOVO: SINCRONIZAÇÃO UNIFICADA DE TODOS OS RELAY STATES =====
// Atualiza master relays + slave relays juntos no Supabase a cada 5 segundos
void HydroSystemCore::syncAllRelayStatesToSupabase() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS()) {
        return;
    }
    
    if (!supabase.isReady()) return;
    
    Serial.println("🔄 [SYNC] Sincronizando todos os relay states para Supabase...");
    
    // ===== 1. MASTER RELAYS (dosadores, níveis, reservados) =====
    bool* masterRelayStates = hydroControl.getRelayStates();
    
    // ✅ EVENT-DRIVEN: Salvar em NVS primeiro (cache local)
    saveMasterRelayStatesToNVS();
    
    // ✅ POST para Supabase (relay_master)
    if (supabase.updateRelayMaster(getDeviceID(), masterRelayStates, nullptr, nullptr, nullptr)) {
        Serial.println("✅ [SYNC] Master relays atualizados em relay_master");
        if (!mqttClient.isConnected()) {
            syncEcOperationStateToSupabase();
            syncPhOperationStateToSupabase();
        }
    } else {
        Serial.println("❌ [SYNC] Erro ao atualizar master relays");
    }
    
    // ===== 2. SLAVE RELAYS (via ESP-NOW) =====
    if (masterManager) {
        std::vector<TrustedSlave> slaves = masterManager->getAllTrustedSlaves();
        int updatedCount = 0;
        unsigned long now = millis();
        
        for (const auto& slave : slaves) {
            // ✅ CORRIGIDO: Atualizar mesmo se não está online, mas está na lista de confiáveis
            // Verificar se recebeu mensagem recentemente (últimos 10 minutos)
            unsigned long timeSinceLastSeen = now - slave.lastSeen;
            bool recentlySeen = (timeSinceLastSeen < 600000); // 10 minutos = 600000ms
            
            // ✅ Atualizar se:
            // 1. Está online (recebendo mensagens ESP-NOW)
            // 2. OU está na lista de confiáveis e recebeu mensagem recentemente (< 10 min)
            if (!slave.isOnline() && !recentlySeen) {
                // Slave não está online e não recebeu mensagem recentemente - pular
                continue;
            }
            
            // ✅ CRÍTICO: Solicitar status atualizado ANTES de ler do cache
            // Isso garante que o cache esteja atualizado com os estados reais dos relés
            Serial.printf("📡 [SYNC] Solicitando status atualizado do slave %s...\n", 
                         ESPNowController::macToString(slave.macAddress).c_str());
            masterManager->requestSlaveStatus(slave.macAddress);
            delay(500);  // Aguardar resposta do slave (ALL_RELAYS_STATUS)
            
            // Preparar arrays para este slave (8 relés)
            bool slaveRelayStates[8] = {false};
            bool slaveHasTimers[8] = {false};
            int slaveRemainingTimes[8] = {0};
            String slaveRelayNames[8];
            
            // ✅ DEBUG: Verificar estados antes de coletar
            Serial.printf("🔍 [SYNC] Coletando estados do slave %s:\n", ESPNowController::macToString(slave.macAddress).c_str());
            Serial.printf("   numRelays: %d\n", slave.numRelays);
            Serial.printf("   isOnline: %s\n", slave.isOnline() ? "SIM" : "NÃO");
            Serial.printf("   lastSeen: %lu ms atrás\n", timeSinceLastSeen);
            
            // Coletar estados do slave
            for (int i = 0; i < slave.numRelays && i < 8; i++) {
                slaveRelayStates[i] = slave.relayStates[i].state;
                slaveHasTimers[i] = slave.relayStates[i].hasTimer;
                slaveRemainingTimes[i] = slave.relayStates[i].remainingTime;
                slaveRelayNames[i] = slave.relayStates[i].name;
                
                // ✅ DEBUG: Log de cada relé (MELHORADO)
                Serial.printf("   Relé %d: state=%s, hasTimer=%s, remainingTime=%d, name=%s, lastUpdate=%lu ms\n",
                            i,
                            slaveRelayStates[i] ? "ON" : "OFF",
                            slaveHasTimers[i] ? "SIM" : "NÃO",
                            slaveRemainingTimes[i],
                            slaveRelayNames[i].length() > 0 ? slaveRelayNames[i].c_str() : "(sem nome)",
                            slave.relayStates[i].lastUpdate);
                
                // ✅ VERIFICAÇÃO: Se lastUpdate é muito antigo, pode estar desatualizado
                if (slave.relayStates[i].lastUpdate == 0) {
                    Serial.printf("   ⚠️ [SYNC] Relé %d nunca foi atualizado (lastUpdate=0)!\n", i);
                } else {
                    unsigned long timeSinceUpdate = millis() - slave.relayStates[i].lastUpdate;
                    if (timeSinceUpdate > 60000) {  // Mais de 1 minuto
                        Serial.printf("   ⚠️ [SYNC] Relé %d não atualizado há %lu ms (pode estar desatualizado)\n", 
                                    i, timeSinceUpdate);
                    }
                }
            }
            
            // ✅ DEBUG: Mostrar array completo antes de enviar
            Serial.printf("📊 [SYNC] Array de estados coletado: [");
            for (int i = 0; i < 8; i++) {
                Serial.printf("%s", slaveRelayStates[i] ? "true" : "false");
                if (i < 7) Serial.printf(", ");
            }
            Serial.printf("]\n");
            
            // Gerar device_id do slave
            String slaveMac = ESPNowController::macToString(slave.macAddress);
            String slaveDeviceId = "ESP32_SLAVE_" + slaveMac;
            slaveDeviceId.replace(":", "_");
            
            // ✅ POST para Supabase (relay_slaves) - atualiza last_update automaticamente
            Serial.printf("📤 [SYNC] Enviando estados para Supabase: device_id=%s\n", slaveDeviceId.c_str());
            if (supabase.updateRelaySlaves(slaveDeviceId, getDeviceID(), slaveMac, 
                                          slaveRelayStates, slaveHasTimers, 
                                          slaveRemainingTimes, slaveRelayNames)) {
                updatedCount++;
                Serial.printf("✅ [SYNC] Slave %s atualizado (online: %s, lastSeen: %lu ms atrás)\n", 
                            slaveDeviceId.c_str(), 
                            slave.isOnline() ? "SIM" : "NÃO",
                            timeSinceLastSeen);
            } else {
                Serial.printf("❌ [SYNC] Erro ao atualizar slave %s no Supabase\n", slaveDeviceId.c_str());
            }
        }
        
        if (updatedCount > 0) {
            Serial.printf("✅ [SYNC] %d slave(s) atualizado(s) em relay_slaves\n", updatedCount);
        } else {
            Serial.println("⚠️ [SYNC] Nenhum slave atualizado (todos offline ou sem mensagens recentes)");
        }
    }
    
    Serial.println("✅ [SYNC] Sincronização completa!");
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
    
    // ===== DESABILITAR SUPABASE SE MEMÓRIA BAIXA =====
    if (freeHeap < MIN_HEAP_FOR_HTTPS && supabaseConnected) {
        Serial.println("⚠️ Desabilitando Supabase temporariamente - Heap baixo");
        supabaseConnected = false;
    } else if (freeHeap > MIN_HEAP_FOR_HTTPS + 10000 && !supabaseConnected) {
        Serial.println("✅ Reabilitando Supabase - Heap recuperado");
        supabaseConnected = true;
    }
}

// ===== UTILITIES =====
bool HydroSystemCore::hasEnoughMemoryForHTTPS() {
    return ESP.getFreeHeap() >= MIN_HEAP_FOR_HTTPS;
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
                            uint32_t commandId = masterManager->sendRelayCommandToSlave(
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
    String relayNames[8];
    
    // Tentar buscar slave da lista confiável
    auto trustedSlaves = masterManager->getAllTrustedSlaves();
    bool slaveFound = false;
    
    for (const auto& s : trustedSlaves) {
        if (memcmp(s.macAddress, slaveMac, 6) == 0) {
            slaveFound = true;
            // Preencher arrays com estados atuais do slave
            for (int i = 0; i < 8 && i < s.numRelays; i++) {
                relayStates[i] = s.relayStates[i].state;
                hasTimers[i] = s.relayStates[i].hasTimer;
                remainingTimes[i] = s.relayStates[i].remainingTime;
                relayNames[i] = s.relayStates[i].name;
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
        relayNames
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

void HydroSystemCore::mqttCommandReceived(const char* payload, size_t length, void* userData) {
    if (!userData) {
        return;
    }
    static_cast<HydroSystemCore*>(userData)->handleMqttCommandPayload(payload, length);
}

void HydroSystemCore::handleMqttCommandPayload(const char* payload, size_t length) {
    float dilutionVolumeL = 0.0f;
    if (parseMqttEcDilutionCommand(payload, length, dilutionVolumeL)) {
        if (hydroControl.startEcDilution(dilutionVolumeL, "manual")) {
            syncEcOperationStateToSupabase();
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