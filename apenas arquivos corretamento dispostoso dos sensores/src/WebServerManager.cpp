#include "WebServerManager.h"
#include <SPIFFS.h>

WebServerManager::WebServerManager() : server(80) {}

void WebServerManager::begin() {
    initSPIFFS();
    server.begin();
}

void WebServerManager::initSPIFFS() {
    if (!SPIFFS.begin(true)) {
        Serial.println("Erro ao montar SPIFFS");
        return;
    }
    Serial.println("SPIFFS montado com sucesso");
    
    // Debug: listar arquivos
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    while(file) {
        Serial.printf("Arquivo: %s\n", file.name());
        file = root.openNextFile();
    }
}

void WebServerManager::setupServer(float& currentTemp, float& currentPH, float& currentTDS,
                                 bool* relayStates, void (*toggleRelay)(int, int)) {
    // Rota principal
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/index.html", "text/html");
    });

    // Rotas para arquivos estáticos
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/style.css", "text/css");
    });

    server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/script.js", "text/javascript");
    });

    // Rota para dados dos sensores
    server.on("/sensors", HTTP_GET, [&currentTemp, &currentPH, &currentTDS, relayStates]
    (AsyncWebServerRequest *request){
        String json = "{";
        json += "\"temperature\":" + String(currentTemp, 1) + ",";
        json += "\"ph\":" + String(currentPH, 2) + ",";
        json += "\"tds\":" + String(currentTDS, 0) + ",";
        json += "\"ec\":" + String(currentTDS * 2, 0) + ",";
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"relayStates\":[";
        for(int i = 0; i < 8; i++) {
            json += relayStates[i] ? "true" : "false";
            if(i < 7) json += ",";
        }
        json += "]}";
        request->send(200, "application/json", json);
    });

    // Rotas para controle dos relés
    for(int i = 0; i < 8; i++) {
        String path = "/toggle" + String(i + 1);
        server.on(path.c_str(), HTTP_GET, [i, toggleRelay](AsyncWebServerRequest *request){
            int seconds = 0;
            if(request->hasParam("seconds")) {
                seconds = request->getParam("seconds")->value().toInt();
            }
            toggleRelay(i, seconds);
            request->send(200, "text/plain", "OK");
        });
    }

    Serial.println("Servidor HTTP iniciado");
}