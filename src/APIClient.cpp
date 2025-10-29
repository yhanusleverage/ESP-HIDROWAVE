#include "APIClient.h"
#include "Config.h"
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <WiFi.h>

APIClient::APIClient() {
    supabaseUrl = "";
    supabaseKey = "";
    deviceId = "";
}

bool APIClient::queueSensorData(const SensorData& data) {
    if (!data.valid) return false;
    sensorBuffer.push(data);
    return true;
}

bool APIClient::queueSystemStatus(const SystemStatus& status) {
    statusBuffer.push(status);
    return true;
}

bool APIClient::shouldSendBatch() {
    if (millis() - lastSendTime >= BATCH_INTERVAL) return true;
    if (sensorBuffer.size() >= MAX_BATCH_SIZE) return true;
    return false;
}

bool APIClient::sendBatch() {
    if (!isConnected() || sensorBuffer.isEmpty()) return false;

    DynamicJsonDocument doc(2048);
    JsonArray sensorArray = doc.createNestedArray("sensors");
    JsonArray statusArray = doc.createNestedArray("status");

    // Processar dados dos sensores
    while (!sensorBuffer.isEmpty() && sensorArray.size() < MAX_BATCH_SIZE) {
        SensorData data = sensorBuffer.shift();
        JsonObject sensorObj = sensorArray.createNestedObject();
        sensorObj["timestamp"] = data.timestamp;
        sensorObj["envTemp"] = data.environmentTemp;
        sensorObj["envHumidity"] = data.environmentHumidity;
        sensorObj["waterTemp"] = data.waterTemp;
        sensorObj["ph"] = data.ph;
        sensorObj["tds"] = data.tds;
        sensorObj["waterLevelOk"] = data.waterLevelOk;
    }

    // Processar status do sistema
    while (!statusBuffer.isEmpty() && statusArray.size() < MAX_BATCH_SIZE) {
        SystemStatus status = statusBuffer.shift();
        JsonObject statusObj = statusArray.createNestedObject();
        statusObj["timestamp"] = millis();
        statusObj["wifiConnected"] = status.wifiConnected;
        statusObj["apiConnected"] = status.apiConnected;
        statusObj["sensorsOk"] = status.sensorsOk;
        statusObj["relaysOk"] = status.relaysOk;
        statusObj["freeHeap"] = status.freeHeap;
        statusObj["wifiRSSI"] = status.wifiRSSI;
    }

    // Preparar payload
    String payload;
    serializeJson(doc, payload);

    // Enviar para Supabase
    String response;
    bool success = makeSupabaseRequest("/api/sensor-data-unified", payload, response);

    if (success) {
        lastSendTime = millis();
        return true;
    }

    return false;
}

void APIClient::update() {
    if (!isConnected()) return;

    unsigned long currentTime = millis();

    // Verificar comandos dos relés (tempo real)
    if (currentTime - lastCheckTime >= POLLING_INTERVAL) {
        checkRelayCommands(deviceId);
        lastCheckTime = currentTime;
    }

    // Enviar dados em lote se necessário
    if (shouldSendBatch()) {
        sendBatch();
    }
}

bool APIClient::makeSupabaseRequest(const String& endpoint, const String& payload, String& response) {
    if (!isConnected() || supabaseUrl.length() == 0) return false;

    http.begin(supabaseUrl + endpoint);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", supabaseKey);
    http.addHeader("Authorization", "Bearer " + supabaseKey);
    http.setTimeout(5000);

    int httpCode = http.POST(payload);
    bool success = (httpCode == 200);
    
    if (success) {
        response = http.getString();
    }
    
    http.end();
    return success;
}

bool APIClient::testConnection() {
    if (!isConnected()) return false;

    // Testar Supabase
    String response;
    if (!makeSupabaseRequest("/api/sensor-data-unified", "{\"test\":true}", response)) {
        return false;
    }

    return true;
}

// ===== MÉTODOS PARA SUPABASE =====

bool APIClient::checkRelayCommands(const String& deviceId) {
    if (!isConnected()) {
        Serial.println("❌ WiFi não conectado - não é possível verificar comandos");
        return false;
    }

    HTTPClient http;
    String url = supabaseUrl + "/api/relay-commands?device_id=" + deviceId + "&status=pending";
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    int httpCode = http.GET();
    
    if (httpCode == 200) {
        String payload = http.getString();
        
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, payload);
        
        if (error) {
            Serial.println("❌ Erro ao parsear JSON dos comandos: " + String(error.c_str()));
            http.end();
            return false;
        }
        
        if (doc["success"] && doc["commands"].is<JsonArray>()) {
            JsonArray commands = doc["commands"];
            
            Serial.println("📥 Recebidos " + String(commands.size()) + " comando(s) pendente(s)");
            
            // Processar cada comando
            for (JsonObject cmd : commands) {
                processRelayCommand(cmd);
            }
            
            http.end();
            return true;
        }
    } else {
        Serial.println("❌ Erro HTTP ao verificar comandos: " + String(httpCode));
    }
    
    http.end();
    return false;
}

