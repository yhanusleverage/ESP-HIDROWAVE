#ifndef HYDRO_STATE_MANAGER_H
#define HYDRO_STATE_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "WiFiConfigServer.h"
#include "HydroSupaManager.h"
#include "HydroSystemCore.h"
#include "AdminWebSocketServer.h"

// Forward declarations
class WebServerTask;
class ESPNowController;
class MasterSlaveManager;

// ===== ESTADOS DO SISTEMA =====
enum HydroSystemState {
    WIFI_CONFIG_MODE,    // Modo AP: configuração WiFi apenas
    HYDRO_ACTIVE_MODE,   // Modo produção: sistema hidropônico puro
    ADMIN_PANEL_MODE     // Modo debug: painel WebSocket temporário
};

enum WifiReconnectPhase {
    WIFI_RECONNECT_IDLE = 0,
    WIFI_RECONNECT_IN_PROGRESS,
    WIFI_RECONNECT_COOLDOWN
};

class HydroStateManager {
private:
    HydroSystemState currentState;
    unsigned long stateStartTime;
    
    // Módulos especializados (apenas um ativo por vez)
    WiFiConfigServer* wifiServer;
    HydroSystemCore* hydroCore;
    AdminWebSocketServer* adminServer;
    
    // ✅ INYECCIÓN DE DEPENDENCIAS: Referencias externas
    WebServerTask* webServerTask;
    ESPNowController* espNowController;
    MasterSlaveManager* masterManager;  // ✅ NOVO: Para evitar uso de extern
    
    Preferences preferences;
    String deviceID;

    WifiReconnectPhase wifiReconnectPhase;
    unsigned long wifiReconnectStartedMs;
    unsigned long wifiLastAttemptMs;
    uint8_t wifiReconnectAttempts;
    
    // Timeouts
    static const unsigned long WIFI_CONFIG_TIMEOUT = 600000;  // 10 min (mais tempo para configurar)
    static const unsigned long ADMIN_PANEL_TIMEOUT = 300000;  // 5 min
    
public:
    HydroStateManager();
    ~HydroStateManager();
    
    // ✅ Setters para configurar dependências externas (Injeção de Dependências)
    void setWebServerTask(WebServerTask* webTask) { webServerTask = webTask; }
    void setESPNowController(ESPNowController* espNow) { espNowController = espNow; }
    void setMasterManager(MasterSlaveManager* masterMgr);  // late-bind após ESP-NOW init
    
    // Controle de estados
    void begin();
    void loop();
    
    // Transições de estado
    void switchToWiFiConfig();
    void switchToHydroActive();
    void switchToAdminPanel();
    
    // Utilities
    HydroSystemState getCurrentState() const { return currentState; }
    String getStateString() const;
    unsigned long getStateUptime() const { return millis() - stateStartTime; }
    
    // Acesso ao HydroSystemCore (para comandos ESP-NOW)
    HydroSystemCore& getHydroSystemCore() { 
        if (!hydroCore) {
            Serial.println("❌ ERRO: HydroSystemCore não inicializado!");
        }
        return *hydroCore; 
    }
    
    // Comandos seriais
    void handleSerialCommand(const String& command);
    
    // ESP-NOW Status
    void printESPNowStatus();

    void dumpWifiReconnectStatus(Stream& out) const;
    
private:
    void cleanup();
    bool hasWiFiCredentials();
    void autoSwitchIfNeeded();
    void handleWifiReconnectRuntime(unsigned long now);
    void startWifiReconnectAttempt(unsigned long now);
    void resetWifiReconnectState();
    void armWifiStation(const String& ssid, const String& password);
    static const char* wifiStatusString(wl_status_t status);
    String getDeviceID();
};

#endif // HYDRO_STATE_MANAGER_H 