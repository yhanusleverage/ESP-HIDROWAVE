#include "WebServerManager.h"
#include "Config.h"
#include "MasterSlaveManager.h"  // ✅ INTEGRAÇÃO ESP-NOW - Usar MasterSlaveManager
#include "ESPNowController.h"   // ✅ Para macToString()
#include "WebServerTask.h"  // ✅ USAR WEBSERVER TASK (Core 1)
#include "DeviceID.h"  // ✅ Para getDeviceID() global
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
    webServerTask(nullptr),  // ✅ USAR WEBSERVER TASK (Core 1)
    isRunning(false),
    systemStatus(nullptr),
    sensorData(nullptr),
    relayStates(nullptr),
    masterManager(nullptr),  // ✅ INTEGRAÇÃO ESP-NOW - Usar MasterSlaveManager
    tempRef(nullptr),
    phRef(nullptr),
    tdsRef(nullptr),
    commandQueue(nullptr),           // ✅ TÓPICO 2: Queue inicializada en initQueueAndMutex()
    systemCacheMutex(nullptr),       // ✅ TÓPICO 2: Mutex inicializado en initQueueAndMutex()
    requestIdCounter(0),              // ✅ TÓPICO 2: Contador inicializado
    requestIdMutex(nullptr) {        // ✅ TÓPICO 2: Mutex inicializado en initQueueAndMutex()
}

WebServerManager::~WebServerManager() {
    cleanupQueueAndMutex();  // ✅ TÓPICO 2: Limpiar Queue y Mutex
    
    if (server) {
        delete server;
    }
    if (adminServer) {
        delete adminServer;
    }
}

