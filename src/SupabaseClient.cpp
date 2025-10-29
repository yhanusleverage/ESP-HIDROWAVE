#include "SupabaseClient.h"
#include "DeviceID.h"

SupabaseClient::SupabaseClient() : 
    isConnected(false),
    lastCommandCheck(0) {
}

SupabaseClient::~SupabaseClient() {
    http.end();
}

bool SupabaseClient::begin(const String& url, const String& key) {
    baseUrl = url;
    apiKey = key;
    
    if (WiFi.status() != WL_CONNECTED) {
        setError("WiFi não conectado");
        return false;
    }
    
    // ===== CONFIGURAÇÃO SSL PARA SUPABASE =====
    // Configurar HTTPClient para HTTPS com certificados auto-assinados
    WiFiClientSecure* secureClient = new WiFiClientSecure();
    secureClient->setInsecure(); // Aceitar certificados auto-assinados para desenvolvimento
    
    http.begin(*secureClient, baseUrl + "/rest/v1/");
    http.setUserAgent("ESP32-Hydro/2.1.0");
    http.setConnectTimeout(15000); // 15 segundos timeout de conexão
    http.setTimeout(20000); // 20 segundos timeout total
    
    Serial.println("🔐 Configurando conexão SSL para Supabase...");
    Serial.println("🔓 Certificados auto-assinados: ACEITOS (desenvolvimento)");
    
    // Testar conexão DNS primeiro
    Serial.printf("🌐 Testando DNS para: %s\n", baseUrl.c_str());
    
    // Testar conexão
    if (testConnection()) {
        isConnected = true;
        Serial.println("✅ Supabase conectado com sucesso");
        return true;
    } else {
        setError("Falha ao conectar com Supabase - verifique DNS/SSL");
        return false;
    }
}

String SupabaseClient::buildAuthHeader() {
    return "Bearer " + apiKey;
}

bool SupabaseClient::makeRequest(const String& method, const String& endpoint, const String& payload) {
    if (!isReady()) {
        setError("Cliente não está pronto");
        return false;
    }
    
    String url = baseUrl + "/rest/v1/" + endpoint;
    
    http.begin(url);
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("Prefer", SUPABASE_PREFER);
    http.addHeader("apikey", apiKey);
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode;
    if (method == "POST") {
        httpCode = http.POST(payload);
    } else if (method == "GET") {
        httpCode = http.GET();
    } else if (method == "PATCH") {
        httpCode = http.PATCH(payload);
    } else {
        setError("Método HTTP não suportado: " + method);
        http.end();
        return false;
    }
    
    if (httpCode >= 200 && httpCode < 300) {
        Serial.printf("✅ %s %s: %d\n", method.c_str(), endpoint.c_str(), httpCode);
        http.end();
        return true;
    } else {
        String response = http.getString();
        setError("HTTP " + String(httpCode) + ": " + response);
        Serial.printf("❌ %s %s: %d - %s\n", method.c_str(), endpoint.c_str(), httpCode, response.c_str());
        http.end();
        return false;
    }
}

bool SupabaseClient::sendEnvironmentData(const EnvironmentReading& reading) {
    String payload = buildEnvironmentPayload(reading);
    return makeRequest("POST", SUPABASE_ENVIRONMENT_TABLE, payload);
}

bool SupabaseClient::sendHydroData(const HydroReading& reading) {
    String payload = buildHydroPayload(reading);
    return makeRequest("POST", SUPABASE_HYDRO_TABLE, payload);
}

bool SupabaseClient::updateDeviceStatus(const DeviceStatusData& status) {
    String payload = buildDeviceStatusPayload(status);
    
    // Usar UPSERT para atualizar ou inserir
    String endpoint = String(SUPABASE_STATUS_TABLE) + "?device_id=eq." + status.deviceId;
    
    // Primeiro tentar UPDATE
    http.begin(baseUrl + "/rest/v1/" + endpoint);
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("Prefer", "resolution=merge-duplicates");
    http.addHeader("apikey", apiKey);
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = http.PATCH(payload);
    
    if (httpCode >= 200 && httpCode < 300) {
        Serial.printf("✅ Device status atualizado: %d\n", httpCode);
        http.end();
        return true;
    } else {
        // Se falhou, tentar INSERT
        http.end();
        return makeRequest("POST", SUPABASE_STATUS_TABLE, payload);
    }
}

