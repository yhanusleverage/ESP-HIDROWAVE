#ifndef MASTER_CONFIG_H
#define MASTER_CONFIG_H

#include <Arduino.h>

// ===== CONFIGURAÇÕES DO MASTER =====

// Identificação do Master
#define MASTER_DEVICE_NAME "MasterHidrowave"
#define MASTER_DEVICE_TYPE "MasterRelayController"
#define MASTER_VERSION "1.0.0"

// Configurações ESP-NOW
#define MASTER_WIFI_CHANNEL 1
#define MASTER_DISCOVERY_INTERVAL 30000      // 30 segundos
#define MASTER_STATUS_CHECK_INTERVAL 10000   // 10 segundos
#define MASTER_COMMAND_TIMEOUT 5000          // 5 segundos

// Configurações de RelayBox Local (8 relés)
#define MASTER_LOCAL_PCF_ADDRESS 0x20
#define MASTER_LOCAL_RELAY_COUNT 8

// Configurações de Comandos Automáticos
#define MASTER_AUTO_COMMAND_INTERVAL 60000   // 1 minuto
#define MASTER_BROADCAST_DELAY 100           // Delay entre comandos broadcast

// Configurações de Monitoramento
#define MASTER_STATUS_PRINT_INTERVAL 30000   // 30 segundos
#define MASTER_HEALTH_CHECK_INTERVAL 60000  // 1 minuto
#define MASTER_DEVICE_TIMEOUT 60000          // 60 segundos para considerar offline

// Configurações de Retry
#define MASTER_MAX_RETRY_ATTEMPTS 3
#define MASTER_RETRY_DELAY 1000
#define MASTER_CRITICAL_COMMAND_TIMEOUT 10000

// Configurações de Log
#define MASTER_DEBUG_LEVEL 2                 // 0=Error, 1=Warning, 2=Info, 3=Debug
#define MASTER_LOG_TO_SERIAL true
#define MASTER_LOG_TO_WEB false

// ===== CONFIGURAÇÕES DE DISPOSITIVOS ESCRAVOS =====

// Tipos de dispositivos suportados
#define SUPPORTED_DEVICE_TYPES "RelayCommandBox,SlaveRelay,RelayBox"

// Configurações de descoberta
#define MASTER_AUTO_DISCOVERY true
#define MASTER_DISCOVERY_RETRY_INTERVAL 5000
#define MASTER_MAX_DISCOVERY_ATTEMPTS 10

// Configurações de reconexão
#define MASTER_AUTO_RECONNECT true
#define MASTER_RECONNECT_INTERVAL 30000
#define MASTER_RECONNECT_ATTEMPTS 5

// ===== CONFIGURAÇÕES DE COMANDOS =====

// Comandos automáticos pré-configurados
struct AutoCommand {
    String name;
    String devicePattern;    // Padrão de nome de dispositivo
    int relayNumber;
    String action;
    int duration;
    unsigned long interval;  // Intervalo em ms
    unsigned long lastExecuted;
    bool enabled;
};

// Comandos automáticos padrão
static const AutoCommand DEFAULT_AUTO_COMMANDS[] = {
    {"Bomba Principal", "SlaveBox*", 0, "on", 30, 300000, 0, true},      // A cada 5 min
    {"Luzes LED", "SlaveBox*", 1, "toggle", 0, 600000, 0, false},        // A cada 10 min
    {"Ventilador", "SlaveBox*", 2, "on", 60, 1800000, 0, false},          // A cada 30 min
    {"Verificação", "SlaveBox*", 7, "status", 0, 60000, 0, true}         // A cada 1 min
};

// ===== CONFIGURAÇÕES DE RELÉS =====

// Mapeamento de relés por função
#define RELAY_PUMP_MAIN 0
#define RELAY_LIGHTS 1
#define RELAY_FAN 2
#define RELAY_HEATER 3
#define RELAY_SOLENOID_1 4
#define RELAY_SOLENOID_2 5
#define RELAY_ALARM 6
#define RELAY_RESERVE 7

// Nomes padrão dos relés - Definido em DataTypes.h
// Usar RELAY_NAMES de DataTypes.h para evitar conflitos

// Configurações de segurança
#define RELAY_MAX_DURATION 3600              // 1 hora máximo
#define RELAY_SAFETY_TIMEOUT 300000          // 5 minutos timeout de segurança
#define RELAY_EMERGENCY_STOP_RELAY 7         // Relé de parada de emergência

// ===== CONFIGURAÇÕES DE INTERFACE =====

// Comandos Serial disponíveis
#define SERIAL_CMD_PREFIX "CMD"
#define SERIAL_BROADCAST_PREFIX "BROADCAST"
#define SERIAL_STATUS_CMD "STATUS"
#define SERIAL_DISCOVER_CMD "DISCOVER"
#define SERIAL_JSON_CMD "JSON"
#define SERIAL_HELP_CMD "HELP"
#define SERIAL_CONFIG_CMD "CONFIG"
#define SERIAL_AUTO_CMD "AUTO"

// Configurações de Web Interface (se implementada)
#define WEB_SERVER_PORT 80
#define WEB_UPDATE_INTERVAL 1000
#define WEB_MAX_CONNECTIONS 4

// ===== CONFIGURAÇÕES DE ESTATÍSTICAS =====

// Intervalos de coleta de estatísticas
#define STATS_COLLECTION_INTERVAL 10000      // 10 segundos
#define STATS_RESET_INTERVAL 3600000         // 1 hora

