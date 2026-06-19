#ifndef WEB_SERVER_MANAGER_H
#define WEB_SERVER_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "DataTypes.h"
#include "WiFiManager.h"
#include "HydroControl.h"
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <vector>

// Forward declaration
class WebServerTask;
class MasterSlaveManager;  // ✅ Cambiar de ESPNowTask a MasterSlaveManager

/**
 * @brief TÓPICO 1: ESTRUCTURA DE COMANDO WEB
 * 
 * Comando enviado desde dashboard web (Core 1) a loop principal (Core 0)
 * Se envía a través de FreeRTOS Queue para comunicación thread-safe entre cores
 */
struct WebCommand {
    enum Type {
        RELAY_CONTROL,      // Controlar relé (on/off/toggle)
        GET_STATUS,         // Obtener status general
        GET_SLAVES,         // Obtener lista de slaves
        DISCOVER_SLAVES,    // Forzar discovery de slaves
        ALL_RELAYS_ON,      // Encender todos los relays
        ALL_RELAYS_OFF      // Apagar todos los relays
    };
    
    Type type;
    uint8_t slaveMac[6];
    String deviceId;        // Device ID del slave
    uint8_t relayNumber;
    String action;          // "on", "off", "toggle", "on_forever"
    int duration;           // Duración en segundos (0 = sin timer)
    uint32_t requestId;     // ID único para rastrear respuesta
    
    WebCommand() : type(GET_STATUS), relayNumber(0), duration(0), requestId(0) {
        memset(slaveMac, 0, 6);
        deviceId = "";
        action = "";
    }
};

/**
 * @brief TÓPICO 1: CACHE DE DATOS DEL SISTEMA
 * 
 * Evita locks largos - Core 0 actualiza, Core 1 lee
 * Protegido por systemCacheMutex
 */
struct SystemDataCache {
    unsigned long lastUpdate;
    int totalSlaves;
    int onlineSlaves;
    int offlineSlaves;
    bool wifiConnected;
    String wifiIP;
    int wifiChannel;
    int wifiRSSI;
    unsigned long uptime;
    uint32_t freeHeap;
    bool supabaseConnected;
    bool systemInitialized;
    bool webServerRunning;
    
    // ✅ CACHE DE SLAVES (thread-safe entre cores)
    // Core 0 serializa slaves para JSON, Core 1 lê do cache
    String slavesJson;  // JSON com lista completa de slaves
    unsigned long slavesLastUpdate;  // Timestamp da última atualização
    
    SystemDataCache() : 
        lastUpdate(0), totalSlaves(0), onlineSlaves(0), offlineSlaves(0),
        wifiConnected(false), wifiChannel(0), wifiRSSI(0),
        uptime(0), freeHeap(0),
        supabaseConnected(false), systemInitialized(false), webServerRunning(false),
        slavesJson(""), slavesLastUpdate(0) {}
};

class WebServerManager {
private:
    AsyncWebServer* server;
    AsyncWebServer* adminServer;
    WebServerTask* webServerTask;  // ✅ Usar WebServerTask (Core 1)
    bool isRunning;
    SystemStatus* systemStatus;
    SensorData* sensorData;
    bool* relayStates;
    #ifndef NUM_RELAYS
        #define NUM_RELAYS 16
    #endif
    // La macro NUM_RELAYS se usa para arrays, constante estática para acceso
    static constexpr int NUM_RELAYS_VALUE = 16;  // Valor debe coincidir con NUM_RELAYS
    
    // ✅ INTEGRAÇÃO ESP-NOW - Usar MasterSlaveManager en lugar de ESPNowTask
    MasterSlaveManager* masterManager;
    
    // ✅ Guardar referências para uso em lambdas
    WiFiManager* wifiManager;
    HydroControl* hydroControl;

    float* tempRef;
    float* phRef;
    float* tdsRef;
    std::function<void(int, int)> onRelayToggle;

    // ✅ TÓPICO 1: QUEUE Y MUTEX (como MASTER-TASK)
    QueueHandle_t commandQueue;              // Queue FreeRTOS: comandos web → Core 0
    SemaphoreHandle_t systemCacheMutex;      // Mutex para systemCache
    SystemDataCache systemCache;             // Cache de datos del sistema
    
    uint32_t requestIdCounter;                // Contador de request IDs
    SemaphoreHandle_t requestIdMutex;        // Mutex para requestIdCounter
    
    // Métodos privados para Queue y Mutex
    bool initQueueAndMutex();                // Inicializar Queue y Mutex
    void cleanupQueueAndMutex();              // Limpiar Queue y Mutex
    bool sendCommandToQueue(const WebCommand& cmd, uint32_t timeoutMs = 100);
    uint32_t generateRequestId();

    void setupUnifiedRoutes();
    void initSPIFFS();
    String getRelayName(int relay);
    bool shouldRefreshSlaveStates(); // ✅ Verificar si estados de slaves estão desatualizados

public:
    WebServerManager();
    ~WebServerManager();
    
    // Método principal para iniciar o painel admin (✅ MELHORIA #3: agora usa WebServerTask Core 1)
    void beginAdminServer(WiFiManager& wifiManager, HydroControl& hydroControl, WebServerTask* webTask, MasterSlaveManager* masterMgr = nullptr);
    void setMasterManager(MasterSlaveManager* masterMgr) { masterManager = masterMgr; }
    void update();
    bool isActive() { return isRunning; }
    
    // Configuração opcional (para uso futuro)
    void setupServer(SystemStatus& status, SensorData& sensors, bool* relayStates);
    void setupServer(float& temperature, float& ph, float& tds, bool* relayStates, std::function<void(int, int)> relayCallback);
    
    // Setters opcionais
    void setSystemStatus(SystemStatus* status) { systemStatus = status; }
    void setSensorData(SensorData* sensors) { sensorData = sensors; }
    void setRelayStates(bool* states) { relayStates = states; }
    
    // ✅ TÓPICO 1: MÉTODOS PÚBLICOS PARA QUEUE Y CACHE
    /**
     * @brief Recibe comando de la queue (llamar desde Core 0 en main.cpp)
     * @param cmd Variable para recibir el comando
     * @param timeoutMs Timeout en milisegundos (0 = no bloqueante)
     * @return true si se recibió un comando
     */
    bool receiveCommand(WebCommand& cmd, uint32_t timeoutMs = 0);
    
    /**
     * @brief Actualiza cache de datos del sistema (llamar desde Core 0)
     * @param cache Datos actualizados
     */
    void updateSystemCache(const SystemDataCache& cache);
    
    /**
     * @brief Obtiene cache de datos del sistema (llamar desde Core 1)
     * @return Cache de datos (copia thread-safe)
     */
    SystemDataCache getSystemCache();
};

#endif // WEB_SERVER_MANAGER_H