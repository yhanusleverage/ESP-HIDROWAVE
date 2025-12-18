#ifndef HYDRO_STATE_MANAGER_H
#define HYDRO_STATE_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include "Config.h"
#include "WiFiConfigServer.h"
#include "WebServerManager.h"
#include "AdminWebSocketServer.h"
#include "HydroSupaManager.h"

// Estados do sistema hidropônico
enum HydroSystemState {
    WIFI_CONFIG_MODE,     // Configuração inicial WiFi
    HYDRO_ACTIVE_MODE,    // Sistema hidropônico ativo
    ADMIN_PANEL_MODE,     // Painel administrativo
    DIAGNOSTIC_MODE,      // Modo diagnóstico
    MAINTENANCE_MODE      // Modo manutenção
};

class HydroStateManager {
private:
    HydroSystemState currentState;
    HydroSystemState previousState;
    unsigned long stateChangeTime;
    
    // Componentes do sistema
    WiFiConfigServer wifiConfigServer;
    WebServerManager webServerManager;
    AdminWebSocketServer adminWebSocket;
    
    // Flags de controle
    bool stateChanged;
    bool forceStateChange;
    
    // Métodos privados de transição
    void enterWiFiConfigMode();
    void enterHydroActiveMode();
    void enterAdminPanelMode();
    void enterDiagnosticMode();
    void enterMaintenanceMode();
    
    void exitCurrentState();
    bool validateStateTransition(HydroSystemState newState);
    
public:
    HydroStateManager();
    ~HydroStateManager();
    
    // Inicialização
    bool begin();
    
    // Loop principal
    void loop();
    
    // Controle de estados
    bool setState(HydroSystemState newState, bool force = false);
    HydroSystemState getState() const { return currentState; }
    HydroSystemState getPreviousState() const { return previousState; }
    
    // Informações de estado
    bool hasStateChanged() const { return stateChanged; }
    unsigned long getTimeInCurrentState() const { return millis() - stateChangeTime; }
    String getStateString() const;
    String getStateString(HydroSystemState state) const;
    String getStateDescription() const;
    
    // Utilitários
    void resetStateFlags() { stateChanged = false; }
    bool isInConfigMode() const { return currentState == WIFI_CONFIG_MODE; }
    bool isInActiveMode() const { return currentState == HYDRO_ACTIVE_MODE; }
    bool isInAdminMode() const { return currentState == ADMIN_PANEL_MODE; }
};

#endif // HYDRO_STATE_MANAGER_H
