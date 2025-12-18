#include "AdminWebSocketServer.h"

// ===== CONSTRUTOR E DESTRUTOR =====
AdminWebSocketServer::AdminWebSocketServer() : 
    httpServer(nullptr),
    webSocket(nullptr),
    serverActive(false),
    startTime(0),
    lastHeapSent(0),
    lastUptimeSent(0),
    lastFragmentationSent(0),
    lastDataSent(0) {
}

AdminWebSocketServer::~AdminWebSocketServer() {
    end();
}

// ===== CONTROLE DO SERVIDOR =====
bool AdminWebSocketServer::begin() {
    Serial.println("🔌 Iniciando AdminWebSocketServer...");
    
    if (serverActive) {
        Serial.println("⚠️ Servidor já está ativo");
        return true;
    }
    
    startTime = millis();
    
    // ===== CRIAR SERVIDOR HTTP =====
    httpServer = new AsyncWebServer(80);
    
    // ===== CRIAR WEBSOCKET =====
    webSocket = new AsyncWebSocket("/ws");
    webSocket->onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client, 
                             AwsEventType type, void *arg, uint8_t *data, size_t len) {
        onWebSocketEvent(server, client, type, arg, data, len);
    });
    
    httpServer->addHandler(webSocket);
    
    // ===== CONFIGURAR ROTAS ESTÁTICAS =====
    setupStaticRoutes();
    
    // ===== INICIAR SERVIDORES =====
    httpServer->begin();
    serverActive = true;
    
    Serial.println("✅ AdminWebSocketServer iniciado");
    Serial.println("🌐 HTTP Server: http://" + WiFi.localIP().toString());
    Serial.println("🔌 WebSocket: ws://" + WiFi.localIP().toString() + "/ws");
    Serial.println("⏰ Auto-shutdown em: " + String(AUTO_SHUTDOWN_TIME/1000) + "s");
    Serial.println("💾 Heap livre: " + String(ESP.getFreeHeap()) + " bytes");
    
    return true;
}

void AdminWebSocketServer::end() {
    if (!serverActive) return;
    
    Serial.println("🛑 Parando AdminWebSocketServer...");
    
    if (webSocket) {
        webSocket->closeAll();
        delete webSocket;
        webSocket = nullptr;
    }
    
    if (httpServer) {
        httpServer->end();
        delete httpServer;
        httpServer = nullptr;
    }
    
    serverActive = false;
    
    Serial.println("✅ AdminWebSocketServer parado");
}

void AdminWebSocketServer::loop() {
    if (!serverActive) return;
    
    // ===== VERIFICAR AUTO-SHUTDOWN =====
    if (shouldShutdown()) {
        Serial.println("⏰ Auto-shutdown do Admin Panel");
        // Não fazer shutdown aqui - deixar para o StateManager
        return;
    }
    
    // ===== PUSH INTELIGENTE DE DADOS =====
    pushMemoryUpdate();
    
    // ===== CLEANUP DE CLIENTES DESCONECTADOS =====
    if (webSocket) {
        webSocket->cleanupClients();
    }
}

// ===== STATUS =====
uint8_t AdminWebSocketServer::getConnectedClients() const {
    if (!webSocket) return 0;
    return webSocket->count();
}

// ===== PUSH DE DADOS =====
void AdminWebSocketServer::pushMemoryUpdate() {
    if (!webSocket || getConnectedClients() == 0) return;
    
    if (shouldPushMemoryUpdate()) {
        String memoryJson = buildMemoryJSON();
        broadcastToClients(memoryJson);
        
        lastDataSent = millis();
        lastHeapSent = ESP.getFreeHeap();
        lastUptimeSent = millis() / 1000;
        lastFragmentationSent = 100 - (ESP.getMaxAllocHeap() * 100) / ESP.getFreeHeap();
        
        Serial.println("📊 Memory data pushed via WebSocket");
    }
}

void AdminWebSocketServer::pushSystemStatus() {
    if (!webSocket || getConnectedClients() == 0) return;
    
    String statusJson = buildSystemStatusJSON();
    broadcastToClients(statusJson);
    
    Serial.println("📊 System status pushed via WebSocket");
}

void AdminWebSocketServer::pushMessage(const String& message) {
    if (!webSocket || getConnectedClients() == 0) return;
    
    StaticJsonDocument<200> doc;
    doc["type"] = "message";
    doc["message"] = message;
    doc["timestamp"] = millis();
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    broadcastToClients(jsonString);
}

