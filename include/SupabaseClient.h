#ifndef SUPABASE_CLIENT_H
#define SUPABASE_CLIENT_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "Config.h"

struct EnvironmentReading {
    float temperature;
    float humidity;
    unsigned long timestamp;
};

struct HydroReading {
    float temperature;
    float ph;
    float tds;
    bool waterLevelOk;
    unsigned long timestamp;
};

struct RelayCommand {
    int id;                    // ID do comando no banco
    int relayNumber;           // 0-15
    String action;             // "on" ou "off"
    int durationSeconds;       // duração em segundos (opcional)
    String status;             // "pending", "sent", "completed", "failed"
    unsigned long timestamp;
};

struct DeviceStatusData {
    String deviceId;
    int wifiRssi;
    uint32_t freeHeap;
    unsigned long uptimeSeconds;
    bool relayStates[16];      // Array de 16 relés
    bool isOnline;
    String firmwareVersion;
    String ipAddress;
    unsigned long timestamp;
};

class SupabaseClient {
private:
    HTTPClient http;
    String baseUrl;
    String apiKey;
    bool isConnected;
    unsigned long lastCommandCheck;
    
    String buildAuthHeader();
    bool makeRequest(const String& method, const String& endpoint, const String& payload = "");
    String buildRelayStatePayload(bool* relayStates, int numRelays);

public:
    SupabaseClient();
    ~SupabaseClient();
    
    bool begin(const String& url, const String& key);
    bool isReady() const { return isConnected && WiFi.status() == WL_CONNECTED; }
    
    // Enviar dados para Supabase
    bool sendEnvironmentData(const EnvironmentReading& reading);
    bool sendHydroData(const HydroReading& reading);
    bool updateDeviceStatus(const DeviceStatusData& status);
    
    // Método genérico para inserir dados
    bool insert(const String& table, const String& jsonData);
    
    // Receber comandos de Supabase
    bool checkForCommands(RelayCommand* commands, int maxCommands, int& commandCount);
    bool markCommandSent(int commandId);
    bool markCommandCompleted(int commandId);
    bool markCommandFailed(int commandId, const String& errorMessage);
    
    // Utilitários
    bool testConnection();
    String getLastError() { return lastError; }
    
private:
    String lastError;
    void setError(const String& error);
    String buildEnvironmentPayload(const EnvironmentReading& reading);
    String buildHydroPayload(const HydroReading& reading);
    String buildDeviceStatusPayload(const DeviceStatusData& status);
    
public:
    // ===== AUTO-REGISTRO =====
    bool autoRegisterDevice(const String& deviceName = "", const String& location = "");
};

#endif // SUPABASE_CLIENT_H 