void APIClient::processRelayCommand(JsonObject command) {
    int commandId = command["id"];
    int relayNumber = command["relay_number"];
    String action = command["action"];
    int duration = command["duration_seconds"] | 0;
    
    Serial.println("🔌 ===== PROCESSANDO COMANDO =====");
    Serial.println("📅 ID: " + String(commandId));
    Serial.println("🔢 Relé: " + String(relayNumber));
    Serial.println("⚡ Ação: " + action);
    Serial.println("⏱️ Duração: " + String(duration) + "s");
    Serial.println("===============================");
    
    // Executar comando via callback
    if (relayCallback) {
        relayCallback(relayNumber, action, duration, String(commandId));
    }
}

bool APIClient::confirmCommandExecution(const String& commandId, const String& status, const String& errorMessage) {
    if (!isConnected()) {
        Serial.println("❌ WiFi não conectado - não é possível confirmar comando");
        return false;
    }

    HTTPClient http;
    String url = supabaseUrl + "/api/relay-commands";
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    DynamicJsonDocument doc(512);
    doc["command_id"] = commandId.toInt();
    doc["status"] = status;
    if (errorMessage.length() > 0) {
        doc["error_message"] = errorMessage;
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    int httpCode = http.PUT(jsonString);
    
    if (httpCode == 200) {
        Serial.println("✅ Comando " + commandId + " confirmado como " + status);
        http.end();
        return true;
    } else {
        Serial.println("❌ Erro ao confirmar comando: " + String(httpCode));
    }

    http.end();
    return false;
}

bool APIClient::sendDeviceStatus(const String& deviceId, const DeviceStatusData& statusData) {
    if (!isConnected()) {
        Serial.println("❌ WiFi não conectado - não é possível enviar status");
        return false;
    }

    HTTPClient http;
    String url = supabaseUrl + "/api/device-status-unified";
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    DynamicJsonDocument doc(2048);
    doc["device_id"] = deviceId;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["uptime_seconds"] = millis() / 1000;
    doc["firmware_version"] = statusData.firmwareVersion;
    doc["ip_address"] = WiFi.localIP().toString();
    
    // Array de estados dos relés
    JsonArray relayStates = doc.createNestedArray("relay_states");
    for (int i = 0; i < 16; i++) {
        relayStates.add(statusData.relayStates[i]);
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    int httpCode = http.POST(jsonString);
    
    if (httpCode == 200) {
        Serial.println("✅ Status do dispositivo enviado com sucesso");
        http.end();
        return true;
    } else {
        Serial.println("❌ Erro ao enviar status: " + String(httpCode));
    }
    
    http.end();
    return false;
}

bool APIClient::sendSensorData(const String& deviceId, const SensorData& data) {
    if (!isConnected()) {
        Serial.println("❌ WiFi não conectado - não é possível enviar dados dos sensores");
        return false;
    }

    HTTPClient http;
    String url = supabaseUrl + "/api/sensor-data-unified";
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    DynamicJsonDocument doc(1024);
    doc["device_id"] = deviceId;
    
    // Dados ambientais
    if (!isnan(data.environmentTemp) && !isnan(data.environmentHumidity)) {
        doc["environment_temperature"] = data.environmentTemp;
        doc["environment_humidity"] = data.environmentHumidity;
    }
    
    // Dados hidropônicos
    if (!isnan(data.waterTemp)) {
        doc["water_temperature"] = data.waterTemp;
    }
    if (!isnan(data.ph)) {
        doc["ph"] = data.ph;
    }
    if (!isnan(data.tds)) {
        doc["tds"] = data.tds;
    }
    doc["water_level_ok"] = data.waterLevelOk;
    
    doc["timestamp"] = data.timestamp;
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    int httpCode = http.POST(jsonString);
    
    if (httpCode == 200) {
        Serial.println("✅ Dados dos sensores enviados com sucesso");
        http.end();
    return true;
    } else {
        Serial.println("❌ Erro ao enviar dados dos sensores: " + String(httpCode));
    }
    
    http.end();
    return false;
}

void APIClient::setRelayCallback(void (*callback)(int relayNumber, String action, int duration, String commandId)) {
    this->relayCallback = callback;
} 