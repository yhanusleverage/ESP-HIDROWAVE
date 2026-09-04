#ifndef HYDRO_SYSTEM_CORE_H
#define HYDRO_SYSTEM_CORE_H

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>  // ✅ Para JsonArray em updateNutrientProportions
#include <vector>  // ✅ NOVO: Para sistema de mapeamento
#include <freertos/FreeRTOS.h>  // ✅ NOVO: Para SemaphoreHandle_t
#include <freertos/semphr.h>  // ✅ NOVO: Para mutex
#include "HydroControl.h"
#include "SupabaseClient.h"
#include "HydroSupaManager.h"
#include "RelayCommandBox.h"
#include "Config.h"
#include "WebServerManager.h"  // ✅ TÓPICO 4: Para procesar comandos de queue
#include "ESPNowController.h"  // ✅ NOVO: Para macToString
#include "MqttClient.h"
#include "MqttCommandDedup.h"
#include "RelayCoordinator.h"
#include "DecisionEngine.h"
#include "DecisionEngineIntegration.h"
#include "StateCacheTypes.h"
#include "StatePersistenceManager.h"
#include "StatusLED.h"
#if ENABLE_HMI_UART
#include "HmiUartBridge.h"
#endif

// Forward declarations para evitar dependencias circulares
class WebServerTask;
class ESPNowController;
class MasterSlaveManager;  // ✅ Para integración ESP-NOW

class HydroSystemCore {
private:
    HydroControl hydroControl;
    RelayCoordinator relayCoordinator;
    RelayCommandBox relayController;  // ✅ Controlador de relés (8 relés)
    SupabaseClient supabase;
    HydroSupaManager hybridSupabase;  // ✅ Manager híbrido
    MqttClientWrapper mqttClient;
    
    // ✅ INYECCIÓN DE DEPENDENCIAS: Referencias a componentes externos
    WebServerTask* webServerTask;      // Ponteiro para WebServerTask (Core 1)
    ESPNowController* espNowController; // Ponteiro para ESPNowController
    MasterSlaveManager* masterManager;  // ✅ Ponteiro para MasterSlaveManager (ESP-NOW)
    WebServerManager* webServerManager;  // ✅ TÓPICO 4: Para procesar comandos de queue
    
    // Estados do sistema
    bool systemReady;
    bool supabaseConnected;
    bool endpointsRegistered;  // ✅ Rastrear se endpoints foram registrados
    unsigned long startTime;
    
    // Controle de timing para operações
    unsigned long lastSensorSend;
    unsigned long lastStatusSend;
    unsigned long lastRelayStatesSync;  // ✅ NOVO: Controle de sincronização unificada de relay states
    unsigned long lastStatusPrint;
    unsigned long lastRulesCheck;      // ✅ NOVO: Controle de verificação de regras (decision_rules)
    unsigned long lastMemoryProtection;
    unsigned long lastEspNowChannelPollMs;
    unsigned long lastMqttTelemetrySend;
    unsigned long lastMqttHeartbeatSend;
    unsigned long lastMqttCloudLastSeen;
    unsigned long lastMqttLevelsPublishMs;
    bool mqttLevelsFingerprintValid;
    uint8_t lastMqttLevelsMask;
    char lastMqttWaterLevel[16];
    char lastMqttInterlockMode[12];
    unsigned long lastEcOperationSync;
    unsigned long lastEcOperationIdleSync;
    unsigned long lastPhOperationSync;
    unsigned long lastPhOperationIdleSync;
    MqttCommandDedup mqttCommandDedup;

    DecisionEngine decisionEngine;
    DecisionEngineIntegration decisionIntegration;
    bool bootOperationInterrupted;
    bool decisionEngineReady;

    StatusLED statusLed;
    unsigned long statusLedSendUntilMs;
    int lastWifiLedState;  // wl_status_t cached; -1 = unset
    void updateStatusLedFromWifi();
    void notifyCloudSend();
    void finishStatusLedSendPulseIfDue();

