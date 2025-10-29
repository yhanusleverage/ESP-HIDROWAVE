#ifndef HYDRO_STATE_MANAGER_H
#define HYDRO_STATE_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "WiFiConfigServer.h"
#include "HydroSupaManager.h"
#include "HydroSystemCore.h"
#include "AdminWebSocketServer.h"

// ===== ESTADOS DO SISTEMA =====
enum HydroSystemState {
    WIFI_CONFIG_MODE,    // Modo AP: configuração WiFi apenas
    HYDRO_ACTIVE_MODE,   // Modo produção: sistema hidropônico puro
    ADMIN_PANEL_MODE     // Modo debug: painel WebSocket temporário
};

class HydroStateManager {
private:
    HydroSystemState currentState;
    unsigned long stateStartTime;
    
    // Módulos especializados (apenas um ativo por vez)
    WiFiConfigServer* wifiServer;
    HydroSystemCore* hydroCore;
    AdminWebSocketServer* adminServer;
    
    Preferences preferences;
    String deviceID;
    
    // Timeouts
    static const unsigned long WIFI_CONFIG_TIMEOUT = 600000;  // 10 min (mais tempo para configurar)
    static const unsigned long ADMIN_PANEL_TIMEOUT = 300000;  // 5 min
    
public:
    HydroStateManager();
    ~HydroStateManager();
    
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
    
private:
    void cleanup();
    bool hasWiFiCredentials();
    void autoSwitchIfNeeded();
    String getDeviceID();
};

#endif // HYDRO_STATE_MANAGER_H 