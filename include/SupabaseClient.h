#ifndef SUPABASE_CLIENT_H
#define SUPABASE_CLIENT_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "Config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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

// ✅ FORK: Tipos de comando
enum CommandType {
    COMMAND_MANUAL = 0,      // Comando manual do usuário (botão)
    COMMAND_RULE = 1,        // Comando gerado por regra de automação
    COMMAND_PERISTALTIC = 2  // Comando de dosagem proporcional
};

struct RelayCommand {
    int id;                    // ID do comando no banco
    int relayNumber;           // 0-15
    String action;             // "on" ou "off"
    int durationSeconds;       // duração em segundos (opcional)
    String status;             // "pending", "processing", "sent", "completed", "failed", "expired"
    unsigned long timestamp;
    // ✅ INTEGRAÇÃO ESP-NOW: Target device ("" ou "local" = relés locais, "SLAVE-NAME" = ESP-NOW slave)
    String target_device_id;  // ID do dispositivo destino ("" = local, "ESP-NOW-SLAVE" = slave remoto)
    
    // ✅ FORK: Tipo de comando (bifurcação)
    String command_type;      // "manual" | "rule" | "peristaltic"
    String triggered_by;      // "manual" | "automation" | "rule" | "peristaltic"
    String rule_id;           // ID da regra (se command_type = "rule")
    String rule_name;         // Nome da regra (se command_type = "rule")
    
    // ✅ PRIORIDADE: Prioridade numérica (0-100). Maior = mais importante
    int priority;             // Default: 50 (média prioridade)
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
    int rebootCount;            // ✅ NOVO: Contador de reinícios (persistido em NVS)
};

// ✅ Estrutura para EC Config (JSON optimizado)
struct ECConfig {
    double base_dose;
    double flow_rate;
    double volume;
    double total_ml;
    double kp;
    double ec_setpoint;
    bool auto_enabled;
    int intervalo_auto_ec;
    unsigned long tempo_recirculacao;  // Em segundos
    String nutrientsJson;  // JSON string de nutrientes (não salvo em NVS, usado para cálculo local)
    
    bool isValid;
};

class SupabaseClient {
private:
    HTTPClient http;
    WiFiClientSecure* secureClient;  // ✅ Cliente SSL persistente para todas las conexiones
    String baseUrl;
    String apiKey;
    bool isConnected;
    unsigned long lastCommandCheck;
    
    // ✅ NOVO: Mutex para thread-safety (proteção contra race conditions)
    SemaphoreHandle_t requestMutex;        // Protege makeRequest() - evita concorrência
    SemaphoreHandle_t commandCheckMutex;   // Protege checkForCommands() - evita race conditions
    
    String buildAuthHeader();
    bool makeRequest(const String& method, const String& endpoint, const String& payload = "");
    String buildRelayStatePayload(bool* relayStates, int numRelays);
    
    // ✅ NOVO: Inicializar e limpar mutexes
    bool initMutexes();
    void cleanupMutexes();

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
    
    // ✅ NOVO: Funções RPC atômicas para buscar comandos
    bool checkForMasterCommands(RelayCommand* commands, int maxCommands, int& commandCount);
    bool checkForSlaveCommands(RelayCommand* commands, int maxCommands, int& commandCount);
    
    // ✅ NOVO: Marcar comandos com suporte para Master/Slave
    bool markCommandSent(int commandId, bool isSlave = false);
    bool markCommandCompleted(int commandId, bool currentState = false, bool isSlave = false);
    bool markCommandFailed(int commandId, const String& errorMessage, bool isSlave = false);
    
    // ✅ NOVO: Atualizar estados dos relés master (relay_master com arrays segregados)
    bool updateRelayMaster(const String& deviceId, bool* relayStates, bool* hasTimers = nullptr, 
                          int* remainingTimes = nullptr, const String* relayNames = nullptr);
    
    // ✅ NOVO: Atualizar estados dos relés de slave (relay_slaves com arrays)
    bool updateRelaySlaves(const String& slaveDeviceId, const String& masterDeviceId,
                          const String& slaveMacAddress, bool* relayStates, 
                          bool* hasTimers = nullptr, int* remainingTimes = nullptr, 
                          const String* relayNames = nullptr);
    
    // ✅ LEGACY: Método antigo (mantido para compatibilidade)
    bool updateSlaveRelayState(const String& masterDeviceId, const String& slaveMacAddress, 
                               const String& slaveDeviceId, int relayNumber, 
                               bool state, bool hasTimer = false, int remainingTime = 0);
    
    // ✅ LEGACY: Método de fallback para compatibilidade (slave_relay_states)
    bool updateSlaveRelayStateLegacy(const String& masterDeviceId, const String& slaveMacAddress, 
                               const String& slaveDeviceId, int relayNumber, 
                               bool state, bool hasTimer = false, int remainingTime = 0);
    
    // ✅ NOVO: Buscar EC Config do Supabase via RPC activate_auto_ec
    bool getECConfigFromSupabase(ECConfig& config);
    
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