    /** Cola de respaldo HTTPS para dose EC (evita pérdida silenciosa quando MQTT OK mas bridge falha). */
    struct PendingNutrientDoseExport {
        char sequenceId[24];
        char nutrientName[32];
        char source[16];
        int relayNumber;
        float dosageMl;
        float dosageTimeSeconds;
        float ecBefore;
        float ecSetpoint;
        uint8_t attempts;
    };
    static const size_t PENDING_NUTRIENT_DOSE_CAP = 4;
    PendingNutrientDoseExport pendingNutrientDoseQueue[PENDING_NUTRIENT_DOSE_CAP];
    uint8_t pendingNutrientDoseHead;
    uint8_t pendingNutrientDoseCount;

    /** Cola de respaldo HTTPS para dose pH (paridade EC). */
    struct PendingPhDoseExport {
        char sequenceId[24];
        char direction[8];
        char source[16];
        int relayNumber;
        float dosageMl;
        float dosageTimeSeconds;
        float phBefore;
        float phSetpoint;
        uint8_t attempts;
    };
    static const size_t PENDING_PH_DOSE_CAP = 4;
    PendingPhDoseExport pendingPhDoseQueue[PENDING_PH_DOSE_CAP];
    uint8_t pendingPhDoseHead;
    uint8_t pendingPhDoseCount;
    static const uint8_t PENDING_DOSE_MAX_ATTEMPTS = 30;

    /** Cola de cierre cloud cuando ACK hardware OK pero SSL/RPC falló */
    struct PendingCloudAck {
        int supabaseCommandId;
        uint32_t espNowCommandId;
        uint8_t slaveMac[6];
        int relayNumber;
        bool currentState;
        uint8_t attempts;
    };
    static const size_t PENDING_CLOUD_ACK_CAP = 32;
    static const uint8_t PENDING_CLOUD_ACK_MAX_ATTEMPTS = 20;
    PendingCloudAck pendingCloudAckQueue[PENDING_CLOUD_ACK_CAP];
    uint8_t pendingCloudAckHead;
    uint8_t pendingCloudAckCount;
    
    // Intervalos otimizados para resposta mais rápida
    static const unsigned long SENSOR_SEND_INTERVAL = 30000;      // 30s (MQTT OK → hydro via broker)
    /** HTTPS hydro quando MQTT offline — mais lento para não saturar TLS/heap */
    static const unsigned long SENSOR_SEND_INTERVAL_MQTT_OFFLINE = 90000;  // 90s
    static const unsigned long STATUS_SEND_INTERVAL = 60000;      // 1 min (mantido para device_status)
    static const unsigned long STATUS_SEND_INTERVAL_MQTT_OFFLINE = 120000; // 2 min fallback HTTPS
    static const unsigned long RELAY_STATES_SYNC_INTERVAL = 30000; // 30s — espelho relay_master/slaves
    static const unsigned long RELAY_STATES_SYNC_FORCE_RF_MS = 60000; // backup ALL_RELAYS RF a cada 60s
    static const unsigned long SLAVE_RELAY_HEARTBEAT_INTERVAL = 45000; // relay/state periódico p/ cloud
    unsigned long lastSlaveRelayHeartbeat;
    unsigned long lastSlaveRelayFullSync;
    static const unsigned long MQTT_CLOUD_LAST_SEEN_INTERVAL = 240000UL; // 4 min — margem sob UI 5 min
    static const unsigned long EC_OPERATION_SYNC_INTERVAL = 12000; // 12s — remaining_sec fresco durante dosing/recirc
    static const unsigned long EC_OPERATION_IDLE_SYNC_INTERVAL = 30000; // 30s — limpa ec_operation huérfano em Supabase
    static const unsigned long PH_OPERATION_SYNC_INTERVAL = 12000;
    static const unsigned long PH_OPERATION_IDLE_SYNC_INTERVAL = 30000;
    static const unsigned long STATUS_PRINT_INTERVAL = 30000;     // 30s
    static const unsigned long RULES_CHECK_INTERVAL = 30000;      // ✅ 30s - Verificação de regras de automação (decision_rules)
    // Eliminar macro antes de definir constante estática
    #ifdef MEMORY_CHECK_INTERVAL
        #undef MEMORY_CHECK_INTERVAL
    #endif
    static const unsigned long MEMORY_CHECK_INTERVAL = 10000;     // 10s
    // Restaurar macro después (opcional, solo si se necesita)
    #ifndef MEMORY_CHECK_INTERVAL
        #define MEMORY_CHECK_INTERVAL 10000
    #endif
    
