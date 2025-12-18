#ifndef SUPABASE_REALTIME_CLIENT_H
#define SUPABASE_REALTIME_CLIENT_H

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "Config.h"

// Estados da conexão WebSocket
enum SupabaseWebSocketState {
    SUPABASE_WS_DISCONNECTED,
    SUPABASE_WS_CONNECTING,
    SUPABASE_WS_CONNECTED,
    SUPABASE_WS_SUBSCRIBED,
    SUPABASE_WS_ERROR
};

// Callback para comandos recebidos
typedef std::function<void(int relayNumber, String action, int duration)> CommandCallback;
typedef std::function<void(String message)> ErrorCallback;

class SupabaseRealtimeClient {
private:
    WebSocketsClient webSocket;
    String projectUrl;
    String apiKey;
    String deviceId;
    
    SupabaseWebSocketState currentState;
    unsigned long lastHeartbeat;
    unsigned long lastReconnectAttempt;
    unsigned long connectionStartTime;
    int reconnectAttempts;
    
    CommandCallback onCommandReceived;
    ErrorCallback onError;
    
    // Phoenix Channel protocol
    int refCounter;
    String channelTopic;
    bool channelJoined;
    
    // Controle de heartbeat
    static const unsigned long HEARTBEAT_INTERVAL = 30000; // 30s
    static const unsigned long RECONNECT_DELAY = 5000;     // 5s
    static const int MAX_RECONNECT_ATTEMPTS = 5;
    
    void handleWebSocketEvent(WStype_t type, uint8_t* payload, size_t length);
    void sendPhoenixMessage(const String& topic, const String& event, const JsonObject& payload);
    void joinChannel();
    void sendHeartbeat();
    void handleIncomingMessage(const String& message);
    void processRelayCommand(const JsonObject& payload);
    
public:
    SupabaseRealtimeClient();
    ~SupabaseRealtimeClient();
    
    // Configuração e controle
    bool begin(const String& projectUrl, const String& apiKey, const String& deviceId);
    void loop();
    void end();
    
    // Callbacks
    void setCommandCallback(CommandCallback callback) { onCommandReceived = callback; }
    void setErrorCallback(ErrorCallback callback) { onError = callback; }
    
    // Status
    bool isConnected() const { return currentState == SUPABASE_WS_CONNECTED || currentState == SUPABASE_WS_SUBSCRIBED; }
    bool isSubscribed() const { return currentState == SUPABASE_WS_SUBSCRIBED; }
    SupabaseWebSocketState getState() const { return currentState; }
    String getStateString() const;
    unsigned long getUptime() const;
    
    // Comunicação
    bool sendDeviceStatus(const String& status);
    bool sendHeartbeatPing();
    
    // Diagnóstico
    void printConnectionInfo() const;
    int getReconnectAttempts() const { return reconnectAttempts; }
};

#endif // SUPABASE_REALTIME_CLIENT_H
