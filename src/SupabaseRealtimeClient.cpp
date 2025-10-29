#include "SupabaseRealtimeClient.h"
#include "DeviceID.h"

SupabaseRealtimeClient::SupabaseRealtimeClient() :
    currentState(SUPABASE_WS_DISCONNECTED),
    lastHeartbeat(0),
    lastReconnectAttempt(0),
    connectionStartTime(0),
    reconnectAttempts(0),
    refCounter(1),
    channelJoined(false) {
}

SupabaseRealtimeClient::~SupabaseRealtimeClient() {
    end();
}

bool SupabaseRealtimeClient::begin(const String& url, const String& key, const String& devId) {
    projectUrl = url;
    apiKey = key;
    deviceId = devId;
    
    if (WiFi.status() != WL_CONNECTED) {
        if (onError) onError("WiFi não conectado");
        return false;
    }
    
    Serial.println("🔌 Iniciando Supabase Realtime...");
    
    // Extrair host da URL (remover https://)
    String host = projectUrl;
    host.replace("https://", "");
    host.replace("http://", "");
    
    // URL WebSocket para Supabase Realtime
    String wsUrl = "/realtime/v1/websocket?apikey=" + apiKey + "&vsn=1.0.0";
    
    Serial.printf("🌐 Conectando a: %s%s\n", host.c_str(), wsUrl.c_str());
    
    // Configurar WebSocket
    webSocket.beginSSL(host, 443, wsUrl);
    webSocket.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
        this->handleWebSocketEvent(type, payload, length);
    });
    
    webSocket.setReconnectInterval(RECONNECT_DELAY);
    webSocket.enableHeartbeat(15000, 3000, 2); // Phoenix heartbeat
    
    currentState = SUPABASE_WS_CONNECTING;
    connectionStartTime = millis();
    
    return true;
}

void SupabaseRealtimeClient::loop() {
    if (currentState == SUPABASE_WS_DISCONNECTED) return;
    
    webSocket.loop();
    
    unsigned long now = millis();
    
    // Heartbeat periódico (Phoenix protocol)
    if (currentState == SUPABASE_WS_CONNECTED && now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
        sendHeartbeat();
        lastHeartbeat = now;
    }
    
    // Reconexão automática em caso de erro
    if (currentState == SUPABASE_WS_ERROR && now - lastReconnectAttempt >= RECONNECT_DELAY) {
        if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
            Serial.printf("🔄 Tentativa de reconexão %d/%d\n", reconnectAttempts + 1, MAX_RECONNECT_ATTEMPTS);
            
            webSocket.disconnect();
            delay(1000);
            
            String host = projectUrl;
            host.replace("https://", "");
            host.replace("http://", "");
            String wsUrl = "/realtime/v1/websocket?apikey=" + apiKey + "&vsn=1.0.0";
            
            webSocket.beginSSL(host, 443, wsUrl);
            currentState = SUPABASE_WS_CONNECTING;
            reconnectAttempts++;
            lastReconnectAttempt = now;
        } else {
            Serial.println("❌ Máximo de tentativas de reconexão atingido");
            currentState = SUPABASE_WS_DISCONNECTED;
        }
    }
}

void SupabaseRealtimeClient::end() {
    if (currentState != SUPABASE_WS_DISCONNECTED) {
        webSocket.disconnect();
        currentState = SUPABASE_WS_DISCONNECTED;
        Serial.println("🔌 Supabase Realtime desconectado");
    }
}

void SupabaseRealtimeClient::handleWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.println("❌ WebSocket desconectado");
            currentState = SUPABASE_WS_ERROR;
            channelJoined = false;
            break;
            
        case WStype_CONNECTED:
            Serial.printf("✅ WebSocket conectado a: %s\n", payload);
            currentState = SUPABASE_WS_CONNECTED;
            reconnectAttempts = 0;
            
            // Juntar-se ao canal do dispositivo
            joinChannel();
            break;
            
        case WStype_TEXT:
            handleIncomingMessage(String((char*)payload));
            break;
            
        case WStype_ERROR:
            Serial.printf("❌ Erro WebSocket: %s\n", payload);
            currentState = SUPABASE_WS_ERROR;
            if (onError) onError("Erro WebSocket: " + String((char*)payload));
            break;
            
        default:
            break;
    }
}

void SupabaseRealtimeClient::joinChannel() {
    // Canal específico para comandos do dispositivo
    channelTopic = "realtime:public:relay_commands:device_id=eq." + deviceId;
    
    DynamicJsonDocument doc(512);
    doc["topic"] = channelTopic;
    doc["event"] = "phx_join";
    doc["payload"] = JsonObject();
    doc["ref"] = String(refCounter++);
    
    String message;
    serializeJson(doc, message);
    
    Serial.printf("📡 Juntando ao canal: %s\n", channelTopic.c_str());
    webSocket.sendTXT(message);
}

