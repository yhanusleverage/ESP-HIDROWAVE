#ifndef ADMIN_WEBSOCKET_SERVER_H
#define ADMIN_WEBSOCKET_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

class AdminWebSocketServer {
private:
    AsyncWebServer* httpServer;
    AsyncWebSocket* webSocket;
    bool serverActive;
    unsigned long startTime;
    
    // Dados para push inteligente
    uint32_t lastHeapSent;
    uint32_t lastUptimeSent;
    uint32_t lastFragmentationSent;
    unsigned long lastDataSent;
    
    // Configurações de push
    static const uint32_t HEAP_CHANGE_THRESHOLD = 5000;      // 5KB mudança
    static const uint32_t FRAGMENTATION_CHANGE_THRESHOLD = 5; // 5% mudança
    static const unsigned long FORCE_UPDATE_INTERVAL = 60000; // 1 min forçado
    
    // Limites
    static const unsigned long AUTO_SHUTDOWN_TIME = 300000;   // 5 min
    static const uint8_t MAX_WS_CLIENTS = 2;                  // Máximo 2 clientes WS
    
public:
    AdminWebSocketServer();
    ~AdminWebSocketServer();
    
    // Controle do servidor
    bool begin();
    void end();
    void loop();
    
    // Status
    bool isActive() const { return serverActive; }
    unsigned long getUptime() const { return millis() - startTime; }
    bool shouldShutdown() const { return (millis() - startTime) > AUTO_SHUTDOWN_TIME; }
    uint8_t getConnectedClients() const;
    
    // Push de dados
    void pushMemoryUpdate();
    void pushSystemStatus();
    void pushMessage(const String& message);
    
private:
    // Handlers WebSocket
    void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
                         AwsEventType type, void *arg, uint8_t *data, size_t len);
    
    // Handlers HTTP para arquivos estáticos
    void setupStaticRoutes();
    
    // Utilities
    String buildMemoryJSON();
    String buildSystemStatusJSON();
    bool shouldPushMemoryUpdate();
    void broadcastToClients(const String& message);
    
    // Proteção
    bool canAcceptNewClient();
};

#endif // ADMIN_WEBSOCKET_SERVER_H 