// Métricas a serem coletadas
#define COLLECT_NETWORK_STATS true
#define COLLECT_DEVICE_STATS true
#define COLLECT_COMMAND_STATS true
#define COLLECT_ERROR_STATS true

// ===== CONFIGURAÇÕES DE ALERTAS =====

// Thresholds para alertas
#define ALERT_DEVICE_OFFLINE_COUNT 2         // Alertar se mais de 2 dispositivos offline
#define ALERT_HIGH_PACKET_LOSS 0.1           // 10% de perda de pacotes
#define ALERT_LOW_MEMORY 10000               // Menos de 10KB de memória livre
#define ALERT_HIGH_TEMPERATURE 70            // Temperatura acima de 70°C

// Configurações de notificação
#define ALERT_NOTIFY_SERIAL true
#define ALERT_NOTIFY_WEB false
#define ALERT_NOTIFY_LED true

// ===== CONFIGURAÇÕES DE DEBUG =====

// Níveis de debug
#define DEBUG_LEVEL_ERROR 0
#define DEBUG_LEVEL_WARNING 1
#define DEBUG_LEVEL_INFO 2
#define DEBUG_LEVEL_DEBUG 3

// Configurações de debug por componente
#define DEBUG_ESPNOW true
#define DEBUG_RELAYS true
#define DEBUG_DISCOVERY true
#define DEBUG_COMMANDS true
#define DEBUG_NETWORK true

// ===== MACROS DE DEBUG =====

#if MASTER_DEBUG_LEVEL >= DEBUG_LEVEL_ERROR
    #define DEBUG_ERROR(x) if(MASTER_LOG_TO_SERIAL) Serial.println("[ERROR] " + String(x))
#else
    #define DEBUG_ERROR(x)
#endif

#if MASTER_DEBUG_LEVEL >= DEBUG_LEVEL_WARNING
    #define DEBUG_WARNING(x) if(MASTER_LOG_TO_SERIAL) Serial.println("[WARNING] " + String(x))
#else
    #define DEBUG_WARNING(x)
#endif

#if MASTER_DEBUG_LEVEL >= DEBUG_LEVEL_INFO
    #define DEBUG_INFO(x) if(MASTER_LOG_TO_SERIAL) Serial.println("[INFO] " + String(x))
#else
    #define DEBUG_INFO(x)
#endif

#if MASTER_DEBUG_LEVEL >= DEBUG_LEVEL_DEBUG
    #define DEBUG_DEBUG(x) if(MASTER_LOG_TO_SERIAL) Serial.println("[DEBUG] " + String(x))
#else
    #define DEBUG_DEBUG(x)
#endif

// ===== CONFIGURAÇÕES DE MEMÓRIA =====

// Tamanhos de buffers
#define MASTER_MAX_DEVICES 20
#define MASTER_MAX_COMMANDS_QUEUE 50
#define MASTER_MAX_LOG_ENTRIES 100
#define MASTER_JSON_BUFFER_SIZE 2048

// Configurações de heap
#define MASTER_MIN_FREE_HEAP 20000           // 20KB mínimo
#define MASTER_HEAP_WARNING_THRESHOLD 50000  // 50KB para aviso
#define MASTER_HEAP_CRITICAL_THRESHOLD 10000 // 10KB crítico

// ===== CONFIGURAÇÕES DE TIMING =====

// Timeouts
#define MASTER_INIT_TIMEOUT 10000            // 10 segundos para inicialização
#define MASTER_COMMAND_TIMEOUT 5000          // 5 segundos para comandos
#define MASTER_DISCOVERY_TIMEOUT 30000       // 30 segundos para descoberta
#define MASTER_STATUS_TIMEOUT 10000          // 10 segundos para status

// Intervalos
#define MASTER_UPDATE_INTERVAL 100           // 100ms para update principal
#define MASTER_SERIAL_INTERVAL 50            // 50ms para processamento serial
#define MASTER_STATS_INTERVAL 1000           // 1 segundo para estatísticas

// ===== CONFIGURAÇÕES DE SEGURANÇA =====

// Validações de entrada
#define MASTER_VALIDATE_RELAY_NUMBER true
#define MASTER_VALIDATE_DEVICE_NAME true
#define MASTER_VALIDATE_COMMAND true
#define MASTER_VALIDATE_DURATION true

// Limites de segurança
#define MASTER_MAX_RELAY_NUMBER 7
#define MASTER_MAX_DURATION 3600
#define MASTER_MAX_DEVICE_NAME_LENGTH 32
#define MASTER_MAX_COMMAND_LENGTH 16

// ===== CONFIGURAÇÕES DE PERFORMANCE =====

// Otimizações
#define MASTER_USE_CALLBACKS true
#define MASTER_USE_ASYNC true
#define MASTER_USE_CACHING true
#define MASTER_USE_COMPRESSION false

// Configurações de rede
#define MASTER_OPTIMIZE_PACKET_SIZE true
#define MASTER_USE_PACKET_BATCHING false
#define MASTER_USE_PRIORITY_QUEUE false

// ===== CONFIGURAÇÕES DE BACKUP =====

// Configurações de backup automático
#define MASTER_BACKUP_ENABLED true
#define MASTER_BACKUP_INTERVAL 3600000       // 1 hora
#define MASTER_BACKUP_RETENTION 7            // 7 dias

// Configurações de restore
#define MASTER_RESTORE_ENABLED true
#define MASTER_RESTORE_TIMEOUT 30000         // 30 segundos

#endif // MASTER_CONFIG_H