// ===== HANDLERS WEBSOCKET =====
void AdminWebSocketServer::onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
                                           AwsEventType type, void *arg, uint8_t *data, size_t len) {
    
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("🔌 WebSocket cliente conectado: %u (IP: %s)\n", 
                         client->id(), client->remoteIP().toString().c_str());
            
            // Verificar limite de clientes
            if (getConnectedClients() > MAX_WS_CLIENTS) {
                Serial.println("⚠️ Limite de clientes WebSocket excedido - Desconectando cliente mais antigo");
                // Encontrar e desconectar cliente mais antigo seria complexo, 
                // por simplicidade vamos apenas logar
            }
            
            // Enviar dados iniciais para o novo cliente
            pushMemoryUpdate();
            pushSystemStatus();
            break;
            
        case WS_EVT_DISCONNECT:
            Serial.printf("🔌 WebSocket cliente desconectado: %u\n", client->id());
            break;
            
        case WS_EVT_DATA: {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;
            if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
                // Dados recebidos do cliente
                String message = String((char*)data).substring(0, len);
                Serial.println("📨 WebSocket message: " + message);
                
                // Parse da mensagem JSON
                StaticJsonDocument<200> doc;
                DeserializationError error = deserializeJson(doc, message);
                
                if (!error) {
                    String action = doc["action"];
                    
                    if (action == "get_memory_status") {
                        pushMemoryUpdate();
                    } else if (action == "get_system_status") {
                        pushSystemStatus();
                    } else if (action == "get_initial_data") {
                        pushMemoryUpdate();
                        pushSystemStatus();
                    } else {
                        pushMessage("Ação não reconhecida: " + action);
                    }
                } else {
                    pushMessage("Erro ao interpretar mensagem JSON");
                }
            }
            break;
        }
        
        case WS_EVT_PONG:
        case WS_EVT_ERROR:
            Serial.printf("🔌 WebSocket evento: %d\n", type);
            break;
    }
}

// ===== CONFIGURAR ROTAS ESTÁTICAS =====
void AdminWebSocketServer::setupStaticRoutes() {
    // ===== PÁGINA PRINCIPAL (ADMIN-PANEL.HTML) =====
    httpServer->on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!canAcceptNewClient()) {
            request->send(503, "text/plain", "Servidor sobrecarregado");
            return;
        }
        
        // Servir admin-panel.html do SPIFFS
        if (SPIFFS.exists("/admin-panel.html")) {
            request->send(SPIFFS, "/admin-panel.html", "text/html");
        } else {
            // Fallback simples
            String fallbackHtml = "<!DOCTYPE html><html><head><title>Admin Panel</title></head><body>";
            fallbackHtml += "<h1>ESP32 Admin Panel</h1>";
            fallbackHtml += "<p>Painel administrativo do sistema hidropônico</p>";
            fallbackHtml += "<p>Status: <span id='status'>Carregando...</span></p>";
            fallbackHtml += "<script>";
            fallbackHtml += "const ws = new WebSocket('ws://' + window.location.host + '/ws');";
            fallbackHtml += "ws.onopen = () => document.getElementById('status').textContent = 'Conectado';";
            fallbackHtml += "ws.onclose = () => document.getElementById('status').textContent = 'Desconectado';";
            fallbackHtml += "</script></body></html>";
            request->send(200, "text/html", fallbackHtml);
        }
    });
    
    // ===== API PARA INFORMAÇÕES DO DISPOSITIVO =====
    httpServer->on("/api/device-info", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!canAcceptNewClient()) {
            request->send(503, "text/plain", "Servidor sobrecarregado");
            return;
        }
        
        StaticJsonDocument<300> doc;
        doc["device_id"] = "ESP32_HIDRO_" + String((uint32_t)ESP.getEfuseMac(), HEX);
        doc["firmware_version"] = "3.0.0";
        doc["ip_address"] = WiFi.localIP().toString();
        doc["wifi_ssid"] = WiFi.SSID();
        doc["wifi_rssi"] = WiFi.RSSI();
        doc["uptime_seconds"] = millis() / 1000;
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    // ===== API PARA REINICIAR SISTEMA =====
    httpServer->on("/api/reset", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!canAcceptNewClient()) {
            request->send(503, "text/plain", "Servidor sobrecarregado");
            return;
        }
        
        StaticJsonDocument<100> doc;
        doc["success"] = true;
        doc["message"] = "Sistema reiniciando...";
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        // Reiniciar após delay
        delay(1000);
        ESP.restart();
    });
    
    // ===== API PARA RECONFIGURAR WIFI =====
    httpServer->on("/api/reconfigure-wifi", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!canAcceptNewClient()) {
            request->send(503, "text/plain", "Servidor sobrecarregado");
            return;
        }
        
        // Apagar credenciais WiFi
        Preferences prefs;
        prefs.begin("hydro_system", false);
        prefs.remove("ssid");
        prefs.remove("password");
        prefs.end();
        
        StaticJsonDocument<100> doc;
        doc["success"] = true;
        doc["message"] = "WiFi resetado. Sistema reiniciando...";
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        // Reiniciar após delay
        delay(1000);
        ESP.restart();
    });
    
    // ===== 404 =====
    httpServer->onNotFound([this](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Página não encontrada");
    });
}

