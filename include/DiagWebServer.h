#ifndef DIAG_WEB_SERVER_H
#define DIAG_WEB_SERVER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "DataTypes.h"

class DiagWebServer {
private:
    AsyncWebServer* server;
    bool isRunning;
    SystemStatus* systemStatus;
    SensorData* sensorData;
    bool* relayStates;
    
    // Funções auxiliares
    void initSPIFFS();
    void setupStaticFiles();
    void setupAPIEndpoints();
    void setupConfigEndpoints();
    
    // Handlers de API
    void handleStatus(AsyncWebServerRequest *request);
    void handleSensors(AsyncWebServerRequest *request);
    void handleRelays(AsyncWebServerRequest *request);
    void handleWiFiConfig(AsyncWebServerRequest *request);
    void handleAPIConfig(AsyncWebServerRequest *request);

public:
    DiagWebServer();
    ~DiagWebServer();
    
    void begin();
    void update();
    bool isActive() { return isRunning; }
    
    void setSystemStatus(SystemStatus* status) { systemStatus = status; }
    void setSensorData(SensorData* sensors) { sensorData = sensors; }
    void setRelayStates(bool* states) { relayStates = states; }
};

#endif // DIAG_WEB_SERVER_H 