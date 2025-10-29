#include "DeviceID.h"
#include <WiFi.h>

// Variable global para cache del device_id
static String cachedDeviceID = "";
static String cachedMACsuffix = "";
static String cachedFullMAC = "";

String generateDeviceID() {
    // Obtener MAC address
    String mac = WiFi.macAddress();
    
    // Validar que el MAC no sea inválido
    if (mac == "00:00:00:00:00:00" || mac.length() < 17) {
        Serial.println("⚠️ MAC address inválido, usando EfuseMac como fallback");
        // Usar EfuseMac como fallback
        uint64_t efuseMac = ESP.getEfuseMac();
        mac = String((uint32_t)(efuseMac >> 16), HEX);
        mac.toUpperCase();
        while (mac.length() < 6) {
            mac = "0" + mac;
        }
    } else {
        // Remover los dos puntos y convertir a mayúsculas
        mac.replace(":", "");
        mac.toUpperCase();
    }
    
    // Tomar los últimos 6 caracteres del MAC (3 bytes)
    String macSuffix = mac.substring(mac.length() - MAC_SUFFIX_LENGTH);
    
    // Crear device_id: ESP32_HIDRO_ + últimos 6 caracteres del MAC
    String deviceId = String(DEVICE_ID_PREFIX) + macSuffix;
    
    Serial.printf("🆔 MAC original: %s\n", WiFi.macAddress().c_str());
    Serial.printf("🆔 MAC procesado: %s\n", mac.c_str());
    Serial.printf("🆔 Device ID generado: %s\n", deviceId.c_str());
    
    return deviceId;
}

String getDeviceID() {
    if (cachedDeviceID == "" || cachedDeviceID == "ESP32_HIDRO_000000") {
        cachedDeviceID = generateDeviceID();
        Serial.println("🆔 Device ID generado: " + cachedDeviceID);
    }
    return cachedDeviceID;
}

// Función para forzar regeneración del device_id
void forceRegenerateDeviceID() {
    cachedDeviceID = "";
    cachedMACsuffix = "";
    cachedFullMAC = "";
    Serial.println("🔄 Cache de Device ID limpiado, regenerando...");
}

String getMACsuffix() {
    if (cachedMACsuffix == "") {
        String mac = WiFi.macAddress();
        mac.replace(":", "");
        mac.toUpperCase();
        cachedMACsuffix = mac.substring(mac.length() - MAC_SUFFIX_LENGTH);
    }
    return cachedMACsuffix;
}

String getFullMAC() {
    if (cachedFullMAC == "") {
        cachedFullMAC = WiFi.macAddress();
        cachedFullMAC.toUpperCase();
    }
    return cachedFullMAC;
}

String getDeviceInfo() {
    String info = "🆔 Device ID: " + getDeviceID() + "\n";
    info += "📶 MAC Address: " + getFullMAC() + "\n";
    info += "🔢 MAC Suffix: " + getMACsuffix() + "\n";
    info += "📡 Chip ID: " + String((uint32_t)ESP.getEfuseMac(), HEX) + "\n";
    info += "💾 Chip Model: " + String(ESP.getChipModel()) + "\n";
    info += "🔄 Chip Revision: " + String(ESP.getChipRevision()) + "\n";
    info += "⚡ CPU Freq: " + String(ESP.getCpuFreqMHz()) + " MHz";
    
    return info;
}

// Función para forzar regeneración del cache (útil para testing)
void clearDeviceIDCache() {
    cachedDeviceID = "";
    cachedMACsuffix = "";
    cachedFullMAC = "";
    Serial.println("🗑️ Cache de Device ID limpiado");
}

// Función para validar el formato del device_id
bool isValidDeviceID(const String& deviceId) {
    // Verificar que empiece con el prefijo correcto
    if (!deviceId.startsWith(DEVICE_ID_PREFIX)) {
        return false;
    }
    
    // Verificar longitud total
    int expectedLength = strlen(DEVICE_ID_PREFIX) + MAC_SUFFIX_LENGTH;
    if (deviceId.length() != expectedLength) {
        return false;
    }
    
    // Verificar que el sufijo sean solo caracteres hexadecimales
    String suffix = deviceId.substring(strlen(DEVICE_ID_PREFIX));
    for (int i = 0; i < suffix.length(); i++) {
        char c = suffix.charAt(i);
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    
    return true;
}

// Función para comparar device_ids (útil para identificar dispositivos)
bool isSameDevice(const String& deviceId1, const String& deviceId2) {
    return deviceId1.equals(deviceId2);
}

// Función para extraer solo el sufijo MAC de un device_id
String extractMACsuffixFromDeviceID(const String& deviceId) {
    if (!isValidDeviceID(deviceId)) {
        return "";
    }
    
    return deviceId.substring(strlen(DEVICE_ID_PREFIX));
}