String SupabaseClient::buildEnvironmentPayload(const EnvironmentReading& reading) {
    DynamicJsonDocument doc(256);
    
    doc["device_id"] = getDeviceID();
    doc["temperature"] = reading.temperature;
    doc["humidity"] = reading.humidity;
    
    String payload;
    serializeJson(doc, payload);
    return payload;
}

String SupabaseClient::buildHydroPayload(const HydroReading& reading) {
    DynamicJsonDocument doc(256);
    
    doc["device_id"] = getDeviceID();
    doc["temperature"] = reading.temperature;
    doc["ph"] = reading.ph;
    doc["tds"] = reading.tds;
    doc["water_level_ok"] = reading.waterLevelOk;
    
    String payload;
    serializeJson(doc, payload);
    return payload;
}

String SupabaseClient::buildDeviceStatusPayload(const DeviceStatusData& status) {
    DynamicJsonDocument doc(1024);
    
    doc["device_id"] = status.deviceId;
    doc["last_seen"] = "now()";
    doc["wifi_rssi"] = status.wifiRssi;
    doc["free_heap"] = status.freeHeap;
    doc["uptime_seconds"] = status.uptimeSeconds;
    doc["is_online"] = status.isOnline;
    doc["firmware_version"] = status.firmwareVersion;
    doc["ip_address"] = status.ipAddress;
    doc["updated_at"] = "now()";
    
    // Array de estados dos relés
    JsonArray relayArray = doc.createNestedArray("relay_states");
    for (int i = 0; i < 16; i++) {
        relayArray.add(status.relayStates[i]);
    }
    
    String payload;
    serializeJson(doc, payload);
    return payload;
}

bool SupabaseClient::checkForCommands(RelayCommand* commands, int maxCommands, int& commandCount) {
    if (!isReady()) {
        return false;
    }
    
    // Verificar apenas a cada COMMAND_POLL_INTERVAL_MS
    unsigned long now = millis();
    if (now - lastCommandCheck < COMMAND_POLL_INTERVAL_MS) {
        return false;
    }
    lastCommandCheck = now;
    
    // BUSCAR COMANDOS PENDENTES usando a mesma query do SQL
    String endpoint = String(SUPABASE_RELAY_TABLE) + "?device_id=eq." + getDeviceID() + "&status=eq.pending&order=created_at.asc&limit=" + maxCommands;
    String fullUrl = baseUrl + "/rest/v1/" + endpoint;
    
    Serial.printf("🔍 Verificando comandos: %s\n", fullUrl.c_str());
    
    // Configurar cliente HTTP com settings robustos
    HTTPClient httpClient;
    WiFiClientSecure* secureClient = new WiFiClientSecure();
    secureClient->setInsecure(); // Para desenvolvimento - aceitar certificados auto-assinados
    
    httpClient.begin(*secureClient, fullUrl);
    httpClient.setConnectTimeout(10000); // 10s timeout conexão
    httpClient.setTimeout(15000); // 15s timeout total
    httpClient.setUserAgent("ESP32-Hydro/2.1.0");
    
    httpClient.addHeader("Authorization", buildAuthHeader());
    httpClient.addHeader("apikey", apiKey);
    httpClient.addHeader("Accept", "application/json");
    
    Serial.println("📡 Enviando requisição GET para comandos...");
    int httpCode = httpClient.GET();
    
    if (httpCode == 200) {
        String response = httpClient.getString();
        httpClient.end();
        
        Serial.printf("✅ Resposta recebida: %d bytes\n", response.length());
        
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, response);
        
        if (error) {
            setError("Erro ao parsear comandos JSON: " + String(error.c_str()));
            return false;
        }
        
        JsonArray commandsArray = doc.as<JsonArray>();
        commandCount = min((int)commandsArray.size(), maxCommands);
        
        for (int i = 0; i < commandCount; i++) {
            JsonObject cmd = commandsArray[i];
            commands[i].id = cmd["id"];
            commands[i].relayNumber = cmd["relay_number"];
            commands[i].action = cmd["action"].as<String>();
            commands[i].durationSeconds = cmd["duration_seconds"] | 0;
            commands[i].status = cmd["status"].as<String>();
            commands[i].timestamp = millis(); // Timestamp local
        }
        
        if (commandCount > 0) {
            Serial.printf("📥 Recebidos %d comandos de relé pendentes\n", commandCount);
        }
        
        return true;
    } else if (httpCode > 0) {
        // Código HTTP válido mas com erro
        String response = httpClient.getString();
        setError("Erro HTTP ao buscar comandos: " + String(httpCode) + " - " + response);
        Serial.printf("❌ HTTP Error %d: %s\n", httpCode, response.c_str());
        httpClient.end();
        return false;
    } else {
        // Erro de conexão (códigos negativos)
        String errorMsg = "Erro de conexão ao buscar comandos: HTTP " + String(httpCode);
        
        // Diagnóstico específico para erros comuns
        switch (httpCode) {
            case -1:
                errorMsg += " (Connection refused - servidor rejeitou conexão)";
                break;
            case -2:
                errorMsg += " (Send header failed - falha ao enviar cabeçalhos)";
                break;
            case -3:
                errorMsg += " (Send payload failed - falha ao enviar dados)";
                break;
            case -4:
                errorMsg += " (Not connected - sem conexão de rede)";
                break;
            case -5:
                errorMsg += " (Connection lost - conexão perdida)";
                break;
            case -6:
                errorMsg += " (No stream - sem fluxo de dados)";
                break;
            case -7:
                errorMsg += " (No HTTP server - servidor não encontrado/DNS falhou)";
                break;
            case -8:
                errorMsg += " (Too less RAM - memória insuficiente)";
                break;
            case -11:
                errorMsg += " (Timeout - tempo esgotado)";
                break;
            default:
                errorMsg += " (Erro desconhecido)";
        }
        
        setError(errorMsg);
        
        // Verificar status WiFi
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("⚠️ WiFi desconectado durante requisição!");
            isConnected = false;
        }
        
        httpClient.end();
        return false;
    }
}