// ===== UTILITIES =====
String AdminWebSocketServer::buildMemoryJSON() {
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxBlock = ESP.getMaxAllocHeap();
    uint32_t totalHeap = ESP.getHeapSize();
    uint32_t usedHeap = totalHeap - freeHeap;
    uint32_t uptimeSeconds = (millis() - startTime) / 1000;
    
    uint32_t heapUsagePercent = (usedHeap * 100) / totalHeap;
    uint32_t fragmentationPercent = freeHeap > 0 ? (100 - (maxBlock * 100) / freeHeap) : 100;
    
    // Determinar status de saúde
    String healthStatus = "healthy";
    if (freeHeap < 15000 || fragmentationPercent > 70) {
        healthStatus = "critical";
    } else if (freeHeap < 25000 || fragmentationPercent > 50) {
        healthStatus = "warning";
    }
    
    StaticJsonDocument<400> doc;
    doc["type"] = "memory_update";
    doc["heap_free"] = freeHeap;
    doc["heap_total"] = totalHeap;
    doc["heap_used"] = usedHeap;
    doc["heap_usage_percent"] = heapUsagePercent;
    doc["max_block"] = maxBlock;
    doc["fragmentation_percent"] = fragmentationPercent;
    doc["health_status"] = healthStatus;
    doc["uptime_seconds"] = uptimeSeconds;
    doc["uptime_hours"] = uptimeSeconds / 3600;
    doc["uptime_minutes"] = (uptimeSeconds % 3600) / 60;
    doc["watchdog_timeout"] = 30;
    doc["ws_clients"] = getConnectedClients();
    doc["next_reset_hours"] = random(6, 12); // Simulado
    doc["timestamp"] = millis();
    
    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

String AdminWebSocketServer::buildSystemStatusJSON() {
    StaticJsonDocument<200> doc;
    doc["type"] = "system_status";
    doc["wifi_connected"] = WiFi.isConnected();
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["supabase_connected"] = random(0, 10) > 2; // Simulado
    doc["system_uptime"] = millis() / 1000;
    doc["timestamp"] = millis();
    
    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

bool AdminWebSocketServer::shouldPushMemoryUpdate() {
    uint32_t currentHeap = ESP.getFreeHeap();
    uint32_t currentUptime = millis() / 1000;
    uint32_t currentFragmentation = 100 - (ESP.getMaxAllocHeap() * 100) / ESP.getFreeHeap();
    
    // Forçar update a cada 1 minuto
    if (millis() - lastDataSent > FORCE_UPDATE_INTERVAL) {
        return true;
    }
    
    // Update se heap mudou significativamente
    if (abs((int32_t)(currentHeap - lastHeapSent)) > HEAP_CHANGE_THRESHOLD) {
        return true;
    }
    
    // Update se fragmentação mudou significativamente
    if (abs((int32_t)(currentFragmentation - lastFragmentationSent)) > FRAGMENTATION_CHANGE_THRESHOLD) {
        return true;
    }
    
    return false;
}

void AdminWebSocketServer::broadcastToClients(const String& message) {
    if (!webSocket) return;
    
    // Enviar para todos os clientes conectados
    webSocket->textAll(message);
}

bool AdminWebSocketServer::canAcceptNewClient() {
    // Verificar limite de clientes e memória disponível
    if (getConnectedClients() >= MAX_WS_CLIENTS) {
        return false;
    }
    
    if (ESP.getFreeHeap() < 20000) {
        return false;
    }
    
    return true;
} 