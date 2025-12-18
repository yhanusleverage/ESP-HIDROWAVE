#ifndef GLOBAL_EVENT_BUS_H
#define GLOBAL_EVENT_BUS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include <vector>
#include <map>
#include "MasterSlaveManager.h"

// ✅ TIPOS DE EVENTOS GLOBAIS
enum class GlobalEventType {
    // Comandos (Frontend → ESP32)
    RELAY_COMMAND = 0,
    SENSOR_READ_REQUEST,
    CONFIG_UPDATE,
    
    // Estados (ESP32 → Frontend)
    RELAY_STATE_CHANGED,
    SENSOR_DATA_UPDATED,
    SLAVE_STATUS_CHANGED,
    
    // Sistema
    SYSTEM_STATUS,
    ERROR,
    ACK,
    DECISION_EXECUTED,  // ✅ NOVO: Decisão automática executada
};

// ✅ ESTRUTURA DE EVENTO
struct GlobalEvent {
    GlobalEventType type;
    JsonObject payload;
    unsigned long timestamp;
    String source;  // "frontend" | "esp32" | "decision_engine"
};

// ✅ CALLBACK TIPO
typedef std::function<void(const GlobalEvent&)> EventCallback;

class GlobalEventBus {
private:
    std::map<GlobalEventType, std::vector<EventCallback>> listeners;
    MasterSlaveManager* masterManager;
    // ⏸️ TEMPORALMENTE DESHABILITADO: AsyncWebSocket* webSocket;
    
    void broadcastToWebSocket(const GlobalEvent& event);
    void processRelayCommand(const JsonObject& payload);
    
public:
    GlobalEventBus();
    
    // ⏸️ TEMPORALMENTE: Sin WebSocket
    void begin(MasterSlaveManager* masterManager);
    
    // ✅ SUBSCRIBER: Escutar eventos
    void subscribe(GlobalEventType eventType, EventCallback callback);
    
    // ✅ PUBLISHER: Publicar eventos
    void publish(GlobalEventType eventType, const JsonObject& payload, const String& source = "esp32");
    
    // ✅ PROCESSAR: Eventos recebidos via WebSocket
    void processWebSocketMessage(const String& message);
    
    // ✅ LOOP: Processar eventos pendentes
    void loop();
};

// ✅ INSTÂNCIA GLOBAL
extern GlobalEventBus* globalEventBus;

#endif

