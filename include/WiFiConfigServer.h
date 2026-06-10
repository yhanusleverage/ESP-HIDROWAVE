#ifndef WIFI_CONFIG_SERVER_H
#define WIFI_CONFIG_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>  // ✅ ESP-DNS (mDNS) para nombres legibles en setup

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
    bool mDNSActive;  // ✅ Estado de mDNS
    
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
    
    // ✅ ESP-DNS (mDNS) - Solo para setup inicial
    bool startMDNS(const String& hostname = "");
    void stopMDNS();
    
    // Status
    bool isActive() const { return serverActive; }
    unsigned long getUptime() const { return millis() - startTime; }
    String getAPIP() const { return AP_IP.toString(); }
    unsigned int getActiveConnections() const { return activeConnections; }
    
    // Callback para sucesso na configuração
    void onWiFiConfigured(std::function<void()> callback);
    
    // Callback: email + nome do dispositivo (portal) + localização
    void onEmailRegistered(std::function<void(String email, String deviceName, String location)> callback);
    
private:
    std::function<void()> onConfiguredCallback;
    std::function<void(String, String, String)> onEmailCallback;
    
    // Métodos utilitários
    String urlDecode(String str);
    unsigned char h2int(char c);
};

#endif // WIFI_CONFIG_SERVER_H 