    // Proteção de memória específica para HTTPS / mbedTLS
    // 30KB era insuficiente: logs bancada falhavam SSL com ~90KB free (fragmentação).
    static const uint32_t MIN_HEAP_FOR_HTTPS = 80000;           // 80KB livre total
    static const uint32_t MIN_CONTIGUOUS_FOR_HTTPS = 40960;     // bloco contíguo p/ TLS
    static const uint32_t HEAP_SUPABASE_DISABLE = 50000;        // desliga client se crítico
    static const uint32_t HEAP_SUPABASE_REENABLE = 90000;       // histerese reabilitar
    
    // ✅ NOVO: Sistema de mapeamento commandId (ESP-NOW) → supabaseCommandId
    struct CommandMapping {
        uint32_t espNowCommandId;
        int supabaseCommandId;
        unsigned long timestamp;
    };
    std::vector<CommandMapping> commandMappings;
    SemaphoreHandle_t mappingsMutex;
    
    // ✅ NOVO: Funções de mapeamento
    void addCommandMapping(uint32_t espNowCommandId, int supabaseCommandId, bool quiet = false);
    int findSupabaseCommandId(uint32_t espNowCommandId);
    /** Remove todos os mapeamentos de um espNow id (batch mask ACK). */
    void drainSupabaseMappingsForEspNow(uint32_t espNowCommandId, std::vector<int>& out);
    void cleanupExpiredMappings();
    /** Fecha todos os tickets Supabase pendentes de um batch (ACK relé 255 / máscara). */
    void completePendingAcksForEspNowCommand(uint32_t espNowCommandId, const uint8_t* slaveMac,
                                             const char* via);

    /** Espera de ACK slave (fallback via ALL_RELAYS_STATUS) */
    struct PendingSlaveCommandAck {
        int supabaseCommandId;
        uint32_t espNowCommandId;
        uint8_t slaveMac[6];
        int relayNumber;
        bool expectedOn;
        unsigned long sentAt;
    };
    static const unsigned long PENDING_SLAVE_ACK_TTL_MS = 60000;
    std::vector<PendingSlaveCommandAck> pendingSlaveCommandAcks;
    SemaphoreHandle_t pendingAckMutex;

    void registerPendingSlaveAck(int supabaseCommandId, uint32_t espNowCommandId,
                                 const uint8_t* slaveMac, int relayNumber, const String& action);
    void reconcilePendingSlaveAcks(const uint8_t* slaveMac, const bool relayStates[8], uint8_t numRelays);
    void cleanupExpiredPendingSlaveAcks();
    void completeSlaveCommand(int supabaseCommandId, uint32_t espNowCommandId,
                              const uint8_t* slaveMac, int relayNumber, bool currentState,
                              const char* via);
    bool tryCloseCloudRelayCommand(int supabaseCommandId, const uint8_t* slaveMac,
                                   int relayNumber, bool currentState,
                                   uint32_t espNowCommandId = 0);
    void enqueuePendingCloudAck(int supabaseCommandId, uint32_t espNowCommandId,
                                const uint8_t* slaveMac, int relayNumber, bool currentState);
    void flushPendingCloudAcks();
    bool hasPendingCloudAcks() const { return pendingCloudAckCount > 0; }
    bool hasPendingSlaveAcks();
    /** Colas ESP-NOW / ACK cloud — para defer de sync no crítico. */
    bool isSslHotPathBusy();
    /** Solo transporte HTTPS real — poll EC/pH config no debe morir por slave offline. */
    bool isSslTransportBusy();