void SupabaseRealtimeClient::sendHeartbeat() {
    DynamicJsonDocument doc(256);
    doc["topic"] = "phoenix";
    doc["event"] = "heartbeat";
    doc["payload"] = JsonObject();
    doc["ref"] = String(refCounter++);
    
    String message;
    serializeJson(doc, message);
    
    webSocket.sendTXT(message);
}

void SupabaseRealtimeClient::handleIncomingMessage(const String& message) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, message);
    
    if (error) {
        Serial.printf("❌ Erro ao parsear mensagem: %s\n", error.c_str());
        return;
    }
    
    String event = doc["event"];
    String topic = doc["topic"];
    
    if (event == "phx_reply" && topic == channelTopic) {
        String status = doc["payload"]["status"];
        if (status == "ok") {
            Serial.println("✅ Canal joined com sucesso");
            currentState = SUPABASE_WS_SUBSCRIBED;
            channelJoined = true;
        } else {
            Serial.printf("❌ Erro ao juntar canal: %s\n", doc["payload"]["response"].as<String>().c_str());
        }
    }
    else if (event == "postgres_changes" && channelJoined) {
        String eventType = doc["payload"]["eventType"];
        if (eventType == "INSERT") {
            // Novo comando de relé
            JsonObject newRecord = doc["payload"]["new"];
            processRelayCommand(newRecord);
        }
    }
    else if (event == "phx_error") {
        Serial.printf("❌ Erro Phoenix: %s\n", message.c_str());
        currentState = SUPABASE_WS_ERROR;
    }
}

void SupabaseRealtimeClient::processRelayCommand(const JsonObject& payload) {
    if (!onCommandReceived) return;
    
    String targetDeviceId = payload["device_id"];
    if (targetDeviceId != deviceId) {
        // Comando não é para este dispositivo
        return;
    }
    
    int relayNumber = payload["relay_number"];
    String action = payload["action"];
    int duration = payload["duration_seconds"] | 0;
    
    Serial.printf("📥 Comando WebSocket: Relé %d -> %s", relayNumber, action.c_str());
    if (duration > 0) {
        Serial.printf(" por %ds", duration);
    }
    Serial.println();
    
    // Chamar callback
    onCommandReceived(relayNumber, action, duration);
}

bool SupabaseRealtimeClient::sendDeviceStatus(const String& status) {
    if (!isConnected()) return false;
    
    DynamicJsonDocument doc(512);
    doc["topic"] = channelTopic;
    doc["event"] = "device_status_update";
    doc["payload"]["device_id"] = deviceId;
    doc["payload"]["status"] = status;
    doc["payload"]["timestamp"] = millis();
    doc["ref"] = String(refCounter++);
    
    String message;
    serializeJson(doc, message);
    
    return webSocket.sendTXT(message);
}

bool SupabaseRealtimeClient::sendHeartbeatPing() {
    if (!isConnected()) return false;
    
    DynamicJsonDocument doc(256);
    doc["topic"] = "realtime:device:" + deviceId;
    doc["event"] = "ping";
    doc["payload"]["device_id"] = deviceId;
    doc["payload"]["timestamp"] = millis();
    doc["ref"] = String(refCounter++);
    
    String message;
    serializeJson(doc, message);
    
    return webSocket.sendTXT(message);
}

String SupabaseRealtimeClient::getStateString() const {
    switch (currentState) {
        case SUPABASE_WS_DISCONNECTED: return "DESCONECTADO";
        case SUPABASE_WS_CONNECTING: return "CONECTANDO";
        case SUPABASE_WS_CONNECTED: return "CONECTADO";
        case SUPABASE_WS_SUBSCRIBED: return "INSCRITO";
        case SUPABASE_WS_ERROR: return "ERRO";
        default: return "DESCONHECIDO";
    }
}

unsigned long SupabaseRealtimeClient::getUptime() const {
    if (currentState == SUPABASE_WS_DISCONNECTED) return 0;
    return millis() - connectionStartTime;
}

void SupabaseRealtimeClient::printConnectionInfo() const {
    Serial.println("\n🔌 === SUPABASE REALTIME STATUS ===");
    Serial.println("📡 Estado: " + getStateString());
    Serial.println("⏰ Uptime: " + String(getUptime() / 1000) + "s");
    Serial.println("🔄 Tentativas reconexão: " + String(reconnectAttempts));
    Serial.print("📺 Canal: ");
    Serial.println(channelJoined ? "✅ Ativo" : "❌ Inativo");
    Serial.println("🆔 Device ID: " + deviceId);
    Serial.println("===================================\n");
}