bool SupabaseClient::markCommandSent(int commandId) {
    String endpoint = String(SUPABASE_RELAY_TABLE) + "?id=eq." + commandId;
    String payload = "{\"status\": \"sent\", \"sent_at\": \"now()\"}";
    
    http.begin(baseUrl + "/rest/v1/" + endpoint);
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("apikey", apiKey);
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = http.PATCH(payload);
    http.end();
    
    return (httpCode >= 200 && httpCode < 300);
}

bool SupabaseClient::markCommandCompleted(int commandId) {
    String endpoint = String(SUPABASE_RELAY_TABLE) + "?id=eq." + commandId;
    String payload = "{\"status\": \"completed\", \"completed_at\": \"now()\"}";
    
    http.begin(baseUrl + "/rest/v1/" + endpoint);
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("apikey", apiKey);
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = http.PATCH(payload);
    http.end();
    
    return (httpCode >= 200 && httpCode < 300);
}

bool SupabaseClient::markCommandFailed(int commandId, const String& errorMessage) {
    String endpoint = String(SUPABASE_RELAY_TABLE) + "?id=eq." + commandId;
    
    DynamicJsonDocument doc(256);
    doc["status"] = "failed";
    doc["error_message"] = errorMessage;
    doc["completed_at"] = "now()";
    
    String payload;
    serializeJson(doc, payload);
    
    http.begin(baseUrl + "/rest/v1/" + endpoint);
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("apikey", apiKey);
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = http.PATCH(payload);
    http.end();
    
    return (httpCode >= 200 && httpCode < 300);
}

