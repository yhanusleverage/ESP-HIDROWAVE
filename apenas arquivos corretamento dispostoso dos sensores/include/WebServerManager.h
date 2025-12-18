#ifndef WEBSERVERMANAGER_H
#define WEBSERVERMANAGER_H

#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>

class WebServerManager {
public:
    WebServerManager();
    void begin();
    void setupServer(
        float& currentTemp,
        float& currentPH,
        float& currentTDS,
        bool* relayStates,
        void (*callback)(int, int)
    );
    
private:
    AsyncWebServer server;
    void (*relayCallback)(int, int);
    void initSPIFFS();
};

#endif