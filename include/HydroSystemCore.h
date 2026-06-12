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

// ===== 🎯 CACHE NVS DE ESTADOS DE MASTER RELAYS (LOCAL, NÃO ESP-NOW) =====
/**
 * @brief Estado cacheado de um relé master (guardado em NVS LOCAL)
 * ✅ SEGREGADO: Master relays são do corpo físico do master, não têm nada a ver com ESP-NOW
 */
struct CachedMasterRelayState {
    uint8_t relayNumber;         // Número do relé (0-15)
    uint8_t state;               // 0=OFF, 1=ON
    uint8_t hasTimer;            // 1=tem timer, 0=sem timer
    uint16_t remainingTime;      // Tempo restante em segundos
    uint32_t timestamp;          // Timestamp da última atualização (millis)
    uint8_t relayType;           // 0=doser (0-7), 1=level (8-11), 2=reserved (12-15)
    uint8_t padding[2];          // Padding para alinhamento
} __attribute__((packed));

/**
 * @brief Cache de estados de TODOS os relés master (16 relés)
 * ✅ SEGREGADO: Guardado em NVS LOCAL (não misturado com ESP-NOW)
 * ✅ EVENT-DRIVEN: NVS → POST Supabase
 */
struct MasterRelayStatesCache {
    uint32_t timestamp;              // Timestamp da última atualização
    uint8_t version;                 // Versão do formato (1 = inicial)
    uint8_t numRelays;               // Número de relés (16)
    uint8_t padding[2];              // Padding para alinhamento
    CachedMasterRelayState states[16]; // Estados dos 16 relés master
    uint8_t checksum;                // Checksum para validação
} __attribute__((packed));

// Forward declarations para evitar dependencias circulares
class WebServerTask;
class ESPNowController;
class MasterSlaveManager;  // ✅ Para integración ESP-NOW

class HydroSystemCore {
private:
    HydroControl hydroControl;
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
    unsigned long lastSupabaseCheck;
    unsigned long lastRulesCheck;      // ✅ NOVO: Controle de verificação de regras (decision_rules)
    unsigned long lastMemoryProtection;
    unsigned long lastMqttTelemetrySend;
    unsigned long lastMqttHeartbeatSend;
    unsigned long lastEcOperationSync;
    unsigned long lastEcOperationIdleSync;
    unsigned long commandPollIntervalMs;
    MqttCommandDedup mqttCommandDedup;
    
    // Intervalos otimizados para resposta mais rápida
    static const unsigned long SENSOR_SEND_INTERVAL = 30000;      // 30s
    static const unsigned long STATUS_SEND_INTERVAL = 60000;      // 1 min (mantido para device_status)
    static const unsigned long RELAY_STATES_SYNC_INTERVAL = 10000; // 10s — espelho relay_master/slaves (após comando há update imediato)
    static const unsigned long EC_OPERATION_SYNC_INTERVAL = 12000; // 12s — remaining_sec fresco durante dosing/recirc
    static const unsigned long EC_OPERATION_IDLE_SYNC_INTERVAL = 30000; // 30s — limpa ec_operation huérfano em Supabase
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
    
    // Proteção de memória específica para HTTPS
    static const uint32_t MIN_HEAP_FOR_HTTPS = 30000;  // 30KB mínimo para SSL
    
    // ✅ NOVO: Sistema de mapeamento commandId (ESP-NOW) → supabaseCommandId
    struct CommandMapping {
        uint32_t espNowCommandId;
        int supabaseCommandId;
        unsigned long timestamp;
    };
    std::vector<CommandMapping> commandMappings;
    SemaphoreHandle_t mappingsMutex;
    
    // ✅ NOVO: Funções de mapeamento
    void addCommandMapping(uint32_t espNowCommandId, int supabaseCommandId);
    int findSupabaseCommandId(uint32_t espNowCommandId);
    void cleanupExpiredMappings();
    
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
    void setMasterManager(MasterSlaveManager* masterMgr) { masterManager = masterMgr; }
    void setWebServerManager(WebServerManager* webMgr) { webServerManager = webMgr; }  // ✅ TÓPICO 4
    
    // ✅ Método para registrar endpoints quando webServerTask estiver disponível
    void registerWebServerEndpoints();
    
    // Status
    bool isReady() const { return systemReady; }
    bool isSupabaseConnected() const { return supabaseConnected; }
    unsigned long getUptime() const { return millis() - startTime; }
    
    // Acesso aos módulos (para comandos seriais)
    HydroControl& getHydroControl() { return hydroControl; }
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
    
private:
    // Operações principais
    void checkSupabaseCommands();
    void checkSupabaseRules();  // ✅ NOVO: Verificar regras de automação (decision_rules)
    void checkECConfigFromSupabase();  // ✅ NOVO: Buscar EC config do Supabase via RPC activate_auto_ec
    void checkPHConfigFromSupabase();  // ✅ Buscar pH config via RPC activate_auto_ph
    void processRelayCommand(const RelayCommand& cmd, bool isSlave, const char* via = "https");
    
    static void mqttCommandReceived(const char* payload, size_t length, void* userData);
    void handleMqttCommandPayload(const char* payload, size_t length);
    unsigned long resolveCommandPollIntervalMs() const;
    
    // ✅ FORK: Processamento separado por tipo de comando
    void processManualCommand(const RelayCommand& cmd, bool isSlave);      // Comando manual (botão)
    void processRuleCommand(const RelayCommand& cmd, bool isSlave);        // Comando de regra (automação)
    void processPeristalticCommand(const RelayCommand& cmd, bool isSlave); // Comando de dosagem proporcional
    
    // ✅ Função auxiliar para executar comando local
    void executeLocalRelayCommand(const RelayCommand& cmd);
    
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
    static void onNutrientDoseStatic(const NutrientDoseEvent* event, void* userData);
    static void onEcOperationSyncStatic(void* userData);
    static void onPhOperationSyncStatic(void* userData);
    void publishMqttTelemetry();
    void publishMqttHeartbeat();
    void performMemoryProtection();
    
    // ✅ TÓPICO 4: Procesar comandos de queue (Core 0)
    void processWebCommands();  // Procesar comandos de WebServerManager queue
    
    // ✅ Método privado para registrar endpoints do WebServer
    void tryRegisterEndpoints();  // Tenta registrar endpoints se webServerTask estiver disponível
    
    // Utilities
    bool hasEnoughMemoryForHTTPS();
    void printPeriodicStatus();
};

#endif // HYDRO_SYSTEM_CORE_H

