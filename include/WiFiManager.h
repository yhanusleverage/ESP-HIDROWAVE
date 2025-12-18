#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H
//implementacao INLINE do wifimanager.cpp

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <functional>
#include "DeviceID.h"

// ===== CONFIGURAÇÕES OTIMIZADAS =====
#ifndef AP_SSID
#define AP_SSID "ESP32_Hidropônico"
#endif

#ifndef AP_PASSWORD
#define AP_PASSWORD ""  // AP aberto para UX otimizada
#endif

// ===== TIMEOUTS CONSOLIDADOS =====
const unsigned long WIFI_CONNECTION_TIMEOUT = 20000;     // 20s para conectar
const unsigned long AP_TIMEOUT = 300000;            // 5min modo AP
const unsigned long RETRY_INTERVAL = 600000;        // 10min entre retries

// ===== CONFIGURAÇÃO IP ESTÁTICA =====
#ifndef USE_STATIC_IP
#define USE_STATIC_IP true  // Habilitar IP estática para Cloudflare Tunnel
#endif

#if USE_STATIC_IP
// IP estática para ESP32 (debe coincidir con config.yml de cloudflared)
const IPAddress STATIC_IP(192, 168, 1, 10);
const IPAddress STATIC_GATEWAY(192, 168, 1, 1);
const IPAddress STATIC_SUBNET(255, 255, 255, 0);
const IPAddress STATIC_DNS1(192, 168, 1, 1);
const IPAddress STATIC_DNS2(8, 8, 8, 8);  // Google DNS como backup
#endif

class WiFiManager {
private:
    // ===== ESTADO MÍNIMO NECESSÁRIO =====
    AsyncWebServer* server;
    bool apModeActive;
    bool isConnecting;
    unsigned long connectionStartTime;
    unsigned long apStartTime;
    unsigned long lastRetryTime;
    unsigned long lastDebugMessage;
    
    // ===== PROTEÇÃO CONTRA SOBRECARGA =====
    unsigned int activeConnections;
    unsigned long lastConnectionCheck;
    
    // ===== CONFIGURAÇÃO ESSENCIAL =====
    String deviceID;
    String firmwareVersion;
    String currentSSID;
    IPAddress AP_IP, AP_GATEWAY, AP_SUBNET;
    Preferences preferences;
    
    // ===== CALLBACK ÚNICO USADO =====
    std::function<void(bool)> onConnectionResult;
    
    // ===== FUNÇÃO AUXILIAR PARA REPETIR STRING =====
    String repeatString(const String& str, int count) {
        String result = "";
        for (int i = 0; i < count; i++) {
            result += str;
        }
        return result;
    }
    
    // ===== MIDDLEWARE DE PROTEÇÃO CONTRA SOBRECARGA =====
    bool checkConnectionLimit(AsyncWebServerRequest *request) {
        unsigned long now = millis();
        
        // Atualizar contador a cada 5 segundos
        if (now - lastConnectionCheck > 5000) {
            activeConnections = WiFi.softAPgetStationNum();
            lastConnectionCheck = now;
            
            if (activeConnections > 0) {
                Serial.println("📊 CONEXÕES ATIVAS: " + String(activeConnections));
            }
        }
        
        // ✅ PROTEGER CONTRA SOBRECARGA
        if (activeConnections > 3) {
            Serial.println("🚨 LIMITANDO ACESSO: Máximo 3 clientes (" + String(activeConnections) + " conectados)");
            
            // Enviar resposta de limite excedido
            request->send(503, "application/json", 
                "{\"success\":false,\"message\":\"Muitas conexões. Tente novamente.\"}");
            return false;
        }
        
        return true; // OK para processar
    }
    
    // ===== MÉTODOS PRIVADOS OTIMIZADOS =====
    bool startAPMode() {
        Serial.println("🔧 Iniciando AP: " + String(AP_SSID));
        
        // ✅ INICIALIZAR SPIFFS ANTES DE SETUP WEB SERVER
        if (!SPIFFS.begin(true)) {
            Serial.println("❌ ERRO: Falha ao inicializar SPIFFS para AP");
            Serial.println("📁 Arquivos HTML não estarão disponíveis");
        } else {
            Serial.println("✅ SPIFFS inicializado para AP");
        }
        
        WiFi.mode(WIFI_AP_STA);
        AP_IP.fromString("192.168.4.1");
        AP_GATEWAY.fromString("192.168.4.1");
        AP_SUBNET.fromString("255.255.255.0");
        WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
        
        bool apStarted = (strlen(AP_PASSWORD) == 0) ? 
            WiFi.softAP(AP_SSID) : WiFi.softAP(AP_SSID, AP_PASSWORD);
        
        if (!apStarted) {
            Serial.println("❌ Erro ao iniciar AP");
            return false;
        }
        
        apModeActive = true;
        apStartTime = millis();
        
        Serial.println("✅ AP ativo: http://192.168.4.1");
        Serial.println("⏰ Timeout: 5 minutos");
        
        setupWebServer();
        return true;
    }
    
