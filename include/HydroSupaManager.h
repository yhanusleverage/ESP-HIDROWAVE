#ifndef HYDRO_SUPA_MANAGER_H
#define HYDRO_SUPA_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include "SupabaseClient.h"
#include "SupabaseRealtimeClient.h"
#include "Config.h"

/**
 * HydroSupaManager - Sistema Híbrido de Comunicação
 * 
 * Combina HTTP e WebSocket para comunicação otimizada:
 * - HTTP: Dados periódicos, status, heartbeat
 * - WebSocket: Comandos em tempo real, notificações
 * - Fallback: HTTP polling se WebSocket falhar
 * 
 * Características:
 * - Auto-detecção de problemas de memória
 * - Reconexão automática
 * - Fallback inteligente
 * - Proteção contra falhas
 */
class HydroSupaManager {
private:
    // Clientes de comunicação
    SupabaseClient httpClient;
    SupabaseRealtimeClient realtimeClient;
    
    // Configuração
    String baseUrl;
    String apiKey;
    String deviceId;
    bool isInitialized;
    
    // Estado do sistema
    bool useWebSocket;
    unsigned long lastHttpSend;
    unsigned long lastStatusUpdate;
    unsigned long lastCommandCheck;
    unsigned long lastWebSocketRetry;
    
    // Controle de falhas
    int httpFailures;
    int wsFailures;
    
    // Constantes
    static const unsigned long HTTP_SEND_INTERVAL = 30000;    // 30s - dados via HTTP
    static const unsigned long STATUS_UPDATE_INTERVAL = 60000; // 60s - status completo
    static const unsigned long HTTP_POLL_INTERVAL = 10000;    // 10s - polling HTTP (fallback)
    static const unsigned long WS_RETRY_INTERVAL = 120000;    // 2min - tentar reativar WS
    
    static const int MAX_HTTP_FAILURES = 5;
    static const int MAX_WS_FAILURES = 3;
    
    // Métodos privados
    bool initializeWebSocket();
    void sendHttpData();
    void updateDeviceStatus();
    void checkHttpCommands();
    void handleWebSocketCommand(int relay, String action, int duration);
    void handleWebSocketError(String error);
    void processCommand(const RelayCommand& cmd, const String& source);
    bool executeRelayCommand(int relay, const String& action, int duration);
    
public:
    HydroSupaManager();
    ~HydroSupaManager();
    
    // Controle principal
    bool begin(const String& url, const String& key);
    void loop();
    void end();
    
    // Status e diagnóstico
    bool isReady() const { return isInitialized && httpClient.isReady(); }
    bool isWebSocketActive() const { return useWebSocket && realtimeClient.isConnected(); }
    bool isHttpActive() const { return httpClient.isReady(); }
    
    void printStatus();
    
    // Acesso aos clientes (para casos especiais)
    SupabaseClient& getHttpClient() { return httpClient; }
    const SupabaseClient& getHttpClient() const { return httpClient; }
    SupabaseRealtimeClient& getRealtimeClient() { return realtimeClient; }
    const SupabaseRealtimeClient& getRealtimeClient() const { return realtimeClient; }
    
    // Controle manual
    void forceWebSocketReconnect();
    void disableWebSocket() { useWebSocket = false; realtimeClient.end(); }
    void enableWebSocket() { if (!useWebSocket) initializeWebSocket(); }
};

#endif // HYBRID_SUPABASE_MANAGER_H