    static const size_t RECENTLY_CLOSED_ACK_CAP = 16;
    static const unsigned long RECENTLY_CLOSED_ACK_TTL_MS = 15000;
    int recentlyClosedSupabaseIds[RECENTLY_CLOSED_ACK_CAP];
    unsigned long recentlyClosedAtMs[RECENTLY_CLOSED_ACK_CAP];
    uint8_t recentlyClosedCount;
    bool wasRecentlyClosedCloudAck(int supabaseCommandId) const;
    void markRecentlyClosedCloudAck(int supabaseCommandId);
#if ENABLE_MQTT
    unsigned long mqttConnectedSinceMs;
    unsigned long lastRuleExecutedMirrorMs;
    bool mqttEcConfigReceived;
    bool mqttPhConfigReceived;
    bool isMqttCommandPathStable() const;
    static void onRuleExecutedMirrorStatic(const RuleExecutedMirrorEvent& event, void* userData);
    void mirrorRuleExecuted(const RuleExecutedMirrorEvent& event);
    bool tryPublishCloudAckViaMqtt(int supabaseCommandId, uint32_t espNowCommandId,
                                   const uint8_t* slaveMac, int relayNumber, bool currentState,
                                   const char* status = "completed");
    void publishSlaveRelayStateMqtt(const uint8_t* slaveMac, int fallbackRelay = -1,
                                    bool fallbackState = false, bool heartbeat = false);
    void scheduleSlaveRelayStateMqtt(const uint8_t* slaveMac, bool urgent, bool heartbeat);
    void flushPendingRelayStateMqtt();
    void forceSlaveRelayMqttFullSync();

    static const size_t RELAY_STATE_COALESCE_SLOTS = 4;
    struct PendingRelayStateSlot {
        uint8_t mac[6];
        unsigned long dueMs;
        bool active;
        bool urgent;
        bool heartbeat;
    };
    struct LastPublishedRelayState {
        uint8_t mac[6];
        bool valid;
        bool states[8];
        bool timers[8];
        int remaining[8];
        bool linkOnline;
        uint8_t numRelays;
    };
    PendingRelayStateSlot pendingRelayStateSlots_[RELAY_STATE_COALESCE_SLOTS];
    LastPublishedRelayState lastPublishedRelayState_[RELAY_STATE_COALESCE_SLOTS];

    int findRelayStateCoalesceSlot(const uint8_t* mac) const;
    int allocRelayStateCoalesceSlot(const uint8_t* mac);
    bool isRelayStateSnapshotUnchanged(const uint8_t* mac, const bool states[8], const bool timers[8],
                                       const int remaining[8], uint8_t numRelays, bool linkOnline) const;
    void rememberPublishedRelayState(const uint8_t* mac, const bool states[8], const bool timers[8],
                                     const int remaining[8], uint8_t numRelays, bool linkOnline);
#endif

#if ESPNOW_RELAY_BATCH_ENABLED
    struct EspNowRelayBatchItem {
        int supabaseCommandId;
        int relayNumber;
        bool wantOn;
    };
    static const size_t ESPNOW_RELAY_BATCH_SLOTS = 4;
    static const size_t ESPNOW_RELAY_BATCH_MAX_ITEMS = 8;
    struct EspNowRelayBatchSlot {
        uint8_t mac[6];
        bool active;
        unsigned long flushAtMs;
        unsigned long openedAtMs;
        uint8_t desiredMask;
        bool maskInitialized;
        RelayOwner owner;
        EspNowRelayBatchItem items[ESPNOW_RELAY_BATCH_MAX_ITEMS];
        size_t itemCount;
    };
    EspNowRelayBatchSlot espNowRelayBatchSlots_[ESPNOW_RELAY_BATCH_SLOTS];

    static bool isEspNowRelayBatchEligible(const RelayCommand& cmd, bool isSlave);
    int findEspNowRelayBatchSlot(const uint8_t* mac, bool create);
    bool initEspNowRelayBatchMask(EspNowRelayBatchSlot& slot);
    bool tryQueueEspNowRelayBatch(const RelayCommand& cmd, const uint8_t* targetMac, RelayOwner owner);
    void flushEspNowRelayBatchSlot(EspNowRelayBatchSlot& slot, const char* reason);
    void flushEspNowRelayBatchesDue(unsigned long now);
    void flushAllEspNowRelayBatches(const char* reason);
#endif
    
public:
    /**
     * @brief Construtor com injeção de dependências
     * @param webTask Ponteiro para WebServerTask (opcional, pode ser nullptr)
     * @param espNow Ponteiro para ESPNowController (opcional, pode ser nullptr)
     * @param masterMgr Ponteiro para MasterSlaveManager (opcional, pode ser nullptr)
     */
    HydroSystemCore(WebServerTask* webTask = nullptr, ESPNowController* espNow = nullptr, MasterSlaveManager* masterMgr = nullptr);
    ~HydroSystemCore();
    