bool SupabaseClient::testConnection() {
    Serial.println("🧪 Testando conexão com Supabase...");
    
    // Verificar WiFi primeiro
    if (WiFi.status() != WL_CONNECTED) {
        setError("WiFi não conectado durante teste");
        return false;
    }
    
    Serial.printf("📡 WiFi OK - IP: %s\n", WiFi.localIP().toString().c_str());
    
    // Configurar cliente HTTP para teste
    HTTPClient testHttp;
    WiFiClientSecure* secureClient = new WiFiClientSecure();
    secureClient->setInsecure(); // Para desenvolvimento - aceitar certificados auto-assinados
    
    String testUrl = baseUrl + "/rest/v1/";
    Serial.printf("🌐 Testando URL: %s\n", testUrl.c_str());
    
    testHttp.begin(*secureClient, testUrl);
    testHttp.setConnectTimeout(15000); // 15s timeout conexão
    testHttp.setTimeout(20000); // 20s timeout total
    testHttp.setUserAgent("ESP32-Hydro/2.1.0");
    
    testHttp.addHeader("apikey", apiKey);
    testHttp.addHeader("Accept", "application/json");
    
    int httpCode = testHttp.GET();
    
    if (httpCode >= 200 && httpCode < 300) {
        Serial.printf("✅ Teste de conexão OK: HTTP %d\n", httpCode);
        testHttp.end();
        return true;
    } else if (httpCode > 0) {
        String response = testHttp.getString();
        Serial.printf("❌ Teste falhou: HTTP %d - %s\n", httpCode, response.c_str());
        setError("Teste de conexão falhou: HTTP " + String(httpCode));
        testHttp.end();
        return false;
    } else {
        // Erro de conexão
        String errorMsg = "Erro de conexão durante teste: HTTP " + String(httpCode);
        
        switch (httpCode) {
            case -7:
                errorMsg += " - Servidor não encontrado (verifique DNS)";
                Serial.println("🔍 Dica: Verifique se o DNS está funcionando");
                Serial.println("🔍 Tente ping google.com ou 8.8.8.8");
                break;
            case -4:
                errorMsg += " - Sem conexão de rede";
                break;
            case -11:
                errorMsg += " - Timeout (conexão muito lenta)";
                break;
        }
        
        setError(errorMsg);
        testHttp.end();
        return false;
    }
}

void SupabaseClient::setError(const String& error) {
    lastError = error;
    Serial.println("❌ SupabaseClient: " + error);
}

// ===== FUNCIÓN DE AUTO-REGISTRO =====
bool SupabaseClient::autoRegisterDevice(const String& deviceName, const String& location) {
    if (!isConnected) {
        setError("Supabase não conectado para auto-registro");
        return false;
    }
    
    Serial.println("🆔 Iniciando auto-registro do dispositivo...");
    
    // Preparar dados do dispositivo
    DynamicJsonDocument doc(512);
    doc["device_id"] = getDeviceID();
    doc["mac_address"] = getFullMAC();
    doc["ip_address"] = WiFi.localIP().toString();
    doc["device_name"] = deviceName.isEmpty() ? ("ESP32 - " + getMACsuffix()) : deviceName;
    doc["location"] = location.isEmpty() ? "Ubicación no especificada" : location;
    doc["device_type"] = "ESP32_HYDROPONIC";
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["is_online"] = true;
    
    String payload;
    serializeJson(doc, payload);
    
    Serial.printf("📤 Payload auto-registro: %s\n", payload.c_str());
    
    // Usar UPSERT (INSERT com ON CONFLICT)
    String endpoint = String(SUPABASE_STATUS_TABLE);
    String fullUrl = baseUrl + "/rest/v1/" + endpoint;
    
    http.begin(fullUrl);
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("apikey", apiKey);
    http.addHeader("Prefer", "resolution=merge-duplicates"); // UPSERT mode
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = http.POST(payload);
    String response = http.getString();
    http.end();
    
    if (httpCode >= 200 && httpCode < 300) {
        Serial.printf("✅ Dispositivo auto-registrado: %s\n", getDeviceID().c_str());
        Serial.printf("📍 Nome: %s | Localização: %s\n", 
                     deviceName.c_str(), location.c_str());
        return true;
    } else {
        Serial.printf("❌ Erro no auto-registro - HTTP %d: %s\n", httpCode, response.c_str());
        setError("Auto-registro falhou: " + String(httpCode));
        return false;
    }
}

// ===== MÉTODO GENÉRICO PARA INSERIR DADOS =====
bool SupabaseClient::insert(const String& table, const String& jsonData) {
    if (!isReady()) {
        setError("Supabase não está pronto");
        return false;
    }
    
    String endpoint = table;
    return makeRequest("POST", endpoint, jsonData);
} 