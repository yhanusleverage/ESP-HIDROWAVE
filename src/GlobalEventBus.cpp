#include "GlobalEventBus.h"
#include "ESPNowController.h"
#include <ArduinoJson.h>

GlobalEventBus* globalEventBus = nullptr;

GlobalEventBus::GlobalEventBus() : 
    masterManager(nullptr) {
    // ⏸️ TEMPORALMENTE DESHABILITADO: webSocket(nullptr)
}

// ⏸️ TEMPORALMENTE: Sin WebSocket
void GlobalEventBus::begin(MasterSlaveManager* masterManager) {
    this->masterManager = masterManager;
    // ⏸️ TEMPORALMENTE DESHABILITADO: this->webSocket = webSocket;
    
    Serial.println("✅ GlobalEventBus inicializado (sin WebSocket - modo básico)");
    Serial.println("   📡 Eventos locales activos");
    Serial.println("   ⏸️ WebSocket deshabilitado temporalmente");
}

void GlobalEventBus::subscribe(GlobalEventType eventType, EventCallback callback) {
    listeners[eventType].push_back(callback);
    Serial.printf("✅ Subscriber registrado para evento: %d\n", (int)eventType);
}

void GlobalEventBus::publish(GlobalEventType eventType, const JsonObject& payload, const String& source) {
    GlobalEvent event;
    event.type = eventType;
    event.payload = payload;
    event.timestamp = millis();
    event.source = source;
    
    // ✅ NOTIFICAR: Todos os subscribers locais
    if (listeners.find(eventType) != listeners.end()) {
        for (auto& callback : listeners[eventType]) {
            callback(event);
        }
    }
    
    // ✅ BROADCAST: Via WebSocket para frontend
    broadcastToWebSocket(event);
}

void GlobalEventBus::broadcastToWebSocket(const GlobalEvent& event) {
    // ⏸️ TEMPORALMENTE DESHABILITADO: WebSocket removido
    // if (!webSocket) return;
    // webSocket->textAll(json);
    // TODO: Implementar cuando WebSocket esté listo
}

void GlobalEventBus::processWebSocketMessage(const String& message) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, message);
    
    if (error) {
        Serial.printf("❌ Erro ao processar mensagem WebSocket: %s\n", error.c_str());
        return;
    }
    
    int eventTypeInt = doc["type"];
    GlobalEventType eventType = (GlobalEventType)eventTypeInt;
    JsonObject payload = doc["payload"];
    
    Serial.printf("📥 Evento recebido via WebSocket: %d\n", eventTypeInt);
    
    switch (eventType) {
        case GlobalEventType::RELAY_COMMAND:
            processRelayCommand(payload);
            break;
            
        case GlobalEventType::SENSOR_READ_REQUEST:
            // Processar requisição de leitura de sensores
            break;
            
        case GlobalEventType::CONFIG_UPDATE:
            // Processar atualização de configuração
            break;
            
        default:
            Serial.printf("⚠️ Tipo de evento desconhecido: %d\n", eventTypeInt);
            break;
    }
}

void GlobalEventBus::processRelayCommand(const JsonObject& payload) {
    String slaveMac = payload["slave_mac"];
    int relayNumber = payload["relay_number"];
    String action = payload["action"];
    
    Serial.printf("⚡ [EventBus] Processando comando de relé: Slave=%s, Relé=%d, Ação=%s\n", 
                  slaveMac.c_str(), relayNumber, action.c_str());
    
    // ✅ CONVERTER: MAC string para uint8_t[6]
    uint8_t mac[6];
    // Função auxiliar para converter MAC string para array
    // Ex: "14:33:5C:38:BF:60" → [0x14, 0x33, 0x5C, 0x38, 0xBF, 0x60]
    sscanf(slaveMac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
    
    // ✅ ENVIAR: Comando via ESP-NOW
    if (masterManager) {
        bool success = masterManager->sendRelayCommandToSlave(
            mac, relayNumber, action, 0, 0, true
        );
        
        // ✅ BROADCAST: Estado optimista imediatamente
        DynamicJsonDocument eventDoc(256);
        eventDoc["slave_mac"] = slaveMac;
        eventDoc["relay_number"] = relayNumber;
        eventDoc["state"] = (action == "on");
        eventDoc["source"] = "manual";
        
        publish(GlobalEventType::RELAY_STATE_CHANGED, eventDoc.as<JsonObject>(), "frontend");
    }
}

void GlobalEventBus::loop() {
    // ✅ LOOP PRIMÁRIO: Processar eventos pendentes
    // Por enquanto, eventos são processados imediatamente
    // Futuro: Fila de eventos para processamento assíncrono
}

