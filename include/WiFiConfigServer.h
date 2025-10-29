#ifndef WIFI_CONFIG_SERVER_H
#define WIFI_CONFIG_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

class WiFiConfigServer {
private:
    AsyncWebServer* server;
    bool serverActive;
    unsigned long startTime;
    unsigned int activeConnections;
    unsigned long lastConnectionCheck;
    
    // Configuração AP
    IPAddress AP_IP, AP_GATEWAY, AP_SUBNET;
    String deviceID;
    
    // Proteção contra sobrecarga
    bool checkConnectionLimit(AsyncWebServerRequest *request);
    
    // Debug
    void printDebug(const String& message);
    
public:
    WiFiConfigServer();
    ~WiFiConfigServer();
    
    // Controle do servidor
    bool begin();
    void end();
    void loop();
    
    // Status
    bool isActive() const { return serverActive; }
    unsigned long getUptime() const { return millis() - startTime; }
    String getAPIP() const { return AP_IP.toString(); }
    unsigned int getActiveConnections() const { return activeConnections; }
    
    // Callback para sucesso na configuração
    void onWiFiConfigured(std::function<void()> callback);
    
    // ✅ NOVO: Callback para registro com email
    void onEmailRegistered(std::function<void(String)> callback);
    
private:
    std::function<void()> onConfiguredCallback;
    std::function<void(String)> onEmailCallback; // ✅ NOVO: Callback email
    
    // Métodos utilitários
    String urlDecode(String str);
    unsigned char h2int(char c);
};

#endif // WIFI_CONFIG_SERVER_H 