    // Controle do sistema
    bool begin();
    void loop();
    void end();
    
    // ✅ Setters para configurar dependências depois da construção (opcional)
    void setWebServerTask(WebServerTask* webTask);
    void setESPNowController(ESPNowController* espNow) { espNowController = espNow; }
    void setMasterManager(MasterSlaveManager* masterMgr);
    void setWebServerManager(WebServerManager* webMgr) { webServerManager = webMgr; }  // ✅ TÓPICO 4
    
    // ✅ Método para registrar endpoints quando webServerTask estiver disponível
    void registerWebServerEndpoints();
    
    // Status
    bool isReady() const { return systemReady; }
    bool isSupabaseConnected() const { return supabaseConnected; }
    unsigned long getUptime() const { return millis() - startTime; }
    
    // Acesso aos módulos (para comandos seriais)
    HydroControl& getHydroControl() { return hydroControl; }
    RelayCoordinator& getRelayCoordinator() { return relayCoordinator; }
    SupabaseClient& getSupabase() { return supabase; }
    
    // ✅ NOVO: Cache NVS de estados dos relés master (EVENT-DRIVEN)
    bool saveMasterRelayStatesToNVS();
    bool loadMasterRelayStatesFromNVS();
    
    // ✅ Acesso ao Controller KP
    ECController& getECController() { return hydroControl.getECController(); }
    const ECController& getECController() const { return hydroControl.getECController(); }
    float getEC() const { return hydroControl.getEC(); }
    void setECSetpoint(float setpoint) { hydroControl.setECSetpoint(setpoint); }
    bool isAutoECEnabled() const { return hydroControl.isAutoECEnabled(); }
    
    // ✅ Acesso ao sistema de proporções dinâmicas
    void updateNutrientProportions(JsonArray nutrients) { 
        hydroControl.updateNutrientProportions(nutrients); 
    }
    
    // Debug e comandos
    void printSystemStatus();
    void printSensorReadings();
    void testSupabaseConnection();

#if ENABLE_HMI_UART && UART_BRINGUP
    void dumpHmiUartLinkStatus(Stream& out) const;
    static void dumpHmiUartLinkStatusStatic(Stream& out);
#endif
    
private:
    // Operações principais
    void applyBootPolicies();
    void initDecisionEngine();
    void wireRelayCoordinatorPolicyCallbacks();
    void checkSupabaseRules();  // stub HTTPS; tipagem via MQTT circ/config
    bool applyECConfig(const ECConfig& config, const char* via);
    bool applyPHConfig(const PHConfig& config, const char* via);
    bool applyCirculationConfigMqtt(const char* payload, size_t length);
    bool upsertFnCirculationRule(const char* slaveMac, int relayIndex, bool enabled);
    bool applyRuleUpsertMqtt(const char* payload, size_t length);
    bool applyRulesManifestMqtt(const char* payload, size_t length);
    /** Ao desactivar regra: OFF dos relés que a regra mantinha em ON. */
    void releaseDecisionRuleActuators(const DecisionRule& rule);
    bool parseMqttEcConfigJson(const char* json, size_t len, ECConfig& config);
    bool parseMqttPhConfigJson(const char* json, size_t len, PHConfig& config);
    void processRelayCommand(const RelayCommand& cmd, bool isSlave, const char* via = "mqtt");
    
    static void mqttIncomingReceived(const char* topic, const char* payload, size_t length, void* userData);
    void handleMqttIncoming(const char* topic, const char* payload, size_t length);
    void handleMqttCommandPayload(const char* payload, size_t length);

#if ENABLE_HMI_UART
    HmiUartBridge hmiUartBridge;
    bool isHmiCloudOk() const;
    static bool hmiCloudOkStatic();
    static String hmiDeviceIdStatic();
    static HydroSystemCore* hmiBridgeInstance;
#endif
    
