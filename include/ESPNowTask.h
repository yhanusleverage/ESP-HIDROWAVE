#ifndef ESPNOW_TASK_H
#define ESPNOW_TASK_H

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <vector>
#include "ESPNowTypes.h"

// ===== CONFIGURAÇÕES DA TASK =====
#define ESPNOW_TASK_CORE 1                    // Core dedicado
#define ESPNOW_TASK_STACK_SIZE 8192           // Stack size
#define ESPNOW_TASK_PRIORITY 5                // Prioridade alta
#define ESPNOW_FIXED_CHANNEL 6                // Canal fixo (sem conflito com WiFi)
#define ESPNOW_QUEUE_SIZE 10                  // Tamanho da queue

// ===== CONFIGURAÇÕES DE TIMING (ARQUITETURA HÍBRIDA) =====
#define ESPNOW_HEARTBEAT_INTERVAL 30000       // Heartbeat broadcast a cada 30s
#define ESPNOW_PING_CYCLE_INTERVAL 6000       // Ping individual rotacionado a cada 6s
#define ESPNOW_CLEANUP_INTERVAL 60000         // Verificar offline a cada 60s
#define ESPNOW_OFFLINE_TIMEOUT 120000         // Marcar offline após 120s (2min) sem comunicação
#define ESPNOW_RETRY_INTERVAL 5000            // Retry a cada 5s
#define ESPNOW_MAX_RETRIES 3                  // Máximo de tentativas

// ===== ESTRUTURAS DE DADOS =====
// Todas as estruturas agora estão definidas em ESPNowTypes.h

// ===== CALLBACKS =====
typedef void (*ESPNowCallback)(const TaskESPNowMessage& message);
typedef void (*SlaveDiscoveryCallback)(const SlaveInfo& slave);
typedef void (*SlaveStatusCallback)(const uint8_t* mac, bool online);

// ===== CLASSE PRINCIPAL =====
class ESPNowTask {
public:
    ESPNowTask();
    ~ESPNowTask();
    
    // ===== INICIALIZAÇÃO =====
    bool begin();
    void end();
    
    // ===== ENVIO DE MENSAGENS =====
    // bool sendWiFiCredentials(...); // DESABILITADO - slaves não precisam WiFi
    bool sendRelayCommand(const uint8_t* targetMac, uint8_t relayNumber, const char* action, uint32_t duration = 0);
    bool sendPing(const uint8_t* targetMac);
    bool sendDiscovery();
    bool sendHeartbeat();
    bool sendChannelChangeNotification(uint8_t oldChannel, uint8_t newChannel, uint8_t reason);
    
    // ===== BROADCAST =====
    // bool broadcastWiFiCredentials(...); // DESABILITADO - slaves não precisam WiFi
    bool broadcastRelayCommand(uint8_t relayNumber, const char* action, uint32_t duration = 0);
    
    // ===== GERENCIAMENTO DE SLAVES =====
    void addSlave(const uint8_t* mac, const char* name, uint8_t relayCount);
    void removeSlave(const uint8_t* mac);
    SlaveInfo* findSlave(const uint8_t* mac);
    std::vector<SlaveInfo> getSlaves();
    int getOnlineSlaveCount();
    
    // ===== CALLBACKS =====
    void setMessageCallback(ESPNowCallback callback);
    void setSlaveDiscoveryCallback(SlaveDiscoveryCallback callback);
    void setSlaveStatusCallback(SlaveStatusCallback callback);
    
    // ===== STATUS =====
    bool isInitialized();
    String getStatusJSON();
    void printStatus();
    
    // ===== UTILITÁRIOS =====
    static String macToString(const uint8_t* mac);
    static bool stringToMac(const String& macStr, uint8_t* mac);
    String getLocalMacString();
    
    // ===== TASK MANAGEMENT =====
    static void taskFunction(void* parameter);
    void processMessageQueue();
    void processHeartbeat();
    void cleanupOfflineSlaves();
    
    // ===== MÉTODOS PARA CONEXÃO AUTOMÁTICA =====
    bool autoConnectToSlaves();
    // bool sendWiFiCredentialsBroadcast(...); // DESABILITADO - slaves não precisam WiFi
    void printSlavesList();
    uint8_t* findSlaveMac(const String& slaveName);

private:
    // ===== VARIÁVEIS DA TASK =====
    TaskHandle_t taskHandle;
    QueueHandle_t messageQueue;
    SemaphoreHandle_t mutex;
    
    // ===== ESP-NOW =====
    bool initialized;
    uint8_t localMac[6];
    uint8_t broadcastMac[6];
    
    // ===== SLAVES =====
    std::vector<SlaveInfo> slaves;
    
    // ===== CALLBACKS =====
    ESPNowCallback messageCallback;
    SlaveDiscoveryCallback discoveryCallback;
    SlaveStatusCallback statusCallback;
    
    // ===== TIMING =====
    uint32_t lastHeartbeat;
    uint32_t lastCleanup;
    uint32_t lastPingCycle;        // Controle do ping rotacionado
    int currentSlaveIndex;         // Índice do slave atual no ciclo de ping
    
    // ===== MÉTODOS PRIVADOS =====
    bool initializeESPNow();
    void registerCallbacks();
    uint8_t calculateChecksum(const uint8_t* data, uint8_t length);
    bool validateMessage(const TaskESPNowMessage& message);
    void processReceivedMessage(const TaskESPNowMessage& message);
    void updateSlaveStatus(const uint8_t* mac, bool online, int rssi = -50);
    
    // ===== CALLBACKS ESTÁTICOS =====
    static void onDataReceived(const uint8_t* mac, const uint8_t* data, int len);
    static void onDataSent(const uint8_t* mac, esp_now_send_status_t status);
    
    // ===== INSTÂNCIA ESTÁTICA =====
    static ESPNowTask* instance;
};

#endif // ESPNOW_TASK_H
