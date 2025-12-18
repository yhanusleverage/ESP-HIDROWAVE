#ifndef THINGSPEAKMANAGER_H
#define THINGSPEAKMANAGER_H

#include <ThingSpeak.h>
#include <WiFi.h>

class ThingSpeakManager {
public:
    ThingSpeakManager();
    void begin();
    void sendData(float temperature, float ph, float ec);

private:
    static const unsigned long CHANNEL_ID;  // Substituir pelo seu Channel ID
    static const char* WRITE_API_KEY;  // Sua Write API Key
    static const unsigned long UPDATE_INTERVAL = 15000;  // 15 segundos entre atualizações
    unsigned long lastUpdate;
    WiFiClient client;
};

#endif