    // ✅ FORK: Processamento separado por tipo de comando
    void processManualCommand(const RelayCommand& cmd, bool isSlave);      // Comando manual (botão)
    void processRuleCommand(const RelayCommand& cmd, bool isSlave);        // Comando de regra (automação)
    void processPeristalticCommand(const RelayCommand& cmd, bool isSlave); // Comando de dosagem proporcional
    
    // ✅ Função auxiliar para executar comando local
    bool executeLocalRelayCommand(const RelayCommand& cmd);
    void maybePublishManualPumpDose(const RelayCommand& cmd);
    
    // ✅ NOVO: Funções auxiliares para atualizar estados
    void updateRelayMasterState(const RelayCommand& cmd);
    void updateRelaySlaveState(const String& slaveDeviceId, const uint8_t* slaveMac, int relayNumber, bool state);
    bool parseMacAddress(const String& macStr, uint8_t* macBytes);
    
    void sendSensorDataToSupabase();
    void sendDeviceStatusToSupabase();
    void syncAllRelayStatesToSupabase();  // ✅ NOVO: Sincronização unificada de todos os relay states
    void syncEcOperationStateToSupabase();
    void syncPhOperationStateToSupabase();
    void handleNutrientDoseEvent(const NutrientDoseEvent* event);
    void handleEcDilutionEvent(const EcDilutionEvent* event);
    uint32_t handleDilutionSlaveRelay(const uint8_t* mac, int relay, bool on);
    void handlePhDoseEvent(const PhDoseEvent* event);
    void handleEcMetricEvent(const EcControllerMetricEvent* event);
    void handlePhMetricEvent(const PhControllerMetricEvent* event);
    void enqueuePendingNutrientDose(const NutrientDoseEvent* event);
    void enqueuePendingPhDose(const PhDoseEvent* event);
    bool tryInsertNutrientDoseHttps(const PendingNutrientDoseExport& item, const char* logLabel);
    bool tryInsertPhDoseHttps(const PendingPhDoseExport& item, const char* logLabel);
    void flushPendingNutrientDoseExports();
    void flushPendingPhDoseExports();
    static void copyNutrientDoseToPending(const NutrientDoseEvent* event, PendingNutrientDoseExport& out);
    static void copyPhDoseToPending(const PhDoseEvent* event, PendingPhDoseExport& out);
    void handlePhGainLearned();
    static void onPhGainLearnedStatic(void* userData);
    void handleEcGainLearned();
    static void onEcGainLearnedStatic(void* userData);
    static void onNutrientDoseStatic(const NutrientDoseEvent* event, void* userData);
    static void onEcDilutionStatic(const EcDilutionEvent* event, void* userData);
    static uint32_t onDilutionSlaveRelayStatic(const uint8_t* mac, int relay, bool on, void* userData);
    static void onPhDoseStatic(const PhDoseEvent* event, void* userData);
    static void onEcMetricStatic(const EcControllerMetricEvent* event, void* userData);
    static void onPhMetricStatic(const PhControllerMetricEvent* event, void* userData);
    static void onEcOperationSyncStatic(void* userData);
    static void onPhOperationSyncStatic(void* userData);
    static void onPhysicalRecircStatic(bool starting, const char* domain, void* userData);
    static RelayOwner resolveCommandOwner(const RelayCommand& cmd);
    void publishMqttTelemetry();
    void publishMqttHeartbeat();
    void publishMqttLevels();
    void maybePublishMqttLevelsOnChange();
    void performMemoryProtection();
    
    // ✅ TÓPICO 4: Procesar comandos de queue (Core 0)
    void processWebCommands();  // Procesar comandos de WebServerManager queue
    
    // ✅ Método privado para registrar endpoints do WebServer
    void tryRegisterEndpoints();  // Tenta registrar endpoints se webServerTask estiver disponível
    void wireMasterManagerIntegration();
    
    // Utilities
    bool hasEnoughMemoryForHTTPS();
    void printPeriodicStatus();
};

#endif // HYDRO_SYSTEM_CORE_H

