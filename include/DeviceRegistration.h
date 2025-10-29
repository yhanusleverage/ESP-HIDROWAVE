#ifndef DEVICE_REGISTRATION_H
#define DEVICE_REGISTRATION_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "DeviceID.h"

/**
 * DeviceRegistration - Sistema de Registro por Email
 * 
 * Funcionalidades:
 * - Registra ESP32 com email do usuário
 * - Envia dados para Supabase via função register_device_with_email
 * - Valida limites de dispositivos
 * - Integração com WiFiConfigServer
 */
class DeviceRegistration {
private:
    HTTPClient http;
    String supabaseUrl;
    String supabaseKey;
    
    // Dados do dispositivo
    String deviceId;
    String macAddress;
    String userEmail;
    
    // Status
    bool isRegistered;
    String lastError;
    
    // Métodos privados
    bool makeSupabaseRequest(const String& endpoint, const String& payload, String& response);
    String buildRegistrationPayload(const String& email, const String& deviceName, const String& location);
    
public:
    DeviceRegistration();
    
    // Configuração
    void setSupabaseConfig(const String& url, const String& key);
    
    // Registro principal
    bool registerDeviceWithEmail(
        const String& email, 
        const String& deviceName = "", 
        const String& location = ""
    );
    
    // Verificações
    bool canAddDevice(const String& email);
    bool isDeviceRegistered() const { return isRegistered; }
    
    // Informações
    String getDeviceId() const { return deviceId; }
    String getUserEmail() const { return userEmail; }
    String getLastError() const { return lastError; }
    
    // Utilitários
    bool validateEmail(const String& email);
    void printRegistrationInfo();
};

// ===== FUNÇÕES GLOBAIS PARA FACILITAR USO =====

// Função principal para registrar durante configuração WiFi
bool registerDeviceWithEmail(const String& userEmail, const String& deviceName = "ESP32 Hidropônico", const String& location = "Estufa");

// Função para verificar se pode adicionar dispositivo
bool canUserAddDevice(const String& userEmail);

#endif // DEVICE_REGISTRATION_H
