#include "DiagWebServer.h"

DiagWebServer::DiagWebServer() {
    server = new AsyncWebServer(80);
    isRunning = false;
    systemStatus = nullptr;
    sensorData = nullptr;
    relayStates = nullptr;
}

DiagWebServer::~DiagWebServer() {
    if (server) {
        delete server;
        server = nullptr;
    }
}

void DiagWebServer::begin() {
    if (!SPIFFS.begin(true)) {
        Serial.println("Erro ao montar SPIFFS");
        return;
    }
    
    // Configurar endpoints
    setupStaticFiles();
    setupAPIEndpoints();
    setupConfigEndpoints();
    
    server->begin();
    isRunning = true;
}

void DiagWebServer::update() {
    // Nada a fazer por enquanto
}

void DiagWebServer::setupStaticFiles() {
    server->serveStatic("/", SPIFFS, "/");
    
    server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/index.html", "text/html");
    });
}

void DiagWebServer::setupAPIEndpoints() {
    server->on("/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!systemStatus) {
            request->send(503, "application/json", "{\"error\":\"Status não disponível\"}");
            return;
        }
        
        StaticJsonDocument<512> doc;
        
        doc["wifi"]["connected"] = systemStatus->wifiConnected;
        doc["wifi"]["rssi"] = systemStatus->wifiRSSI;
        doc["api"]["connected"] = systemStatus->apiConnected;
        doc["system"]["uptime"] = systemStatus->uptime;
        doc["system"]["freeHeap"] = systemStatus->freeHeap;
        doc["system"]["lastError"] = systemStatus->lastError;
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    server->on("/sensors", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!sensorData) {
            request->send(503, "application/json", "{\"error\":\"Dados dos sensores não disponíveis\"}");
            return;
        }
        
        StaticJsonDocument<512> doc;
        
        doc["environment"]["temperature"] = sensorData->environmentTemp;
        doc["environment"]["humidity"] = sensorData->environmentHumidity;
        doc["water"]["temperature"] = sensorData->waterTemp;
        doc["water"]["ph"] = sensorData->ph;
        doc["water"]["tds"] = sensorData->tds;
        doc["water"]["level"] = sensorData->waterLevelOk;
        doc["timestamp"] = sensorData->timestamp;
        doc["valid"] = sensorData->valid;
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    server->on("/relays", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!relayStates) {
            request->send(503, "application/json", "{\"error\":\"Estado dos relés não disponível\"}");
            return;
        }
        
        StaticJsonDocument<1024> doc;
        JsonArray relays = doc.createNestedArray("relays");
        
        for (int i = 0; i < 8; i++) {
            JsonObject relay = relays.createNestedObject();
            relay["id"] = i;
            relay["name"] = RELAY_NAMES[i];
            relay["state"] = relayStates[i];
            relay["config"]["auto_mode"] = RELAY_CONFIGS[i].autoMode;
            relay["config"]["max_duration"] = RELAY_CONFIGS[i].maxDuration;
            relay["config"]["safety_lock"] = RELAY_CONFIGS[i].safetyLock;
        }
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
}

void DiagWebServer::setupConfigEndpoints() {
    server->on("/config/wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("ssid", true) || !request->hasParam("password", true)) {
            request->send(400, "application/json", "{\"error\":\"Parâmetros inválidos\"}");
            return;
        }
        
        String ssid = request->getParam("ssid", true)->value();
        String password = request->getParam("password", true)->value();
        
        // TODO: Implementar configuração do WiFi
        
        request->send(200, "application/json", "{\"message\":\"Configuração do WiFi atualizada\"}");
    });
    
    server->on("/config/api", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("url", true)) {
            request->send(400, "application/json", "{\"error\":\"URL da API não fornecida\"}");
            return;
        }
        
        String url = request->getParam("url", true)->value();
        
        // TODO: Implementar configuração da API
        
        request->send(200, "application/json", "{\"message\":\"Configuração da API atualizada\"}");
    });
} 