#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <CircularBuffer.hpp>
#include "APIConfig.h"
#include "DataTypes.h"
#include "Config.h"

// Buffer circular para dados dos sensores
#define SENSOR_BUFFER_SIZE 10
#define MAX_BATCH_SIZE 5  // Máximo de itens por lote

// ===== ESTRUTURAS PARA SUPABASE =====
struct DeviceStatusData {
    String firmwareVersion;
    bool relayStates[16];
    int wifiRssi;
    unsigned long freeHeap;
    unsigned long uptimeSeconds;
    String ipAddress;
};

class APIClient {
private:
    HTTPClient http;
    String supabaseUrl;
    String supabaseKey;
    String deviceId;
    
    // Buffers circulares para dados
    CircularBuffer<SensorData, SENSOR_BUFFER_SIZE> sensorBuffer;
    CircularBuffer<SystemStatus, SENSOR_BUFFER_SIZE> statusBuffer;
    
    // Timestamps de último envio
    unsigned long lastStatusTime = 0;
    unsigned long lastSendTime = 0;
    unsigned long lastCheckTime = 0;
    
    // Intervalos de envio
    static const unsigned long STATUS_UPDATE_INTERVAL = 30000;  // 30 segundos
    static const unsigned long POLLING_INTERVAL = 5000;        // 5 segundos
    static const unsigned long BATCH_INTERVAL = 60000;         // 1 minuto

    // Callback para comandos dos relés
    void (*relayCallback)(int relayNumber, String action, int duration, String commandId) = nullptr;

    // Métodos privados
    bool makeSupabaseRequest(const String& endpoint, const String& payload, String& response);
    bool shouldSendBatch();
    
    // Métodos privados para Supabase
    void processRelayCommand(JsonObject command);

public:
    APIClient();
    
    // Configuração
    void setSupabaseConfig(const String& url, const String& key) { 
        supabaseUrl = url; 
        supabaseKey = key; 
    }
    void setDeviceId(const String& id) { deviceId = id; }
    
    // Status e conexão
    bool isConnected() { return WiFi.status() == WL_CONNECTED; }
    bool testConnection();
    
    // Envio de dados otimizado
    bool queueSensorData(const SensorData& data);
    bool queueSystemStatus(const SystemStatus& status);
    bool sendBatch();
    
    // ===== MÉTODOS PARA SUPABASE =====
    // Controle de relés
    bool checkRelayCommands(const String& deviceId);
    bool confirmCommandExecution(const String& commandId, const String& status, const String& errorMessage = "");
    
    // Status do dispositivo
    bool sendDeviceStatus(const String& deviceId, const DeviceStatusData& statusData);
    
    // Dados dos sensores
    bool sendSensorData(const String& deviceId, const SensorData& data);
    
    // Configurar callback
    void setRelayCallback(void (*callback)(int relayNumber, String action, int duration, String commandId));
    
    // Método para processar dados em background
    void update();
};

#endif // API_CLIENT_H 