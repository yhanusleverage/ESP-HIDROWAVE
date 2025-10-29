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

class WebServerManager {
private:
    AsyncWebServer* server;
    AsyncWebServer* adminServer;
    bool isRunning;
    SystemStatus* systemStatus;
    SensorData* sensorData;
    bool* relayStates;
    static const int NUM_RELAYS = 16;

    float* tempRef;
    float* phRef;
    float* tdsRef;
    std::function<void(int, int)> onRelayToggle;

    void setupUnifiedRoutes();
    void initSPIFFS();
    String getRelayName(int relay);

public:
    WebServerManager();
    ~WebServerManager();
    
    // Método principal para iniciar o painel admin
    void beginAdminServer(WiFiManager& wifiManager, HydroControl& hydroControl);
    
    void begin();
    void update();
    bool isActive() { return isRunning; }
    
    // Configuração opcional (para uso futuro)
    void setupServer(SystemStatus& status, SensorData& sensors, bool* relayStates);
    void setupServer(float& temperature, float& ph, float& tds, bool* relayStates, std::function<void(int, int)> relayCallback);
    
    // Setters opcionais
    void setSystemStatus(SystemStatus* status) { systemStatus = status; }
    void setSensorData(SensorData* sensors) { sensorData = sensors; }
    void setRelayStates(bool* states) { relayStates = states; }
};

#endif // WEB_SERVER_MANAGER_H