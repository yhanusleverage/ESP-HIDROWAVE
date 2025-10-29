#include "DeviceRegistration.h"

DeviceRegistration::DeviceRegistration() : 
    isRegistered(false),
    deviceId(getDeviceID()),
    macAddress(WiFi.macAddress()) {
}

void DeviceRegistration::setSupabaseConfig(const String& url, const String& key) {
    supabaseUrl = url;
    supabaseKey = key;
}

bool DeviceRegistration::registerDeviceWithEmail(const String& email, const String& deviceName, const String& location) {
    Serial.println("📧 Registrando dispositivo com email: " + email);
    
    // Validar email
    if (!validateEmail(email)) {
        lastError = "Email inválido";
        Serial.println("❌ " + lastError);
        return false;
    }
    
    // Verificar configuração Supabase
    if (supabaseUrl.isEmpty() || supabaseKey.isEmpty()) {
        setSupabaseConfig(SUPABASE_URL, SUPABASE_ANON_KEY);
    }
    
    // Verificar WiFi
    if (WiFi.status() != WL_CONNECTED) {
        lastError = "WiFi não conectado";
        Serial.println("❌ " + lastError);
        return false;
    }
    
    // Construir payload
    String payload = buildRegistrationPayload(email, deviceName, location);
    Serial.println("📤 Payload: " + payload);
    
    // Fazer requisição
    String response;
    if (makeSupabaseRequest("/rest/v1/rpc/register_device_with_email", payload, response)) {
        Serial.println("✅ Resposta do servidor: " + response);
        
        // Parse da resposta
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, response);
        
        if (!error && doc["success"].as<bool>()) {
            isRegistered = true;
            userEmail = email;
            Serial.println("🎉 Dispositivo registrado com sucesso!");
            Serial.println("👤 Email: " + email);
            Serial.println("🆔 Device ID: " + deviceId);
            Serial.println("📱 Total dispositivos: " + String(doc["device_count"].as<int>()));
            return true;
        } else {
            lastError = doc["message"].as<String>();
            if (lastError.isEmpty()) lastError = "Erro na resposta do servidor";
            Serial.println("❌ Erro no registro: " + lastError);
            return false;
        }
    }
    
    return false;
}

bool DeviceRegistration::canAddDevice(const String& email) {
    String payload = "{\"p_user_email\":\"" + email + "\"}";
    String response;
    
    if (makeSupabaseRequest("/rest/v1/rpc/can_add_device", payload, response)) {
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, response);
        
        if (!error) {
            bool canAdd = doc["can_add"].as<bool>();
            Serial.println("🔍 Usuário " + email + " pode adicionar: " + (canAdd ? "SIM" : "NÃO"));
            if (!canAdd) {
                Serial.println("📊 Atual: " + String(doc["current_count"].as<int>()) + 
                              " / Máximo: " + String(doc["max_allowed"].as<int>()));
            }
            return canAdd;
        }
    }
    
    return true; // Em caso de erro, permitir (fail-safe)
}

bool DeviceRegistration::makeSupabaseRequest(const String& endpoint, const String& payload, String& response) {
    if (ESP.getFreeHeap() < 30000) {
        lastError = "Memória insuficiente para HTTPS";
        Serial.println("⚠️ " + lastError);
        return false;
    }
    
    http.begin(supabaseUrl + endpoint);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", supabaseKey);
    http.addHeader("Authorization", "Bearer " + supabaseKey);
    http.setTimeout(10000);
    
    Serial.println("🌐 Fazendo requisição para: " + supabaseUrl + endpoint);
    
    int httpCode = http.POST(payload);
    
    if (httpCode == 200) {
        response = http.getString();
        http.end();
        return true;
    } else {
        lastError = "HTTP Error: " + String(httpCode);
        if (httpCode > 0) {
            String errorResponse = http.getString();
            Serial.println("❌ Erro HTTP " + String(httpCode) + ": " + errorResponse);
            lastError += " - " + errorResponse;
        } else {
            Serial.println("❌ Erro de conexão: " + String(httpCode));
        }
        http.end();
        return false;
    }
}

String DeviceRegistration::buildRegistrationPayload(const String& email, const String& deviceName, const String& location) {
    StaticJsonDocument<512> doc;
    
    doc["p_device_id"] = deviceId;
    doc["p_mac_address"] = macAddress;
    doc["p_user_email"] = email;
    doc["p_ip_address"] = WiFi.localIP().toString();
    
    String finalDeviceName = deviceName;
    if (finalDeviceName.isEmpty()) {
        finalDeviceName = "ESP32 - " + macAddress.substring(macAddress.length() - 8);
    }
    doc["p_device_name"] = finalDeviceName;
    
    String finalLocation = location;
    if (finalLocation.isEmpty()) {
        finalLocation = "Localização não especificada";
    }
    doc["p_location"] = finalLocation;
    
    String payload;
    serializeJson(doc, payload);
    return payload;
}

bool DeviceRegistration::validateEmail(const String& email) {
    // Validação simples de email
    if (email.length() < 5) return false;
    if (email.indexOf('@') == -1) return false;
    if (email.indexOf('.') == -1) return false;
    if (email.indexOf('@') > email.lastIndexOf('.')) return false;
    
    return true;
}

void DeviceRegistration::printRegistrationInfo() {
    Serial.println("\n📋 === INFORMAÇÕES DE REGISTRO ===");
    Serial.println("🆔 Device ID: " + deviceId);
    Serial.println("📶 MAC Address: " + macAddress);
    Serial.println("🌐 IP Address: " + WiFi.localIP().toString());
    Serial.println("👤 Email: " + (userEmail.isEmpty() ? "Não registrado" : userEmail));
    Serial.println("✅ Registrado: " + String(isRegistered ? "SIM" : "NÃO"));
    if (!lastError.isEmpty()) {
        Serial.println("❌ Último erro: " + lastError);
    }
    Serial.println("================================\n");
}

// ===== IMPLEMENTAÇÃO DAS FUNÇÕES GLOBAIS =====

// Instância global para facilitar uso
DeviceRegistration globalDeviceRegistration;

bool registerDeviceWithEmail(const String& userEmail, const String& deviceName, const String& location) {
    globalDeviceRegistration.setSupabaseConfig(SUPABASE_URL, SUPABASE_ANON_KEY);
    return globalDeviceRegistration.registerDeviceWithEmail(userEmail, deviceName, location);
}

bool canUserAddDevice(const String& userEmail) {
    globalDeviceRegistration.setSupabaseConfig(SUPABASE_URL, SUPABASE_ANON_KEY);
    return globalDeviceRegistration.canAddDevice(userEmail);
}
