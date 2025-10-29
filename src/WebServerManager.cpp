#include "WebServerManager.h"
#include "Config.h"
#include <SPIFFS.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// Referências externas necessárias
extern bool systemInitialized;
extern bool supabaseConnected;
extern bool webServerRunning;

WebServerManager::WebServerManager() : 
    server(nullptr),
    adminServer(nullptr),
    isRunning(false),
    systemStatus(nullptr),
    sensorData(nullptr),
    relayStates(nullptr),
    tempRef(nullptr),
    phRef(nullptr),
    tdsRef(nullptr) {
}

WebServerManager::~WebServerManager() {
    if (server) {
        delete server;
    }
    if (adminServer) {
        delete adminServer;
    }
}

void WebServerManager::begin() {
    // Esta função é mantida para compatibilidade, mas não será usada
    // pois agora usamos beginAdminServer() diretamente
    Serial.println("⚠️ Use beginAdminServer() em vez de begin()");
}

void WebServerManager::beginAdminServer(WiFiManager& wifiManager, HydroControl& hydroControl) {
    if (adminServer != nullptr) {
        Serial.println("⚠️ Admin server já iniciado");
        return;
    }
    
    Serial.println("🌐 Iniciando PAINEL ADMIN na PORTA 80...");
    
    // Inicializar SPIFFS se ainda não foi
    if (!SPIFFS.begin(true)) {
        Serial.println("❌ Erro ao inicializar SPIFFS");
        return;
    }
    
    // IMPORTANTE: Este servidor só funciona quando WiFi está conectado (modo Station)
    // NÃO interfere com o Access Point do WiFiManager (modo AP)
    adminServer = new AsyncWebServer(80);
    
    // ✅ USAR ARQUIVO index.html DO SPIFFS PARA PÁGINA PRINCIPAL
    adminServer->serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
    
    // ✅ Servir arquivos estáticos (CSS, JS)
    adminServer->serveStatic("/style.css", SPIFFS, "/style.css");
    adminServer->serveStatic("/script.js", SPIFFS, "/script.js");
    
    // ✅ API para informações do dispositivo
    adminServer->on("/api/device-info", HTTP_GET, [&wifiManager](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(512);
        
        // Debug: obtener device_id con logs
        String deviceId = wifiManager.getDeviceID();
        Serial.printf("🔍 WebServer: Device ID solicitado: %s\n", deviceId.c_str());
        Serial.printf("🔍 WebServer: MAC Address: %s\n", WiFi.macAddress().c_str());
        
        doc["device_id"] = deviceId;
        doc["firmware_version"] = wifiManager.getFirmwareVersion();
        doc["ip_address"] = wifiManager.getStationIP();
        doc["connected"] = wifiManager.isConnected();
        doc["uptime"] = millis() / 1000;
        doc["free_heap"] = ESP.getFreeHeap();
        
        String response;
        serializeJson(doc, response);
        Serial.printf("🔍 WebServer: Respuesta JSON: %s\n", response.c_str());
        
        request->send(200, "application/json", response);
    });
    
    // ✅ API para sensores (COMPATÍVEL COM index.html)
    adminServer->on("/api/sensors", HTTP_GET, [&hydroControl](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(512);
        doc["temperature"] = hydroControl.getTemperature();
        doc["humidity"] = 65.0; // Simulated - implementar DHT22 se necessário
        doc["ph"] = hydroControl.getpH();
        doc["tds"] = hydroControl.getTDS();
        doc["water_level_ok"] = hydroControl.isWaterLevelOk();
        doc["temp_water"] = hydroControl.getTemperature(); // Mesmo sensor por enquanto
        doc["timestamp"] = millis();
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    // ✅ API para relés (COMPATÍVEL COM index.html)
    adminServer->on("/api/relays", HTTP_GET, [&hydroControl, this](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(1024);
        JsonArray relays = doc.createNestedArray("relays");
        
        bool* relayStates = hydroControl.getRelayStates();
        for (int i = 0; i < 16; i++) {
            JsonObject relay = relays.createNestedObject();
            relay["id"] = i;
            relay["state"] = relayStates[i];
            relay["name"] = this->getRelayName(i);
        }
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    // ✅ API para toggle de relés (COMPATÍVEL COM index.html)
    adminServer->on("/api/relay", HTTP_POST, [&hydroControl](AsyncWebServerRequest *request) {
        if (request->hasParam("relay", true)) {
            int relay = request->getParam("relay", true)->value().toInt();
            int duration = 0;
            
            if (request->hasParam("duration", true)) {
                duration = request->getParam("duration", true)->value().toInt();
            }
            
            if (relay >= 0 && relay < 16) {
                hydroControl.toggleRelay(relay, duration);
                
                DynamicJsonDocument doc(256);
                doc["success"] = true;
                doc["relay"] = relay;
                doc["new_state"] = hydroControl.getRelayStates()[relay];
                doc["duration"] = duration;
                
                String response;
                serializeJson(doc, response);
                request->send(200, "application/json", response);
                
                Serial.printf("🔌 API: Relé %d -> %s\n", relay, hydroControl.getRelayStates()[relay] ? "ON" : "OFF");
            } else {
                request->send(400, "application/json", "{\"error\":\"Invalid relay number\"}");
            }
        } else {
            request->send(400, "application/json", "{\"error\":\"Missing relay parameter\"}");
        }
    });
    
    // ✅ API para status do sistema
    adminServer->on("/api/system-status", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(512);
        doc["system_initialized"] = systemInitialized;
        doc["supabase_connected"] = supabaseConnected;
        doc["web_server_running"] = webServerRunning;
        doc["free_heap"] = ESP.getFreeHeap();
        doc["uptime_seconds"] = millis() / 1000;
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    // ✅ API de compatibilidade (mantendo endpoints antigos)
    adminServer->on("/api/supabase-status", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{\"connected\":" + String(supabaseConnected ? "true" : "false") + "}";
        request->send(200, "application/json", json);
    });
    
    // ✅ Página de status detalhado (texto simples)
    adminServer->on("/status", HTTP_GET, [&wifiManager](AsyncWebServerRequest *request) {
        String status = "🌱 ESP32 HIDROPÔNICO - STATUS\n";
        status += "================================\n";
        status += "🆔 Device ID: " + wifiManager.getDeviceID() + "\n";
        status += "🔧 Firmware: " + wifiManager.getFirmwareVersion() + "\n";
        status += "🌐 IP: " + wifiManager.getStationIP() + "\n";
        status += "⏰ Uptime: " + String(millis() / 1000) + " segundos\n";
        status += "💾 Heap Livre: " + String(ESP.getFreeHeap()) + " bytes\n";
        status += "🌱 Sistema: " + String(systemInitialized ? "✅ Pronto" : "⏳ Inicializando") + "\n";
        status += "☁️ Supabase: " + String(supabaseConnected ? "✅ Conectado" : "❌ Desconectado") + "\n";
        status += "🌐 Web Server: " + String(webServerRunning ? "✅ Ativo" : "❌ Inativo") + "\n";
        request->send(200, "text/plain", status);
    });
    
    // ✅ Reset do sistema
    adminServer->on("/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(128);
        doc["success"] = true;
        doc["message"] = "Sistema reiniciando em 3 segundos...";
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        Serial.println("🔄 Reset solicitado via painel admin");
        delay(1000);
        ESP.restart();
    });
    
    // ✅ API Reset do sistema (endpoint compatível com interface)
    adminServer->on("/api/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(128);
        doc["success"] = true;
        doc["message"] = "Sistema reiniciando em 3 segundos...";
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        Serial.println("🔄 API Reset solicitado via painel admin");
        
        // Usar task separada para evitar problemas com delay
        xTaskCreate([](void* param) {
            delay(2000);
            ESP.restart();
            vTaskDelete(NULL);
        }, "reset_task", 2048, NULL, 1, NULL);
    });
    
    // ✅ Reconfiguração WiFi
    adminServer->on("/reconfigure-wifi", HTTP_GET, [&wifiManager](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(128);
        doc["success"] = true;
        doc["message"] = "Resetando WiFi e voltando ao modo AP...";
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        Serial.println("📶 Reconfiguração WiFi solicitada via painel admin");
        Serial.println("🗑️ Limpando credenciais WiFi salvas...");
        
        // Resetar configurações WiFi
        wifiManager.resetSettings();
        
        Serial.println("🔄 Reiniciando para modo AP...");
        delay(1000);
        ESP.restart();
    });
    
    // ✅ API Reconfiguração WiFi (endpoint compatível com interface)
    adminServer->on("/api/reconfigure-wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(128);
        doc["success"] = true;
        doc["message"] = "Resetando WiFi e voltando ao modo AP...";
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        Serial.println("📶 API Reconfiguração WiFi solicitada");
        Serial.println("🗑️ Limpando credenciais WiFi...");
        
        // Usar task separada para operação segura
        xTaskCreate([](void* param) {
            delay(1000);
            
            // Limpar credenciais WiFi diretamente via Preferences
            Preferences prefs;
            if (prefs.begin("hydro_system", false)) {
                prefs.remove("ssid");
                prefs.remove("password");
                prefs.remove("device_name");
                prefs.end();
                Serial.println("✅ Credenciais WiFi removidas");
            }
            
            delay(1000);
            ESP.restart();
            vTaskDelete(NULL);
        }, "wifi_reset_task", 2048, NULL, 1, NULL);
    });
    
    adminServer->begin();
    webServerRunning = true;
    
    Serial.println("✅ PAINEL ADMIN iniciado na PORTA 80");
    Serial.println("📁 Usando arquivo: index.html + style.css + script.js");
    Serial.println("🌐 Acesso DIRETO: http://" + wifiManager.getStationIP());
    Serial.println("📱 Interface Completa: Dashboard + Relés + Sensores");
    Serial.println("🔒 IMPORTANTE: Access Point do WiFiManager preservado!");
}

void WebServerManager::update() {
    // Implementação futura se necessário
}

void WebServerManager::setupUnifiedRoutes() {
    // Esta função é mantida apenas por compatibilidade
    // Não é mais usada com a nova arquitetura
}

void WebServerManager::setupServer(SystemStatus& status, SensorData& sensors, bool* states) {
    systemStatus = &status;
    sensorData = &sensors;
    relayStates = states;
    
    Serial.println("⚠️ setupServer() - Use beginAdminServer() para funcionalidade completa");
}

void WebServerManager::setupServer(float& temperature, float& ph, float& tds, bool* states, std::function<void(int, int)> relayCallback) {
    tempRef = &temperature;
    phRef = &ph;
    tdsRef = &tds;
    relayStates = states;
    onRelayToggle = relayCallback;
    
    Serial.println("⚠️ setupServer() simples - Use beginAdminServer() para funcionalidade completa");
}

void WebServerManager::initSPIFFS() {
    if (!SPIFFS.begin(true)) {
        Serial.println("❌ Erro ao montar SPIFFS");
        return;
    }
    Serial.println("✅ SPIFFS montado com sucesso");
}

String WebServerManager::getRelayName(int relay) {
    // Nomes padrão conforme o prompt ESP32 Integration
    switch(relay) {
        case 0: return "💧 Bomba Principal";
        case 1: return "🧪 Bomba Nutrientes";
        case 2: return "⚗️ Bomba pH";
        case 3: return "💨 Ventilador";
        case 4: return "💡 Luz UV";
        case 5: return "🔥 Aquecedor";
        case 6: return "🌊 Bomba Circulação";
        case 7: return "🫧 Bomba Oxigenação";
        case 8: return "🚪 Válvula Entrada";
        case 9: return "🚪 Válvula Saída";
        case 10: return "🔄 Sensor Agitador";
        case 11: return "🌱 Luz LED Crescimento";
        case 12: return "📱 Reserva 1";
        case 13: return "📱 Reserva 2";
        case 14: return "📱 Reserva 3";
        case 15: return "📱 Reserva 4";
        default: return "Relé " + String(relay);
    }
}