#include "WiFiConfigServer.h"
#include <Preferences.h>
#include <SPIFFS.h>
#include "HybridStateManager.h"
#include <ArduinoJson.h>
#include "Config.h"  // ✅ Para FIRMWARE_VERSION


// ===== CONSTRUTOR E DESTRUTOR =====
WiFiConfigServer::WiFiConfigServer() : 
    server(nullptr),
    serverActive(false),
    startTime(0),
    activeConnections(0),
    lastConnectionCheck(0),
    AP_IP(192, 168, 4, 1),
    AP_GATEWAY(192, 168, 4, 1),
    AP_SUBNET(255, 255, 255, 0) {
    
    deviceID = "ESP32_HIDRO_" + String((uint32_t)ESP.getEfuseMac(), HEX);
}

WiFiConfigServer::~WiFiConfigServer() {
    end();
}

// ===== CONTROLE DO SERVIDOR =====
bool WiFiConfigServer::begin() {
    Serial.println("🌐 Iniciando WiFi Config Server...");
    
    if (serverActive) {
        Serial.println("⚠️ Servidor já está ativo");
        return true;
    }
    
    // Configurar Access Point
    WiFi.mode(WIFI_AP);
    String apName = "ESP32_Hidropônico";
    String apPassword = "";  // Sem senha para facilitar acesso
    
    Serial.println("📡 Configurando Access Point:");
    Serial.println("   SSID: " + apName);
    Serial.println("   Sem senha (aberto)");
    Serial.println("   IP: " + AP_IP.toString());
    
    if (!WiFi.softAP(apName.c_str())) {
        Serial.println("❌ Erro ao criar Access Point");
        return false;
    }
    
    if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET)) {
        Serial.println("❌ Erro ao configurar IP do Access Point");
        return false;
    }
    
    delay(2000); // Aguardar estabilização do AP
    
    // Criar servidor HTTP
    server = new AsyncWebServer(80);
    
    // ===== PÁGINA PRINCIPAL =====
    server->on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkConnectionLimit(request)) return;
        
        // Servir HTML do SPIFFS
        if (SPIFFS.exists("/wifi-setup.html")) {
            request->send(SPIFFS, "/wifi-setup.html", "text/html");
        } else {
            // Fallback simples caso o arquivo não exista
            String fallbackHtml = "<!DOCTYPE html><html><head><title>WiFi Setup</title></head><body>";
            fallbackHtml += "<h1>ESP32 WiFi Setup</h1>";
            fallbackHtml += "<form action='/api/connect-wifi' method='POST'>";
            fallbackHtml += "<p>SSID: <input type='text' name='ssid' required></p>";
            fallbackHtml += "<p>Password: <input type='password' name='password'></p>";
            fallbackHtml += "<p>Device Name: <input type='text' name='deviceName' value='" + deviceID + "'></p>";
            fallbackHtml += "<p><button type='submit'>Connect</button></p>";
            fallbackHtml += "</form></body></html>";
            request->send(200, "text/html", fallbackHtml);
        }
    });
    
    // ===== API PARA SCAN DE REDES WIFI =====
    server->on("/api/scan-networks", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkConnectionLimit(request)) return;
        
        Serial.println("📡 Iniciando scan WiFi...");
        
        int networksFound = WiFi.scanNetworks();
        StaticJsonDocument<2048> doc;
        JsonArray networks = doc.createNestedArray("networks");
        
        if (networksFound > 0) {
            Serial.printf("📡 Encontradas %d redes\n", networksFound);
            
            for (int i = 0; i < networksFound && i < 20; i++) {
                JsonObject network = networks.createNestedObject();
                network["ssid"] = WiFi.SSID(i);
                network["rssi"] = WiFi.RSSI(i);
                network["encryption"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "secured";
                
                Serial.printf("   %d: %s (%d dBm)\n", i+1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
            }
            
            doc["success"] = true;
            doc["count"] = networksFound;
        } else {
            Serial.println("📡 Nenhuma rede encontrada");
            doc["success"] = false;
            doc["message"] = "Nenhuma rede encontrada";
        }
        
        WiFi.scanDelete();
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    // ===== API PARA CONECTAR WIFI =====
    server->on("/api/connect-wifi", HTTP_POST, [this](AsyncWebServerRequest *request) {
        printDebug("📡 Requisição POST para conectar WiFi");
        
        if (!checkConnectionLimit(request)) return;
        
        String ssid = "";
        String password = "";
        String deviceName = "";
        
        // ===== EXTRAIR PARÂMETROS DO POST =====
        if (request->hasParam("ssid", true)) {
            ssid = request->getParam("ssid", true)->value();
        }
        if (request->hasParam("password", true)) {
            password = request->getParam("password", true)->value();
        }
        if (request->hasParam("deviceName", true)) {
            deviceName = request->getParam("deviceName", true)->value();
        }
        
        printDebug("📝 SSID recebido: '" + ssid + "'");
        printDebug("📝 Password recebido: " + String(password.length() > 0 ? "[" + String(password.length()) + " chars]" : "[VAZIO]"));
        printDebug("📝 Device Name recebido: '" + deviceName + "'");
        
        // ===== VALIDAÇÃO BÁSICA =====
        if (ssid.length() == 0) {
            printDebug("❌ ERRO: SSID vazio");
            StaticJsonDocument<200> doc;
            doc["success"] = false;
            doc["message"] = "SSID é obrigatório";
            
            String response;
            serializeJson(doc, response);
            request->send(400, "application/json", response);
            return;
        }
        
        // ===== SALVAMENTO DIRETO DAS CREDENCIAIS =====
        printDebug("💾 Salvando credenciais...");
        
        Preferences prefs;
        if (!prefs.begin("hydro_system", false)) {
            printDebug("❌ ERRO: Falha ao abrir Preferences");
            StaticJsonDocument<200> doc;
            doc["success"] = false;
            doc["message"] = "Erro interno ao salvar configurações";
            
            String response;
            serializeJson(doc, response);
            request->send(500, "application/json", response);
            return;
        }
        
        size_t ssidSize = prefs.putString("ssid", ssid);
        size_t passSize = prefs.putString("password", password);
        String finalDeviceName = deviceName.length() > 0 ? deviceName : deviceID;
        size_t nameSize = prefs.putString("device_name", finalDeviceName);
        
        prefs.end();
        
        printDebug("💾 SSID salvo: " + String(ssidSize) + " bytes");
        printDebug("💾 Password salvo: " + String(passSize) + " bytes");
        printDebug("💾 Device Name salvo: " + String(nameSize) + " bytes");
        
        if (ssidSize == 0) {
            printDebug("❌ ERRO: Falha ao salvar SSID");
            StaticJsonDocument<200> doc;
            doc["success"] = false;
            doc["message"] = "Erro ao salvar SSID";
            
            String response;
            serializeJson(doc, response);
            request->send(500, "application/json", response);
            return;
        }
        
        // ===== TESTE RÁPIDO DE CONEXÃO =====
        printDebug("🔄 Testando conexão WiFi...");
        
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(ssid.c_str(), password.c_str());
        
        unsigned long startTime = millis();
        bool connected = false;
        String connectionResult = "";
        
        while ((millis() - startTime) < 10000) { // Reduzido para 10s
            if (WiFi.status() == WL_CONNECTED) {
                connected = true;
                connectionResult = "Conectado! IP: " + WiFi.localIP().toString();
                printDebug("✅ " + connectionResult);
                break;
            }
            delay(250);
        }
        
        if (!connected) {
            connectionResult = "Teste falhou - mas credenciais foram salvas";
            printDebug("⚠️ " + connectionResult);
        }
        
        // ===== RESPOSTA DE SUCESSO =====
        StaticJsonDocument<300> doc;
        doc["success"] = true;
        doc["message"] = "WiFi configurado com sucesso";
        doc["connection_test"] = connected;
        doc["connection_result"] = connectionResult;
        doc["will_restart"] = true;
        doc["restart_delay"] = 3;
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        // ===== REINÍCIO PROGRAMADO =====
        printDebug("⏰ Reiniciando em 3 segundos...");
        
        if (onConfiguredCallback) {
            xTaskCreate([](void* param) {
                delay(3000);
                auto callback = reinterpret_cast<std::function<void()>*>(param);
                (*callback)();
                vTaskDelete(NULL);
            }, "restart_task", 2048, &onConfiguredCallback, 1, NULL);
        }
    });
    
    // ===== PÁGINA 404 =====
    server->onNotFound([this](AsyncWebServerRequest *request) {
        if (!checkConnectionLimit(request)) return;
        request->send(404, "text/plain", "Página não encontrada");
    });
    
    // Iniciar servidor
    server->begin();
    serverActive = true;
    startTime = millis();
    
    Serial.println("✅ WiFi Config Server iniciado");
    Serial.println("🌐 Acesse: http://" + AP_IP.toString());
    Serial.println("📱 SSID: ESP32_Hidropônico (sem senha)");
    
    // ✅ NOVO: API para salvar configuração com email
    server->on("/save-config-with-email", HTTP_POST, [this](AsyncWebServerRequest *request) {
        printDebug("📧 Requisição POST para salvar config com email");
        
        if (!checkConnectionLimit(request)) return;
        
        // ✅ Processar dados do FormData
        String userEmail = request->hasParam("userEmail", true) ? request->getParam("userEmail", true)->value() : "";
        String deviceName = request->hasParam("deviceName", true) ? request->getParam("deviceName", true)->value() : "";
        String location = request->hasParam("location", true) ? request->getParam("location", true)->value() : "";
        String ssid = request->hasParam("ssid", true) ? request->getParam("ssid", true)->value() : "";
        String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
        
        // Se deviceName e location não foram fornecidos, usar padrões
        if (deviceName.isEmpty()) {
            deviceName = "ESP32 - " + WiFi.macAddress().substring(WiFi.macAddress().length() - 8);
        }
        if (location.isEmpty()) {
            location = "Localização não especificada";
        }
        
        printDebug("📧 Email: " + userEmail);
        printDebug("📱 Device: " + deviceName);
        printDebug("📍 Location: " + location);
        printDebug("📡 SSID: " + ssid);
        
        // Validar dados obrigatórios
        if (userEmail.isEmpty() || ssid.isEmpty() || password.isEmpty()) {
            StaticJsonDocument<200> responseDoc;
            responseDoc["success"] = false;
            responseDoc["message"] = "Email, SSID e senha são obrigatórios";
            
            String response;
            serializeJson(responseDoc, response);
            request->send(400, "application/json", response);
            return;
        }
        
        // Salvar configuração WiFi nas preferences
        Preferences preferences;
        if (preferences.begin("hydro_system", false)) {
            preferences.putString("ssid", ssid);
            preferences.putString("password", password);
            preferences.putString("user_email", userEmail);
            preferences.putString("device_name", deviceName);
            preferences.putString("location", location);
            preferences.end();
        }
        
        // Chamar callback de email se configurado
        if (onEmailCallback) {
            onEmailCallback(userEmail);
        }
        
        // Resposta de sucesso
        StaticJsonDocument<200> responseDoc;
        responseDoc["success"] = true;
        responseDoc["message"] = "Configuração salva com sucesso";
        responseDoc["device_id"] = deviceID;
        responseDoc["user_email"] = userEmail;
        
        String response;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
        
        // Chamar callback de configuração WiFi
        if (onConfiguredCallback) {
            delay(1000);
            onConfiguredCallback();
        }
    });
    
    // ✅ NOVO: Endpoint para informações do dispositivo
    server->on("/api/device-info", HTTP_GET, [this](AsyncWebServerRequest *request) {
        StaticJsonDocument<300> doc;
        doc["device_id"] = deviceID;
        doc["mac_address"] = WiFi.macAddress();
        doc["ip_address"] = WiFi.softAPIP().toString();
        doc["ap_ip"] = WiFi.softAPIP().toString();
        doc["active_connections"] = WiFi.softAPgetStationNum();
        doc["uptime"] = (millis() - startTime) / 1000;
        doc["connected"] = WiFi.isConnected();
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    // ✅ NOVO: Endpoint para resetar o dispositivo
    server->on("/api/reset", HTTP_POST, [this](AsyncWebServerRequest *request) {
        printDebug("🔄 Requisição para reiniciar dispositivo");
        
        StaticJsonDocument<200> doc;
        doc["success"] = true;
        doc["message"] = "Dispositivo reiniciando...";
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        // Agendar reinício
        xTaskCreate([](void* param) {
            delay(2000);
            Serial.println("🔄 Reiniciando dispositivo...");
            ESP.restart();
            vTaskDelete(NULL);
        }, "reset_task", 2048, NULL, 1, NULL);
    });
    
    return true;
}

void WiFiConfigServer::end() {
    if (!serverActive) return;
    
    Serial.println("🛑 Parando WiFi Config Server...");
    
    if (server) {
        server->end();
        delete server;
        server = nullptr;
    }
    
    WiFi.softAPdisconnect(true);
    serverActive = false;
    
    Serial.println("✅ WiFi Config Server parado");
}

void WiFiConfigServer::loop() {
    if (!serverActive) return;
    
    // Verificar conexões ativas a cada 5 segundos
    if (millis() - lastConnectionCheck > 5000) {
        activeConnections = WiFi.softAPgetStationNum();
        lastConnectionCheck = millis();
        
        if (activeConnections > 0) {
            Serial.printf("👥 Clientes conectados no AP: %d\n", activeConnections);
        }
    }
}

// ===== CALLBACKS =====
void WiFiConfigServer::onWiFiConfigured(std::function<void()> callback) {
    onConfiguredCallback = callback;
}

// ✅ NOVO: Implementar callback para email
void WiFiConfigServer::onEmailRegistered(std::function<void(String)> callback) {
    onEmailCallback = callback;
}

// ===== MÉTODOS PRIVADOS =====
bool WiFiConfigServer::checkConnectionLimit(AsyncWebServerRequest *request) {
    // Limite simples: máximo 3 conexões simultâneas
    if (activeConnections > 3) {
        request->send(503, "text/plain", "Servidor sobrecarregado. Tente novamente.");
        return false;
    }
    return true;
}

String WiFiConfigServer::urlDecode(String str) {
    String decoded = "";
    char c;
    char code0;
    char code1;
    for (int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (c == '+') {
            decoded += ' ';
        } else if (c == '%') {
            i++;
            code0 = str.charAt(i);
            i++;
            code1 = str.charAt(i);
            c = (h2int(code0) << 4) | h2int(code1);
            decoded += c;
        } else {
            decoded += c;
        }
    }
    return decoded;
}

unsigned char WiFiConfigServer::h2int(char c) {
    if (c >= '0' && c <= '9') {
        return((unsigned char)c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return((unsigned char)c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return((unsigned char)c - 'A' + 10);
    }
    return(0);
}

// ===== FUNÇÃO DE DEBUG =====
void WiFiConfigServer::printDebug(const String& message) {
    Serial.println("🌐 [WiFiConfig] " + message);
} 