void WebServerManager::beginAdminServer(WiFiManager& wifiManager, HydroControl& hydroControl, WebServerTask* webTask, MasterSlaveManager* masterMgr) {
    Serial.println("\n🌐 ========================================");
    Serial.println("🌐 WebServerManager::beginAdminServer()");
    Serial.println("🌐 ========================================");
    Serial.printf("   webTask: %s\n", webTask ? "✅ Disponível" : "❌ nullptr");
    Serial.printf("   webTask->isInitialized(): %s\n", (webTask && webTask->isInitialized()) ? "✅ SIM" : "❌ NÃO");
    Serial.printf("   masterMgr: %s\n", masterMgr ? "✅ Disponível" : "❌ nullptr");
    Serial.printf("   Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.println("========================================\n");
    
    if (!webTask || !webTask->isInitialized()) {
        Serial.println("❌ WebServerTask no inicializado - no se puede configurar endpoints");
        Serial.println("   ⚠️ Endpoints NÃO serão registrados!");
        return;
    }
    
    // ✅ TÓPICO 2: Inicializar Queue y Mutex (como MASTER-TASK)
    Serial.println("\n🔧 ========================================");
    Serial.println("🔧 INICIALIZANDO QUEUE Y MUTEX");
    Serial.println("🔧 ========================================");
    if (!initQueueAndMutex()) {
        Serial.println("❌ Error al inicializar Queue y Mutex");
        return;
    }
    Serial.println("✅ Queue y Mutex inicializados correctamente");
    Serial.println("========================================\n");
    
    // ✅ Guardar referencia a MasterSlaveManager
    masterManager = masterMgr;
    Serial.printf("🔍 [beginAdminServer] masterManager recebido: %s\n", masterManager ? "✅ Disponível" : "❌ nullptr");
    if (!masterManager) {
        Serial.println("⚠️ [beginAdminServer] AVISO CRÍTICO: masterManager é nullptr!");
        Serial.println("   Endpoint /api/slaves retornará array vazio");
        Serial.println("   Verifique ordem de inicialização: masterManager->begin() ANTES de HydroSystemCore::begin()");
    }
    
    // ✅ CORREÇÃO CRÍTICA: Guardar referências a wifiManager e hydroControl
    this->wifiManager = &wifiManager;
    this->hydroControl = &hydroControl;
    
    // ✅ GUARDAR REFERENCIA A WEBSERVER TASK (Core 1)
    this->webServerTask = webTask;
    
    // ✅ INTEGRAÇÃO ESP-NOW - Usar MasterSlaveManager
    if (masterManager) {
        Serial.println("✅ WebServerManager: ESP-NOW integrado via MasterSlaveManager!");
    } else {
        Serial.println("⚠️ WebServerManager: ESP-NOW não disponível (modo standalone)");
    }
    
    Serial.println("🌐 Configurando endpoints en WebServerTask (Core 1)...");
    Serial.println("   ✓ Todas las APIs, endpoints web y Supabase correrán en Core 1");
    
    // ✅ OBTENER SERVIDOR DE WEBSERVER TASK (Core 1)
    AsyncWebServer* server = webTask->getServer();
    if (!server) {
        Serial.println("❌ Error: WebServerTask no tiene servidor");
        return;
    }
    
    // ✅ API para informações do dispositivo (Core 1)
    // ✅ ENDPOINT DESABILITADO - Usando Supabase (device_status)
    // Frontend lê de: supabase.from('device_status')
    // ESP32 escreve em: sendDeviceStatusToSupabase() (cada 60s)
#if 0
    Serial.println("🔧 [WebServerManager] Registrando endpoint /api/device-info");
    webTask->addEndpoint("/api/device-info", HTTP_GET, [this](AsyncWebServerRequest *request) {
        // ✅ WRAPPER DE SEGURANÇA: Garantir resposta sempre
        if (!request) {
            Serial.println("❌ [API] Request é nullptr em /api/device-info!");
            return;
        }
        
        Serial.println("\n📡 [API] /api/device-info solicitado");
        
        // ✅ Proteção contra heap baixo
        if (ESP.getFreeHeap() < 5000) {
            Serial.println("❌ [API] Heap muito baixo!");
            request->send(500, "application/json", "{\"error\":\"Insufficient memory\",\"free_heap\":" + String(ESP.getFreeHeap()) + "}");
            return;
        }
        
        DynamicJsonDocument doc(512);
        
        // ✅ Verificar se documento foi criado
        if (doc.capacity() == 0) {
            Serial.println("❌ [API] Falha ao criar DynamicJsonDocument!");
            request->send(500, "application/json", "{\"error\":\"JSON allocation failed\"}");
            return;
        }
        
        // Debug: obtener device_id con logs
        Serial.printf("🔍 [API] wifiManager é nullptr? %s\n", this->wifiManager ? "NÃO" : "SIM");
        
        String deviceId = "N/A";
        String firmwareVersion = "N/A";
        String ipAddress = WiFi.localIP().toString();
        bool connected = WiFi.isConnected();
        
        if (this->wifiManager) {
            deviceId = this->wifiManager->getDeviceID();
            firmwareVersion = this->wifiManager->getFirmwareVersion();
            String stationIP = this->wifiManager->getStationIP();
            if (stationIP.length() > 0) {
                ipAddress = stationIP;
            }
            connected = this->wifiManager->isConnected();
        } else {
            // ✅ FALLBACK: Usar DeviceID global se disponível
            deviceId = getDeviceID();  // Função global
            Serial.println("⚠️ [API] wifiManager é nullptr, usando DeviceID global");
        }
        
        Serial.printf("🔍 WebServer: Device ID: %s\n", deviceId.c_str());
        Serial.printf("🔍 WebServer: MAC Address: %s\n", WiFi.macAddress().c_str());
        Serial.printf("🔍 WebServer: IP Address: %s\n", ipAddress.c_str());
        
        doc["device_id"] = deviceId;
        doc["firmware_version"] = firmwareVersion;
        doc["ip_address"] = ipAddress;
        doc["connected"] = connected;
        doc["uptime"] = millis() / 1000;
        doc["free_heap"] = ESP.getFreeHeap();
        
        String response;
        size_t bytesWritten = serializeJson(doc, response);
        if (bytesWritten == 0 || response.length() == 0) {
            Serial.println("❌ [API] Erro ao serializar JSON em /api/device-info");
            request->send(500, "application/json", "{\"error\":\"JSON serialization failed\"}");
            return;
        }
        
        // ✅ Verificar se response é válido
        if (response.length() == 0 || response.charAt(0) != '{') {
            Serial.println("❌ [API] Resposta inválida! Enviando fallback.");
            response = "{\"error\":\"Invalid response\",\"device_id\":\"N/A\",\"free_heap\":" + String(ESP.getFreeHeap()) + "}";
        }
        
        Serial.printf("✅ [API] Resposta JSON: %d bytes\n", response.length());
        Serial.printf("🔍 WebServer: Respuesta JSON: %s\n", response.c_str());
        request->send(200, "application/json", response);
    });
    Serial.println("✅ [WebServerManager] Endpoint /api/device-info registrado");
#endif
    
    // ✅ ENDPOINT DESABILITADO - Usando Supabase (hydro_measurements + environment_data)
    // Frontend lê de: supabase.from('hydro_measurements') e supabase.from('environment_data')
    // ESP32 escreve em: sendSensorDataToSupabase() (cada 30s)
#if 0
    // ✅ API para sensores (COMPATÍVEL COM index.html) - Core 1
    webTask->addEndpoint("/api/sensors", HTTP_GET, [this](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(512);
        doc["temperature"] = this->hydroControl ? this->hydroControl->getTemperature() : 0.0;
        doc["humidity"] = 65.0; // Simulated - implementar DHT22 se necessário
        doc["ph"] = this->hydroControl ? this->hydroControl->getpH() : 0.0;
        doc["ec"] = this->hydroControl ? this->hydroControl->getEC() : 0.0;
        doc["water_level_ok"] = this->hydroControl ? this->hydroControl->isWaterLevelOk() : false;
        doc["temp_water"] = this->hydroControl ? this->hydroControl->getTemperature() : 0.0; // Mesmo sensor por enquanto
        doc["timestamp"] = millis();
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
#endif
    
    // ✅ API para EC Controller - GET /api/ec-controller/config
    webTask->addEndpoint("/api/ec-controller/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!this->hydroControl) {
            request->send(500, "application/json", "{\"error\":\"HydroControl not available\"}");
            return;
        }
        
        DynamicJsonDocument doc(512);
        auto& ecController = this->hydroControl->getECController();
        
        doc["baseDose"] = ecController.getBaseDose();
        doc["flowRate"] = ecController.getFlowRate();
        doc["volume"] = ecController.getVolume();
        doc["totalMl"] = ecController.getTotalMl();
        doc["kp"] = ecController.getKp();
        doc["ecSetpoint"] = this->hydroControl->getECSetpoint();
        doc["ecActual"] = this->hydroControl->getEC();
        doc["autoEnabled"] = this->hydroControl->isAutoECEnabled();
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    // ✅ API para EC Controller - POST /api/ec-controller/config
    webTask->addPostEndpoint("/api/ec-controller/config", 
        [this](AsyncWebServerRequest *request) {
            // Handler de request (vacío, el body se procesa en onBody)
        },
        nullptr,  // onUpload
        [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (index != 0) return; // Processar apenas o primeiro chunk
            
            if (!this->hydroControl) {
                request->send(500, "application/json", "{\"error\":\"HydroControl not available\"}");
                return;
            }
            
            // Parsear JSON del body
            String body = String((char*)data).substring(0, len);
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, body);
            
            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            
            JsonVariant json = doc.as<JsonVariant>();
            auto& ecController = this->hydroControl->getECController();
            
            // Atualizar parâmetros do controller (apenas os enviados)
            bool configChanged = false;
            
            if (json.containsKey("baseDose")) {
                ecController.setBaseDose(json["baseDose"]);
                configChanged = true;
            }
            if (json.containsKey("flowRate")) {
                ecController.setFlowRate(json["flowRate"]);
                configChanged = true;
            }
            if (json.containsKey("volume")) {
                ecController.setVolume(json["volume"]);
                configChanged = true;
            }
            if (json.containsKey("totalMl")) {
                ecController.setTotalMl(json["totalMl"]);
                configChanged = true;
            }
            if (json.containsKey("kp")) {
                ecController.setKp(json["kp"]);
                configChanged = true;
            }
            if (json.containsKey("ecSetpoint")) {
                this->hydroControl->setECSetpoint(json["ecSetpoint"]);  // Já salva automaticamente
            }
            if (json.containsKey("autoEnabled")) {
                this->hydroControl->setAutoECEnabled(json["autoEnabled"]);  // Já salva automaticamente
            }
            
            // ✅ Salvar configuração no NVS para persistência (se algum parâmetro mudou)
            if (configChanged) {
                this->hydroControl->saveECControllerConfig();
            }
            
            request->send(200, "application/json", "{\"success\":true}");
        }
    );
    
    // ✅ API para atualizar proporções da tabela nutricional
    webTask->addPostEndpoint("/api/ec-controller/nutrient-proportions",
        [this](AsyncWebServerRequest *request) {
            // Handler de request (vacío, el body se procesa en onBody)
        },
        nullptr,  // onUpload
        [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (index != 0) return; // Processar apenas o primeiro chunk
            
            if (!this->hydroControl) {
                request->send(500, "application/json", "{\"error\":\"HydroControl not available\"}");
                return;
            }
            
            // Parsear JSON del body
            String body = String((char*)data).substring(0, len);
            DynamicJsonDocument doc(2048);
            DeserializationError error = deserializeJson(doc, body);
            
            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            
            JsonVariant json = doc.as<JsonVariant>();
            
            // Receber array de nutrientes com mlPerLiter
            if (json.containsKey("nutrients") && json["nutrients"].is<JsonArray>()) {
                JsonArray nutrients = json["nutrients"].as<JsonArray>();
                this->hydroControl->updateNutrientProportions(nutrients);
                request->send(200, "application/json", "{\"success\":true}");
            } else {
                request->send(400, "application/json", "{\"error\":\"Missing nutrients array\"}");
            }
        }
    );
    
    // ✅ TÓPICO 3: API para calcular u(t) sem executar (preview)
    webTask->addPostEndpoint("/api/ec-controller/calculate",
        [this](AsyncWebServerRequest *request) {
            // Handler de request (vacío, el body se procesa en onBody)
        },
        nullptr,  // onUpload
        [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (index != 0) return; // Processar apenas o primeiro chunk
            
            if (!this->hydroControl) {
                request->send(500, "application/json", "{\"error\":\"HydroControl not available\"}");
                return;
            }
            
            // Parsear JSON del body
            String body = String((char*)data).substring(0, len);
            DynamicJsonDocument doc(1024);
            DeserializationError jsonError = deserializeJson(doc, body);
            
            if (jsonError) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            
            JsonVariant json = doc.as<JsonVariant>();
            
            // Obter parâmetros (opcional - pode usar valores atuais do controller)
            float ecSetpoint = json.containsKey("ecSetpoint") ? json["ecSetpoint"].as<float>() : this->hydroControl->getECSetpoint();
            float ecActual = json.containsKey("ecActual") ? json["ecActual"].as<float>() : this->hydroControl->getEC();
            
            // Obter ECController
            auto& ecController = this->hydroControl->getECController();
            
            // Calcular u(t)
            float ut = ecController.calculateDosage(ecSetpoint, ecActual);
            float ecError = ecSetpoint - ecActual;
            // Calcular k = baseDose / totalMl (mesma fórmula do Controller)
            float k = (ecController.getTotalMl() > 0) ? (ecController.getBaseDose() / ecController.getTotalMl()) : 1.0;
            float dosageTime = ecController.calculateDosageTime(ut);
            
            // Calcular distribuição proporcional (se houver proporções configuradas)
            DynamicJsonDocument responseDoc(2048);
            responseDoc["ut"] = ut;
            responseDoc["error"] = ecError;
            responseDoc["k"] = k;
            responseDoc["dosageTime"] = dosageTime;
            responseDoc["ecSetpoint"] = ecSetpoint;
            responseDoc["ecActual"] = ecActual;
            responseDoc["flowRate"] = ecController.getFlowRate();
            responseDoc["volume"] = ecController.getVolume();
            responseDoc["kp"] = ecController.getKp();
            
            // Calcular distribuição proporcional baseada nas proporções dinâmicas
            JsonArray distribution = responseDoc.createNestedArray("distribution");
            
            // Obter proporções dinâmicas do HydroControl (se disponível)
            // Nota: Precisamos acessar as proporções dinâmicas - por enquanto retornar estrutura vazia
            // O frontend calculará a distribuição baseado nas proporções que ele já tem
            
            String response;
            serializeJson(responseDoc, response);
            request->send(200, "application/json", response);
        }
    );
    
    // ✅ TÓPICO 3: API para executar dosagem proporcional
    webTask->addPostEndpoint("/api/ec-controller/execute",
        [this](AsyncWebServerRequest *request) {
            // Handler de request (vacío, el body se procesa en onBody)
        },
        nullptr,  // onUpload
        [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (index != 0) return; // Processar apenas o primeiro chunk
            
            if (!this->hydroControl) {
                request->send(500, "application/json", "{\"error\":\"HydroControl not available\"}");
                return;
            }
            
            // Verificar se já há uma dosagem ativa
            if (this->hydroControl->isDosageActive()) {
                request->send(409, "application/json", "{\"error\":\"Dosage already active\",\"message\":\"A dosagem já está em execução. Aguarde ou cancele a dosagem atual.\"}");
                return;
            }
            
            // Parsear JSON del body
            String body = String((char*)data).substring(0, len);
            DynamicJsonDocument doc(2048);
            DeserializationError error = deserializeJson(doc, body);
            
            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            
            JsonVariant json = doc.as<JsonVariant>();
            
            // Opção 1: Executar com u(t) calculado e distribuição proporcional
            if (json.containsKey("ut") && json.containsKey("distribution") && json["distribution"].is<JsonArray>()) {
                float ut = json["ut"].as<float>();
                JsonArray distribution = json["distribution"].as<JsonArray>();
                int intervalo = json.containsKey("interval") ? json["interval"].as<int>() : 3;
                
                // Executar dosagem usando executeWebDosage
                this->hydroControl->executeWebDosage(distribution, intervalo);
                
                DynamicJsonDocument responseDoc(512);
                responseDoc["success"] = true;
                responseDoc["ut"] = ut;
                responseDoc["message"] = "Dosagem iniciada com sucesso";
                
                String response;
                serializeJson(responseDoc, response);
                request->send(200, "application/json", response);
                return;
            }
            
            // Opção 2: Calcular u(t) automaticamente e executar
            if (json.containsKey("ecSetpoint") && json.containsKey("ecActual")) {
                float ecSetpoint = json["ecSetpoint"].as<float>();
                float ecActual = json["ecActual"].as<float>();
                
                auto& ecController = this->hydroControl->getECController();
                float ut = ecController.calculateDosage(ecSetpoint, ecActual);
                
                if (ut <= 0.0) {
                    request->send(400, "application/json", "{\"error\":\"No dosage needed\",\"message\":\"EC atual está acima ou igual ao setpoint\"}");
                    return;
                }
                
                // Usar startSimpleSequentialDosage que distribui automaticamente
                this->hydroControl->startSimpleSequentialDosage(ut, ecSetpoint, ecActual);
                
                DynamicJsonDocument responseDoc(512);
                responseDoc["success"] = true;
                responseDoc["ut"] = ut;
                responseDoc["message"] = "Dosagem calculada e iniciada automaticamente";
                
                String response;
                serializeJson(responseDoc, response);
                request->send(200, "application/json", response);
                return;
            }
            
            // Erro: parâmetros insuficientes
            request->send(400, "application/json", "{\"error\":\"Missing parameters\",\"message\":\"Forneça 'ut' e 'distribution' OU 'ecSetpoint' e 'ecActual'\"}");
        }
    );
    
    // ✅ ENDPOINT DESABILITADO - Usando Supabase (relay_states)
    // Frontend lê de: supabase.from('relay_states').eq('relay_type', 'local')
    // ESP32 escreve em: sendDeviceStatusToSupabase() (relay_states array) + trigger SQL
#if 0
    // ✅ API para relés (COMPATÍVEL COM index.html) - Core 1
    webTask->addEndpoint("/api/relays", HTTP_GET, [this](AsyncWebServerRequest *request) {
        // ✅ NOVO: Verificar se há parâmetro ?refresh=true para forçar atualização
        bool shouldRefresh = false;
        if (request->hasParam("refresh")) {
            String refreshParam = request->getParam("refresh")->value();
            shouldRefresh = (refreshParam == "true" || refreshParam == "1");
        }
        
        // ✅ CRÍTICO: Solicitar atualização de estados de slaves se necessário
        if ((shouldRefresh || this->shouldRefreshSlaveStates()) && this->masterManager) {
            Serial.println("🔄 [API /relays] Solicitando atualização de estados dos slaves...");
            this->masterManager->requestAllSlavesRelayStatus();
        }
        
        DynamicJsonDocument doc(2048); // ✅ AUMENTADO: Para incluir slaves
        JsonArray relays = doc.createNestedArray("relays");
        
        // ✅ PASSO 1: Adicionar relés LOCAIS do Master (0-15)
        bool* relayStates = this->hydroControl ? this->hydroControl->getRelayStates() : nullptr;
        if (relayStates) {
            for (int i = 0; i < 16; i++) {
                JsonObject relay = relays.createNestedObject();
                relay["id"] = i;
                relay["state"] = relayStates[i];
                relay["name"] = this->getRelayName(i);
                relay["type"] = "local"; // ✅ NOVO: Indicar que é relé local
            }
        }
        
        // ✅ PASSO 2: Adicionar relés dos SLAVES (se masterManager disponível)
        if (this->masterManager) {
            std::vector<TrustedSlave> slaves = this->masterManager->getAllTrustedSlaves();
            for (const auto& slave : slaves) {
                if (slave.isOnline()) {
                    for (int i = 0; i < slave.numRelays && i < 8; i++) {
                        JsonObject relay = relays.createNestedObject();
                        // ✅ ID único: slave_mac + relay_number (ex: "14:33:5C:38:BF:60_0")
                        String uniqueId = ESPNowController::macToString(slave.macAddress) + "_" + String(i);
                        uniqueId.replace(":", "");
                        relay["id"] = uniqueId;
                        relay["state"] = slave.relayStates[i].state;
                        relay["name"] = slave.relayStates[i].name.length() > 0 
                            ? slave.relayStates[i].name 
                            : (slave.deviceName + " - Relé " + String(i + 1));
                        relay["type"] = "slave"; // ✅ NOVO: Indicar que é relé de slave
                        relay["slave_mac"] = ESPNowController::macToString(slave.macAddress);
                        relay["slave_name"] = slave.deviceName;
                        relay["relay_number"] = i;
                        relay["has_timer"] = slave.relayStates[i].hasTimer;
                        relay["remaining_time"] = slave.relayStates[i].remainingTime;
                        // ✅ NOVO: Información de confiabilidad del estado
                        unsigned long age = (slave.relayStates[i].lastUpdate == 0) 
                            ? ULONG_MAX 
                            : (millis() - slave.relayStates[i].lastUpdate);
                        relay["state_age_ms"] = (slave.relayStates[i].lastUpdate == 0) ? -1 : (int)age;
                        relay["state_reliable"] = (slave.relayStates[i].lastUpdate != 0 && age < 10000);
                    }
                }
            }
        }
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
#endif
    
    // ✅ ENDPOINT DESABILITADO - Usando Supabase (relay_commands)
    // Frontend escreve em: supabase.from('relay_commands').insert({...})
    // ESP32 lê de: checkForCommands() (cada 30s)
#if 0
    // ✅ CRÍTICO: Registrar /api/relay/slave ANTES de /api/relay para evitar conflito de roteamento
    // ✅ NUEVO: API SIMPLE Y DIRECTA para comandos de slaves (sin pasar por Supabase)
    // Endpoint: POST /api/relay/slave
    // Body: { "slave_mac": "14:33:5C:38:BF:60", "relay_number": 2, "action": "on" }
    Serial.println("🔧 [WebServerManager] Registrando endpoint /api/relay/slave...");
    Serial.printf("   webTask: %s\n", webTask ? "✅ Disponível" : "❌ nullptr");
    Serial.printf("   webTask->isInitialized(): %s\n", webTask && webTask->isInitialized() ? "✅ SIM" : "❌ NÃO");
    Serial.printf("   masterManager: %s\n", this->masterManager ? "✅ Disponível" : "❌ nullptr");
    
    webTask->addPostEndpoint("/api/relay/slave",
        [this](AsyncWebServerRequest *request) {
            // ✅ DEBUG: Log quando requisição chega
            Serial.println("\n🚨 ========================================");
            Serial.println("🚨 POST /api/relay/slave RECEBIDO!");
            Serial.println("🚨 ========================================");
            Serial.printf("   URL: %s\n", request->url().c_str());
            Serial.printf("   Method: %s\n", request->methodToString());
            Serial.printf("   Host: %s\n", request->host().c_str());
            Serial.printf("   Remote IP: %s\n", request->client()->remoteIP().toString().c_str());
            Serial.printf("   masterManager pointer: %p\n", this->masterManager);
            Serial.printf("   masterManager é nullptr? %s\n", this->masterManager ? "❌ NÃO" : "✅ SIM (PROBLEMA!)");
            AsyncWebHeader* contentTypeHeader = request->getHeader("Content-Type");
            if (contentTypeHeader) {
                Serial.printf("   Content-Type: %s\n", contentTypeHeader->value().c_str());
            }
            AsyncWebHeader* contentLengthHeader = request->getHeader("Content-Length");
            if (contentLengthHeader) {
                Serial.printf("   Content-Length: %s\n", contentLengthHeader->value().c_str());
            }
            Serial.println("   ⏳ Aguardando body...");
            Serial.println("========================================\n");
            
            // ✅ CRÍTICO: Verificar se request está válido
            if (!request) {
                Serial.println("❌ [API] Request é nullptr no onRequest handler!");
                return;
            }
            
            // ✅ CRÍTICO: Verificar se client está conectado
            if (!request->client() || !request->client()->connected()) {
                Serial.println("❌ [API] Client não está conectado!");
                request->send(503, "application/json", "{\"error\":\"Client disconnected\"}");
                return;
            }
            
            // Handler vacío - procesamos en onBody
        },
        nullptr,  // onUpload
        [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            Serial.printf("\n📦 [DEBUG] onBody chamado: index=%u, len=%u, total=%u\n", index, len, total);
            
            // ✅ CRÍTICO: Verificar se request está válido
            if (!request) {
                Serial.println("❌ [API] Request é nullptr no onBody handler!");
                return;
            }
            
            // ✅ CRÍTICO: Verificar se client está conectado
            if (!request->client() || !request->client()->connected()) {
                Serial.println("❌ [API] Client desconectado durante recebimento do body!");
                return;
            }
            
            if (index != 0) {
                Serial.printf("   ⏭️  Ignorando chunk (index != 0)\n");
                return; // Solo procesar primer chunk
            }
            
            Serial.println("\n📡 [API] /api/relay/slave - Comando directo recibido");
            Serial.printf("🔍 [DEBUG] Body recebido: len=%u, index=%u, total=%u\n", len, index, total);
            
            // ✅ CRÍTICO: Verificar se temos dados
            if (len == 0 || total == 0) {
                Serial.println("❌ [API] Body vazio ou inválido!");
                request->send(400, "application/json", "{\"error\":\"Empty body\"}");
                return;
            }
            
            if (len > 0) {
                Serial.printf("   Primeiros 200 chars do body: ");
                size_t maxPrint = (len < 200) ? len : 200;
                for (size_t i = 0; i < maxPrint; i++) {
                    Serial.printf("%c", data[i]);
                }
                Serial.println();
            }
            
            // Parsear JSON
            DynamicJsonDocument doc(512);
            DeserializationError error = deserializeJson(doc, (char*)data, len);
            
            if (error) {
                Serial.printf("❌ [API] Error al parsear JSON: %s\n", error.c_str());
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            
            // Validar parámetros
            if (!doc.containsKey("slave_mac") || !doc.containsKey("relay_number") || !doc.containsKey("action")) {
                request->send(400, "application/json", "{\"error\":\"Missing required fields: slave_mac, relay_number, action\"}");
                return;
            }
            
            String slaveMacStr = doc["slave_mac"].as<String>();
            int relayNumber = doc["relay_number"].as<int>();
            String action = doc["action"].as<String>();
            int duration = doc.containsKey("duration_seconds") ? doc["duration_seconds"].as<int>() : 0;
            
            // Validar relay_number
            if (relayNumber < 0 || relayNumber > 7) {
                request->send(400, "application/json", "{\"error\":\"relay_number must be 0-7\"}");
                return;
            }
            
            // Validar action
            if (action != "on" && action != "off") {
                request->send(400, "application/json", "{\"error\":\"action must be 'on' or 'off'\"}");
                return;
            }
            
            // ✅ DEBUG: Verificar MasterSlaveManager con logging detallado
            Serial.println("\n🔍 [DEBUG] Verificando masterManager...");
            Serial.printf("   masterManager pointer: %p\n", this->masterManager);
            Serial.printf("   this pointer: %p\n", this);
            Serial.printf("   webServerTask pointer: %p\n", this->webServerTask);
            
            if (!this->masterManager) {
                Serial.println("\n❌ ========================================");
                Serial.println("❌ [API] masterManager é nullptr!");
                Serial.println("❌ ========================================");
                Serial.println("   Possíveis causas:");
                Serial.println("   1. MasterSlaveManager não foi inicializado");
                Serial.println("   2. WebServerManager::beginAdminServer() foi chamado com masterMgr=nullptr");
                Serial.println("   3. Problema na ordem de inicialização");
                Serial.println("   4. WebServerManager foi recriado sem masterManager");
                Serial.println("   5. masterManager foi deletado após inicialização");
                Serial.println("========================================\n");
                
                DynamicJsonDocument errorDoc(512);
                errorDoc["error"] = "ESP-NOW not available";
                errorDoc["message"] = "MasterSlaveManager not initialized";
                errorDoc["debug"] = "masterManager is nullptr - check initialization order";
                errorDoc["masterManager_pointer"] = "0x00000000";
                errorDoc["this_pointer"] = String((uint32_t)this, HEX);
                errorDoc["webServerTask_pointer"] = this->webServerTask ? String((uint32_t)this->webServerTask, HEX) : "nullptr";
                String errorResponse;
                serializeJson(errorDoc, errorResponse);
                request->send(503, "application/json", errorResponse);
                return;
            }
            Serial.println("✅ [API] masterManager está disponível");
            Serial.printf("   masterManager pointer válido: %p\n", this->masterManager);
            
            // Convertir MAC string a bytes
            uint8_t slaveMac[6];
            if (!ESPNowController::stringToMac(slaveMacStr, slaveMac)) {
                Serial.printf("❌ [API] MAC inválido: %s\n", slaveMacStr.c_str());
                request->send(400, "application/json", "{\"error\":\"Invalid MAC address format\"}");
                return;
            }
            
            Serial.printf("📤 [API] Enviando comando: slave=%s, relay=%d, action=%s\n", 
                         slaveMacStr.c_str(), relayNumber, action.c_str());
            
            // ✅ NUEVO: Generar command_id para rastrear el comando
            // El Master generará su propio command_id internamente, pero aquí podemos usar un timestamp
            uint32_t commandId = millis(); // ID simple basado en tiempo
            
            // Enviar comando via ESP-NOW
            // Nota: sendRelayCommandToSlave genera su propio command_id internamente
            bool success = this->masterManager->sendRelayCommandToSlave(
                slaveMac, 
                relayNumber, 
                action, 
                duration
            );
            
            // Preparar respuesta
            DynamicJsonDocument response(512);
            response["success"] = success;
            response["slave_mac"] = slaveMacStr;
            response["relay_number"] = relayNumber;
            response["action"] = action;
            response["duration_seconds"] = duration;
            
            if (success) {
                // ✅ NUEVO: Informar que el ACK actualizará Supabase automáticamente
                response["message"] = "Comando enviado via ESP-NOW - ACK actualizará estado en Supabase";
                response["note"] = "El estado será actualizado automáticamente cuando el Slave envíe ACK";
                response["ack_expected"] = true;
                
                String responseStr;
                serializeJson(response, responseStr);
                request->send(200, "application/json", responseStr);
                Serial.printf("✅ [API] Comando enviado con éxito - ACK actualizará Supabase automáticamente\n");
            } else {
                response["error"] = "Failed to send ESP-NOW command";
                response["message"] = "Slave puede estar offline o no encontrado - Comando será enviado cuando vuelva online";
                response["queued"] = true; // ✅ NUEVO: Indica que el comando fue encolado
                
                String responseStr;
                serializeJson(response, responseStr);
                request->send(202, "application/json", responseStr); // ✅ 202 Accepted (comando encolado)
                Serial.printf("⚠️ [API] Comando encolado - será enviado cuando slave vuelva online\n");
            }
        }
    );
    // ✅ NOVO: Handler GET para /api/relay/slave (informação sobre o endpoint)
    // ✅ REGISTRADO ANTES DO POST para melhor organização
    webTask->addEndpoint("/api/relay/slave", HTTP_GET, [this](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(512);
        doc["endpoint"] = "/api/relay/slave";
        doc["method"] = "POST";
        doc["description"] = "Controlar relés de slaves ESP-NOW";
        doc["format"] = "JSON";
        doc["required_fields"] = JsonArray();
        doc["required_fields"].add("slave_mac");
        doc["required_fields"].add("relay_number");
        doc["required_fields"].add("action");
        doc["optional_fields"] = JsonArray();
        doc["optional_fields"].add("duration_seconds");
        doc["example"] = JsonObject();
        doc["example"]["slave_mac"] = "14:33:5C:38:BF:60";
        doc["example"]["relay_number"] = 0;
        doc["example"]["action"] = "on";
        doc["example"]["duration_seconds"] = 0;
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    Serial.println("✅ [WebServerManager] Endpoint GET /api/relay/slave registrado (info)");
    
    Serial.println("✅ [WebServerManager] Endpoint POST /api/relay/slave registrado");
    
    // ✅ TÓPICO 3: API para toggle de relés usando QUEUE (como MASTER-TASK)
    // ✅ MEJORADO: Ahora usa Queue FreeRTOS en lugar de llamadas directas
    webTask->addPostEndpoint("/api/relay",
        [this, &hydroControl](AsyncWebServerRequest *request) {
        // ✅ Validación mejorada (como en MASTER-TASK)
        if (!request->hasParam("relay", true)) {
            request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing relay parameter\"}");
            return;
        }
        
        int relay = request->getParam("relay", true)->value().toInt();
        int duration = 0;
        String deviceId = "";
        String action = "toggle";  // Padrão: toggle
        
        // Validar número de relé
        if (relay < 0 || relay >= 16) {
            request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid relay number (0-15)\"}");
            return;
        }
        
        if (request->hasParam("duration", true)) {
            duration = request->getParam("duration", true)->value().toInt();
            if (duration < 0) duration = 0;  // Validar duración
        }
        
        // ✅ NOVO: Verificar se é relé local ou remoto
        if (request->hasParam("device_id", true)) {
            deviceId = request->getParam("device_id", true)->value();
        }
        
        // Verificar parâmetro action (on/off/toggle)
        if (request->hasParam("action", true)) {
            action = request->getParam("action", true)->value();
            action.toLowerCase();  // Normalizar a minúsculas
        }
        
        // Validar acción
        if (action != "on" && action != "off" && action != "toggle" && action != "on_forever") {
            request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid action (on/off/toggle/on_forever)\"}");
            return;
        }
        
        // ✅ TÓPICO 3: CREAR COMANDO Y ENVIAR A QUEUE (como MASTER-TASK)
        WebCommand cmd;
        cmd.type = WebCommand::RELAY_CONTROL;
        cmd.relayNumber = relay;
        cmd.action = action;
        cmd.duration = duration;
        cmd.deviceId = deviceId;
        cmd.requestId = this->generateRequestId();
        
        // Si es remoto, buscar MAC del slave
        if (!deviceId.isEmpty() && deviceId != "local" && deviceId != "MASTER" && this->masterManager) {
            auto trustedSlaves = this->masterManager->getAllTrustedSlaves();
            for (const auto& slave : trustedSlaves) {
                String slaveDeviceId = "ESP32_SLAVE_" + ESPNowController::macToString(slave.macAddress);
                slaveDeviceId.replace(":", "_");
                if (slaveDeviceId == deviceId || slaveDeviceId.equalsIgnoreCase(deviceId)) {
                    memcpy(cmd.slaveMac, slave.macAddress, 6);
                    break;
                }
            }
        }
        
        // ✅ TÓPICO 3: ENVIAR COMANDO A QUEUE (Core 1 → Core 0)
        if (this->sendCommandToQueue(cmd, 100)) {
            DynamicJsonDocument doc(256);
            doc["status"] = "ok";
            doc["success"] = true;
            doc["message"] = "Command queued successfully";
            doc["request_id"] = cmd.requestId;
            doc["target"] = deviceId.isEmpty() ? "local" : "remote";
            doc["relay"] = relay;
            doc["action"] = action;
            
            String response;
            serializeJson(doc, response);
            request->send(200, "application/json", response);
            
            Serial.printf("✅ TÓPICO 3: Comando encolado: relay=%d, action=%s, deviceId=%s, requestId=%u\n",
                         relay, action.c_str(), deviceId.c_str(), cmd.requestId);
        } else {
            // ✅ CONDICIONAL: Si queue falla, ejecutar directamente (fallback)
            Serial.println("⚠️ Queue llena, ejecutando comando directamente (fallback)");
            
            // ✅ CONDICIONAL: Local vs Remoto (mejorado con manejo de errores)
            if (deviceId.isEmpty() || deviceId == "local" || deviceId == "MASTER") {
                // ===== RELÉ LOCAL (Master) =====
                bool success = false;
                bool newState = false;
                
                try {
                    if (action == "toggle") {
                        if (this->hydroControl) {
                            this->hydroControl->toggleRelay(relay, duration);
                            success = true;
                            newState = this->hydroControl->getRelayStates()[relay];
                        }
                    } else if (action == "on" || action == "on_forever") {
                        if (action == "on_forever") duration = 0;  // Permanente
                        if (this->hydroControl) {
                            this->hydroControl->setRelay(relay, true, duration);
                            success = true;
                            newState = true;
                        }
                    } else if (action == "off") {
                        if (this->hydroControl) {
                            this->hydroControl->setRelay(relay, false, 0);
                            success = true;
                            newState = false;
                        }
                    }
                } catch (...) {
                    success = false;
                }
                
                if (success) {
                    DynamicJsonDocument doc(256);
                    doc["status"] = "ok";
                    doc["success"] = true;
                    doc["target"] = "local";
                    doc["relay"] = relay;
                    doc["action"] = action;
                    doc["new_state"] = newState;
                    doc["duration"] = duration;
                    doc["message"] = "Command executed successfully";
                    
                    String response;
                    serializeJson(doc, response);
                    request->send(200, "application/json", response);
                    
                    Serial.printf("✅ API LOCAL: Relé %d -> %s (action: %s, duration: %d)\n", 
                                 relay, newState ? "ON" : "OFF", action.c_str(), duration);
                } else {
                    DynamicJsonDocument doc(256);
                    doc["status"] = "error";
                    doc["success"] = false;
                    doc["target"] = "local";
                    doc["relay"] = relay;
                    doc["error"] = "Failed to execute command";
                    
                    String response;
                    serializeJson(doc, response);
                    request->send(500, "application/json", response);
                    
                    Serial.printf("❌ API LOCAL: Error al ejecutar comando en relé %d\n", relay);
                }
            } else {
                // ===== RELÉ REMOTO (ESP-NOW Slave) =====
                DynamicJsonDocument doc(256);
                
                if (!this->masterManager) {
                    // ESP-NOW não está disponível
                    doc["status"] = "error";
                    doc["success"] = false;
                    doc["target"] = "remote";
                    doc["device_id"] = deviceId;
                    doc["relay"] = relay;
                    doc["action"] = action;
                    doc["error"] = "ESP-NOW not initialized";
                    doc["message"] = "MasterSlaveManager not initialized";
                    
                    String response;
                    serializeJson(doc, response);
                    request->send(503, "application/json", response);
                    
                    Serial.printf("⚠️ API REMOTO: ESP-NOW não disponível (device_id=%s)\n", deviceId.c_str());
                    return;
                }
                        
                // ✅ Buscar Slave por device_id (formato: ESP32_SLAVE_XX_XX_XX_XX_XX_XX)
                const uint8_t* targetMac = nullptr;
                auto trustedSlaves = this->masterManager->getAllTrustedSlaves();
                for (const auto& slave : trustedSlaves) {
                    // Gerar device_id do slave (igual ao formato usado no /api/slaves)
                    String slaveDeviceId = "ESP32_SLAVE_" + ESPNowController::macToString(slave.macAddress);
                    slaveDeviceId.replace(":", "_");
                    
                    // Comparar com device_id recebido
                    if (slaveDeviceId == deviceId || slaveDeviceId.equalsIgnoreCase(deviceId)) {
                        targetMac = slave.macAddress;
                        Serial.printf("✅ [API] Slave encontrado: %s -> MAC: %s\n", 
                                    deviceId.c_str(), ESPNowController::macToString(slave.macAddress).c_str());
                        break;
                    }
                }
                
                if (!targetMac) {
                    // Slave não encontrado
                    doc["status"] = "error";
                    doc["success"] = false;
                    doc["target"] = "remote";
                    doc["device_id"] = deviceId;
                    doc["relay"] = relay;
                    doc["action"] = action;
                    doc["error"] = "Slave not found";
                    doc["message"] = "Device ID not registered in ESP-NOW";
                    
                    String response;
                    serializeJson(doc, response);
                    request->send(404, "application/json", response);
                    
                    Serial.printf("⚠️ API REMOTO: Slave '%s' não encontrado\n", deviceId.c_str());
                    return;
                }
                
                // Enviar comando via ESP-NOW usando MasterSlaveManager
                bool success = this->masterManager->sendRelayCommandToSlave(targetMac, relay, action, duration);
                
                if (success) {
                    doc["status"] = "ok";
                    doc["success"] = true;
                    doc["target"] = "remote";
                    doc["device_id"] = deviceId;
                    doc["relay"] = relay;
                    doc["action"] = action;
                    doc["duration"] = duration;
                    doc["message"] = "Command sent via ESP-NOW";
                    
                    String response;
                    serializeJson(doc, response);
                    request->send(200, "application/json", response);
                    
                    Serial.printf("✅ API REMOTO: device_id=%s relay=%d action=%s duration=%d (enviado via ESP-NOW)\n",
                                 deviceId.c_str(), relay, action.c_str(), duration);
                } else {
                    doc["status"] = "error";
                    doc["success"] = false;
                    doc["target"] = "remote";
                    doc["device_id"] = deviceId;
                    doc["relay"] = relay;
                    doc["action"] = action;
                    doc["error"] = "Failed to send ESP-NOW command";
                    doc["message"] = "Check slave connection status";
                    
                    String response;
                    serializeJson(doc, response);
                    request->send(500, "application/json", response);
                    
                    Serial.printf("❌ API REMOTO: Falha ao enviar comando para %s (relay=%d, action=%s)\n", 
                                 deviceId.c_str(), relay, action.c_str());
                }
            }
        }
        },
        nullptr,  // onUpload (no usado)
        nullptr   // onBody (no usado - usamos parámetros GET)
    );
#endif  // ✅ FIM DO #if 0 da linha 569
    
    // ✅ ENDPOINT DESABILITADO - Usando Supabase (device_status)
    // Frontend lê de: supabase.from('device_status')
    // ESP32 escreve em: sendDeviceStatusToSupabase() (cada 60s)
#if 0
    // ✅ API para status do sistema (Core 1) - MEJORADO basado en MASTER-TASK
    webTask->addEndpoint("/api/system-status", HTTP_GET, [this](AsyncWebServerRequest *request) {
        // ✅ WRAPPER DE SEGURANÇA: Garantir que sempre retorne JSON válido
        if (!request) {
            Serial.println("❌ [API] Request é nullptr em /api/system-status!");
            return;
        }
        
        Serial.println("\n📡 [API] /api/system-status solicitado");
        
        // ✅ Proteção contra heap baixo
        if (ESP.getFreeHeap() < 5000) {
            Serial.println("❌ [API] Heap muito baixo!");
            request->send(500, "application/json", "{\"error\":\"Insufficient memory\",\"free_heap\":" + String(ESP.getFreeHeap()) + "}");
            return;
        }
        
        DynamicJsonDocument doc(1024);
        
        // ✅ Verificar se o documento foi criado
        if (doc.capacity() == 0) {
            Serial.println("❌ [API] Falha ao criar DynamicJsonDocument!");
            request->send(500, "application/json", "{\"error\":\"JSON allocation failed\"}");
            return;
        }
        
        // Sistema info (similar a MASTER-TASK)
        doc["system"] = "ESP32-HIDROWAVE";
        doc["mode"] = "Master Controller";
        doc["uptime_ms"] = millis();
        doc["uptime_seconds"] = millis() / 1000;
        doc["free_heap"] = ESP.getFreeHeap();
        doc["system_initialized"] = systemInitialized;
        doc["supabase_connected"] = supabaseConnected;
        doc["web_server_running"] = webServerRunning;
        
        // WiFi info (similar a MASTER-TASK)
        doc["wifi_mode"] = "AP+STA";
        doc["sta_connected"] = this->wifiManager ? this->wifiManager->isConnected() : WiFi.isConnected();
        if (this->wifiManager && this->wifiManager->isConnected()) {
            doc["sta_ip"] = this->wifiManager->getStationIP();
            doc["sta_channel"] = WiFi.channel();
            doc["sta_rssi"] = WiFi.RSSI();
            doc["sta_ssid"] = WiFi.SSID();
        } else if (WiFi.isConnected()) {
            doc["sta_ip"] = WiFi.localIP().toString();
            doc["sta_channel"] = WiFi.channel();
            doc["sta_rssi"] = WiFi.RSSI();
            doc["sta_ssid"] = WiFi.SSID();
        } else {
            doc["sta_ip"] = "N/A";
            doc["sta_channel"] = 0;
            doc["sta_rssi"] = 0;
            doc["sta_ssid"] = "N/A";
        }
        doc["ap_ssid"] = "ESP32-HIDROWAVE-AP";
        doc["ap_ip"] = WiFi.softAPIP().toString();
        doc["ap_clients"] = WiFi.softAPgetStationNum();
        
        // ESP-NOW info (si está disponible)
        if (this->masterManager) {
            doc["espnow_available"] = true;
            doc["slaves_total"] = this->masterManager->getTrustedSlaveCount();
            doc["slaves_online"] = this->masterManager->getOnlineSlaveCount();
            doc["slaves_offline"] = this->masterManager->getTrustedSlaveCount() - this->masterManager->getOnlineSlaveCount();
        } else {
            doc["espnow_available"] = false;
            doc["slaves_total"] = 0;
            doc["slaves_online"] = 0;
            doc["slaves_offline"] = 0;
        }
        
        String response;
        size_t bytesWritten = serializeJson(doc, response);
        if (bytesWritten == 0 || response.length() == 0) {
            Serial.println("❌ [API] Erro ao serializar JSON em /api/system-status");
            request->send(500, "application/json", "{\"error\":\"JSON serialization failed\"}");
            return;
        }
        
        Serial.printf("✅ [API] Resposta JSON: %d bytes\n", response.length());
        
        // ✅ Verificar se response é válido antes de enviar
        if (response.length() == 0) {
            Serial.println("❌ [API] Resposta vazia! Enviando fallback.");
            response = "{\"error\":\"Empty response\",\"system\":\"ESP32-HIDROWAVE\",\"free_heap\":" + String(ESP.getFreeHeap()) + "}";
        }
        
        // ✅ Garantir que sempre retorne JSON válido
        if (response.charAt(0) != '{') {
            Serial.println("❌ [API] Resposta não é JSON válido! Enviando fallback.");
            response = "{\"error\":\"Invalid response\",\"system\":\"ESP32-HIDROWAVE\",\"free_heap\":" + String(ESP.getFreeHeap()) + "}";
        }
        
        request->send(200, "application/json", response);
    });
#endif
    
    // ✅ ENDPOINT DESABILITADO - Usando Supabase (device_status)
    // Frontend lê de: supabase.from('device_status')
    // ESP32 escreve em: sendDeviceStatusToSupabase() (cada 60s)
#if 0
    // ✅ TÓPICO 4: API /api/status usando CACHE (como MASTER-TASK)
    webTask->addEndpoint("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
        // ✅ TÓPICO 4: Obtener cache del sistema (thread-safe, leído desde Core 1)
        SystemDataCache cache = this->getSystemCache();
        
        DynamicJsonDocument doc(1024);
        
        // Sistema info (del cache)
        doc["system"] = "ESP32-HIDROWAVE";
        doc["mode"] = "Master Controller";
        doc["uptime_ms"] = cache.uptime > 0 ? cache.uptime : millis();
        doc["free_heap"] = cache.freeHeap > 0 ? cache.freeHeap : ESP.getFreeHeap();
        
        // WiFi info (del cache o actual)
        doc["wifi_mode"] = "AP+STA";
        bool wifiConnected = cache.wifiConnected || (this->wifiManager && this->wifiManager->isConnected());
        doc["sta_connected"] = wifiConnected;
        if (wifiConnected) {
            doc["sta_ip"] = cache.wifiIP.length() > 0 ? cache.wifiIP : (this->wifiManager ? this->wifiManager->getStationIP() : "N/A");
            doc["sta_channel"] = cache.wifiChannel > 0 ? cache.wifiChannel : WiFi.channel();
            doc["sta_rssi"] = cache.wifiRSSI != 0 ? cache.wifiRSSI : WiFi.RSSI();
        }
        doc["ap_ssid"] = "ESP32-HIDROWAVE-AP";
        doc["ap_ip"] = WiFi.softAPIP().toString();
        doc["ap_clients"] = WiFi.softAPgetStationNum();
        
        // ESP-NOW info (del cache)
        doc["slaves_total"] = cache.totalSlaves;
        doc["slaves_online"] = cache.onlineSlaves;
        doc["slaves_offline"] = cache.offlineSlaves;
        
        // Sistema info adicional
        doc["system_initialized"] = cache.systemInitialized;
        doc["supabase_connected"] = cache.supabaseConnected;
        doc["web_server_running"] = cache.webServerRunning;
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
#endif
    
    // ✅ ENDPOINT DESABILITADO - Usando Supabase (device_status.last_seen)
    // Frontend verifica: device_status.last_seen (últimos 60s = conectado)
#if 0
    // ✅ API de compatibilidad (manteniendo endpoints antiguos) - Core 1
    webTask->addEndpoint("/api/supabase-status", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(128);
        doc["connected"] = supabaseConnected;
        doc["status"] = supabaseConnected ? "connected" : "disconnected";
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
#endif
    
    // ✅ ENDPOINT DESABILITADO - Descoberta automática via Supabase
    // Slaves se registram automaticamente em device_status e relay_states
#if 0
    // ✅ TÓPICO 3: API para discovery de slaves usando QUEUE (similar a MASTER-TASK)
    webTask->addEndpoint("/api/discover", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!this->masterManager) {
            DynamicJsonDocument doc(256);
            doc["status"] = "error";
            doc["message"] = "ESP-NOW not available";
            String response;
            serializeJson(doc, response);
            request->send(503, "application/json", response);
            return;
        }
        
        // ✅ TÓPICO 3: Crear comando y enviar a queue
        WebCommand cmd;
        cmd.type = WebCommand::DISCOVER_SLAVES;
        cmd.requestId = this->generateRequestId();
        
        if (this->sendCommandToQueue(cmd, 100)) {
            DynamicJsonDocument doc(256);
            doc["status"] = "ok";
            doc["message"] = "Discovery started (queued)";
            doc["request_id"] = cmd.requestId;
            
            String response;
            serializeJson(doc, response);
            request->send(200, "application/json", response);
            
            Serial.println("✅ TÓPICO 3: Comando DISCOVER encolado");
        } else {
            DynamicJsonDocument doc(256);
            doc["status"] = "error";
            doc["message"] = "Queue full - command not queued";
            
            String response;
            serializeJson(doc, response);
            request->send(503, "application/json", response);
            
            Serial.println("❌ TÓPICO 3: Queue llena para DISCOVER");
        }
    });
#endif
    
    // ✅ ENDPOINT DESABILITADO - Usando Supabase (relay_commands múltiplos)
    // Frontend cria múltiplos comandos: for (i=0; i<16; i++) createRelayCommand(...)
#if 0
    // ✅ TÓPICO 3: API para controlar todos los relays usando QUEUE (similar a MASTER-TASK)
    webTask->addEndpoint("/api/all/on", HTTP_POST, [this](AsyncWebServerRequest *request) {
        // ✅ TÓPICO 3: Crear comando y enviar a queue
        WebCommand cmd;
        cmd.type = WebCommand::ALL_RELAYS_ON;
        cmd.requestId = this->generateRequestId();
        
        if (this->sendCommandToQueue(cmd, 100)) {
            DynamicJsonDocument doc(256);
            doc["status"] = "ok";
            doc["message"] = "All relays turned ON (queued)";
            doc["count"] = 16;
            doc["request_id"] = cmd.requestId;
            
            String response;
            serializeJson(doc, response);
            request->send(200, "application/json", response);
            
            Serial.println("✅ TÓPICO 3: Comando ALL_ON encolado");
        } else {
            DynamicJsonDocument doc(256);
            doc["status"] = "error";
            doc["message"] = "Queue full - command not queued";
            
            String response;
            serializeJson(doc, response);
            request->send(503, "application/json", response);
            
            Serial.println("❌ TÓPICO 3: Queue llena para ALL_ON");
        }
    });
    
    webTask->addEndpoint("/api/all/off", HTTP_POST, [this](AsyncWebServerRequest *request) {
        // ✅ TÓPICO 3: Crear comando y enviar a queue
        WebCommand cmd;
        cmd.type = WebCommand::ALL_RELAYS_OFF;
        cmd.requestId = this->generateRequestId();
        
        if (this->sendCommandToQueue(cmd, 100)) {
            DynamicJsonDocument doc(256);
            doc["status"] = "ok";
            doc["message"] = "All relays turned OFF (queued)";
            doc["count"] = 16;
            doc["request_id"] = cmd.requestId;
            
            String response;
            serializeJson(doc, response);
            request->send(200, "application/json", response);
            
            Serial.println("✅ TÓPICO 3: Comando ALL_OFF encolado");
        } else {
            DynamicJsonDocument doc(256);
            doc["status"] = "error";
            doc["message"] = "Queue full - command not queued";
            
            String response;
            serializeJson(doc, response);
            request->send(503, "application/json", response);
            
            Serial.println("❌ TÓPICO 3: Queue llena para ALL_OFF");
        }
    });
#endif
    
    // ✅ ENDPOINT DESABILITADO - Usando Supabase (device_status + relay_states tipo 'slave')
    // Frontend lê de: supabase.from('device_status') + supabase.from('relay_states').eq('relay_type', 'slave')
    // ESP32 escreve em: updateSlaveRelayState() quando recebe estado via ESP-NOW
#if 0
    // ✅ API para listar slaves ESP-NOW - VERSÃO SIMPLES (igual ao /status que funciona)
    // Frontend usa isso para criar campos de nomeação (um para cada relé)
    // ✅ REGISTRAR ENDPOINT /api/slaves
    Serial.println("\n🔧 ========================================");
    Serial.println("🔧 [WebServerManager] Registrando endpoint /api/slaves");
    Serial.println("🔧 ========================================");
    Serial.printf("   masterManager: %s\n", masterManager ? "✅ Disponível" : "❌ nullptr");
    Serial.printf("   webTask: %s\n", webTask ? "✅ Disponível" : "❌ nullptr");
    
    Serial.println("🔧 [WebServerManager] Chamando addEndpoint para /api/slaves...");
    Serial.printf("   webTask: %s\n", webTask ? "✅ Disponível" : "❌ nullptr");
    Serial.printf("   webTask->isInitialized(): %s\n", webTask && webTask->isInitialized() ? "✅ SIM" : "❌ NÃO");
    
    webTask->addEndpoint("/api/slaves", HTTP_GET, [this](AsyncWebServerRequest *request) {
        // ✅ WRAPPER DE SEGURANÇA: Garantir que sempre retorne JSON válido
        if (!request) {
            Serial.println("❌ [API] Request é nullptr em /api/slaves!");
            return;
        }
        
        // ✅ CRÍTICO: Try-catch para capturar qualquer exceção e retornar erro 500 com mensagem
        try {
            Serial.println("\n📡 ========================================");
            Serial.println("📡 [API] /api/slaves solicitado");
            Serial.println("📡 ========================================");
            Serial.printf("📡 [API] URL: %s\n", request->url().c_str());
            Serial.printf("📡 [API] Método: %s\n", request->methodToString());
            Serial.printf("📡 [API] Host: %s\n", request->host().c_str());
            Serial.printf("💾 Heap livre: %d bytes\n", ESP.getFreeHeap());
            Serial.printf("🔍 masterManager: %s\n", this->masterManager ? "✅ Disponível" : "❌ nullptr");
            Serial.printf("🔍 webServerManager: %s\n", this ? "✅ Disponível" : "❌ nullptr");
        
        // ✅ NOVO: Verificar se há parâmetro ?refresh=true para forçar atualização de estados
        bool shouldRefresh = false;
        if (request->hasParam("refresh")) {
            String refreshParam = request->getParam("refresh")->value();
            shouldRefresh = (refreshParam == "true" || refreshParam == "1");
        }
        
        // ✅ CRÍTICO: Verificar se estados dos relés estão desatualizados
        bool statesOutdated = false;
        if (this->masterManager) {
            std::vector<TrustedSlave> slaves = this->masterManager->getAllTrustedSlaves();
            unsigned long now = millis();
            for (const auto& slave : slaves) {
                if (slave.isOnline()) {
                    // ✅ CORRIGIDO: Verificar se algum relé tem lastUpdate muito antigo OU nunca foi atualizado
                    for (int i = 0; i < slave.numRelays && i < 8; i++) {
                        // ✅ CRÍTICO: Se lastUpdate = 0, significa que nunca foi atualizado
                        bool neverUpdated = (slave.relayStates[i].lastUpdate == 0);
                        unsigned long age = neverUpdated 
                            ? ULONG_MAX  // ✅ Tratar como desatualizado se nunca atualizado
                            : (now - slave.relayStates[i].lastUpdate);
                            
                        if (neverUpdated || age > 5000) {  // ✅ NOVO: Verificar lastUpdate = 0
                            statesOutdated = true;
                            Serial.printf("⚠️ [API] Estado do relé %d do slave %s desatualizado (lastUpdate: %lu, age: %lu ms)\n", 
                                        i, slave.deviceName.c_str(), slave.relayStates[i].lastUpdate, age);
                            break;
                        }
                    }
                    if (statesOutdated) break;
                }
            }
        }
        
        // ✅ CRÍTICO: Solicitar atualização de estados se:
        // 1. Refresh explícito solicitado (?refresh=true)
        // 2. Estados estão desatualizados (> 5 segundos ou nunca atualizados)
        // ⚠️ NÃO USAR delay() aqui - bloqueia Core 1 e causa watchdog timeout
        if ((shouldRefresh || statesOutdated) && this->masterManager) {
            Serial.println("🔄 [API] Solicitando atualização de estados dos slaves...");
            Serial.printf("   Motivo: %s\n", shouldRefresh ? "Refresh explícito" : "Estados desatualizados");
            this->masterManager->requestAllSlavesRelayStatus();
            // ✅ CORREÇÃO: Não usar delay() - estados serão atualizados assincronamente via ESP-NOW
            // O cache será atualizado no próximo loop do Core 0
        }
        
        // ✅ TRATAMENTO DE ERRO: Se heap muito baixo, retornar erro
        uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 15000) {  // ✅ AUMENTADO: 15KB mínimo (SSL precisa de ~30KB)
            Serial.printf("❌ [API] Heap muito baixo! %d bytes (mínimo: 15000)\n", freeHeap);
            request->send(500, "application/json", "{\"error\":\"Insufficient memory\",\"free_heap\":" + String(freeHeap) + ",\"slaves\":[]}");
            return;
        }
        
        // ✅ SOLUÇÃO: LER DO CACHE PRIMEIRO (thread-safe entre cores, SEM alocar JSON grande)
        // Core 0 atualiza cache a cada 2s, Core 1 lê do cache
        SystemDataCache cache = this->getSystemCache();
        
        unsigned long cacheAge = cache.slavesLastUpdate > 0 ? (millis() - cache.slavesLastUpdate) : 0;
        Serial.printf("📊 [API] Cache de slaves: %d bytes, atualizado há %lu ms\n", 
                     cache.slavesJson.length(), cacheAge);
        Serial.printf("📊 [API] Cache totalSlaves: %d, onlineSlaves: %d, offlineSlaves: %d\n",
                     cache.totalSlaves, cache.onlineSlaves, cache.offlineSlaves);
        
        // ✅ USAR CACHE: Retornar JSON do cache (thread-safe)
        if (cache.slavesJson.length() > 0) {
            Serial.println("✅ [API] Retornando slaves do cache (thread-safe)");
            Serial.printf("📡 [API] JSON do cache: %d bytes\n", cache.slavesJson.length());
            int previewLength = cache.slavesJson.length() > 200 ? 200 : cache.slavesJson.length();
            Serial.printf("📡 [API] Primeiros 200 chars: %s\n", cache.slavesJson.substring(0, previewLength).c_str());
            
            // ✅ Verificar se JSON é válido antes de enviar
            if (cache.slavesJson.charAt(0) == '{' || cache.slavesJson.charAt(0) == '[') {
                Serial.println("✅ [API] JSON válido, enviando resposta...");
                request->send(200, "application/json", cache.slavesJson);
                Serial.println("✅ [API] Resposta enviada com sucesso!");
            } else {
                Serial.println("⚠️ [API] JSON do cache parece inválido (primeiro char: '" + String(cache.slavesJson.charAt(0)) + "'), usando fallback direto");
                // Continuar para fallback direto do masterManager
            }
            
            // Se JSON válido, já retornou acima. Se inválido, continuar para fallback
            if (cache.slavesJson.charAt(0) == '{' || cache.slavesJson.charAt(0) == '[') {
                return;
            }
        }
        
        // ✅ FALLBACK: Se cache vazio, verificar se masterManager está disponível
        if (!this->masterManager) {
            Serial.println("❌ [API] masterManager é nullptr - não é possível obter slaves");
            Serial.println("   💡 Verifique ordem de inicialização: masterManager->begin() ANTES de HydroSystemCore::begin()");
            DynamicJsonDocument errorDoc(512);
            errorDoc["error"] = "MasterSlaveManager not initialized";
            errorDoc["message"] = "masterManager is nullptr - check initialization order";
            errorDoc["slaves"] = JsonArray();
            errorDoc["debug"] = JsonObject();
            errorDoc["debug"]["masterManager_null"] = true;
            errorDoc["debug"]["cache_empty"] = true;
            errorDoc["debug"]["heap_free"] = ESP.getFreeHeap();
            String errorResponse;
            if (serializeJson(errorDoc, errorResponse) > 0) {
                Serial.printf("📡 [API] Enviando erro 503: %d bytes\n", errorResponse.length());
                request->send(503, "application/json", errorResponse);
            } else {
                Serial.println("❌ [API] Falha ao serializar erro, enviando fallback");
                request->send(503, "application/json", "{\"error\":\"MasterSlaveManager not initialized\",\"slaves\":[]}");
            }
            return;
        }
        
        // ✅ FALLBACK DIRETO: Se cache vazio mas masterManager disponível, buscar diretamente
        Serial.println("⚠️ [API] Cache de slaves vazio, mas masterManager disponível");
        Serial.printf("   📊 Cache totalSlaves: %d, Online: %d, Offline: %d\n", 
                     cache.totalSlaves, cache.onlineSlaves, cache.offlineSlaves);
        
        // ✅ BUSCAR DIRETAMENTE DO MASTERMANAGER (fallback quando cache vazio)
        Serial.println("   🔍 Buscando slaves diretamente do masterManager...");
        
        // ⚠️ CORREÇÃO: NÃO USAR delay() aqui - bloqueia Core 1 e causa watchdog timeout
        // Estados serão atualizados assincronamente via ESP-NOW no Core 0
        
        // ✅ VERIFICAR HEAP ANTES DE CRIAR JSON GRANDE
        uint32_t heapBefore = ESP.getFreeHeap();
        if (heapBefore < 20000) {  // ✅ Mínimo 20KB para criar JSON de slaves
            Serial.printf("   ⚠️ Heap insuficiente para criar JSON: %d bytes (mínimo: 20000)\n", heapBefore);
            DynamicJsonDocument errorDoc(256);
            errorDoc["error"] = "Insufficient memory for JSON creation";
            errorDoc["free_heap"] = heapBefore;
            errorDoc["slaves"] = JsonArray();
            String errorResponse;
            serializeJson(errorDoc, errorResponse);
            request->send(500, "application/json", errorResponse);
            return;
        }
        
        // ✅ CRÍTICO: Buscar slaves DEPOIS de solicitar estados (para ter dados atualizados)
        std::vector<TrustedSlave> slaves = this->masterManager->getAllTrustedSlaves();
        Serial.printf("   ✅ Encontrados %d slave(s) diretamente do masterManager\n", slaves.size());
        
        if (slaves.size() > 0) {
            // ✅ CRIAR JSON DIRETAMENTE (fallback quando cache não está pronto)
            // ✅ OTIMIZAÇÃO: Calcular tamanho necessário dinamicamente
            // Cada slave: ~400-600 bytes (com 8 relés)
            // Buffer: slaves.size() * 600 + 256 (overhead)
            size_t estimatedSize = (slaves.size() * 600) + 256;
            if (estimatedSize > 8192) estimatedSize = 8192;  // Limitar a 8KB máximo
            if (estimatedSize < 1024) estimatedSize = 1024;  // Mínimo 1KB
            
            Serial.printf("   📝 Criando JSON diretamente (buffer: %d bytes para %d slaves)...\n", estimatedSize, slaves.size());
            
            DynamicJsonDocument doc(estimatedSize);
            
            // ✅ Verificar se documento foi criado
            if (doc.capacity() == 0) {
                Serial.println("   ❌ Falha ao criar DynamicJsonDocument!");
                request->send(500, "application/json", "{\"error\":\"JSON allocation failed\",\"slaves\":[]}");
                return;
            }
            
            JsonArray slavesArray = doc.createNestedArray("slaves");
            if (slavesArray.isNull()) {
                Serial.println("   ❌ Falha ao criar array 'slaves'!");
                request->send(500, "application/json", "{\"error\":\"Failed to create slaves array\",\"slaves\":[]}");
                return;
            }
            
            for (const auto& slave : slaves) {
                JsonObject slaveObj = slavesArray.createNestedObject();
                
                // ✅ Gerar device_id correto (ESP32_SLAVE_XX_XX_XX_XX_XX_XX)
                String deviceId = "ESP32_SLAVE_" + ESPNowController::macToString(slave.macAddress);
                deviceId.replace(":", "_");
                
                slaveObj["device_id"] = deviceId;
                slaveObj["device_name"] = slave.deviceName;
                slaveObj["device_type"] = slave.deviceType;
                slaveObj["mac_address"] = ESPNowController::macToString(slave.macAddress);
                slaveObj["is_online"] = slave.isOnline();
                slaveObj["num_relays"] = slave.numRelays;
                slaveObj["last_seen"] = slave.lastSeen;
                slaveObj["operational"] = slave.operational;
                
                // Adicionar estados dos relés
                JsonArray relaysArray = slaveObj.createNestedArray("relays");
                for (int i = 0; i < slave.numRelays && i < 8; i++) {
                    JsonObject relayObj = relaysArray.createNestedObject();
                    relayObj["relay_number"] = i;
                    // ✅ CRÍTICO: Ler estado real do slave (atualizado via ESP-NOW)
                    bool relayState = slave.relayStates[i].state;
                    bool neverUpdated = (slave.relayStates[i].lastUpdate == 0);
                    unsigned long stateAge = neverUpdated 
                        ? ULONG_MAX 
                        : (millis() - slave.relayStates[i].lastUpdate);
                    
                    // ✅ DEBUG MELHORADO: Log sempre para diagnóstico
                    Serial.printf("   🔌 [API] Slave %s - Relé %d: %s (lastUpdate: %lu, age: %lu ms, confiável: %s)\n",
                                 slave.deviceName.c_str(), i, 
                                 relayState ? "ON" : "OFF", 
                                 slave.relayStates[i].lastUpdate,
                                 stateAge,
                                 (neverUpdated || stateAge > 10000) ? "NÃO" : "SIM");
                    
                    // ✅ ADICIONAR: Flag indicando se estado é confiável
                    bool stateIsReliable = (!neverUpdated && stateAge < 10000);
                    
                    relayObj["state"] = relayState;
                    relayObj["has_timer"] = slave.relayStates[i].hasTimer;
                    relayObj["remaining_time"] = slave.relayStates[i].remainingTime;
                    relayObj["name"] = slave.relayStates[i].name.length() > 0 ? slave.relayStates[i].name : ("Relé " + String(i + 1));
                    // ✅ NOVO: Adicionar timestamp da última atualização para debug
                    relayObj["last_update_ms"] = slave.relayStates[i].lastUpdate;
                    relayObj["state_age_ms"] = neverUpdated ? -1 : (int)stateAge;  // -1 se nunca atualizado
                    relayObj["state_reliable"] = stateIsReliable;  // ✅ NOVO: Indica se estado é confiável
                }
            }
            
            String response;
            if (serializeJson(doc, response) > 0) {
                Serial.printf("✅ [API] JSON criado diretamente: %d bytes\n", response.length());
                Serial.printf("📡 [API] Primeiros 200 chars: %s\n", response.substring(0, 200).c_str());
                request->send(200, "application/json", response);
            } else {
                Serial.println("❌ [API] Falha ao serializar JSON direto, enviando resposta simples");
                request->send(200, "application/json", "{\"slaves\":[]}");
            }
        } else {
            // Nenhum slave encontrado
            Serial.println("   ℹ️ Nenhum slave encontrado no masterManager");
            
            DynamicJsonDocument fallbackDoc(512);
            fallbackDoc["slaves"] = JsonArray();
            fallbackDoc["total"] = 0;
            fallbackDoc["online"] = 0;
            fallbackDoc["offline"] = 0;
            fallbackDoc["cache_age_ms"] = cacheAge;
            fallbackDoc["message"] = "Nenhum slave encontrado - verifique se slaves estão ligados e no mesmo canal ESP-NOW";
            
            String fallbackResponse;
            if (serializeJson(fallbackDoc, fallbackResponse) > 0) {
                Serial.printf("📡 [API] Enviando fallback (sem slaves): %d bytes\n", fallbackResponse.length());
                request->send(200, "application/json", fallbackResponse);
            } else {
                Serial.println("❌ [API] Falha ao serializar fallback, enviando resposta simples");
                request->send(200, "application/json", "{\"slaves\":[]}");
            }
        }
        return;
        
        // ❌ CÓDIGO ANTIGO REMOVIDO: Acesso direto entre cores causava problemas
        // Agora usamos cache thread-safe atualizado do Core 0
        // Todo o código de processamento de slaves foi movido para Core 0 (HydroSystemCore::loop())
        } catch (const std::exception& e) {
            Serial.printf("❌ [API] Exceção em /api/slaves: %s\n", e.what());
            request->send(500, "application/json", "{\"error\":\"Internal server error\",\"message\":\"" + String(e.what()) + "\",\"slaves\":[]}");
            return;
        } catch (...) {
            Serial.println("❌ [API] Exceção desconhecida em /api/slaves");
            request->send(500, "application/json", "{\"error\":\"Internal server error\",\"message\":\"Unknown exception\",\"slaves\":[]}");
            return;
        }
    });
    Serial.println("✅ [WebServerManager] Endpoint /api/slaves registrado");
#endif
    
    // ✅ ENDPOINT DE DEBUG - Manter opcional para debug local
    webTask->addEndpoint("/api/slaves/refresh", HTTP_POST, [this](AsyncWebServerRequest *request) {
        Serial.println("\n🔄 [API] /api/slaves/refresh solicitado");
        
        if (!this->masterManager) {
            DynamicJsonDocument errorDoc(256);
            errorDoc["error"] = "MasterSlaveManager not initialized";
            errorDoc["success"] = false;
            String errorResponse;
            serializeJson(errorDoc, errorResponse);
            request->send(503, "application/json", errorResponse);
            return;
        }
        
        // Solicitar atualização de estados de todos os slaves
        this->masterManager->requestAllSlavesRelayStatus();
        
        DynamicJsonDocument doc(256);
        doc["success"] = true;
        doc["message"] = "Status refresh requested - states will be updated shortly";
        doc["note"] = "Use /api/slaves?refresh=true to get updated states immediately";
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        Serial.println("✅ [API] Refresh de estados solicitado");
    });
    Serial.println("✅ [WebServerManager] Endpoint /api/slaves/refresh registrado");
    
    // ✅ DEBUG: Endpoint ULTRA SIMPLES (igual ao /status que funciona)
    webTask->addEndpoint("/api/slaves/debug", HTTP_GET, [this](AsyncWebServerRequest *request) {
        Serial.println("🔍 [DEBUG] /api/slaves/debug solicitado");
        
        // ✅ VERSÃO ULTRA SIMPLES: Buffer pequeno, igual ao /status
        DynamicJsonDocument doc(512);
        JsonObject debug = doc.createNestedObject("debug");
        
        // ✅ Informações básicas (sem acessar estruturas complexas)
        debug["heap_free"] = ESP.getFreeHeap();
        debug["masterManager_ptr"] = (this->masterManager != nullptr) ? "not_null" : "null";
        
        // ✅ Tentar obter contagem de slaves (sem try-catch, que não funciona bem em Arduino)
        if (this->masterManager) {
            Serial.println("🔍 [DEBUG] masterManager disponível, tentando getAllTrustedSlaves()...");
            std::vector<TrustedSlave> trustedSlaves = this->masterManager->getAllTrustedSlaves();
            debug["total_slaves"] = (int)trustedSlaves.size();
            debug["status"] = "ok";
            Serial.printf("✅ [DEBUG] getAllTrustedSlaves() OK: %d slaves\n", trustedSlaves.size());
        } else {
            debug["error"] = "MasterSlaveManager não disponível";
            debug["total_slaves"] = 0;
            debug["status"] = "no_manager";
            Serial.println("⚠️ [DEBUG] masterManager é nullptr");
        }
        
        String response;
        serializeJson(doc, response);
        Serial.printf("📡 [DEBUG] Resposta JSON: %d bytes\n", response.length());
        Serial.println("📡 [DEBUG] JSON: " + response);
        request->send(200, "application/json", response);
    });
    
    // ✅ DEBUG: Endpoint para imprimir trustedSlaves no Serial (sem retornar JSON)
    webTask->addEndpoint("/api/slaves/print", HTTP_GET, [this](AsyncWebServerRequest *request) {
        Serial.println("\n🖨️ ========================================");
        Serial.println("🖨️ [PRINT] /api/slaves/print solicitado");
        Serial.println("🖨️ ========================================");
        
        if (!this->masterManager) {
            Serial.println("❌ [PRINT] MasterSlaveManager é nullptr!");
            request->send(200, "text/plain", "❌ MasterSlaveManager não disponível\nVerifique o Serial para mais detalhes.");
            return;
        }
        
        // Usar a função existente printTrustedSlaves()
        this->masterManager->printTrustedSlaves();
        
        // Também imprimir JSON formatado
        Serial.println("\n📄 [PRINT] JSON que seria enviado por /api/slaves:");
        Serial.println("==========================================");
        
        DynamicJsonDocument doc(2048);
        JsonArray slavesArray = doc.createNestedArray("slaves");
        
        std::vector<TrustedSlave> trustedSlaves = this->masterManager->getAllTrustedSlaves();
        
        for (const auto& slave : trustedSlaves) {
            JsonObject slaveObj = slavesArray.createNestedObject();
            
            String deviceId = "ESP32_SLAVE_" + ESPNowController::macToString(slave.macAddress);
            deviceId.replace(":", "_");
            slaveObj["device_id"] = deviceId;
            slaveObj["device_name"] = slave.deviceName;
            slaveObj["device_type"] = slave.deviceType;
            slaveObj["mac_address"] = ESPNowController::macToString(slave.macAddress);
            slaveObj["is_online"] = slave.isOnline();
            slaveObj["num_relays"] = slave.numRelays;
            slaveObj["last_seen"] = slave.lastSeen;
            
            JsonArray relaysArray = slaveObj.createNestedArray("relays");
            for (int i = 0; i < slave.numRelays && i < 16; i++) {
                JsonObject relayObj = relaysArray.createNestedObject();
                relayObj["relay_number"] = i;
                
                String relayName = slave.relayStates[i].name;
                if (relayName.isEmpty()) {
                    relayName = "Relé " + String(i);
                }
                relayObj["name"] = relayName;
                relayObj["state"] = slave.relayStates[i].state;
                relayObj["has_timer"] = slave.relayStates[i].hasTimer;
                relayObj["remaining_time"] = slave.relayStates[i].remainingTime;
            }
        }
        
        String jsonResponse;
        serializeJsonPretty(doc, jsonResponse);  // Pretty print para Serial
        Serial.println(jsonResponse);
        Serial.println("==========================================\n");
        
        request->send(200, "text/plain", "✅ Dados impressos no Serial!\nVerifique o Serial Monitor para ver os detalhes.");
    });
    
    // ✅ DEBUG: Página HTML amigável para ver slaves (para usar no navegador)
    webTask->addEndpoint("/debug/slaves", HTTP_GET, [this](AsyncWebServerRequest *request) {
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta charset='UTF-8'>";
        html += "<title>🔍 Debug Slaves ESP-NOW</title>";
        html += "<style>";
        html += "body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }";
        html += "h1 { color: #333; }";
        html += ".slave { background: white; padding: 15px; margin: 10px 0; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }";
        html += ".slave h2 { color: #4CAF50; margin-top: 0; }";
        html += ".info { margin: 5px 0; }";
        html += ".label { font-weight: bold; color: #666; }";
        html += ".online { color: #4CAF50; }";
        html += ".offline { color: #f44336; }";
        html += ".relay { background: #f9f9f9; padding: 8px; margin: 5px 0; border-left: 3px solid #2196F3; }";
        html += ".json-box { background: #263238; color: #aed581; padding: 15px; border-radius: 5px; overflow-x: auto; }";
        html += "pre { margin: 0; white-space: pre-wrap; word-wrap: break-word; }";
        html += "a { color: #2196F3; text-decoration: none; margin: 0 10px; }";
        html += "a:hover { text-decoration: underline; }";
        html += ".nav { background: white; padding: 15px; border-radius: 8px; margin-bottom: 20px; }";
        html += "</style></head><body>";
        html += "<div class='nav'>";
        html += "<h1>🔍 Debug Slaves ESP-NOW</h1>";
        html += "<a href='/api/slaves/debug' target='_blank'>📄 Ver JSON</a>";
        html += "<a href='/api/slaves/print'>🖨️ Imprimir no Serial</a>";
        html += "<a href='/api/slaves'>📡 Endpoint Normal</a>";
        html += "<a href='/debug/slaves'>🔄 Atualizar</a>";
        html += "</div>";
        
        if (!this->masterManager) {
            html += "<div class='slave'>";
            html += "<h2>❌ Erro</h2>";
            html += "<p>MasterSlaveManager não está disponível (nullptr)</p>";
            html += "<p>Verifique se masterManager foi passado corretamente para beginAdminServer()</p>";
            html += "</div>";
            html += "</body></html>";
            request->send(200, "text/html", html);
            return;
        }
        
        std::vector<TrustedSlave> trustedSlaves = this->masterManager->getAllTrustedSlaves();
        
        html += "<div class='slave'>";
        html += "<h2>📊 Estatísticas</h2>";
        html += "<p class='info'><span class='label'>Total de Slaves:</span> <strong>" + String(trustedSlaves.size()) + "</strong></p>";
        html += "<p class='info'><span class='label'>MasterManager:</span> <span class='online'>✅ Disponível</span></p>";
        html += "</div>";
        
        if (trustedSlaves.empty()) {
            html += "<div class='slave'>";
            html += "<h2>⚠️ Nenhum Slave Encontrado</h2>";
            html += "<p>Nenhum slave foi encontrado em <code>trustedSlaves</code>.</p>";
            html += "<p><strong>Verifique:</strong></p>";
            html += "<ul>";
            html += "<li>Se o slave enviou DEVICE_INFO</li>";
            html += "<li>Se o slave está em trustedSlaves (use printTrustedSlaves())</li>";
            html += "<li>Se o Master recebeu o DEVICE_INFO corretamente</li>";
            html += "</ul>";
            html += "</div>";
        } else {
            for (size_t idx = 0; idx < trustedSlaves.size(); idx++) {
                const auto& slave = trustedSlaves[idx];
                
                html += "<div class='slave'>";
                html += "<h2>📋 Slave #" + String(idx + 1) + ": " + slave.deviceName + "</h2>";
                
                html += "<div class='info'><span class='label'>MAC Address:</span> " + ESPNowController::macToString(slave.macAddress) + "</div>";
                String deviceIdForHtml = "ESP32_SLAVE_" + ESPNowController::macToString(slave.macAddress);
                deviceIdForHtml.replace(":", "_");
                html += "<div class='info'><span class='label'>Device ID:</span> " + deviceIdForHtml + "</div>";
                html += "<div class='info'><span class='label'>Device Type:</span> " + slave.deviceType + "</div>";
                html += "<div class='info'><span class='label'>Status:</span> " + String((int)slave.status) + " (0=UNKNOWN, 1=DISCOVERED, 2=PING_RECEIVED, 3=HANDSHAKE_OK, 4=ONLINE, 5=OFFLINE, 6=ERROR)</div>";
                html += "<div class='info'><span class='label'>Online:</span> <span class='" + String(slave.isOnline() ? "online" : "offline") + "'>" + String(slave.isOnline() ? "✅ SIM" : "❌ NÃO") + "</span></div>";
                html += "<div class='info'><span class='label'>Num Relays:</span> " + String(slave.numRelays) + "</div>";
                html += "<div class='info'><span class='label'>Operational:</span> " + String(slave.operational ? "✅ SIM" : "❌ NÃO") + "</div>";
                html += "<div class='info'><span class='label'>Last Seen:</span> " + String(slave.lastSeen) + " ms (" + String(slave.getTimeSinceLastSeen() / 1000) + " segundos atrás)</div>";
                html += "<div class='info'><span class='label'>RSSI:</span> " + String(slave.rssi) + " dBm</div>";
                html += "<div class='info'><span class='label'>WiFi Channel:</span> " + String(slave.wifiChannel) + "</div>";
                html += "<div class='info'><span class='label'>Uptime:</span> " + String(slave.uptime) + " segundos</div>";
                html += "<div class='info'><span class='label'>Free Heap:</span> " + String(slave.freeHeap) + " bytes</div>";
                
                html += "<h3>🔌 Estados dos Relés:</h3>";
                for (int i = 0; i < 8; i++) {
                    html += "<div class='relay'>";
                    html += "<strong>Relé " + String(i) + ":</strong> ";
                    html += "<span class='label'>Estado:</span> " + String(slave.relayStates[i].state ? "🟢 ON" : "🔴 OFF") + " | ";
                    html += "<span class='label'>Timer:</span> " + String(slave.relayStates[i].hasTimer ? "✅ SIM" : "❌ NÃO") + " | ";
                    html += "<span class='label'>Tempo Restante:</span> " + String(slave.relayStates[i].remainingTime) + " seg | ";
                    html += "<span class='label'>Nome:</span> '" + slave.relayStates[i].name + "'";
                    html += "</div>";
                }
                
                html += "<h3>📊 Estatísticas de Comunicação:</h3>";
                html += "<div class='info'><span class='label'>Pings Received:</span> " + String(slave.pingsReceived) + "</div>";
                html += "<div class='info'><span class='label'>Pings Sent:</span> " + String(slave.pingsSent) + "</div>";
                html += "<div class='info'><span class='label'>Pongs Received:</span> " + String(slave.pongsReceived) + "</div>";
                html += "<div class='info'><span class='label'>Pongs Sent:</span> " + String(slave.pongsSent) + "</div>";
                html += "<div class='info'><span class='label'>Messages Received:</span> " + String(slave.messagesReceived) + "</div>";
                html += "<div class='info'><span class='label'>Messages Lost:</span> " + String(slave.messagesLost) + "</div>";
                html += "<div class='info'><span class='label'>Errors:</span> " + String(slave.errors) + "</div>";
                
                html += "</div>";
            }
        }
        
        // Adicionar JSON completo no final
        DynamicJsonDocument doc(4096);
        JsonArray slavesArray = doc.createNestedArray("slaves");
        for (const auto& slave : trustedSlaves) {
            JsonObject slaveObj = slavesArray.createNestedObject();
            String deviceIdJson = "ESP32_SLAVE_" + ESPNowController::macToString(slave.macAddress);
            deviceIdJson.replace(":", "_");
            slaveObj["device_id"] = deviceIdJson;
            slaveObj["device_name"] = slave.deviceName;
            slaveObj["device_type"] = slave.deviceType;
            slaveObj["mac_address"] = ESPNowController::macToString(slave.macAddress);
            slaveObj["is_online"] = slave.isOnline();
            slaveObj["num_relays"] = slave.numRelays;
            slaveObj["last_seen"] = slave.lastSeen;
            
            JsonArray relaysArray = slaveObj.createNestedArray("relays");
            for (int i = 0; i < slave.numRelays && i < 16; i++) {
                JsonObject relayObj = relaysArray.createNestedObject();
                relayObj["relay_number"] = i;
                String relayName = slave.relayStates[i].name;
                if (relayName.isEmpty()) {
                    relayName = "Relé " + String(i);
                }
                relayObj["name"] = relayName;
                relayObj["state"] = slave.relayStates[i].state;
                relayObj["has_timer"] = slave.relayStates[i].hasTimer;
                relayObj["remaining_time"] = slave.relayStates[i].remainingTime;
            }
        }
        
        String jsonResponse;
        serializeJsonPretty(doc, jsonResponse);
        
        html += "<div class='slave'>";
        html += "<h2>📄 JSON Completo (formato que vai para o frontend)</h2>";
        html += "<div class='json-box'><pre>" + jsonResponse + "</pre></div>";
        html += "</div>";
        
        html += "</body></html>";
        request->send(200, "text/html", html);
    });
    
    // ✅ Página de status detalhado (texto simples) - Core 1
    webTask->addEndpoint("/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
        String status = "🌱 ESP32 HIDROPÔNICO - STATUS\n";
        status += "================================\n";
        status += "🆔 Device ID: " + (this->wifiManager ? this->wifiManager->getDeviceID() : "N/A") + "\n";
        status += "🔧 Firmware: " + (this->wifiManager ? this->wifiManager->getFirmwareVersion() : "N/A") + "\n";
        status += "🌐 IP: " + (this->wifiManager ? this->wifiManager->getStationIP() : "N/A") + "\n";
        status += "⏰ Uptime: " + String(millis() / 1000) + " segundos\n";
        status += "💾 Heap Livre: " + String(ESP.getFreeHeap()) + " bytes\n";
        status += "🌱 Sistema: " + String(systemInitialized ? "✅ Pronto" : "⏳ Inicializando") + "\n";
        status += "☁️ Supabase: " + String(supabaseConnected ? "✅ Conectado" : "❌ Desconectado") + "\n";
        status += "🌐 Web Server: " + String(webServerRunning ? "✅ Ativo" : "❌ Inativo") + "\n";
        request->send(200, "text/plain", status);
    });
    
    // ✅ Reset do sistema
    webTask->addEndpoint("/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
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
    webTask->addEndpoint("/api/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
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
    webTask->addEndpoint("/reconfigure-wifi", HTTP_GET, [this](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(128);
        doc["success"] = true;
        doc["message"] = "Resetando WiFi e voltando ao modo AP...";
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        Serial.println("📶 Reconfiguração WiFi solicitada via painel admin");
        Serial.println("🗑️ Limpando credenciais WiFi salvas...");
        
        // Resetar configurações WiFi
        if (this->wifiManager) {
            this->wifiManager->resetSettings();
        }
        
        Serial.println("🔄 Reiniciando para modo AP...");
        delay(1000);
        ESP.restart();
    });
    
    // ✅ API Reconfiguração WiFi (endpoint compatível com interface)
    webTask->addEndpoint("/api/reconfigure-wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
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
    
    // ✅ NO INICIAR adminServer - WebServerTask ya lo maneja en Core 1
    isRunning = true;
    webServerRunning = true;
    
    Serial.println("✅ PAINEL ADMIN configurado en WebServerTask (Core 1)");
    Serial.println("📁 Usando arquivo: index.html + style.css + script.js");
    Serial.println("🌐 Acesso DIRETO: http://" + (this->wifiManager ? this->wifiManager->getStationIP() : "N/A"));
    Serial.println("📱 Interface Completa: Dashboard + Relés + Sensores");
    Serial.println("🔒 IMPORTANTE: Access Point do WiFiManager preservado!");
    Serial.println("🎯 TODOS los endpoints corren en Core 1 (WebServerTask)");
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

// ============================================
// ✅ TÓPICO 2: IMPLEMENTACIÓN DE QUEUE Y MUTEX
// ============================================

bool WebServerManager::initQueueAndMutex() {
    Serial.println("🔧 Inicializando Queue y Mutex...");
    
    // ✅ TÓPICO 2.1: Crear Queue FreeRTOS (10 comandos máximo, como MASTER-TASK)
    commandQueue = xQueueCreate(10, sizeof(WebCommand));
    if (!commandQueue) {
        Serial.println("❌ Error al crear command queue");
        return false;
    }
    Serial.println("✅ Command queue creada (10 comandos)");
    
    // ✅ TÓPICO 2.2: Crear Mutex para system cache
    systemCacheMutex = xSemaphoreCreateMutex();
    if (!systemCacheMutex) {
        Serial.println("❌ Error al crear system cache mutex");
        vQueueDelete(commandQueue);
        commandQueue = nullptr;
        return false;
    }
    Serial.println("✅ System cache mutex creado");
    
    // ✅ TÓPICO 2.3: Crear Mutex para request ID counter
    requestIdMutex = xSemaphoreCreateMutex();
    if (!requestIdMutex) {
        Serial.println("❌ Error al crear request ID mutex");
        vSemaphoreDelete(systemCacheMutex);
        systemCacheMutex = nullptr;
        vQueueDelete(commandQueue);
        commandQueue = nullptr;
        return false;
    }
    Serial.println("✅ Request ID mutex creado");
    
    Serial.println("✅ Queue y Mutex inicializados correctamente");
    return true;
}

void WebServerManager::cleanupQueueAndMutex() {
    if (commandQueue) {
        vQueueDelete(commandQueue);
        commandQueue = nullptr;
        Serial.println("✅ Command queue eliminada");
    }
    
    if (systemCacheMutex) {
        vSemaphoreDelete(systemCacheMutex);
        systemCacheMutex = nullptr;
        Serial.println("✅ System cache mutex eliminado");
    }
    
    if (requestIdMutex) {
        vSemaphoreDelete(requestIdMutex);
        requestIdMutex = nullptr;
        Serial.println("✅ Request ID mutex eliminado");
    }
}

bool WebServerManager::sendCommandToQueue(const WebCommand& cmd, uint32_t timeoutMs) {
    if (!commandQueue) {
        Serial.println("❌ Command queue no inicializada");
        return false;
    }
    
    TickType_t ticks = timeoutMs > 0 ? pdMS_TO_TICKS(timeoutMs) : 0;
    BaseType_t result = xQueueSend(commandQueue, &cmd, ticks);
    
    if (result == pdTRUE) {
        Serial.printf("✅ Comando enviado a queue: type=%d, relay=%d, action=%s\n", 
                     cmd.type, cmd.relayNumber, cmd.action.c_str());
        return true;
    } else {
        Serial.println("❌ Error al enviar comando a queue (queue llena o timeout)");
        return false;
    }
}

uint32_t WebServerManager::generateRequestId() {
    uint32_t id = 0;
    
    if (!requestIdMutex) {
        return millis(); // Fallback si mutex no está inicializado
    }
    
    if (xSemaphoreTake(requestIdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        id = ++requestIdCounter;
        xSemaphoreGive(requestIdMutex);
    }
    
    return id;
}

bool WebServerManager::receiveCommand(WebCommand& cmd, uint32_t timeoutMs) {
    if (!commandQueue) {
        return false;
    }
    
    TickType_t ticks = timeoutMs > 0 ? pdMS_TO_TICKS(timeoutMs) : 0;
    BaseType_t result = xQueueReceive(commandQueue, &cmd, ticks);
    
    return (result == pdTRUE);
}

void WebServerManager::updateSystemCache(const SystemDataCache& cache) {
    if (!systemCacheMutex) return;
    
    if (xSemaphoreTake(systemCacheMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        systemCache = cache;
        systemCache.lastUpdate = millis();
        xSemaphoreGive(systemCacheMutex);
    }
}

SystemDataCache WebServerManager::getSystemCache() {
    SystemDataCache cache;
    
    if (!systemCacheMutex) return cache;
    
    if (xSemaphoreTake(systemCacheMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cache = systemCache;
        xSemaphoreGive(systemCacheMutex);
    }
    
    return cache;
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

bool WebServerManager::shouldRefreshSlaveStates() {
    if (!masterManager) {
        return false;
    }
    
    std::vector<TrustedSlave> slaves = masterManager->getAllTrustedSlaves();
    unsigned long now = millis();
    
    for (const auto& slave : slaves) {
        if (slave.isOnline()) {
            for (int i = 0; i < slave.numRelays && i < 8; i++) {
                bool neverUpdated = (slave.relayStates[i].lastUpdate == 0);
                unsigned long age = neverUpdated 
                    ? ULONG_MAX 
                    : (now - slave.relayStates[i].lastUpdate);
                    
                if (neverUpdated || age > 5000) {  // ✅ Estados desatualizados (> 5s ou nunca atualizados)
                    return true;
                }
            }
        }
    }
    
    return false;
}