#ifndef ESPNOW_TYPES_H
#define ESPNOW_TYPES_H

#include <Arduino.h>

// ===== TIPOS DE MENSAGEM (TASK ESP-NOW) =====
// NOTA: Renomeado para TaskMessageType para evitar conflito com MessageType do ESPNowController
enum TaskMessageType {
    TASK_MSG_WIFI_CREDENTIALS = 1,
    TASK_MSG_RELAY_COMMAND = 2,
    TASK_MSG_PING = 3,
    TASK_MSG_PONG = 4,
    TASK_MSG_DISCOVERY = 5,
    TASK_MSG_STATUS_REQUEST = 6,
    TASK_MSG_STATUS_RESPONSE = 7,
    TASK_MSG_HEARTBEAT = 8,
    TASK_MSG_CHANNEL_CHANGE = 9    // Notificação de mudança de canal
};

// ===== ESTRUTURAS DE DADOS (TASK ESP-NOW) =====
// NOTA: Renomeado para TaskESPNowMessage para evitar conflito com ESPNowMessage do ESPNowController
struct TaskESPNowMessage {
    TaskMessageType type;
    uint8_t targetMac[6];     // MAC destino (FF:FF:FF:FF:FF:FF para broadcast)
    uint8_t senderMac[6];     // MAC origem
    uint32_t timestamp;       // Timestamp da mensagem
    uint8_t data[200];        // Dados da mensagem
    uint8_t dataSize;         // Tamanho dos dados
    uint8_t checksum;         // Checksum para validação
    uint8_t retryCount;       // Contador de retry
};

struct WiFiCredentials {
    char ssid[33];            // SSID (máximo 32 chars + null)
    char password[65];        // Senha (máximo 64 chars + null)
    uint8_t channel;          // Canal WiFi
    uint8_t checksum;         // Checksum
};

struct ESPNowRelayCommand {
    uint8_t relayNumber;      // Número do relé (0-7)
    char action[16];          // Ação (on, off, toggle, on_forever)
    uint32_t duration;        // Duração em segundos (0 = permanente)
    uint8_t checksum;         // Checksum
};

struct ChannelChangeNotification {
    uint8_t oldChannel;       // Canal anterior
    uint8_t newChannel;       // Novo canal
    uint8_t reason;           // Motivo: 1=WiFi mudou, 2=Manual, 3=Interferência
    uint32_t changeTime;      // Timestamp da mudança
    uint8_t checksum;         // Checksum
};

struct SlaveInfo {
    uint8_t mac[6];           // MAC do slave
    char name[32];             // Nome do slave
    bool online;              // Status online
    uint32_t lastSeen;        // Última comunicação
    uint8_t relayCount;       // Número de relés
    int rssi;                 // Força do sinal
    uint32_t pingTimestamp;   // Timestamp do último ping enviado (para medir RTT)
    uint32_t latency;         // Latência em ms (RTT do ping/pong)
};

struct MasterInfo {
    uint8_t mac[6];           // MAC do master
    bool online;              // Status online
    uint32_t lastSeen;        // Última comunicação
    int rssi;                 // Força do sinal
};

#endif // ESPNOW_TYPES_H