    bool tryAutoConnect() {
        String ssid = preferences.getString("ssid", "");
        String password = preferences.getString("password", "");
        
        if (ssid.length() == 0) {
            Serial.println("📝 Sem credenciais salvas");
            return false;
        }
        
        Serial.println("🔄 Conectando: " + ssid);
        
        // ✅ CONFIGURAR IP ESTÁTICA ANTES DE CONECTAR (para Cloudflare Tunnel)
        #if USE_STATIC_IP
        Serial.println("🔧 Configurando IP estática: " + STATIC_IP.toString());
        if (!WiFi.config(STATIC_IP, STATIC_GATEWAY, STATIC_SUBNET, STATIC_DNS1, STATIC_DNS2)) {
            Serial.println("⚠️ Aviso: Falha ao configurar IP estática, usando DHCP");
            // Continuar con DHCP si falla (fallback automático)
        } else {
            Serial.println("✅ IP estática configurada: " + STATIC_IP.toString());
        }
        #else
        Serial.println("ℹ️ Usando IP dinâmica (DHCP)");
        #endif
        
        WiFi.begin(ssid.c_str(), password.c_str());
        
        isConnecting = true;
        connectionStartTime = millis();
        currentSSID = ssid;
        
        // ✅ TIMEOUT NÃO-BLOQUEANTE (removido delay)
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < CONNECTION_TIMEOUT) {
            // Verificação não-bloqueante a cada 100ms
            if ((millis() - startTime) % 100 == 0) {
                if (WiFi.status() == WL_CONNECT_FAILED) {
                    Serial.println("❌ Falha na autenticação");
                    break;
                }
            }
            yield(); // ✅ YIELD em vez de delay
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            isConnecting = false;
            Serial.println("✅ WiFi: " + WiFi.localIP().toString());
            return true;
        } else {
            isConnecting = false;
            Serial.println("⏰ Timeout - Ativando AP");
            WiFi.disconnect(true);
            return false;
        }
    }
    
    void setupWebServer() {
        if (server) delete server;
        server = new AsyncWebServer(80);
        
        // ✅ VERIFICAR SE SPIFFS ESTÁ OK
        Serial.println("🔍 Verificando arquivos SPIFFS...");
        if (!SPIFFS.exists("/wifi-config.html")) {
            Serial.println("❌ ERRO: wifi-config.html não encontrado no SPIFFS!");
            Serial.println("📁 Usando fallback simples...");
        } else {
            Serial.println("✅ wifi-config.html encontrado");
        }
        
        // ✅ SERVIDOR ESTÁTICO PRINCIPAL
        server->serveStatic("/", SPIFFS, "/").setDefaultFile("wifi-config.html");
        
        // ✅ FALLBACK SIMPLES PARA PÁGINA PRINCIPAL
        server->on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
            Serial.println("🌐 Página principal solicitada");
            
            // Tentar servir do SPIFFS primeiro
            if (SPIFFS.exists("/wifi-config.html")) {
                request->send(SPIFFS, "/wifi-config.html", "text/html");
            } else {
                // ✅ FALLBACK BÁSICO SEM EMOJIS NEM CÓDIGO COMPLEXO
                String html = "<!DOCTYPE html><html><head><title>ESP32 WiFi</title></head><body>";
                html += "<h1>ESP32 - Configuracao WiFi</h1>";
                html += "<form action='/api/connect-wifi' method='POST'>";
                html += "<p>SSID: <input type='text' name='ssid' required></p>";
                html += "<p>Senha: <input type='password' name='password'></p>";
                html += "<p><button type='submit'>Conectar</button></p>";
                html += "</form></body></html>";
                request->send(200, "text/html", html);
            }
        });
        
        // ✅ ENDPOINT ÚNICO PARA DEVICE INFO
        server->on("/api/device-info", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkConnectionLimit(request)) return; // ✅ PROTEÇÃO
            
            Serial.println("📱 API device-info chamada");
            StaticJsonDocument<512> doc;
            doc["device_id"] = deviceID;
            doc["firmware_version"] = firmwareVersion;
            doc["ap_ip"] = WiFi.softAPIP().toString();
            doc["connected"] = WiFi.isConnected();
            doc["uptime"] = millis() / 1000;
            doc["active_connections"] = activeConnections; // ✅ INCLUIR CONTADOR
            
            if (apModeActive) {
                doc["time_left_seconds"] = (AP_TIMEOUT - (millis() - apStartTime)) / 1000;
            }
            
            String response;
            serializeJson(doc, response);
            request->send(200, "application/json", response);
        });
        
        // ✅ ENDPOINT UNIFICADO PARA CONNECT-WIFI
        server->on("/api/connect-wifi", HTTP_POST, 
            [this](AsyncWebServerRequest *request) {
                if (!checkConnectionLimit(request)) return; // ✅ PROTEÇÃO
                Serial.println("📡 connect-wifi POST chamado");
                this->handleConnectWiFi(request);
            }, 
            NULL, 
            [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
                // Proteção já aplicada no handler principal
                this->handleConnectWiFiBody(request, data, len, index, total);
            }
        );
        
        server->on("/api/connect-wifi", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkConnectionLimit(request)) return; // ✅ PROTEÇÃO
            Serial.println("📡 connect-wifi GET chamado");
            this->handleConnectWiFiGet(request);
        });
        
        // ✅ ENDPOINT SIMPLIFICADO PARA SCAN - VERSÃO CORRIGIDA
        server->on("/api/scan-networks", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkConnectionLimit(request)) return; // ✅ PROTEÇÃO CRÍTICA
            
            Serial.println("🔍 DEBUG: Iniciando scan simplificado...");
            
            // ✅ GARANTIR MODO CORRETO PARA SCAN
            WiFiMode_t currentMode = WiFi.getMode();
            Serial.println("📡 DEBUG: Modo WiFi atual: " + String(currentMode));
            
            if (currentMode == WIFI_OFF || currentMode == WIFI_AP) {
                Serial.println("🔧 DEBUG: Alterando para modo AP+STA para scan...");
                WiFi.mode(WIFI_AP_STA);
                delay(1000); // Dar tempo para estabilizar
            }
            
            // ✅ SCAN SÍNCRONO SIMPLES E CONFIÁVEL
            Serial.println("🔍 DEBUG: Executando scan síncrono...");
            int numNetworks = WiFi.scanNetworks(false, true); // async=false, show_hidden=true
            Serial.println("📡 DEBUG: Scan retornou: " + String(numNetworks) + " redes");
            
            // ✅ CONSTRUIR RESPOSTA JSON SIMPLES
            StaticJsonDocument<4096> doc;
            JsonArray networks = doc.createNestedArray("networks");
            
            if (numNetworks > 0) {
                // Limitar a 15 redes para evitar overflow
                int maxNetworks = min(numNetworks, 15);
                
                for (int i = 0; i < maxNetworks; i++) {
                    String ssid = WiFi.SSID(i);
                    if (ssid.length() == 0) continue; // Pular SSIDs vazios
                    
                    JsonObject network = networks.createNestedObject();
                    network["ssid"] = ssid;
                    network["rssi"] = WiFi.RSSI(i);
                    network["encryption"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "secured";
                    
                    // Calcular força do sinal em porcentagem
                    int rssi = WiFi.RSSI(i);
                    int strength = 0;
                    if (rssi >= -50) strength = 100;
                    else if (rssi >= -60) strength = 80;
                    else if (rssi >= -70) strength = 60;
                    else if (rssi >= -80) strength = 40;
                    else strength = 20;
                    
                    network["strength"] = strength;
                    
                    Serial.println("📶 DEBUG: Rede " + String(i) + ": " + ssid + " (" + String(rssi) + "dBm, " + String(strength) + "%)");
                }
            }
            
            doc["networks_count"] = networks.size();
            doc["success"] = true;
            
            String response;
            serializeJson(doc, response);
            
            Serial.println("✅ DEBUG: Enviando " + String(networks.size()) + " redes para o cliente");
            request->send(200, "application/json", response);
        });
        
        // ✅ FALLBACK: SCAN EM TEXTO SIMPLES
        server->on("/scan-simple", HTTP_GET, [this](AsyncWebServerRequest *request) {
            Serial.println("🔍 DEBUG: Scan texto simples");
            
            WiFi.mode(WIFI_AP_STA);
            delay(500);
            
            int numNetworks = WiFi.scanNetworks();
            
            String html = "<html><head><title>Redes WiFi</title></head><body>";
            html += "<h2>Redes WiFi Encontradas</h2>";
            
            if (numNetworks > 0) {
                html += "<ul>";
                for (int i = 0; i < numNetworks; i++) {
                    html += "<li><b>" + WiFi.SSID(i) + "</b> (" + String(WiFi.RSSI(i)) + " dBm)";
                    html += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " [ABERTA]" : " [SEGURA]";
                    html += "</li>";
                }
                html += "</ul>";
            } else {
                html += "<p>Nenhuma rede encontrada</p>";
            }
            
            html += "<br><a href='/'>Voltar</a></body></html>";
            request->send(200, "text/html", html);
        });
        
        // ✅ ENDPOINT PARA RESET (CHAMADO PELO WIFI-CONFIG.HTML)
        server->on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
            Serial.println("🔄 Reset solicitado via API");
            
            StaticJsonDocument<128> doc;
            doc["success"] = true;
            doc["message"] = "Dispositivo reiniciando...";
            
            String response;
            serializeJson(doc, response);
            request->send(200, "application/json", response);
            
            delay(1000);
            ESP.restart();
        });
        
        // ✅ ENDPOINT PARA SAÚDE DA MEMÓRIA (PAINEL ADMIN) - OTIMIZADO
        server->on("/api/memory-health", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkConnectionLimit(request)) return; // ✅ PROTEÇÃO
            
            uint32_t freeHeap = ESP.getFreeHeap();
            uint32_t maxBlock = ESP.getMaxAllocHeap();
            uint32_t totalHeap = ESP.getHeapSize();
            uint32_t uptime = millis();
            
            // Calcular métricas essenciais apenas
            uint32_t usedHeap = totalHeap - freeHeap;
            uint32_t usagePercent = (usedHeap * 100) / totalHeap;
            uint32_t fragmentationPercent = freeHeap > 0 ? (100 - (maxBlock * 100) / freeHeap) : 100;
            
            // Status de saúde simplificado
            String healthStatus = "healthy";
            if(freeHeap < 10000 || fragmentationPercent > 60) {
                healthStatus = "critical";
            } else if(freeHeap < 20000 || fragmentationPercent > 40) {
                healthStatus = "warning";
            }
            
            // JSON MINIMALISTA - 512 bytes apenas
            StaticJsonDocument<512> doc;
            doc["heap_total"] = totalHeap;
            doc["heap_free"] = freeHeap;
            doc["heap_usage_percent"] = usagePercent;
            doc["fragmentation_percent"] = fragmentationPercent;
            doc["health_status"] = healthStatus;
            doc["uptime_hours"] = (uptime / 1000) / 3600;
            doc["uptime_minutes"] = ((uptime / 1000) / 60) % 60;
            doc["active_connections"] = activeConnections;
            doc["connection_limit"] = 3;
            doc["watchdog_timeout"] = 30;
            doc["next_reset_hours"] = (usagePercent > 50) ? 6 : 12;
            
            String response;
            serializeJson(doc, response);
            request->send(200, "application/json", response);
        });
        
        // ✅ HANDLER 404 PARA DEBUG
        server->onNotFound([](AsyncWebServerRequest *request) {
            Serial.println("❌ 404: " + request->url());
            String html = "<h1>404 - Pagina nao encontrada</h1>";
            html += "<p>URL: " + request->url() + "</p>";
            html += "<p><a href='/'>Voltar para pagina inicial</a></p>";
            request->send(404, "text/html", html);
        });
        
        server->begin();
        Serial.println("✅ Servidor AP iniciado na porta 80");
        Serial.println("🌐 Acesse: http://192.168.4.1");
    }
    
    // ✅ HANDLERS SEPARADOS PARA ORGANIZAÇÃO
    void handleConnectWiFi(AsyncWebServerRequest *request) {
        String ssid = request->hasParam("ssid", true) ? request->getParam("ssid", true)->value() : "";
        String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
        String deviceName = request->hasParam("device_name", true) ? request->getParam("device_name", true)->value() : "";
        
        processWiFiCredentials(request, ssid, password, deviceName);
    }
    
    void handleConnectWiFiBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index != 0) return; // Processar apenas o primeiro chunk
        
        String jsonBody = String((char*)data).substring(0, len);
        Serial.println("📡 DEBUG: JSON Body recebido: " + jsonBody);
        
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, jsonBody);
        
        if (error) {
            Serial.println("❌ DEBUG: Erro ao processar JSON: " + String(error.c_str()));
            StaticJsonDocument<128> errorDoc;
            errorDoc["success"] = false;
            errorDoc["message"] = "Dados JSON inválidos";
            String response;
            serializeJson(errorDoc, response);
            request->send(400, "application/json", response);
            return;
        }
        
        String ssid = doc["ssid"].as<String>();
        String password = doc["password"].as<String>();
        String deviceName = doc["device_name"].as<String>();
        
        Serial.println("📡 DEBUG: SSID extraído do JSON: '" + ssid + "'");
        Serial.println("📡 DEBUG: Password length: " + String(password.length()));
        Serial.println("📡 DEBUG: Device name: '" + deviceName + "'");
        
        processWiFiCredentials(request, ssid, password, deviceName);
    }
    
    void handleConnectWiFiGet(AsyncWebServerRequest *request) {
        String ssid = request->hasParam("ssid") ? request->getParam("ssid")->value() : "";
        String password = request->hasParam("password") ? request->getParam("password")->value() : "";
        String deviceName = request->hasParam("device_name") ? request->getParam("device_name")->value() : "";
        
        processWiFiCredentials(request, ssid, password, deviceName);
    }
    
    void processWiFiCredentials(AsyncWebServerRequest *request, const String& ssid, const String& password, const String& deviceName) {
        Serial.println("💾 DEBUG: processWiFiCredentials chamada");
        Serial.println("💾 DEBUG: SSID recebido: '" + ssid + "' (length: " + String(ssid.length()) + ")");
        Serial.println("💾 DEBUG: Password length: " + String(password.length()));
        Serial.println("💾 DEBUG: Device name: '" + deviceName + "'");
        
        if (ssid.length() == 0) {
            Serial.println("❌ DEBUG: SSID está vazio!");
            StaticJsonDocument<128> doc;
            doc["success"] = false;
            doc["message"] = "SSID não pode estar vazio";
            String response;
            serializeJson(doc, response);
            request->send(400, "application/json", response);
            return;
        }
        
        // ✅ ABRIR PREFERENCES SE NÃO ESTIVER ABERTO
        if (!preferences.isKey("ssid")) {
            Serial.println("🔧 DEBUG: Inicializando Preferences...");
            preferences.begin("wifi", false);
        }
        
        // Salvar credenciais
        Serial.println("💾 DEBUG: Salvando credenciais...");
        preferences.putString("ssid", ssid);
        preferences.putString("password", password);
        if (deviceName.length() > 0) {
            preferences.putString("device_name", deviceName);
            Serial.println("💾 DEBUG: Device name salvo: " + deviceName);
        }
        
        Serial.println("✅ DEBUG: Credenciais salvas com sucesso!");
        
        StaticJsonDocument<128> doc;
        doc["success"] = true;
        doc["message"] = "Credenciais salvas! Reiniciando em 3 segundos...";
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        Serial.println("🔄 DEBUG: Enviando resposta de sucesso");
        Serial.println("💾 DEBUG: WiFi salvo - SSID: " + ssid);
        Serial.println("🔄 DEBUG: Reiniciando ESP32 em 2 segundos...");
        
        delay(2000);
        ESP.restart();
    }

