#include "DeviceRegistration.h"
#include "Config.h"

static String freshMacAddress() {
    String mac = getFullMAC();
    if (mac.length() < 11 || mac == "00:00:00:00:00:00") {
        mac = WiFi.macAddress();
        mac.toUpperCase();
    }
    return mac;
}

DeviceRegistration::DeviceRegistration() : 
    isRegistered(false),
    deviceId(getDeviceID()),
    macAddress(freshMacAddress()) {
}

void DeviceRegistration::setSupabaseConfig(const String& url, const String& key) {
    supabaseUrl = url;
    supabaseKey = key;
}

bool DeviceRegistration::registerDeviceWithEmail(const String& email, const String& deviceName, const String& location) {
    deviceId = getDeviceID();
    macAddress = freshMacAddress();
    Serial.println("📧 Registrando dispositivo com email: " + email);
    Serial.println("📶 MAC atual: " + macAddress);
    Serial.println("🆔 Device ID: " + deviceId);
    
    String normalizedEmail = email;
    normalizedEmail.trim();
    normalizedEmail.toLowerCase();
    normalizedEmail.replace("\"", "");
    normalizedEmail.replace("'", "");
    normalizedEmail.replace(" ", "");
    
    // Validar email
    if (!validateEmail(normalizedEmail)) {
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
    String payload = buildRegistrationPayload(normalizedEmail, deviceName, location);
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
            userEmail = normalizedEmail;

            // Só zera NVS no portal (WiFiConfigServer) ou troca de dono — não em todo boot
            if (doc.containsKey("owner_changed") && doc["owner_changed"].as<bool>()) {
                resetRebootCount();
                Serial.println("🔄 Dono alterado — reboot_count NVS zerado");
            }

            Serial.println("🎉 Dispositivo registrado com sucesso!");
            Serial.println("👤 Email pedido: " + normalizedEmail);
            if (doc.containsKey("stored_user_email")) {
                Serial.println("👤 Email na BD: " + doc["stored_user_email"].as<String>());
            }
            if (doc.containsKey("owner_changed") && doc["owner_changed"].as<bool>()) {
                String prev = doc.containsKey("previous_owner_email")
                    ? doc["previous_owner_email"].as<String>() : "";
                Serial.println("🔄 Dono do equipamento alterado (antes: " + prev + ")");
            }
            if (doc.containsKey("email_applied") && !doc["email_applied"].as<bool>()) {
                Serial.println("⚠️ Email nao gravado — verifique o RPC no Supabase");
            }
            Serial.println("🆔 Device ID: " + deviceId);
            Serial.println("📍 Localização enviada: " + location);
            Serial.println("📱 Total dispositivos deste email: " + String(doc["device_count"].as<int>()));
            if (doc.containsKey("user_profile_ensured")) {
                Serial.println(String("👤 Perfil public.users: ") +
                    (doc["user_profile_ensured"].as<bool>() ? "OK" : "FALHOU"));
            }
            if (doc.containsKey("total_devices")) {
                Serial.println("👤 users.total_devices: " + String(doc["total_devices"].as<int>()));
            }
            if (doc.containsKey("reboot_count_reset") && doc["reboot_count_reset"].as<bool>()) {
                Serial.println("🔄 reboot_count na BD zerado (novo ciclo no portal)");
            }
            return true;
        } else if (response.indexOf("\"success\": true") >= 0 ||
                   response.indexOf("\"success\":true") >= 0) {
            // JSON malformado no RPC mas registro OK (ex.: ""user_email" typo no payload)
            isRegistered = true;
            userEmail = normalizedEmail;
            Serial.println("🎉 Dispositivo registrado com sucesso! (parse fallback — JSON RPC malformado)");
            Serial.println("👤 Email pedido: " + normalizedEmail);
            Serial.println("🆔 Device ID: " + deviceId);
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