public:
    // ===== CONSTRUTOR OTIMIZADO =====
    WiFiManager() : server(nullptr), apModeActive(false), isConnecting(false),
                   connectionStartTime(0), apStartTime(0), lastRetryTime(0), lastDebugMessage(0),
                   activeConnections(0), lastConnectionCheck(0) {
        deviceID = ""; // Se inicializará después cuando WiFi esté listo
        firmwareVersion = "2.1.0";
    }

    ~WiFiManager() {
        if (server) { delete server; server = nullptr; }
    }

    // ===== MÉTODOS PRINCIPAIS =====
    bool begin() {
        Serial.println("🌐 Iniciando WiFiManager otimizado...");
        
        if (!SPIFFS.begin(true)) {
            Serial.println("❌ Erro SPIFFS");
            return false;
        }
        
        preferences.begin("wifi", false);
        lastRetryTime = millis();
        
        return tryAutoConnect() ? true : startAPMode();
    }
    
    void loop() {
        unsigned long now = millis();
        
        // ✅ LÓGICA SIMPLIFICADA E OTIMIZADA
        if (isConnecting && (now - connectionStartTime > CONNECTION_TIMEOUT)) {
            Serial.println("⏰ Timeout conexão");
            isConnecting = false;
            if (onConnectionResult) onConnectionResult(false);
            if (!apModeActive) startAPMode();
        }
        
        if (isConnecting && WiFi.status() == WL_CONNECTED) {
            Serial.println("✅ WiFi conectado!");
            isConnecting = false;
            if (onConnectionResult) onConnectionResult(true);
            
            if (server) {
                server->end();
                delete server;
                server = nullptr;
            }
            WiFi.softAPdisconnect(true);
            apModeActive = false;
        }
        
        if (apModeActive && (now - apStartTime > AP_TIMEOUT)) {
            Serial.println("⏰ Timeout AP - Tentando reconectar...");
            
            // ✅ VERIFICAR SE HÁ CREDENCIAIS ANTES DE FECHAR O AP
            String savedSSID = preferences.getString("ssid", "");
            
            if (savedSSID.length() > 0) {
                Serial.println("📝 Credenciais encontradas - Tentando reconectar...");
                
                // Fechar AP apenas se temos credenciais para tentar
                if (server) {
                    server->end();
                    delete server;
                    server = nullptr;
                }
                WiFi.softAPdisconnect(true);
                apModeActive = false;
                
                if (!tryAutoConnect()) {
                    Serial.println("❌ Falha na reconexão - Reativando AP...");
                    startAPMode(); // ✅ VOLTAR IMEDIATAMENTE PARA AP
                }
            } else {
                Serial.println("📝 Sem credenciais salvas - Mantendo AP ativo...");
                // ✅ RESETAR TIMER DO AP PARA MANTER ATIVO SEM CREDENCIAIS
                apStartTime = now;
                
                // ✅ MOSTRAR MENSAGEM DETALHADA A CADA 30 SEGUNDOS
                if (now - lastDebugMessage >= 30000) { // 30 segundos
                    lastDebugMessage = now;
                    
                    Serial.println("\n" + repeatString("=", 60));
                    Serial.println("🌐 ESP32 HIDROPÔNICO - MODO CONFIGURAÇÃO ATIVO");
                    Serial.println("⏰ DEBUG: Uptime: " + String(now / 1000) + " segundos");
                    Serial.println("📶 DEBUG: Access Point PERMANENTE (sem credenciais)");
                    Serial.println("🔗 DEBUG: CONECTE-SE À REDE: " + String(AP_SSID));
                    Serial.println("🔓 DEBUG: REDE ABERTA - Sem senha");
                    Serial.println("🌐 DEBUG: Acesse: http://192.168.4.1");
                    Serial.println("💡 DEBUG: Configure WiFi para ativar sistema hidropônico");
                    Serial.println("🆔 DEBUG: Device ID: " + deviceID);
                    Serial.println("🔧 DEBUG: Firmware: " + firmwareVersion);
                    Serial.println(repeatString("=", 60) + "\n");
                }
            }
        }
        
        if (!WiFi.isConnected() && !apModeActive && !isConnecting && 
            (now - lastRetryTime > RETRY_INTERVAL)) {
            Serial.println("🔄 Retry automático...");
            if (!tryAutoConnect()) {
                Serial.println("❌ Falha no retry - Ativando AP...");
                startAPMode(); // ✅ VOLTAR PARA AP SE RETRY FALHAR
            }
            lastRetryTime = now;
        }
    }
    
    // ===== GETTERS ESSENCIAIS =====
    bool isConnected() const { return WiFi.status() == WL_CONNECTED; }
    String getStationIP() const { return WiFi.localIP().toString(); }
    String getDeviceID() const { 
        // Si deviceID está vacío, generarlo ahora
        if (deviceID.isEmpty()) {
            const_cast<WiFiManager*>(this)->deviceID = ::getDeviceID();
        }
        return deviceID; 
    }
    String getFirmwareVersion() const { return firmwareVersion; }
    bool isInAPMode() const { return apModeActive; }
    
    // ===== MÉTODOS NECESSÁRIOS =====
    void onConnection(std::function<void(bool)> callback) { onConnectionResult = callback; }
    void resetSettings() { preferences.clear(); }
};

#endif // WIFI_MANAGER_H